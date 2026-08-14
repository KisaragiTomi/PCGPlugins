#include "ComputeShaderLandscapeRoad.h"

#include "RoadMeshComponent.h"
#include "RoadTypes.h"
#include "RoadBuilderShaders.h"
#include "ComputeShaderMeshGenerator.h"
#include "Components/SplineComponent.h"
#include "Containers/Ticker.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Landscape.h"
#include "Misc/App.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "RHICommandList.h"

// May this actor run a blocking road generation right now? Every case below is one where the
// generation would either write into something shared with all instances or burn a full GPU stall
// (RebuildRoad ends in FlushRenderingCommands) for output nobody will ever look at.
static bool CSRoad_CanGenerateFor(const AActor* Actor)
{
	if (!IsValid(Actor)) return false;
	// A CDO/archetype has no world, and anything a generation leaves on it propagates into every
	// instance created afterwards.
	if (Actor->IsTemplate()) return false;
	// Cook and every other commandlet: the road exists only as GPU buffers and nothing about it is
	// serialized, so a per-road-actor GPU stall during a cook buys exactly nothing.
	if (IsRunningCommandlet() || !FApp::CanEverRender()) return false;
	// The road compute path needs SM5; the road operator refuses to build below it, so generating
	// would only pay the CPU resample and the heightmap rasterize for a mesh that cannot draw.
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return false;
	const UWorld* World = Actor->GetWorld();
	if (!World) return false;
	// Only worlds somebody is actually looking at. Inactive worlds (a level held open by the editor
	// but not the one being edited) and preview worlds (asset thumbnails, the Blueprint editor
	// viewport) both register components and neither renders this road.
	const EWorldType::Type WorldType = World->WorldType;
	return WorldType == EWorldType::Editor || WorldType == EWorldType::PIE || WorldType == EWorldType::Game;
}

ACSLandscapeRoad::ACSLandscapeRoad()
{
	RoadMesh = CreateDefaultSubobject<URoadMeshComponent>(TEXT("RoadMesh"));
	RoadMesh->SetupAttachment(RootComponent);
}

void ACSLandscapeRoad::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// Stays an unconditional rebuild: this is the authoring response (RoadWidth edited, actor moved,
	// undo), and the input it is reacting to has just changed by definition. EnsureRoadGeometry() is
	// the restore path and must not be confused with it.
	RebuildRoad();
}

void ACSLandscapeRoad::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	ScheduleEnsureRoadGeometry();
}

void ACSLandscapeRoad::CollectSplines(TArray<USplineComponent*>& OutSplines) const
{
	TArray<USplineComponent*> OwnSplines;
	GetComponents<USplineComponent>(OwnSplines);
	OutSplines.Append(OwnSplines);

	TArray<AActor*> Attached;
	GetAttachedActors(Attached, /*bResetArray=*/true, /*bRecursivelyIncludeNestedActors=*/true);
	for (AActor* Child : Attached)
	{
		if (!IsValid(Child)) continue;
		TArray<USplineComponent*> ChildSplines;
		Child->GetComponents<USplineComponent>(ChildSplines);
		OutSplines.Append(ChildSplines);
	}
}

bool ACSLandscapeRoad::BuildRoadInput(FRoadBuildInput& Input) const
{
	TArray<USplineComponent*> Splines;
	CollectSplines(Splines);

	Input.SampleStep = SampleStep;
	Input.RoadHalfWidth = RoadWidth * 0.5f;
	Input.IntersectionMergeRadius = IntersectionMergeFactor;
	Input.UVLengthScale = 1.0f / FMath::Max(UVTileLength, 1.0f);

	const FTransform ActorToLocal = GetActorTransform().Inverse();

	for (USplineComponent* Spline : Splines)
	{
		if (!IsValid(Spline) || Spline->GetNumberOfSplinePoints() < 2) continue;

		const float Length = Spline->GetSplineLength();
		if (Length < KINDA_SMALL_NUMBER) continue;

		const int32 NumSamples = FMath::Clamp(FMath::CeilToInt(Length / SampleStep) + 1, 2, 4096);
		const float Step = Length / float(NumSamples - 1);

		FRoadSplineRange Range;
		Range.FirstPoint = Input.Points.Num();
		Range.NumPoints = NumSamples;
		Range.Length = Length;
		Range.HalfWidth = Input.RoadHalfWidth;

		for (int32 i = 0; i < NumSamples; ++i)
		{
			const float Dist = Step * float(i);
			const FVector WorldPos = Spline->GetLocationAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World);
			const FVector LocalPos = ActorToLocal.TransformPosition(WorldPos);

			FRoadSplinePoint Point;
			Point.Position = FVector3f(LocalPos);
			Point.DistanceAlongSpline = Dist;
			Input.Points.Add(Point);
			Input.LocalBounds += LocalPos;
		}
		Input.Splines.Add(Range);
	}

	if (Input.Splines.Num() == 0) return false;

	// Conservative GPU allocation sizes (mirrors ARoadGeneratorActor::RebuildRoads).
	const uint32 NumSplines = Input.Splines.Num();
	const uint32 TotalSamples = Input.Points.Num();
	const uint32 MaxLegsPerJunction = 12; // MAX_LEGS in RoadBuilder.usf

	Input.MaxIntersections = FMath::Clamp<uint32>(NumSplines * NumSplines * 4 + 8, 16, 1024);
	const uint32 MaxCorners = Input.MaxIntersections * MaxLegsPerJunction;
	const uint32 RibbonVerts = TotalSamples * 2 + NumSplines * 2;
	const uint32 CornerVerts = TotalSamples * 4 + MaxCorners * 6;
	Input.MaxVertices = RibbonVerts + CornerVerts + 64;
	Input.MaxIndices = Input.MaxVertices * 3;

	if (Input.LocalBounds.IsValid)
		Input.LocalBounds = Input.LocalBounds.ExpandBy(FVector(RoadWidth * 2.0, RoadWidth * 2.0, RoadWidth));

	return true;
}

void ACSLandscapeRoad::EnsureRoadHeightRT()
{
	// Match RT_Result (sized by ReadLandscapeDataToTexture) so the depth->result convert is 1:1.
	if (!RT_Result) return;
	const int32 W = RT_Result->SizeX;
	const int32 H = RT_Result->SizeY;
	if (RT_RoadHeight && RT_RoadHeight->SizeX == W && RT_RoadHeight->SizeY == H) return;

	RT_RoadHeight = UKismetRenderingLibrary::CreateRenderTarget2D(
		this, W, H, ETextureRenderTargetFormat::RTF_R32f, FLinearColor::Black,
		/*bAutoGenerateMipMaps=*/false, /*bSupportUAV=*/true);
}

void ACSLandscapeRoad::RebuildRoad()
{
	if (!RoadMesh) return;

	FRoadBuildInput Input;
	if (!BuildRoadInput(Input)) return;

	// 1) Build and draw the visible road mesh. The samples are in this actor's local space
	//    (BuildRoadInput resamples them there), so that is the transform the component bakes the
	//    geometry into world space with — and the one the saved asset comes back out of.
	RoadMesh->MeshMaterial = RoadMaterial;
	RoadMesh->SetBuildInput(Input, GetActorTransform());

	if (!Input.LocalBounds.IsValid) return;
	const FBox WorldBounds = Input.LocalBounds.TransformBy(GetActorTransform());

	// 2) Fit the capture box to the road so the edit-layer merge covers exactly the road area.
	if (Box)
	{
		Box->SetWorldScale3D(FVector::OneVector);
		Box->SetWorldLocation(WorldBounds.GetCenter());
		Box->SetBoxExtent(WorldBounds.GetExtent());
	}

	// 3) Size the RTs to the landscape's sampling density over the box, so the road heightmap is
	//    1:1 with the landscape heightmap (no resampling, most precise). Drives RT_LandscapeData /
	//    RT_Result / RT_RoadHeight.
	if (const ALandscape* Landscape = FindLandscape())
	{
		const float QuadSize = FMath::Max(FMath::Abs(Landscape->GetActorScale3D().X), 1.0f);
		const int32 ResX = FMath::Clamp(FMath::CeilToInt(WorldBounds.GetSize().X / QuadSize) + 1, 32, 2048);
		const int32 ResY = FMath::Clamp(FMath::CeilToInt(WorldBounds.GetSize().Y / QuadSize) + 1, 32, 2048);
		EnsureRTs(ResX, ResY);
	}

	// 4) Read the current landscape under the box (sets Orig_LandscapeData + reuses the RT sizes).
	ReadLandscapeDataToTexture();
	if (!RT_Result) return;

	EnsureRoadHeightRT();
	if (!RT_RoadHeight) return;

	// 4) Rasterize road -> RT_RoadHeight (depth), then convert -> RT_Result (WorldZ, coverage).
	const float CameraHeight = WorldBounds.Max.Z + 10000.0f;
	BuildRoadHeightRT(Input, WorldBounds, CameraHeight);

	// 5) The parent edit-layer merge sets the terrain to the road height where covered
	//    (lerp(existingTerrain, WorldZ, coverage)) -- correct units, no additive spike.
	bHasResult = true;
	bRealtimeUpdate = false; // use only the cached road result, not the realtime external blend
	RequestLandscapeUpdate(true);
}

bool ACSLandscapeRoad::EnsureRoadGeometry()
{
	if (!RoadMesh) return false;
	// The idempotent half of the contract. Asked of the component rather than assumed from a
	// "did I already run" flag on the actor, because the geometry can disappear without this actor
	// hearing about it (the component being reset, a failed build) and a stale flag would then
	// permanently refuse to restore it.
	if (RoadMesh->HasGeneratedGeometry()) return true;
	if (!CSRoad_CanGenerateFor(this)) return false;

	// No usable spline is "there is no road here", not "restore an empty road". Bailing before
	// RebuildRoad() also keeps it from resizing the RTs and re-publishing an empty edit-layer
	// contribution over whatever the landscape currently holds. The predicate is the one
	// BuildRoadInput() accepts a spline on; a road whose splines arrive later is picked up by the
	// next RebuildRoad() (the construction rerun that attaching them triggers).
	TArray<USplineComponent*> Splines;
	CollectSplines(Splines);
	const bool bHasUsableSpline = Splines.ContainsByPredicate([](const USplineComponent* Spline)
	{
		return IsValid(Spline) && Spline->GetNumberOfSplinePoints() >= 2 && Spline->GetSplineLength() >= KINDA_SMALL_NUMBER;
	});
	if (!bHasUsableSpline) return false;

	RebuildRoad();
	return RoadMesh->HasGeneratedGeometry();
}

// Why PostRegisterAllComponents, and why the work is still pushed a tick out.
//
// Rejected alternatives:
//   PostLoad               — runs mid-package-load. GetWorld() is not dependable there, the child
//                            spline actors CollectSplines() walks are not attached yet, and
//                            RebuildRoad() ends in FlushRenderingCommands, which is the last thing
//                            to do from inside async loading.
//   OnRegister             — fires per component and is re-entered by every re-registration
//                            (transform edit, visibility toggle, a construction rerun). A blocking
//                            GPU build at that frequency is not affordable, and RoadMesh registers
//                            before the actor's remaining components exist.
//   OnConstruction         — kept, but as the authoring rebuild, not as the restore. It is also not
//                            a universal restore point: construction scripts are only re-run for
//                            uncooked data (see UWorld::UpdateLevelStreaming), so nothing calls it
//                            for a level-placed actor once the data is cooked.
//   PostEditChangeProperty — editor-only and user-driven; never fires on a level load at all, so it
//                            can never be the thing that brings the road back.
//
// PostRegisterAllComponents is the one hook that fires in the editor, in PIE and in a game world,
// and only once every component of this actor is live. The build is deferred to the next tick for
// two independent reasons: a level load runs IncrementalRegisterComponents before
// IncrementalRunConstructionScripts, so in an uncooked world OnConstruction -> RebuildRoad() is
// already queued up behind us and building here would cost a second full GPU stall on every level
// open; and blocking on the GPU from inside the level's registration pass blocks the load itself.
// By the time the ticker fires, the idempotent check above turns that ordering into a no-op.
void ACSLandscapeRoad::ScheduleEnsureRoadGeometry()
{
	if (bRoadGeometryRestoreAttempted) return;
	if (!CSRoad_CanGenerateFor(this)) return;

	bRoadGeometryRestoreAttempted = true;
	// Weak, because the actor can be destroyed (level closed, undo of a placement, PIE ending)
	// between the schedule and the tick, and the ticker outlives it.
	TWeakObjectPtr<ACSLandscapeRoad> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
	{
		if (ACSLandscapeRoad* Road = WeakThis.Get()) Road->EnsureRoadGeometry();
		return false; // one shot
	}), 0.0f);
}

void ACSLandscapeRoad::BuildRoadHeightRT(const FRoadBuildInput& Input, const FBox& WorldBounds, float CameraHeight)
{
	if (!RT_RoadHeight || !RT_Result) return;
	FTextureRenderTargetResource* DepthRes = RT_RoadHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* ResultRes = RT_Result->GameThread_GetRenderTargetResource();
	if (!DepthRes || !ResultRes) return;

	const uint32 MaxVerts = FMath::Max(Input.MaxVertices, 64u);
	const uint32 MaxIdx = FMath::Max(Input.MaxIndices, 192u);
	const uint32 TriCap = FMath::Max(MaxIdx / 3u, 1u);
	const FMatrix44f LocalToWorld(GetActorTransform().ToMatrixWithScale());
	AComputeShaderMeshGenerator* Self = this;

	// Terrain-influence params -> shader inputs (falloff cm -> texel radius via the box mapping).
	const float Influence = FMath::Clamp(RoadInfluence, 0.0f, 1.0f);
	const float RoadOffset = RoadHeightOffset;
	const float TexelWorld = (RT_Result->SizeX > 0)
		? static_cast<float>(WorldBounds.GetSize().X) / static_cast<float>(RT_Result->SizeX) : 1.0f;
	// Height-diffusion budget: a shoulder of ~R texels needs ~0.5*R^2 Jacobi iterations.
	const float FalloffTexels = RoadEdgeFalloff / FMath::Max(TexelWorld, 1.0f);
	const int32 DiffuseIters = FMath::Clamp(FMath::RoundToInt(0.5f * FalloffTexels * FalloffTexels), 0, 256);

	ENQUEUE_RENDER_COMMAND(CSLandscapeRoadHeightmap)(
	[Self, Input, LocalToWorld, WorldBounds, CameraHeight, DepthRes, ResultRes, MaxVerts, MaxIdx, TriCap, Influence, RoadOffset, DiffuseIters]
	(FRHICommandListImmediate& RHICmdList)
	{
		const ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;

		TRefCountPtr<FRDGPooledBuffer> PosPooled = AllocatePooledBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), MaxVerts * 3), TEXT("CSLR.Positions"));
		TRefCountPtr<FRDGPooledBuffer> TanPooled = AllocatePooledBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxVerts * 2), TEXT("CSLR.Tangents"));
		TRefCountPtr<FRDGPooledBuffer> UVPooled = AllocatePooledBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), MaxVerts * 2), TEXT("CSLR.TexCoords"));
		TRefCountPtr<FRDGPooledBuffer> ColPooled = AllocatePooledBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxVerts), TEXT("CSLR.Colors"));
		TRefCountPtr<FRDGPooledBuffer> IdxPooled = AllocatePooledBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxIdx), TEXT("CSLR.Indices"));
		TRefCountPtr<FRDGPooledBuffer> ArgPooled = AllocatePooledBuffer(
			FRDGBufferDesc::CreateIndirectDesc(sizeof(uint32), 5), TEXT("CSLR.IndirectArgs"));

		// Graph 1: build the road geometry into the pooled buffers.
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSLandscapeRoad.Build"));
			FRoadGeometryBuffers Out;
			Out.Positions = GraphBuilder.RegisterExternalBuffer(PosPooled);
			Out.Tangents = GraphBuilder.RegisterExternalBuffer(TanPooled);
			Out.TexCoords = GraphBuilder.RegisterExternalBuffer(UVPooled);
			Out.Colors = GraphBuilder.RegisterExternalBuffer(ColPooled);
			Out.Indices = GraphBuilder.RegisterExternalBuffer(IdxPooled);
			Out.IndirectArgs = GraphBuilder.RegisterExternalBuffer(ArgPooled);
			BuildRoadGeometryRDG(GraphBuilder, FeatureLevel, Input, Out);
			GraphBuilder.SetBufferAccessFinal(Out.Positions, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(Out.Indices, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		}

		FShaderResourceViewRHIRef PosSRV = RHICmdList.CreateShaderResourceView(PosPooled->GetRHI(),
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_FLOAT));
		FShaderResourceViewRHIRef IdxSRV = RHICmdList.CreateShaderResourceView(IdxPooled->GetRHI(),
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_UINT));
		if (!PosSRV.IsValid() || !IdxSRV.IsValid()) return;

		// Graph 2: indexed geometry -> depth heightmap, then depth -> (WorldZ, coverage) result.
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSLandscapeRoad.Rasterize"));
			FRDGTextureRef DepthTex = RegisterExternalTexture(GraphBuilder, DepthRes->GetRenderTargetTexture(), TEXT("CSLR.Depth"));
			FRDGTextureRef ResultTex = RegisterExternalTexture(GraphBuilder, ResultRes->GetRenderTargetTexture(), TEXT("CSLR.Result"));
			Self->RasterizeIndexedMeshToHeightmapRDG(GraphBuilder,
				PosSRV.GetReference(), IdxSRV.GetReference(), TriCap, LocalToWorld, DepthTex, WorldBounds, CameraHeight);

			// Convert into a temp UAV texture, then copy to RT_Result (not UAV-capable itself).
			const FRDGTextureDesc TmpDesc = FRDGTextureDesc::Create2D(DepthTex->Desc.Extent, PF_FloatRGBA,
				FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
			FRDGTextureRef TmpResult = GraphBuilder.CreateTexture(TmpDesc, TEXT("CSLR.ResultTmp"));
			AddRoadDepthToResultPass(GraphBuilder, FeatureLevel, DepthTex, TmpResult, CameraHeight,
				Influence, RoadOffset, DiffuseIters);
			AddCopyTexturePass(GraphBuilder, TmpResult, ResultTex, FRHICopyTextureInfo());
			GraphBuilder.Execute();
		}
	});

	FlushRenderingCommands();
}
