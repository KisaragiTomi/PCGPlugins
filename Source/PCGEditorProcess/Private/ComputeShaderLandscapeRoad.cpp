#include "ComputeShaderLandscapeRoad.h"

#include "RoadMeshComponent.h"
#include "RoadTypes.h"
#include "RoadBuilderShaders.h"
#include "ComputeShaderMeshGenerator.h"
#include "Components/SplineComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Landscape.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"

ACSLandscapeRoad::ACSLandscapeRoad()
{
	RoadMesh = CreateDefaultSubobject<URoadMeshComponent>(TEXT("RoadMesh"));
	RoadMesh->SetupAttachment(RootComponent);
}

void ACSLandscapeRoad::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RebuildRoad();
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

	// 1) Render the visible road mesh (SetBuildInput consumes a moved snapshot).
	RoadMesh->RoadMaterial = RoadMaterial;
	FRoadBuildInput RenderInput = Input;
	RoadMesh->SetBuildInput(MoveTemp(RenderInput));

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
