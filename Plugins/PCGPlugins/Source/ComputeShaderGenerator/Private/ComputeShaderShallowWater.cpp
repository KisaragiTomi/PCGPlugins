#include "ComputeShaderShallowWater.h"
#include "ComputeShaderGenerateHepler.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "SparseTileDispatchHelper.h"
#include "Landscape.h"
#include "Components/BillboardComponent.h"
#include "Engine/DecalActor.h"
#include "Kismet/GameplayStatics.h"
#include "ClearQuad.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "ThirdParty/RenderDoc/renderdoc_app.h"
#include "Windows/HideWindowsPlatformTypes.h"

static RENDERDOC_API_1_1_1* GRenderDocAPI = nullptr;

static void InitRenderDocAPI()
{
	if (GRenderDocAPI) return;

	HMODULE RDMod = GetModuleHandleA("renderdoc.dll");
	if (!RDMod) return;

	auto GetAPIFn = reinterpret_cast<pRENDERDOC_GetAPI>(reinterpret_cast<void*>(GetProcAddress(RDMod, "RENDERDOC_GetAPI")));
	if (!GetAPIFn) return;

	if (GetAPIFn(eRENDERDOC_API_Version_1_1_1, reinterpret_cast<void**>(&GRenderDocAPI)) != 1)
	{
		GRenderDocAPI = nullptr;
		return;
	}

	FString CapturePath = FPaths::ProjectSavedDir() / TEXT("RenderDoc") / TEXT("CSSW_Capture");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(CapturePath), true);
	GRenderDocAPI->SetLogFilePathTemplate(TCHAR_TO_ANSI(*CapturePath));
	UE_LOG(LogTemp, Log, TEXT("[CSSW] RenderDoc API loaded. Capture path: %s"), *CapturePath);
}
#endif

DECLARE_STATS_GROUP(TEXT("CSSW"), STATGROUP_CSSW, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("CSSW Execute"), STAT_CSSW_Execute, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Capture"), STAT_CSSW_Capture, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Total"), STAT_CSSW_Total, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW TileReadback"), STAT_CSSW_TileReadback, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW ISMUpdate"), STAT_CSSW_ISMUpdate, STATGROUP_CSSW);
DECLARE_GPU_STAT_NAMED(Stat_ShallowWater, TEXT("ShallowWater"));

#ifdef NUM_THREADS_PER_GROUP_DIMENSION_X
#undef NUM_THREADS_PER_GROUP_DIMENSION_X
#endif

#define NUM_THREADS_PER_GROUP_DIMENSION_X 16

#ifdef NUM_THREADS_PER_GROUP_DIMENSION_Y
#undef NUM_THREADS_PER_GROUP_DIMENSION_Y
#endif

#define NUM_THREADS_PER_GROUP_DIMENSION_Y 16



class FShallowWaterSim : public FGlobalShader
{
public:
		
	enum class EShallowWaterSimStep : uint8
	{
		SW_CompactActiveTiles,
		SW_FinalizeCompact,
		SW_VelocityHeightSim,
		SW_ShallowIntegrate,
		SW_Result,
		SW_SetHeight,
		SW_SmoothHeight,
		MAX
	};
	class FShallowWaterSimStep : SHADER_PERMUTATION_ENUM_CLASS("SWS", EShallowWaterSimStep);
	class FSplineRange : SHADER_PERMUTATION_BOOL("USESPLINERANGE");
	class FWaterfallExtentPerm : SHADER_PERMUTATION_ENUM_CLASS("WATERFALL_PERM", EWaterfallExpansion);
	using FPermutationDomain = TShaderPermutationDomain<FShallowWaterSimStep, FSplineRange, FWaterfallExtentPerm>;

	static TShaderMapRef<FShallowWaterSim> CreatePermutation(EShallowWaterSimStep Permutation, bool UseSplineRange = false, EWaterfallExpansion WfExtent = EWaterfallExpansion::Expansion_5)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FSplineRange>(UseSplineRange);
		PermutationVector.Set<FShallowWaterSimStep>(Permutation);
		PermutationVector.Set<FWaterfallExtentPerm>(WfExtent);
		TShaderMapRef<FShallowWaterSim> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	DECLARE_GLOBAL_SHADER(FShallowWaterSim);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterSim, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_VelocityHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_Source)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SplineScaleDist)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_CopyLandscape)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ResultDepthWet)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ResultSmoothHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VelHeightSimA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VelHeightSimB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultVelHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultDepthWet)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SmoothHeightA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SmoothHeightB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultSmoothHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TileMask)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWB_SourceUVRads)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWB_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWB_CompactCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWB_CompactIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, B_CompactCounter)
		RDG_BUFFER_ACCESS(CompactIndirectArgs, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER(int, CloseBound)
		SHADER_PARAMETER(int, BCount_SourceUVRads)
		SHADER_PARAMETER(int, DispatchExpandPixels)

		SHADER_PARAMETER(FVector4f, SourceUVRad)
		SHADER_PARAMETER(FVector4f, ModifierUVRad)
		SHADER_PARAMETER(FVector2f, CopyValidUV)
		SHADER_PARAMETER(float, DT)
		SHADER_PARAMETER(float, Friction)
		SHADER_PARAMETER(float, SeaLevel)
		SHADER_PARAMETER(float, ActorLocationZ)
		SHADER_PARAMETER(float, AdvectFoam)
		SHADER_PARAMETER(float, FoamFadeSpeed)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const auto Step = PermutationVector.Get<FShallowWaterSimStep>();
		const auto WfPerm = PermutationVector.Get<FWaterfallExtentPerm>();
		if (Step != EShallowWaterSimStep::SW_VelocityHeightSim && WfPerm != EWaterfallExpansion::Expansion_5)
			return false;
		return true;
	}
	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_DIMENSION_X);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_DIMENSION_Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), NUM_THREADS_PER_GROUP_DIMENSION_Z);
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("SW_COMPACTACTIVETILES"),
			TEXT("SW_FINALIZECOMPACT"),
			TEXT("SW_VELOCITYHEIGHTSIM"),
			TEXT("SW_SHALLOWINTEGRATE"),
			TEXT("SW_RESULT"),
			TEXT("SW_SETHEIGHT"),
			TEXT("SW_SMOOTHHEIGHT"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EShallowWaterSimStep::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FShallowWaterSimStep>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
		OutEnvironment.SetDefine(TEXT("VELOCITY_CLAMP"), CSSW_VELOCITY_CLAMP);
		if (PermutationVector.Get<FShallowWaterSimStep>() == EShallowWaterSimStep::SW_VelocityHeightSim)
		{
			OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), TEXT("CalVelocityHeight_SmoothData"));
			OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_EXTENT"), 0);
		}
		if (PermutationVector.Get<FShallowWaterSimStep>() == EShallowWaterSimStep::SW_Result)
		{
			OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), TEXT("CalShallowWaterResult_ShareData"));
			OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_EXTENT"), 2);
		}
		if (PermutationVector.Get<FSplineRange>() == true)
		{
			OutEnvironment.SetDefine(TEXT("USESPLINERANGE"), 1);
		}

		constexpr int32 WaterfallExtentLUT[] = { 5, 7, 10 };
		const int32 WfIdx = FMath::Clamp(static_cast<int32>(PermutationVector.Get<FWaterfallExtentPerm>()), 0, 2);
		OutEnvironment.SetDefine(TEXT("WATERFALL_EXTENT"), WaterfallExtentLUT[WfIdx]);
	}
};

IMPLEMENT_GLOBAL_SHADER(FShallowWaterSim, "/Plugin/PCGPlugins/Shaders/Private/ShallowWater.usf", "ShallowWater", SF_Compute);

namespace
{
int32 ComputeDispatchExpandPixels(int32 Iteration, int32 TextureResolution)
{
	const int32 IterationMargin = FMath::Max(Iteration, 1) * 2;
	return FMath::Clamp(IterationMargin + 4, 4, FMath::Max(TextureResolution, 4));
}
}


using namespace CSHepler;
ACSShallowWaterCapture::ACSShallowWaterCapture()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CaptureRoot"));
	SetRootComponent(SceneComponent);
	
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(SceneComponent, TEXT("Box"));
	Box->SetBoxExtent(FVector(50,50,50));
	ReusltMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualizationMesh"));
	ReusltMesh->BoundsScale = 100;
	ReusltMesh->bVisibleInRayTracing = false;
	ReusltMesh->SetupAttachment(SceneComponent, TEXT("VisualizationMesh"));
	SimVisHISM = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("SimVisHISM"));
	SimVisHISM->SetupAttachment(SceneComponent, TEXT("SimVisHISM"));
	SimVisHISM->SetVisibility(false);
	SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SimVisHISM->SetCastShadow(false);
	SimVisHISM->bVisibleInRayTracing = false;
	SimVisHISM->NumCustomDataFloats = 0;
	CausticsDecal = CreateDefaultSubobject<UDecalComponent>(TEXT("CausticsDecal"));
	CausticsDecal->SetupAttachment(SceneComponent, TEXT("CausticsDecal"));

	
	CaptureSceneDepth = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureSceneDepth"));
	CaptureSceneDepth->OrthoWidth = TextureSize * WorldPixelSize;
	CaptureSceneDepth->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureSceneDepth->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	CaptureSceneDepth->SetRelativeRotation(FRotator(-90, -90, 0));
	CaptureSceneDepth->SetRelativeLocation(FVector(0, 0, MaxHeight));
	CaptureSceneDepth->bCaptureEveryFrame = false;
	CaptureSceneDepth->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureSceneDepth->SetupAttachment(SceneComponent, TEXT("CaptureSceneDepth"));
	
}

bool ACSShallowWaterCapture::ShouldTickIfViewportsOnly() const
{
	return true;
}

void ACSShallowWaterCapture::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bSolverActive)
	{
		ShallowWaterSolverSoucePoint(5);
	}
}


void ACSShallowWaterCapture::PostLoad()
{
	Super::PostLoad();

	if (!SimVisHISM)
	{
		SimVisHISM = FindComponentByClass<UHierarchicalInstancedStaticMeshComponent>();
		if (!SimVisHISM)
		{
			SimVisHISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, TEXT("SimVisHISM"));
			SimVisHISM->SetupAttachment(SceneComponent);
			if (!HasAnyFlags(RF_ClassDefaultObject))
			{
				SimVisHISM->RegisterComponent();
			}
		}
		SimVisHISM->SetVisibility(false);
		SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SimVisHISM->SetCastShadow(false);
		SimVisHISM->bVisibleInRayTracing = false;
		SimVisHISM->NumCustomDataFloats = 0;
		UE_LOG(LogTemp, Warning, TEXT("ACSShallowWaterCapture::PostLoad - Recreated SimVisHISM for %s"), *GetName());
	}
}

void ACSShallowWaterCapture::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (GWorld)
	{
		GWorld->GetTimerManager().ClearTimer(ConstructionDebounceHandle);
		GWorld->GetTimerManager().SetTimer(ConstructionDebounceHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]() { ConstructionComponent(); }),
			0.01f, false);
	}
	else
	{
		ConstructionComponent();
	}
}


void ACSShallowWaterCapture::ConstructionComponent()
{
	const bool bSolverWasActive = bSolverActive;
	if (bSolverWasActive)
	{
		bSolverActive = false;
		bSolverPausedForConstruction = true;
	}

	Clean();
	FVector RelativeScale = FVector(CaptureSceneDepth->OrthoWidth / 100, CaptureSceneDepth->OrthoWidth / 100, MaxHeight / 100);
	Box->SetRelativeScale3D(RelativeScale);
	Box->SetRelativeLocation(FVector(0, 0, Scale3DZ * 50));
	Box->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

	FVector DecalRelativeScale = FVector(MaxHeight / 100, CaptureSceneDepth->OrthoWidth / 100, CaptureSceneDepth->OrthoWidth / 100);
	CausticsDecal->SetRelativeScale3D(DecalRelativeScale);
	CausticsDecal->SetRelativeRotation(FRotator(-90, 0, 0));
	CausticsDecal->DecalSize = FVector(500, 50, 50);
	CausticsDecal->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	
	CaptureSceneDepth->SetRelativeLocation(FVector(0, 0, MaxHeight));
	CaptureSceneDepth->OrthoWidth = CaptureSize;
	CaptureSceneDepth->HiddenActors = {this};
	TextureSize = FMath::RoundUpToPowerOfTwo(FMath::Max(16, FMath::CeilToInt32(CaptureSize / WorldPixelSize)));
	ReusltMesh->SetRelativeScale3D(FVector::OneVector * CaptureSize / 100);
	ReusltMesh->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = CaptureSize;
	SimUVInvSize = CaptureSize > 0.f ? 1.f / CaptureSize : 0.f;

	SetActorScale3D(FVector::OneVector);

	if (bSolverWasActive)
	{
		bSolverActive = true;
		bSolverPausedForConstruction = false;
	}
}

void ACSShallowWaterCapture::ShallowWaterSolverSoucePoint(int32 InIteration)
{
	if (GFrameCounter == LastSolverFrameNumber) return;
	LastSolverFrameNumber = GFrameCounter;

	if (!CheckAndCreateTexture_SWSourcePoint()) return;

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);

	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = CaptureSize;
	SimUVInvSize = CaptureSize > 0.f ? 1.f / CaptureSize : 0.f;

	TArray<FVector4> SourceData = GetSources();
	if (SourceData.Num() == 0) return;

	bool bDoCapture = bCaptureNextSolverFrame;
	if (bDoCapture)
	{
		InitRenderDocAPI();
		if (GRenderDocAPI)
		{
			bCaptureNextSolverFrame = false;
		}
		else
		{
			bDoCapture = false;
			UE_LOG(LogTemp, Warning, TEXT("[CSSW] RenderDoc not loaded. Launch editor with -RenderDoc flag. Capture request preserved."));
		}
	}

	TArray<FVector4f> SourceUVRads;
	FBoxSphereBounds Bounds = Box->Bounds;
	FVector ActorLocation = GetActorLocation();
	SourceUVRads.Reserve(SourceData.Num());
	for (int i = 0; i < SourceData.Num(); i++)
	{
		FVector4 PerSourceData = SourceData[i];
		FVector SourceLocation = FVector(PerSourceData.X, PerSourceData.Y, PerSourceData.Z);
		FVector RelativeSourceLocation = SourceLocation - (Bounds.Origin - Bounds.BoxExtent);
		FVector SourceUV = RelativeSourceLocation / ( Bounds.BoxExtent * 2 );
		FVector4f SourceUVRad = FVector4f( SourceUV.X, SourceUV.Y, SourceLocation.Z, PerSourceData.W);
		SourceUVRads.Add(SourceUVRad);
	}
	
	RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultDepthWet->ResizeTarget(TextureSize, TextureSize);
	RT_SmoothHeight->ResizeTarget(TextureSize, TextureSize);
	
	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();

	const int32 TileMaskWidth = FMath::DivideAndRoundUp((int32)TextureSize, NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp((int32)TextureSize, NUM_THREADS_PER_GROUP_DIMENSION_Y);
	if (!RT_TileMask)
	{
		RT_TileMask = NewObject<UTextureRenderTarget2D>(this);
		RT_TileMask->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
		RT_TileMask->bCanCreateUAV = true;
		RT_TileMask->ClearColor = FLinearColor::Black;
		RT_TileMask->InitAutoFormat(TileMaskWidth, TileMaskHeight);
		RT_TileMask->UpdateResourceImmediate(true);
	}
	else
	{
		RT_TileMask->ResizeTarget(TileMaskWidth, TileMaskHeight);
	}
	FTextureRenderTargetResource* R_TileMask = RT_TileMask->GameThread_GetRenderTargetResource();

	TileMaskReadbackWidth = TileMaskWidth;
	TileMaskReadbackHeight = TileMaskHeight;

	const int32 TileMaskReadIdx = 1 - TileMaskReadbackWriteIdx;
	if (SimVisHISM && SimVisHISM->GetStaticMesh() && TileMaskReadback[TileMaskReadIdx] && TileMaskReadback[TileMaskReadIdx]->IsReady())
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_TileReadback);

		int32 RowPitchInPixels = 0;
		const FFloat16Color* ReadbackData = static_cast<const FFloat16Color*>(TileMaskReadback[TileMaskReadIdx]->Lock(RowPitchInPixels));
		if (ReadbackData && TileMaskReadbackWidth > 0)
		{
			const int32 SubsampleFactor = 2;
			const int32 ISMTileCountX = FMath::DivideAndRoundUp(TileMaskReadbackWidth, SubsampleFactor);
			const int32 ISMTileCountY = FMath::DivideAndRoundUp(TileMaskReadbackHeight, SubsampleFactor);
			const int32 TotalSlots = ISMTileCountX * ISMTileCountY;

			TArray<uint8> NewTileBits;
			NewTileBits.SetNumZeroed(TotalSlots);

			for (int32 TY = 0; TY < ISMTileCountY; TY++)
			{
				for (int32 TX = 0; TX < ISMTileCountX; TX++)
				{
					bool bActive = false;
					for (int32 DY = 0; DY < SubsampleFactor && !bActive; DY++)
					{
						for (int32 DX = 0; DX < SubsampleFactor && !bActive; DX++)
						{
							const int32 X = TX * SubsampleFactor + DX;
							const int32 Y = TY * SubsampleFactor + DY;
							if (X < TileMaskReadbackWidth && Y < TileMaskReadbackHeight)
							{
								if (ReadbackData[Y * RowPitchInPixels + X].R.GetFloat() > 0.5f)
									bActive = true;
							}
						}
					}
					NewTileBits[TY * ISMTileCountX + TX] = bActive ? 1 : 0;
				}
			}

			const float TileWorldSize = CaptureSize / (float)ISMTileCountX;
			const float HalfCapture = CaptureSize * 0.5f;

			{
				const FVector ActorLoc = GetActorLocation();
				for (const FVector4& Src : SourceData)
				{
					const float LocalX = (float)(Src.X - ActorLoc.X);
					const float LocalY = (float)(Src.Y - ActorLoc.Y);
					const float Range = (float)Src.W * 3.0f;

					const int32 MinTXi = FMath::Clamp(FMath::FloorToInt32((LocalX - Range + HalfCapture) / TileWorldSize), 0, ISMTileCountX - 1);
					const int32 MaxTXi = FMath::Clamp(FMath::FloorToInt32((LocalX + Range + HalfCapture) / TileWorldSize), 0, ISMTileCountX - 1);
					const int32 MinTYi = FMath::Clamp(FMath::FloorToInt32((LocalY - Range + HalfCapture) / TileWorldSize), 0, ISMTileCountY - 1);
					const int32 MaxTYi = FMath::Clamp(FMath::FloorToInt32((LocalY + Range + HalfCapture) / TileWorldSize), 0, ISMTileCountY - 1);

					for (int32 TY = MinTYi; TY <= MaxTYi; TY++)
					{
						for (int32 TX = MinTXi; TX <= MaxTXi; TX++)
						{
							NewTileBits[TY * ISMTileCountX + TX] = 1;
						}
					}
				}
			}

			if (NewTileBits != CachedTileBits)
			{
				SCOPE_CYCLE_COUNTER(STAT_CSSW_ISMUpdate);
				CachedTileBits = NewTileBits;

				const FVector ActiveScale(TileWorldSize / 100.0f, TileWorldSize / 100.0f, 1.0f);

				SimVisHISM->ClearInstances();
				ISMTileSlots.Reset();

				TArray<FTransform> TransformsToAdd;
				for (int32 Slot = 0; Slot < TotalSlots; Slot++)
				{
					if (!NewTileBits[Slot]) continue;
					ISMTileSlots.Add(Slot);
					const int32 TX = Slot % ISMTileCountX;
					const int32 TY = Slot / ISMTileCountX;
					const float CenterX = (TX + 0.5f) * TileWorldSize - HalfCapture;
					const float CenterY = (TY + 0.5f) * TileWorldSize - HalfCapture;
					TransformsToAdd.Emplace(FQuat::Identity, FVector(CenterX, CenterY, 0.0f), ActiveScale);
				}

				if (TransformsToAdd.Num() > 0)
				{
					SimVisHISM->AddInstances(TransformsToAdd, false, false);
				}

				CachedActiveTileCount = ISMTileSlots.Num();
				SimVisHISM->SetVisibility(true);
				ReusltMesh->SetVisibility(false);
			}
		}
		TileMaskReadback[TileMaskReadIdx]->Unlock();
	}

	const int32 ResultReadIdx = 1 - ResultReadbackWriteIdx;
	if (ResultReadback[ResultReadIdx] && ResultReadback[ResultReadIdx]->IsReady())
	{
		int32 RowPitch = 0;
		const FFloat16Color* Data = static_cast<const FFloat16Color*>(ResultReadback[ResultReadIdx]->Lock(RowPitch));
		if (Data && RowPitch > 0)
		{
			const int32 W = (int32)TextureSize;
			const int32 H = (int32)TextureSize;
			if (W > 0 && H > 0 && RowPitch >= W)
			{
				CachedResultWidth = W;
				CachedResultHeight = H;
				CachedResultPixels.SetNumUninitialized(W * H);
				for (int32 Y = 0; Y < H; Y++)
				{
					FMemory::Memcpy(&CachedResultPixels[Y * W], &Data[Y * RowPitch], W * sizeof(FFloat16Color));
				}
			}
		}
		ResultReadback[ResultReadIdx]->Unlock();
	}

	InIteration = FMath::Max(InIteration, 1);
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this ](FRHICommandListImmediate& RHICmdList)
	{
#if PLATFORM_WINDOWS
		if (bDoCapture && GRenderDocAPI) GRenderDocAPI->StartFrameCapture(nullptr, nullptr);
#endif

		SCOPED_GPU_STAT(RHICmdList, Stat_ShallowWater);
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);

			TShaderMapRef<FShallowWaterSim> ComputeShader_CompactActiveTiles = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_CompactActiveTiles);
			TShaderMapRef<FShallowWaterSim> ComputeShader_FinalizeCompact = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_FinalizeCompact);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalSmoothHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SmoothHeight);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalVelocityHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_VelocityHeightSim, false, WaterfallExpansionIterations);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalShallowIntegrate = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_ShallowIntegrate);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalResult = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_Result);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			const FIntVector SingleGroupCount(1, 1, 1);
			
			CREATE_TEXTURE_UAV_16_OUT(DebugView)
			CREATE_TEXTURE_UAV_16_OUT(ResultVelHeight)
			CREATE_TEXTURE_UAV_16_OUTP(ResultDepthWet)
			
			CREATE_TEXTURE_UAV_32_OUT(ResultSmoothHeight)
			
			CREATE_TEXTURE_UAV_32(VelHeightSimA)
			CREATE_TEXTURE_UAV_32(VelHeightSimB)
			CREATE_TEXTURE_UAV_32(SmoothHeightA)
			CREATE_TEXTURE_UAV_32(SmoothHeightB)
			
			CREATE_RDG(SceneDepth)
			CREATE_RDG(VelocityHeight)
			
			CREATE_UAVB_32(SourceUVRads)

			FCompactTileBuffers CompactBuffers = CreateCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);

			FRDGTextureRef RDG_TileMask = RegisterExternalTexture(GraphBuilder, R_TileMask->GetRenderTargetTexture(), TEXT("TileMask_RT"));
			FRDGTextureUAVRef RDGUAV_TileMask = GraphBuilder.CreateUAV(RDG_TileMask);

			PassParameters->DT = DT;
			PassParameters->Friction = Friction;
			PassParameters->SeaLevel = SeaLevel;
			PassParameters->ActorLocationZ = ActorLocation.Z;
			PassParameters->AdvectFoam = AdvectFoam;
			PassParameters->FoamFadeSpeed = FoamFadeSpeed;
			PassParameters->CloseBound = CloseBound;
			PassParameters->BCount_SourceUVRads = SourceUVRads.Num();
			PassParameters->DispatchExpandPixels = ComputeDispatchExpandPixels(InIteration, TextureSize.X);
			BindCompactTileBuffers(PassParameters, CompactBuffers);
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			AddCopyTexturePass(GraphBuilder, RDG_VelocityHeight, TRDG_VelHeightSimA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, TRDG_SmoothHeightA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultVelHeight, TRDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultDepthWet, TRDG_ResultDepthWet, FRHICopyTextureInfo());

			// --- Compact active tiles ---
			ResetCompactCounter(GraphBuilder, CompactBuffers);
			const FIntVector FullTileGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
			{
				FShallowWaterSim::FParameters* CompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*CompactPassParameters = *PassParameters;
				CompactPassParameters->B_CompactTileCoords = nullptr;
				CompactPassParameters->B_CompactCounter = nullptr;
				CompactPassParameters->CompactIndirectArgs = nullptr;
				CompactPassParameters->RWB_CompactIndirectArgs = nullptr;
				CompactPassParameters->RW_TileMask = RDGUAV_TileMask;
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("CompactActiveTiles"),
					CompactPassParameters,
					ERDGPassFlags::AsyncCompute,
					[CompactPassParameters, ComputeShader_CompactActiveTiles, FullTileGroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_CompactActiveTiles, *CompactPassParameters, FullTileGroupCount);
					});
			}
			{
				FShallowWaterSim::FParameters* FinalizeCompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*FinalizeCompactPassParameters = *PassParameters;
				NullifyAllCompactTileBindings(FinalizeCompactPassParameters);
				FinalizeCompactPassParameters->RWB_CompactCounter = CompactBuffers.CounterUAV;
				FinalizeCompactPassParameters->RWB_CompactIndirectArgs = CompactBuffers.IndirectArgsUAV;
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("FinalizeCompact"),
					FinalizeCompactPassParameters,
					ERDGPassFlags::AsyncCompute,
					[FinalizeCompactPassParameters, ComputeShader_FinalizeCompact, SingleGroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_FinalizeCompact, *FinalizeCompactPassParameters, SingleGroupCount);
					});
			}


			FRDGTextureRef CurrentVelHeightTextureA = TRDG_VelHeightSimA;
			FRDGTextureRef CurrentVelHeightTextureB = TRDG_VelHeightSimB;
			FRDGTextureRef CurrentSmoothHeightTextureA = TRDG_SmoothHeightA;
			FRDGTextureRef CurrentSmoothHeightTextureB = TRDG_SmoothHeightB;
			FRDGTextureUAVRef CurrentVelHeightSimA = RDGUAV_VelHeightSimA;
			FRDGTextureUAVRef CurrentVelHeightSimB = RDGUAV_VelHeightSimB;
			FRDGTextureUAVRef CurrentSmoothHeightA = RDGUAV_SmoothHeightA;
			FRDGTextureUAVRef CurrentSmoothHeightB = RDGUAV_SmoothHeightB;

			for (int32 i = 0 ; i < InIteration; i++)
			{
				FShallowWaterSim::FParameters* VelocityHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*VelocityHeightPassParameters = *PassParameters;
				VelocityHeightPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				VelocityHeightPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				VelocityHeightPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				VelocityHeightPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(VelocityHeightPassParameters);
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("CalVelocityHeight"),
					VelocityHeightPassParameters,
					ERDGPassFlags::AsyncCompute,
					[VelocityHeightPassParameters, ComputeShader_CalVelocityHeight](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::DispatchIndirect(
							RHICmdList,
							ComputeShader_CalVelocityHeight,
							*VelocityHeightPassParameters,
							VelocityHeightPassParameters->CompactIndirectArgs->GetIndirectRHICallBuffer(),
							0);
					});

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentSmoothHeightTextureA, CurrentSmoothHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
				Swap(CurrentSmoothHeightA, CurrentSmoothHeightB);

				FShallowWaterSim::FParameters* ShallowIntegratePassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*ShallowIntegratePassParameters = *PassParameters;
				ShallowIntegratePassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				ShallowIntegratePassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				ShallowIntegratePassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				ShallowIntegratePassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(ShallowIntegratePassParameters);
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("CalShallowIntegrate"),
					ShallowIntegratePassParameters,
					ERDGPassFlags::AsyncCompute,
					[ShallowIntegratePassParameters, ComputeShader_CalShallowIntegrate](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::DispatchIndirect(
							RHICmdList,
							ComputeShader_CalShallowIntegrate,
							*ShallowIntegratePassParameters,
							ShallowIntegratePassParameters->CompactIndirectArgs->GetIndirectRHICallBuffer(),
							0);
					});

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
			}

			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			ResultPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
			ResultPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
			ResultPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
			ResultPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
			NullifyCompactTileUAVs(ResultPassParameters);
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Result"),
				ResultPassParameters,
				ERDGPassFlags::AsyncCompute,
				[ResultPassParameters, ComputeShader_CalResult](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::DispatchIndirect(
						RHICmdList,
						ComputeShader_CalResult,
						*ResultPassParameters,
						ResultPassParameters->CompactIndirectArgs->GetIndirectRHICallBuffer(),
						0);
				});
			
			AddCopyTexturePass(GraphBuilder, TRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultVelHeight, RDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, CurrentVelHeightTextureA, RDG_VelocityHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();

		const int32 TileWriteIdx = TileMaskReadbackWriteIdx;
		if (!TileMaskReadback[TileWriteIdx])
			TileMaskReadback[TileWriteIdx] = new FRHIGPUTextureReadback(TEXT("TileMaskReadback"));
		TileMaskReadback[TileWriteIdx]->EnqueueCopy(RHICmdList, R_TileMask->GetRenderTargetTexture());

		const int32 ResultWriteIdx = ResultReadbackWriteIdx;
		if (!ResultReadback[ResultWriteIdx])
			ResultReadback[ResultWriteIdx] = new FRHIGPUTextureReadback(TEXT("ResultReadback"));
		ResultReadback[ResultWriteIdx]->EnqueueCopy(RHICmdList, R_ResultVelHeight->GetRenderTargetTexture());

#if PLATFORM_WINDOWS
		if (bDoCapture && GRenderDocAPI) GRenderDocAPI->EndFrameCapture(nullptr, nullptr);
#endif
	});

	TileMaskReadbackWriteIdx = 1 - TileMaskReadbackWriteIdx;
	ResultReadbackWriteIdx = 1 - ResultReadbackWriteIdx;
}


void ACSShallowWaterCapture::SetHeight()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);
	
	RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultDepthWet->ResizeTarget(TextureSize, TextureSize);
	RT_SmoothHeight->ResizeTarget(TextureSize, TextureSize);

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			TShaderMapRef<FShallowWaterSim> ComputeShader_SetHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SetHeight);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalResult = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_Result);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			
			FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("UAV_DebugView")); 
			FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
			FRDGTextureRef RDG_VelHeightSimA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Sim_A"));
			FRDGTextureUAVRef RDGUAV_VelHeightSimA = GraphBuilder.CreateUAV(RDG_VelHeightSimA);
			FRDGTextureRef TmpRDG_Result = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));
			FRDGTextureUAVRef RDGUAV_Result = GraphBuilder.CreateUAV(TmpRDG_Result);
			FRDGTextureRef TmpRDG_ResultDepthWet = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_ResultDepthWet"));
			FRDGTextureUAVRef RDGUAV_ResultDepthWet = GraphBuilder.CreateUAV(TmpRDG_ResultDepthWet);
			FRDGTextureRef TmpRDG_ResultSmoothHeight = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_ResultSmoothHeight"));
			FRDGTextureUAVRef RDGUAV_ResultSmoothHeight = GraphBuilder.CreateUAV(TmpRDG_ResultSmoothHeight);
			FRDGTextureRef RDG_SmoothHeightA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightA"));
			FRDGTextureUAVRef RDGUAV_SmoothHeightA = GraphBuilder.CreateUAV(RDG_SmoothHeightA);
			
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("SceneDepth_RT"));
			FRDGTextureRef RDG_VelocityHeight = RegisterExternalTexture(GraphBuilder, R_VelocityHeight->GetRenderTargetTexture(), TEXT("VelocityHeight_RT"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_ResultVelHeight->GetRenderTargetTexture(), TEXT("Result_RT"));
			FRDGTextureRef RDG_ResultDepthWet = RegisterExternalTexture(GraphBuilder, R_ResultDepthWet->GetRenderTargetTexture(), TEXT("ResultDepthWet_RT"));
			FRDGTextureRef RDG_ResultSmoothHeight = RegisterExternalTexture(GraphBuilder, R_ResultSmoothHeight->GetRenderTargetTexture(), TEXT("ResultSmoothHeight_RT"));

			FCompactTileBuffers CompactBuffers = CreateFullScreenCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);

			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->T_VelocityHeight = RDG_VelocityHeight;
			PassParameters->T_ResultDepthWet = RDG_ResultDepthWet;
			PassParameters->T_ResultSmoothHeight = RDG_ResultSmoothHeight;
			PassParameters->DT = DT;
			PassParameters->Friction = Friction;
			PassParameters->SeaLevel = SeaLevel;
			PassParameters->ActorLocationZ = GetActorLocation().Z;
			PassParameters->BCount_SourceUVRads = 0;
			PassParameters->DispatchExpandPixels = 0;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RW_ResultVelHeight = RDGUAV_Result;
			PassParameters->RW_ResultDepthWet = RDGUAV_ResultDepthWet;
			PassParameters->RW_ResultSmoothHeight = RDGUAV_ResultSmoothHeight;
			PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimA;
			PassParameters->RW_SmoothHeightA = RDGUAV_SmoothHeightA;
			PassParameters->RWB_SourceUVRads = nullptr;
			BindCompactTileBuffers(PassParameters, CompactBuffers);
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, RDG_SmoothHeightA, FRHICopyTextureInfo());

			FShallowWaterSim::FParameters* SetHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*SetHeightPassParameters = *PassParameters;
			NullifyAllCompactTileBindings(SetHeightPassParameters);
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("SetHeight"),
				SetHeightPassParameters,
				ERDGPassFlags::AsyncCompute,
				[SetHeightPassParameters, ComputeShader_SetHeight, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_SetHeight, *SetHeightPassParameters, GroupCount);
				});
			
			// Result pass — uses CompactIndirectArgs, only needs SRV reads
			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			NullifyCompactTileUAVs(ResultPassParameters);
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Result"),
				ResultPassParameters,
				ERDGPassFlags::AsyncCompute,
				[ResultPassParameters, ComputeShader_CalResult](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::DispatchIndirect(
						RHICmdList,
						ComputeShader_CalResult,
						*ResultPassParameters,
						ResultPassParameters->CompactIndirectArgs->GetIndirectRHICallBuffer(),
						0);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimA, RDG_VelocityHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			
		}
		GraphBuilder.Execute();
	});
}

void ACSShallowWaterCapture::HeightSmooth()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);
	
	RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			TShaderMapRef<FShallowWaterSim> ComputeShader_SmoothHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SmoothHeight);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			
			FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("UAV_DebugView")); 
			FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
			FRDGTextureRef RDG_VelHeightSimA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Sim_A"));
			FRDGTextureUAVRef RDGUAV_VelHeightSimA = GraphBuilder.CreateUAV(RDG_VelHeightSimA);
			FRDGTextureRef TmpRDG_Result = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));
			FRDGTextureUAVRef RDGUAV_Result = GraphBuilder.CreateUAV(TmpRDG_Result);

			FRDGTextureRef RDG_SmoothHeightA = nullptr;
			FRDGTextureUAVRef RDGUAV_SmoothHeightA = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, RDG_SmoothHeightA, RDGUAV_SmoothHeightA, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightA"));

			FRDGTextureRef RDG_SmoothHeightB = nullptr;
			FRDGTextureUAVRef RDGUAV_SmoothHeightB = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, RDG_SmoothHeightB, RDGUAV_SmoothHeightB, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightB"));
			
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("SceneDepth_RT"));
			FRDGTextureRef RDG_VelocityHeight = RegisterExternalTexture(GraphBuilder, R_VelocityHeight->GetRenderTargetTexture(), TEXT("VelocityHeight_RT"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_ResultVelHeight->GetRenderTargetTexture(), TEXT("Result_RT"));
			
			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RW_SmoothHeightA = RDGUAV_SmoothHeightA;
			PassParameters->RW_SmoothHeightB = RDGUAV_SmoothHeightB;
			PassParameters->BCount_SourceUVRads = 0;
			PassParameters->DispatchExpandPixels = 0;
			PassParameters->RWB_SourceUVRads = nullptr;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("SmoothHeight"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_SmoothHeight, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_SmoothHeight, *PassParameters, GroupCount);
				});
			
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			
		}
		GraphBuilder.Execute();
	});
}

void ACSShallowWaterCapture::Clean()
{
	UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultVelHeight, FLinearColor(0, 0, -9999, 1));
	UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultDepthWet,  FLinearColor(-9999, -9999, -9999, -9999));
	UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_VelocityHeight,  FLinearColor(0, 0, -9999, 1));
}

void ACSShallowWaterCapture::CleanDepthWet_Construct()
{
	if (RT_ResultDepthWet
	&& RT_ResultDepthWet->GetResource()
	&& GWorld)
	{
		FTextureRenderTargetResource* RenderTargetResource = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
		FLinearColor ClearColor = FLinearColor(-9999, -9999, -9999, -9999);
		ENQUEUE_RENDER_COMMAND(ClearRTCommand)(
			[RenderTargetResource, ClearColor](FRHICommandList& RHICmdList)
		{
			FRHIRenderPassInfo RPInfo(RenderTargetResource->GetRenderTargetTexture(), ERenderTargetActions::DontLoad_Store);
			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::Unknown, ERHIAccess::RTV));
			RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearRT"));
			DrawClearQuad(RHICmdList, ClearColor);
			RHICmdList.EndRenderPass();

			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::RTV, ERHIAccess::SRVMask));
		});
	}
}

TArray<FVector4> ACSShallowWaterCapture::GetSources()
{
	TArray<FVector4> SourceLocations;
	for (TActorIterator<ACSSHallowWaterSource> It(GWorld, ACSSHallowWaterSource::StaticClass()); It; ++It)
	{
		ACSSHallowWaterSource* Actor = *It;
		FVector Location = Actor->GetActorLocation();
		FBox AABB = FBox::BuildAABB(Box->Bounds.Origin, Box->Bounds.BoxExtent * FVector(1.2, 1.2, 9999));
		
		if (!AABB.IsInsideOrOn(Location)) continue;
		
		SourceLocations.Add(FVector4(Location.X, Location.Y, Location.Z, Actor->GetActorScale3D().X / CaptureSize * 500));
	}

	return SourceLocations;
}

void ACSShallowWaterCapture::CleanupAttachedActors()
{
	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	for (AActor* Actor : AttachedActors)
	{
		Actor->GetRootComponent()->SetVisibility(false);
		TArray<USceneComponent*> Components;
		Actor->GetComponents(USceneComponent::StaticClass(), Components);
		for (USceneComponent* Component : Components)
		{
			Component->SetVisibility(false);
		}
	}
	ReusltMesh->SetVisibility(false);
	CausticsDecal->SetVisibility(true);
}

void ACSShallowWaterCapture::SetMaterialParameter_Implementation()
{
	if (RT_ResultDepthWet
		&& RT_ResultDepthWet->GetResource()
		&& GWorld)
	{
		FTextureRenderTargetResource* RenderTargetResource = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
		FLinearColor ClearColor = FLinearColor(-9999, -9999, -9999, -9999);
		ENQUEUE_RENDER_COMMAND(ClearRTCommand)(
			[RenderTargetResource, ClearColor](FRHICommandList& RHICmdList)
		{
			FRHIRenderPassInfo RPInfo(RenderTargetResource->GetRenderTargetTexture(), ERenderTargetActions::DontLoad_Store);
			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::Unknown, ERHIAccess::RTV));
			RHICmdList.BeginRenderPass(RPInfo, TEXT("ClearRT"));
			DrawClearQuad(RHICmdList, ClearColor);
			RHICmdList.EndRenderPass();

			RHICmdList.Transition(FRHITransitionInfo(RenderTargetResource->GetRenderTargetTexture(), ERHIAccess::RTV, ERHIAccess::SRVMask));
		});
	}

	if (ReusltMesh && WaterMaterial)
	{
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(ReusltMesh->GetMaterial(0));
		if (!MID || MID->Parent != WaterMaterial)
		{
			MID = UMaterialInstanceDynamic::Create(WaterMaterial, this);
			ReusltMesh->SetMaterial(0, MID);
		}
		MID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		MID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) MID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) MID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
		VisWaterMaterial = MID;
	}

	if (CausticsDecal && DecalMaterial)
	{
		UMaterialInstanceDynamic* DecalMID = Cast<UMaterialInstanceDynamic>(CausticsDecal->GetDecalMaterial());
		if (!DecalMID || DecalMID->Parent != DecalMaterial)
		{
			DecalMID = UMaterialInstanceDynamic::Create(DecalMaterial, this);
			CausticsDecal->SetDecalMaterial(DecalMID);
		}
		DecalMID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		DecalMID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) DecalMID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) DecalMID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
		VisDecalMaterial = DecalMID;
	}
}

ACSSHallowWaterSource::ACSSHallowWaterSource()
{
	
}


void ACSShallowWaterCapture::CaptureSceneDepthNow()
{
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Capture);
	TArray<AActor*> TagedActors;
	for (TActorIterator<AActor> It(GWorld, AActor::StaticClass()); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor->Tags.Contains(SWCaptureTag) && !Actor->GetClass()->IsChildOf(ALandscape::StaticClass())) continue;
		if (Actor->GetClass()->IsChildOf(ALandscape::StaticClass()))
		{
			ALandscape* Landscape = Cast<ALandscape>(Actor);
			Landscape->MaxLODLevel = 0;
		}
		TagedActors.Add(Actor);
	}
	CaptureSceneDepth->ShowOnlyActors = TagedActors;
	CaptureSceneDepth->CaptureScene();
}

void ACSShallowWaterCapture::RequestRenderDocCapture()
{
	bCaptureNextSolverFrame = true;
}

void ACSShallowWaterCapture::ShallowWaterSolverSoucePointWithCapture(int32 InIteration)
{
	bCaptureNextSolverFrame = true;
	ShallowWaterSolverSoucePoint(InIteration);
}

void ACSShallowWaterCapture::StartSolver()
{
	bSolverActive = true;
	bSolverPausedForConstruction = false;
	OnSolverStarted();
}

void ACSShallowWaterCapture::StopSolver()
{
	bSolverActive = false;
	bSolverPausedForConstruction = false;
}

void ACSShallowWaterCapture::ToggleSimVisualization()
{
	bSolverActive = !bSolverActive;
	
	if (bSolverActive)
	{
		if (!SimVisHISM || !SimVisHISM->GetStaticMesh()) return;

		SimVisHISM->ClearInstances();
		ISMTileSlots.Reset();
		CachedTileBits.Reset();
		CachedActiveTileCount = 0;

		if (ReusltMesh) ReusltMesh->SetVisibility(false);
		SimVisHISM->SetVisibility(true);
	}
	else
	{
		if (SimVisHISM) SimVisHISM->SetVisibility(false);
		if (ReusltMesh) ReusltMesh->SetVisibility(true);
	}
}

void ACSShallowWaterCapture::OnSolverStarted_Implementation()
{
	UE_LOG(LogTemp, Log, TEXT("OnSolverStarted_Implementation: Default C++ implementation called on %s"), *GetName());
}

ACSShallowWaterCapture::FOnBakeResultMesh ACSShallowWaterCapture::OnBakeResultMeshDelegate;

void ACSShallowWaterCapture::BakeResultMesh()
{
	if (OnBakeResultMeshDelegate.IsBound())
	{
		OnBakeResultMeshDelegate.Execute(this);
		OnBakeComplete();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeResultMesh: No handler bound (Editor module not loaded?) on %s"), *GetName());
	}
}

void ACSShallowWaterCapture::OnBakeComplete_Implementation()
{
	StopSolver();
	if (SimVisHISM) SimVisHISM->SetVisibility(false);
	if (ReusltMesh) ReusltMesh->SetVisibility(true);
}

void ACSShallowWaterCapture::ShowDebugViewPlane(float Duration)
{
	UWorld* World = GetWorld();
	if (!World) return;

	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClassWithTag(World, AStaticMeshActor::StaticClass(), FName("CSSWVM"), Found);
	for (AActor* A : Found)
	{
		if (A) A->Destroy();
	}
	DebugViewPlaneActor.Reset();

	if (!DebugMesh) return;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Spawned = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		ReusltMesh->GetComponentTransform(),
		SpawnParams);
	if (!Spawned) return;

	UStaticMeshComponent* SMC = Spawned->GetStaticMeshComponent();
	SMC->SetStaticMesh(DebugMesh);

	UMaterialInstanceDynamic* MID = SMC->CreateDynamicMaterialInstance(0, DebugMesh->GetMaterial(0));
	if (MID && RT_DebugView)
	{
		MID->SetTextureParameterValue(FName("Height"), RT_DebugView);
	}

	Spawned->Tags.Add(FName("CSSWVM"));
	DebugViewPlaneActor = Spawned;

	if (Duration > 0.f)
	{
		World->GetTimerManager().ClearTimer(DebugViewPlaneTimerHandle);
		World->GetTimerManager().SetTimer(DebugViewPlaneTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				if (DebugViewPlaneActor.IsValid())
				{
					DebugViewPlaneActor->Destroy();
					DebugViewPlaneActor.Reset();
				}
			}),
			Duration, false);
	}
}

