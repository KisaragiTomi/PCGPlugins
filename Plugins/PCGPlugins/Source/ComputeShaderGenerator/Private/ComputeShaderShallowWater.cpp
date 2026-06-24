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
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ObjectSaveContext.h"
#include "DrawDebugHelpers.h"

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

static int32 GCSSWUseSparseIndirect = 0;
static FAutoConsoleVariableRef CVarCSSWUseSparseIndirect(
	TEXT("pcg.CSSW.UseSparseIndirect"),
	GCSSWUseSparseIndirect,
	TEXT("Use sparse-tile DispatchIndirect for CSSW simulation passes. Disabled by default to avoid D3D12 ExecuteIndirect failures while opening editor maps."));

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
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, RWB_SourceUVRads)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWB_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWB_CompactCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWB_CompactIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, B_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, B_CompactCounter)
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
		SHADER_PARAMETER(float, MaxWaterRisePerFrame)
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
ACSShallowWaterCapture::ACSShallowWaterCapture(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	GeneratorBounds->SetBoxExtent(FVector(50,50,50));
	ReusltMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualizationMesh"));
	ReusltMesh->BoundsScale = 100;
	ReusltMesh->bVisibleInRayTracing = false;
	ReusltMesh->SetupAttachment(SceneRoot, TEXT("VisualizationMesh"));
	SimVisHISM = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("SimVisHISM"));
	SimVisHISM->SetupAttachment(SceneRoot, TEXT("SimVisHISM"));
	SimVisHISM->SetVisibility(false);
	SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SimVisHISM->SetCastShadow(false);
	SimVisHISM->bVisibleInRayTracing = false;
	SimVisHISM->NumCustomDataFloats = 0;
	SimVisHISM->bNeverDistanceCull = true;
	SimVisHISM->bAllowCullDistanceVolume = false;
	CausticsDecal = CreateDefaultSubobject<UDecalComponent>(TEXT("CausticsDecal"));
	CausticsDecal->SetupAttachment(SceneRoot, TEXT("CausticsDecal"));

	CaptureSceneDepth = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureSceneDepth"));
	CaptureSceneDepth->OrthoWidth = TextureSize * WorldPixelSize;
	CaptureSceneDepth->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureSceneDepth->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	CaptureSceneDepth->SetRelativeRotation(FRotator(-90, -90, 0));
	CaptureSceneDepth->SetRelativeLocation(FVector(0, 0, MaxHeight));
	CaptureSceneDepth->bCaptureEveryFrame = false;
	CaptureSceneDepth->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureSceneDepth->SetupAttachment(SceneRoot, TEXT("CaptureSceneDepth"));
}

void ACSShallowWaterCapture::ClearSolverTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SolverTimerHandle);
	}
	SolverTimerHandle.Invalidate();
}

bool ACSShallowWaterCapture::IsSolverTimerActive() const
{
	if (UWorld* World = GetWorld())
	{
		return World->GetTimerManager().TimerExists(SolverTimerHandle);
	}
	return SolverTimerHandle.IsValid();
}

void ACSShallowWaterCapture::ScheduleSolverTimerTick()
{
	if (IsActorBeingDestroyed())
	{
		ClearSolverTimer();
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		ClearSolverTimer();
		return;
	}

	const int32 ExpectedSolverReadbackGeneration = SolverReadbackGeneration;
	FTimerDelegate SolverTimerDelegate = FTimerDelegate::CreateWeakLambda(this, [this, ExpectedSolverReadbackGeneration]()
	{
		HandleSolverTimerTick(ExpectedSolverReadbackGeneration);
	});

	if (SolverTimerRate > 0.0f)
	{
		World->GetTimerManager().SetTimer(SolverTimerHandle, SolverTimerDelegate, SolverTimerRate, false);
	}
	else
	{
		SolverTimerHandle = World->GetTimerManager().SetTimerForNextTick(SolverTimerDelegate);
	}
}

void ACSShallowWaterCapture::HandleSolverTimerTick(int32 ExpectedSolverReadbackGeneration)
{
	if (ExpectedSolverReadbackGeneration != SolverReadbackGeneration || IsActorBeingDestroyed())
	{
		return;
	}

	if (ExpectedSolverReadbackGeneration == SolverReadbackGeneration && !IsActorBeingDestroyed())
	{
		ScheduleSolverTimerTick();
	}
	ShallowWaterSolverSoucePoint(Iteration);
}

void ACSShallowWaterCapture::StopSimulationRuntime(bool bResetVisualization)
{
	ClearSolverTimer();
	bCaptureNextSolverFrame = false;
	ResetSolverReadbackState(true, false);

	if (bResetVisualization)
	{
		bSimVisActive = false;
		if (SimVisHISM)
		{
			ResetSimVisTiles();
			SimVisHISM->SetVisibility(false);
		}
		if (ReusltMesh)
		{
			ReusltMesh->SetVisibility(true);
		}
	}
}

void ACSShallowWaterCapture::UpdateSimulationPreviewMesh()
{
	if (!ReusltMesh) return;

	if (SimulationPreviewMesh && ReusltMesh->GetStaticMesh() != SimulationPreviewMesh)
	{
		ReusltMesh->SetStaticMesh(SimulationPreviewMesh);
	}

	ReusltMesh->SetRelativeScale3D(FVector::OneVector * CaptureSize / 100);

	if (SimulationWaterMaterial)
	{
		WaterMaterial = SimulationWaterMaterial;
	}
	if (WaterMaterial)
	{
		ReusltMesh->SetMaterial(0, WaterMaterial);
	}

	if (SimulationDecalMaterial)
	{
		DecalMaterial = SimulationDecalMaterial;
	}
	if (CausticsDecal && DecalMaterial)
	{
		CausticsDecal->SetDecalMaterial(DecalMaterial);
	}

	ReusltMesh->MarkRenderStateDirty();
}

bool ACSShallowWaterCapture::EnsureSimVisHISMReady()
{
	if (!SimVisHISM) return false;

	if (!SimVisHISM->GetStaticMesh())
	{
		UStaticMesh* PreviewMesh = SimulationPreviewMesh;
		if (!PreviewMesh && ReusltMesh)
		{
			PreviewMesh = ReusltMesh->GetStaticMesh();
		}
		if (!PreviewMesh && DebugMesh)
		{
			PreviewMesh = DebugMesh;
		}
		if (!PreviewMesh) return false;

		SimVisHISM->SetStaticMesh(PreviewMesh);
		if (WaterMaterial)
		{
			SimVisHISM->SetMaterial(0, WaterMaterial);
		}
	}

	SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SimVisHISM->SetCastShadow(false);
	SimVisHISM->bNeverDistanceCull = true;
	SimVisHISM->bAllowCullDistanceVolume = false;
	SimVisHISM->SetVisibility(bSimVisActive);
	return true;
}

void ACSShallowWaterCapture::ResetSimVisTiles()
{
	if (SimVisHISM)
	{
		SimVisHISM->ClearInstances();
		SimVisHISM->BuildTreeIfOutdated(false, true);
		SimVisHISM->MarkRenderStateDirty();
	}
	ISMTileSlots.Reset();
	CachedTileBits.Reset();
	CachedActiveTileCount = 0;
}

void ACSShallowWaterCapture::ResetSolverReadbackState(bool bAdvanceGeneration, bool bClearCachedResult)
{
	if (bAdvanceGeneration)
	{
		SolverReadbackGeneration++;
		if (SolverReadbackGeneration <= 0)
		{
			SolverReadbackGeneration = 1;
		}
	}

	TileMaskReadbackWriteIdx = 0;
	ResultReadbackWriteIdx = 0;
	TileMaskReadbackWidth = 0;
	TileMaskReadbackHeight = 0;
	LastSolverFrameNumber = 0;
	for (int32 i = 0; i < ReadbackBufferCount; i++)
	{
		TileMaskReadbackCopyWidth[i] = 0;
		TileMaskReadbackCopyHeight[i] = 0;
		TileMaskReadbackGeneration[i] = 0;
		ResultReadbackCopyWidth[i] = 0;
		ResultReadbackCopyHeight[i] = 0;
		ResultReadbackGeneration[i] = 0;
	}
	if (bClearCachedResult)
	{
		CachedResultPixels.Reset();
		CachedResultWidth = 0;
		CachedResultHeight = 0;
	}
}

bool ACSShallowWaterCapture::ShouldTickIfViewportsOnly() const
{
	return false;
}

void ACSShallowWaterCapture::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void ACSShallowWaterCapture::PostLoad()
{
	Super::PostLoad();

	StopSimulationRuntime(true);

	if (!SimVisHISM)
	{
		SimVisHISM = FindComponentByClass<UHierarchicalInstancedStaticMeshComponent>();
		if (!SimVisHISM)
		{
			SimVisHISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, TEXT("SimVisHISM"));
			SimVisHISM->SetupAttachment(SceneRoot);
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
		SimVisHISM->bNeverDistanceCull = true;
		SimVisHISM->bAllowCullDistanceVolume = false;
		UE_LOG(LogTemp, Warning, TEXT("ACSShallowWaterCapture::PostLoad - Recreated SimVisHISM for %s"), *GetName());
	}
}

void ACSShallowWaterCapture::BeginPlay()
{
	Super::BeginPlay();

	StopSimulationRuntime(true);
	SetActorTickEnabled(false);
}

void ACSShallowWaterCapture::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ConstructionDebounceHandle);
		World->GetTimerManager().SetTimer(ConstructionDebounceHandle,
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
	const bool bRestartSolverAfterConstruction = IsSolverTimerActive();
	if (bRestartSolverAfterConstruction)
	{
		ClearSolverTimer();
	}

	Clean();
	FVector RelativeScale = FVector(CaptureSize / 100, CaptureSize / 100, MaxHeight / 100);
	GeneratorBounds->SetRelativeScale3D(RelativeScale);
	GeneratorBounds->SetRelativeLocation(FVector(0, 0, Scale3DZ * 50));
	GeneratorBounds->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

	FVector DecalRelativeScale = FVector(MaxHeight / 100, CaptureSize / 100, CaptureSize / 100);
	CausticsDecal->SetRelativeScale3D(DecalRelativeScale);
	CausticsDecal->SetRelativeRotation(FRotator(-90, 0, 0));
	CausticsDecal->DecalSize = FVector(500, 50, 50);
	CausticsDecal->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	
	CaptureSceneDepth->SetRelativeLocation(FVector(0, 0, MaxHeight));
	CaptureSceneDepth->OrthoWidth = CaptureSize;
	CaptureSceneDepth->HiddenActors = {this};
	TextureSize = FMath::RoundUpToPowerOfTwo(FMath::Max(16, FMath::CeilToInt32(CaptureSize / WorldPixelSize)));
	if (bUseBakedResultMesh && BakedResultMesh)
	{
		ReusltMesh->SetStaticMesh(BakedResultMesh);
		ReusltMesh->SetRelativeScale3D(FVector::OneVector);
		ReusltMesh->MarkRenderStateDirty();
	}
	else
	{
		UpdateSimulationPreviewMesh();
	}
	ReusltMesh->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = CaptureSize;
	SimUVInvSize = CaptureSize > 0.f ? 1.f / CaptureSize : 0.f;

	SetActorScale3D(FVector::OneVector);

	if (bRestartSolverAfterConstruction)
	{
		ScheduleSolverTimerTick();
	}
	else
	{
		ClearSolverTimer();
	}
}

void ACSShallowWaterCapture::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	StopSimulationRuntime(true);
	Super::PreSave(ObjectSaveContext);
}

void ACSShallowWaterCapture::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSimulationRuntime(true);
	Super::EndPlay(EndPlayReason);
}

void ACSShallowWaterCapture::ShallowWaterSolverSoucePoint(int32 InIteration)
{
	if (bUseBakedResultMesh)
	{
		UseSimulationResultMesh();
	}

	if (GFrameCounter == LastSolverFrameNumber) return;
	LastSolverFrameNumber = GFrameCounter;

	if (!CheckAndCreateTexture_SWSourcePoint()) return;

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);

	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = CaptureSize;
	SimUVInvSize = CaptureSize > 0.f ? 1.f / CaptureSize : 0.f;

	TArray<FVector4> SourceData = GetSources();
	if (SourceData.Num() == 0)
	{
		ResetSimVisTiles();
		return;
	}

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
	FBoxSphereBounds Bounds = GeneratorBounds->Bounds;
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
	
	if (!RT_SceneDepth)
	{
		UE_LOG(LogTemp, Error, TEXT("[CSSW] RT_SceneDepth is null in solver tick — call StartSolver first"));
		return;
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
	}

	const bool bNeedsTileMaskResourceRebuild =
		RT_TileMask->SizeX != TileMaskWidth ||
		RT_TileMask->SizeY != TileMaskHeight ||
		RT_TileMask->RenderTargetFormat != ETextureRenderTargetFormat::RTF_RGBA16f ||
		!RT_TileMask->bCanCreateUAV;

	RT_TileMask->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
	RT_TileMask->bCanCreateUAV = true;
	RT_TileMask->ClearColor = FLinearColor::Black;

	if (bNeedsTileMaskResourceRebuild)
	{
		RT_TileMask->InitAutoFormat(TileMaskWidth, TileMaskHeight);
		RT_TileMask->UpdateResourceImmediate(true);
	}
	FTextureRenderTargetResource* R_TileMask = RT_TileMask->GameThread_GetRenderTargetResource();

	TileMaskReadbackWidth = TileMaskWidth;
	TileMaskReadbackHeight = TileMaskHeight;

	// Rebuild HISM instances from a per-tile active bitmask.
	// OR-merges with the previous frame's CachedTileBits to prevent flicker,
	// and early-outs when nothing changed.
	auto ApplySimVisTiles = [&](const TArray<uint8>& NewTileBits, int32 ISMTileCountX, int32 ISMTileCountY)
	{
		if (!IsSolverTimerActive() || !EnsureSimVisHISMReady() || ISMTileCountX <= 0 || ISMTileCountY <= 0)
		{
			return;
		}

		const int32 TotalSlots = ISMTileCountX * ISMTileCountY;
		const bool bInstanceCountMatchesCache = SimVisHISM->GetInstanceCount() == CachedActiveTileCount;
		if (NewTileBits.Num() != TotalSlots)
		{
			return;
		}

		// Temporal stabilization: keep tiles that were active last frame
		TArray<uint8> StableTileBits = NewTileBits;
		if (CachedTileBits.Num() == TotalSlots)
		{
			for (int32 Slot = 0; Slot < TotalSlots; Slot++)
			{
				StableTileBits[Slot] = StableTileBits[Slot] || CachedTileBits[Slot] ? 1 : 0;
			}
		}

		if (bInstanceCountMatchesCache && StableTileBits == CachedTileBits)
		{
			return;
		}

		SCOPE_CYCLE_COUNTER(STAT_CSSW_ISMUpdate);
		CachedTileBits = StableTileBits;

		// Convert tile indices to world-space transforms for the HISM
		const float TileWorldSize = CaptureSize / (float)ISMTileCountX;
		const float HalfCapture = CaptureSize * 0.5f;
		const FVector ActiveScale(TileWorldSize / 100.0f, TileWorldSize / 100.0f, 1.0f);

		SimVisHISM->ClearInstances();
		ISMTileSlots.Reset();

		TArray<FTransform> TransformsToAdd;
		for (int32 Slot = 0; Slot < TotalSlots; Slot++)
		{
			if (!StableTileBits[Slot]) continue;
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
		SimVisHISM->BuildTreeIfOutdated(false, true);
		SimVisHISM->MarkRenderStateDirty();
		SimVisHISM->SetVisibility(true);
		if (ReusltMesh)
		{
			ReusltMesh->SetVisibility(false);
		}
	};

	// CPU fallback: mark tiles active if a water source overlaps them (3x radius).
	// Used when GPU readback isn't ready yet (first frames) or fails validation.
	auto BuildSourceFallbackTiles = [&](int32 ISMTileCountX, int32 ISMTileCountY)
	{
		TArray<uint8> FallbackTileBits;
		const int32 TotalSlots = ISMTileCountX * ISMTileCountY;
		if (TotalSlots <= 0)
		{
			return FallbackTileBits;
		}

		FallbackTileBits.SetNumZeroed(TotalSlots);
		const float TileWorldSize = CaptureSize / (float)ISMTileCountX;
		const float HalfCapture = CaptureSize * 0.5f;
		const FVector ActorLoc = GetActorLocation();

		for (const FVector4& Src : SourceData)
		{
			const float LocalX = (float)(Src.X - ActorLoc.X);
			const float LocalY = (float)(Src.Y - ActorLoc.Y);
			const float Range = FMath::Max((float)Src.W * 3.0f, TileWorldSize);

			const int32 MinTXi = FMath::Clamp(FMath::FloorToInt32((LocalX - Range + HalfCapture) / TileWorldSize), 0, ISMTileCountX - 1);
			const int32 MaxTXi = FMath::Clamp(FMath::FloorToInt32((LocalX + Range + HalfCapture) / TileWorldSize), 0, ISMTileCountX - 1);
			const int32 MinTYi = FMath::Clamp(FMath::FloorToInt32((LocalY - Range + HalfCapture) / TileWorldSize), 0, ISMTileCountY - 1);
			const int32 MaxTYi = FMath::Clamp(FMath::FloorToInt32((LocalY + Range + HalfCapture) / TileWorldSize), 0, ISMTileCountY - 1);

			for (int32 TY = MinTYi; TY <= MaxTYi; TY++)
			{
				for (int32 TX = MinTXi; TX <= MaxTXi; TX++)
				{
					FallbackTileBits[TY * ISMTileCountX + TX] = 1;
				}
			}
		}

		return FallbackTileBits;
	};

	// Double-buffered GPU readback: read from the opposite index to what the GPU is writing.
	// Each tile in the GPU TileMask is 2x2 subsampled for the HISM grid.
	constexpr int32 SimVisSubsampleFactor = 2;
	const int32 FallbackTileCountX = FMath::DivideAndRoundUp(TileMaskWidth, SimVisSubsampleFactor);
	const int32 FallbackTileCountY = FMath::DivideAndRoundUp(TileMaskHeight, SimVisSubsampleFactor);
	const int32 TileMaskReadIdx = 1 - TileMaskReadbackWriteIdx;
	bool bAppliedSimVisThisFrame = false;

	// Try GPU TileMask readback — only proceed if the buffer matches current generation and is ready
	if (IsSolverTimerActive() && TileMaskReadbackGeneration[TileMaskReadIdx] == SolverReadbackGeneration && EnsureSimVisHISMReady() && TileMaskReadback[TileMaskReadIdx] && TileMaskReadback[TileMaskReadIdx]->IsReady())
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_TileReadback);

		int32 RowPitchInPixels = 0;
		int32 ReadbackBufferHeight = 0;
		const FFloat16Color* ReadbackData = static_cast<const FFloat16Color*>(TileMaskReadback[TileMaskReadIdx]->Lock(RowPitchInPixels, &ReadbackBufferHeight));

		const int32 ReadbackWidth = TileMaskReadbackCopyWidth[TileMaskReadIdx] > 0 ? TileMaskReadbackCopyWidth[TileMaskReadIdx] : TileMaskReadbackWidth;
		const int32 ReadbackHeight = TileMaskReadbackCopyHeight[TileMaskReadIdx] > 0 ? TileMaskReadbackCopyHeight[TileMaskReadIdx] : TileMaskReadbackHeight;
		const int32 ISMTileCountX = FMath::DivideAndRoundUp(FMath::Max(ReadbackWidth, 0), SimVisSubsampleFactor);
		const int32 ISMTileCountY = FMath::DivideAndRoundUp(FMath::Max(ReadbackHeight, 0), SimVisSubsampleFactor);
		const int32 TotalSlots = ISMTileCountX * ISMTileCountY;
		bool bUsedReadbackData = false;

		if (ReadbackData && RowPitchInPixels >= ReadbackWidth && ReadbackBufferHeight >= ReadbackHeight && TotalSlots > 0)
		{
			TArray<uint8> NewTileBits;
			NewTileBits.SetNumZeroed(TotalSlots);

			for (int32 TY = 0; TY < ISMTileCountY; TY++)
			{
				for (int32 TX = 0; TX < ISMTileCountX; TX++)
				{
					bool bActive = false;
					for (int32 DY = 0; DY < SimVisSubsampleFactor && !bActive; DY++)
					{
						for (int32 DX = 0; DX < SimVisSubsampleFactor && !bActive; DX++)
						{
							const int32 X = TX * SimVisSubsampleFactor + DX;
							const int32 Y = TY * SimVisSubsampleFactor + DY;
							if (X < ReadbackWidth && Y < ReadbackHeight)
							{
								if (ReadbackData[Y * RowPitchInPixels + X].R.GetFloat() > 0.5f)
									bActive = true;
							}
						}
					}
					NewTileBits[TY * ISMTileCountX + TX] = bActive ? 1 : 0;
				}
			}

			const TArray<uint8> SourceFallbackBits = BuildSourceFallbackTiles(ISMTileCountX, ISMTileCountY);
			if (SourceFallbackBits.Num() == NewTileBits.Num())
			{
				for (int32 Slot = 0; Slot < TotalSlots; Slot++)
				{
					NewTileBits[Slot] = NewTileBits[Slot] || SourceFallbackBits[Slot] ? 1 : 0;
				}
			}

			ApplySimVisTiles(NewTileBits, ISMTileCountX, ISMTileCountY);
			bAppliedSimVisThisFrame = true;
			bUsedReadbackData = true;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSSW] TileMask readback invalid. Data=%s Pitch=%d BufferH=%d Expected=%dx%d. Using fallback sim-vis tiles."),
				ReadbackData ? TEXT("Valid") : TEXT("Null"),
				RowPitchInPixels,
				ReadbackBufferHeight,
				ReadbackWidth,
				ReadbackHeight);
		}

		if (!bUsedReadbackData && CachedTileBits.Num() == 0 && TotalSlots > 0)
		{
			ApplySimVisTiles(BuildSourceFallbackTiles(ISMTileCountX, ISMTileCountY), ISMTileCountX, ISMTileCountY);
			bAppliedSimVisThisFrame = true;
		}
		if (ReadbackData)
		{
			TileMaskReadback[TileMaskReadIdx]->Unlock();
		}
	}
	if (IsSolverTimerActive() && !bAppliedSimVisThisFrame && FallbackTileCountX > 0 && FallbackTileCountY > 0)
	{
		ApplySimVisTiles(BuildSourceFallbackTiles(FallbackTileCountX, FallbackTileCountY), FallbackTileCountX, FallbackTileCountY);
	}

	// Read back simulation result (velocity/height) into CachedResultPixels for CPU queries.
	// Also double-buffered with generation check to avoid reading stale or in-flight data.
	const int32 ResultReadIdx = 1 - ResultReadbackWriteIdx;
	if (ResultReadbackGeneration[ResultReadIdx] == SolverReadbackGeneration && ResultReadback[ResultReadIdx] && ResultReadback[ResultReadIdx]->IsReady())
	{
		int32 RowPitch = 0;
		int32 ReadbackBufferHeight = 0;
		const FFloat16Color* Data = static_cast<const FFloat16Color*>(ResultReadback[ResultReadIdx]->Lock(RowPitch, &ReadbackBufferHeight));
		if (Data && RowPitch > 0)
		{
			const int32 W = ResultReadbackCopyWidth[ResultReadIdx] > 0 ? ResultReadbackCopyWidth[ResultReadIdx] : (int32)TextureSize;
			const int32 H = ResultReadbackCopyHeight[ResultReadIdx] > 0 ? ResultReadbackCopyHeight[ResultReadIdx] : (int32)TextureSize;
			if (W > 0 && H > 0 && RowPitch >= W && ReadbackBufferHeight >= H)
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
		if (Data)
		{
			ResultReadback[ResultReadIdx]->Unlock();
		}
	}

	InIteration = FMath::Max(InIteration, 1);
	const int32 TileWriteIdx = TileMaskReadbackWriteIdx;
	const int32 ResultWriteIdx = ResultReadbackWriteIdx;
	TileMaskReadbackCopyWidth[TileWriteIdx] = TileMaskWidth;
	TileMaskReadbackCopyHeight[TileWriteIdx] = TileMaskHeight;
	TileMaskReadbackGeneration[TileWriteIdx] = SolverReadbackGeneration;
	ResultReadbackCopyWidth[ResultWriteIdx] = (int32)TextureSize;
	ResultReadbackCopyHeight[ResultWriteIdx] = (int32)TextureSize;
	ResultReadbackGeneration[ResultWriteIdx] = SolverReadbackGeneration;
	const bool bUseSparseIndirect = GCSSWUseSparseIndirect != 0;
	const EWaterfallExpansion CapturedWaterfallExpansionIterations = WaterfallExpansionIterations;
	const float CapturedDT = DT;
	const float CapturedFriction = Friction;
	const float CapturedSeaLevel = SeaLevel;
	const float CapturedAdvectFoam = AdvectFoam;
	const float CapturedFoamFadeSpeed = FoamFadeSpeed;
	const float CapturedMaxWaterRisePerFrame = MaxWaterRisePerFrame;
	const int32 CapturedCloseBound = CloseBound;
	const int32 CapturedInIteration = InIteration;
	if (!TileMaskReadback[TileWriteIdx])
		TileMaskReadback[TileWriteIdx] = new FRHIGPUTextureReadback(TEXT("TileMaskReadback"));
	if (!ResultReadback[ResultWriteIdx])
		ResultReadback[ResultWriteIdx] = new FRHIGPUTextureReadback(TEXT("ResultReadback"));
	FRHIGPUTextureReadback* TileMaskReadbackForRender = TileMaskReadback[TileWriteIdx];
	FRHIGPUTextureReadback* ResultReadbackForRender = ResultReadback[ResultWriteIdx];

	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight, R_ResultDepthWet, R_ResultSmoothHeight, R_TileMask,
	 SourceUVRads = MoveTemp(SourceUVRads), bDoCapture, bUseSparseIndirect, CapturedWaterfallExpansionIterations,
	 CapturedDT, CapturedFriction, CapturedSeaLevel, ActorLocation, CapturedAdvectFoam, CapturedFoamFadeSpeed,
	 CapturedMaxWaterRisePerFrame, CapturedCloseBound, CapturedInIteration, TileMaskWidth, TileMaskHeight, TileMaskReadbackForRender,
	 ResultReadbackForRender](FRHICommandListImmediate& RHICmdList)
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
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;

			TShaderMapRef<FShallowWaterSim> ComputeShader_CompactActiveTiles = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_CompactActiveTiles);
			TShaderMapRef<FShallowWaterSim> ComputeShader_FinalizeCompact = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_FinalizeCompact);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalSmoothHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SmoothHeight);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalVelocityHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_VelocityHeightSim, false, CapturedWaterfallExpansionIterations);
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
			
			CREATE_RDG_STRUCTURED_UPLOAD_SRV(SourceUVRads, FVector4f, SourceUVRads, TEXT("SourceUVRads"))

			FCompactTileBuffers CompactBuffers = bUseSparseIndirect
				? CreateCompactTileBuffers(
					GraphBuilder, (uint32)SizeX, (uint32)SizeY,
					NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y)
				: CreateFullScreenCompactTileBuffers(
					GraphBuilder, (uint32)SizeX, (uint32)SizeY,
					NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);
			FCompactTileBuffers VisualTileMaskBuffers = CreateCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);
			const FIntVector SimTileGroupCount((int32)CompactBuffers.MaxTileCount, 1, 1);

			FRDGTextureRef TRDG_TileMask = nullptr;
			FRDGTextureUAVRef RDGUAV_TileMask = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TRDG_TileMask, RDGUAV_TileMask, FIntPoint(TileMaskWidth, TileMaskHeight), PF_FloatRGBA, TEXT("UAV_TileMask"), FLinearColor::Black);
			FRDGTextureRef RDG_TileMask = RegisterExternalTexture(GraphBuilder, R_TileMask->GetRenderTargetTexture(), TEXT("TileMask_RT"));

			PassParameters->DT = CapturedDT;
			PassParameters->Friction = CapturedFriction;
			PassParameters->SeaLevel = CapturedSeaLevel;
			PassParameters->ActorLocationZ = ActorLocation.Z;
			PassParameters->AdvectFoam = CapturedAdvectFoam;
			PassParameters->FoamFadeSpeed = CapturedFoamFadeSpeed;
			PassParameters->MaxWaterRisePerFrame = CapturedMaxWaterRisePerFrame;
			PassParameters->CloseBound = CapturedCloseBound;
			PassParameters->BCount_SourceUVRads = SourceUVRads.Num();
			PassParameters->RWB_SourceUVRads = SourceUVRadsSRV;
			PassParameters->DispatchExpandPixels = ComputeDispatchExpandPixels(CapturedInIteration, TextureSize.X);
			BindCompactTileBuffers(PassParameters, CompactBuffers);
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			AddCopyTexturePass(GraphBuilder, RDG_VelocityHeight, TRDG_VelHeightSimA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, TRDG_SmoothHeightA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultVelHeight, TRDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultDepthWet, TRDG_ResultDepthWet, FRHICopyTextureInfo());

			const FIntVector FullTileGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
			if (bUseSparseIndirect)
			{
				ResetCompactCounter(GraphBuilder, CompactBuffers);

				FShallowWaterSim::FParameters* CompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*CompactPassParameters = *PassParameters;
				CompactPassParameters->B_CompactTileCoords = nullptr;
				CompactPassParameters->B_CompactCounter = nullptr;
				CompactPassParameters->CompactIndirectArgs = nullptr;
				CompactPassParameters->RWB_CompactIndirectArgs = nullptr;
				CompactPassParameters->RW_TileMask = RDGUAV_TileMask;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("CompactActiveTiles"),
					ComputePassFlags,
					ComputeShader_CompactActiveTiles,
					CompactPassParameters,
					FullTileGroupCount);

				FShallowWaterSim::FParameters* FinalizeCompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*FinalizeCompactPassParameters = *PassParameters;
				NullifyAllCompactTileBindings(FinalizeCompactPassParameters);
				FinalizeCompactPassParameters->RWB_CompactCounter = CompactBuffers.CounterUAV;
				FinalizeCompactPassParameters->RWB_CompactIndirectArgs = CompactBuffers.IndirectArgsUAV;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FinalizeCompact"),
					ComputePassFlags,
					ComputeShader_FinalizeCompact,
					FinalizeCompactPassParameters,
					SingleGroupCount);
			}


			FRDGTextureRef CurrentVelHeightTextureA = TRDG_VelHeightSimA;
			FRDGTextureRef CurrentVelHeightTextureB = TRDG_VelHeightSimB;
			FRDGTextureRef CurrentSmoothHeightTextureA = TRDG_SmoothHeightA;
			FRDGTextureRef CurrentSmoothHeightTextureB = TRDG_SmoothHeightB;
			FRDGTextureUAVRef CurrentVelHeightSimA = RDGUAV_VelHeightSimA;
			FRDGTextureUAVRef CurrentVelHeightSimB = RDGUAV_VelHeightSimB;
			FRDGTextureUAVRef CurrentSmoothHeightA = RDGUAV_SmoothHeightA;
			FRDGTextureUAVRef CurrentSmoothHeightB = RDGUAV_SmoothHeightB;

			for (int32 i = 0 ; i < CapturedInIteration; i++)
			{
				FShallowWaterSim::FParameters* VelocityHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*VelocityHeightPassParameters = *PassParameters;
				VelocityHeightPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				VelocityHeightPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				VelocityHeightPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				VelocityHeightPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(VelocityHeightPassParameters);
				if (bUseSparseIndirect)
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalVelocityHeight"),
						ComputePassFlags,
						ComputeShader_CalVelocityHeight,
						VelocityHeightPassParameters,
						CompactBuffers.IndirectArgsBuffer,
						0);
				}
				else
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalVelocityHeight"),
						ComputePassFlags,
						ComputeShader_CalVelocityHeight,
						VelocityHeightPassParameters,
						SimTileGroupCount);
				}

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
				if (bUseSparseIndirect)
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalShallowIntegrate"),
						ComputePassFlags,
						ComputeShader_CalShallowIntegrate,
						ShallowIntegratePassParameters,
						CompactBuffers.IndirectArgsBuffer,
						0);
				}
				else
				{
					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("CalShallowIntegrate"),
						ComputePassFlags,
						ComputeShader_CalShallowIntegrate,
						ShallowIntegratePassParameters,
						SimTileGroupCount);
				}

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
			if (bUseSparseIndirect)
			{
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("Result"),
					ComputePassFlags,
					ComputeShader_CalResult,
					ResultPassParameters,
					CompactBuffers.IndirectArgsBuffer,
					0);
			}
			else
			{
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("Result"),
					ComputePassFlags,
					ComputeShader_CalResult,
					ResultPassParameters,
					SimTileGroupCount);
			}

			ResetCompactCounter(GraphBuilder, VisualTileMaskBuffers);
			FShallowWaterSim::FParameters* VisualTileMaskPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*VisualTileMaskPassParameters = *PassParameters;
			BindCompactTileBuffers(VisualTileMaskPassParameters, VisualTileMaskBuffers);
			VisualTileMaskPassParameters->B_CompactTileCoords = nullptr;
			VisualTileMaskPassParameters->B_CompactCounter = nullptr;
			VisualTileMaskPassParameters->CompactIndirectArgs = nullptr;
			VisualTileMaskPassParameters->RWB_CompactIndirectArgs = nullptr;
			VisualTileMaskPassParameters->T_ResultDepthWet = TRDG_ResultDepthWet;
			VisualTileMaskPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
			VisualTileMaskPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
			VisualTileMaskPassParameters->RW_TileMask = RDGUAV_TileMask;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("CompactActiveTiles_SimVis"),
				ComputePassFlags,
				ComputeShader_CompactActiveTiles,
				VisualTileMaskPassParameters,
				FullTileGroupCount);
			
			AddCopyTexturePass(GraphBuilder, TRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultVelHeight, RDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, CurrentVelHeightTextureA, RDG_VelocityHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_TileMask, RDG_TileMask, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();

		TileMaskReadbackForRender->EnqueueCopy(RHICmdList, R_TileMask->GetRenderTargetTexture());
		ResultReadbackForRender->EnqueueCopy(RHICmdList, R_ResultVelHeight->GetRenderTargetTexture());

#if PLATFORM_WINDOWS
		if (bDoCapture && GRenderDocAPI) GRenderDocAPI->EndFrameCapture(nullptr, nullptr);
#endif
	});

	TileMaskReadbackWriteIdx = 1 - TileWriteIdx;
	ResultReadbackWriteIdx = 1 - ResultWriteIdx;
}


void ACSShallowWaterCapture::SetHeight()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	if (!RT_SceneDepth) { UE_LOG(LogTemp, Error, TEXT("[CSSW] RT_SceneDepth is null in SetHeight")); return; }
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
	const float CapturedDT = DT;
	const float CapturedFriction = Friction;
	const float CapturedSeaLevel = SeaLevel;
	const float CapturedActorLocationZ = GetActorLocation().Z;

	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight, R_ResultDepthWet, R_ResultSmoothHeight,
	 CapturedDT, CapturedFriction, CapturedSeaLevel, CapturedActorLocationZ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;
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
			PassParameters->DT = CapturedDT;
			PassParameters->Friction = CapturedFriction;
			PassParameters->SeaLevel = CapturedSeaLevel;
			PassParameters->ActorLocationZ = CapturedActorLocationZ;
			PassParameters->MaxWaterRisePerFrame = 1e10f;
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
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SetHeight"),
				ComputePassFlags,
				ComputeShader_SetHeight,
				SetHeightPassParameters,
				GroupCount);
			
			// Result pass — uses CompactIndirectArgs, only needs SRV reads
			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			NullifyCompactTileUAVs(ResultPassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Result"),
				ComputePassFlags,
				ComputeShader_CalResult,
				ResultPassParameters,
				CompactBuffers.IndirectArgsBuffer,
				0);
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
	if (!RT_SceneDepth) { UE_LOG(LogTemp, Error, TEXT("[CSSW] RT_SceneDepth is null in HeightSmooth")); return; }
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
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;
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
			
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SmoothHeight"),
				ComputePassFlags,
				ComputeShader_SmoothHeight,
				PassParameters,
				GroupCount);
			
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
	&& GetWorld())
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
	UWorld* World = GetWorld();
	if (!World) return SourceLocations;

	for (TActorIterator<ACSSHallowWaterSource> It(World, ACSSHallowWaterSource::StaticClass()); It; ++It)
	{
		ACSSHallowWaterSource* Actor = *It;
		FVector Location = Actor->GetActorLocation();
		FBox AABB = FBox::BuildAABB(GeneratorBounds->Bounds.Origin, GeneratorBounds->Bounds.BoxExtent * FVector(1.2, 1.2, 9999));
		
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
	&& GetWorld())
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

	if (SimVisHISM && WaterMaterial)
	{
		UMaterialInstanceDynamic* HISM_MID = Cast<UMaterialInstanceDynamic>(SimVisHISM->GetMaterial(0));
		if (!HISM_MID || HISM_MID->Parent != WaterMaterial)
		{
			HISM_MID = UMaterialInstanceDynamic::Create(WaterMaterial, this);
			SimVisHISM->SetMaterial(0, HISM_MID);
		}
		HISM_MID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		HISM_MID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) HISM_MID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) HISM_MID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
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
	UWorld* World = GetWorld();
	if (!World) return;

	for (TActorIterator<AActor> It(World, AActor::StaticClass()); It; ++It)
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

	const int32 SafeTextureSize = ResolveTextureSize();
	if (!CaptureSceneDepth->TextureTarget)
	{
		CaptureSceneDepth->TextureTarget = UKismetRenderingLibrary::CreateRenderTarget2D(
			this, SafeTextureSize, SafeTextureSize,
			ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	}
	CaptureSceneDepth->CaptureScene();
	RT_SceneDepth = CaptureSceneDepth->TextureTarget;
}

void ACSShallowWaterCapture::CaptureSceneDepthFromTriangles()
{
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Capture);
	UWorld* World = GetWorld();
	if (!World) return;

	const int32 SafeTextureSize = ResolveTextureSize();
	const float ActorZ = GetActorLocation().Z;
	if (!RT_SceneDepth)
	{
		RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor(ActorZ + MaxHeight + 9000, 0, 0, 1), true, false);
		if (!RT_SceneDepth) return;
	}
	RT_SceneDepth->ResizeTarget(SafeTextureSize, SafeTextureSize);

	FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		FVector Center = GetActorLocation();
		float HalfSize = CaptureSize * 0.5f;
		QueryBox = FBox(
			FVector(Center.X - HalfSize, Center.Y - HalfSize, Center.Z - MaxHeight),
			FVector(Center.X + HalfSize, Center.Y + HalfSize, Center.Z + MaxHeight));
	}

	FCSBoxScenePreparedData Prepared = PrepareBoxSceneTriangles(World, QueryBox, -1, TArray<FVector>(), 0.0f, SWCaptureTag);

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	const float CapturedCameraHeight = GetActorLocation().Z + MaxHeight;
	const FBox CapturedBounds = QueryBox;

	ENQUEUE_RENDER_COMMAND(CaptureSceneDepthFromTriangles)(
	[this, R_SceneDepth, Prepared, CapturedCameraHeight, CapturedBounds](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FCSStaticMeshTriangleRDGOutput TriangleOutput = AddPreparedBoxSceneTrianglesToRDG(
			GraphBuilder, RHICmdList, Prepared, TEXT("CSSW.SceneDepthTriangles"));

		if (!TriangleOutput.TriangleVertices)
		{
			GraphBuilder.Execute();
			return;
		}

		FRDGTextureRef RDG_SceneDepth = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(R_SceneDepth->GetRenderTargetTexture(), TEXT("CSSW.RT_SceneDepth")));

		RasterizeTriangleSoupToHeightmapRDG(
			GraphBuilder, TriangleOutput, RDG_SceneDepth,
			CapturedBounds, CapturedCameraHeight);

		GraphBuilder.Execute();
	});

	FlushRenderingCommands();
}

void ACSShallowWaterCapture::DrawDebugHeightMapPoints(float Duration, int32 Stride, float PointSize, bool bUseLiquidSearch)
{
	if (!RT_SceneDepth || !GetWorld()) return;

	TArray<FLinearColor> Pixels;
	FRenderTarget* RenderTarget = RT_SceneDepth->GameThread_GetRenderTargetResource();
	if (!RenderTarget) return;
	RenderTarget->ReadLinearColorPixels(Pixels);

	if (Pixels.Num() == 0) return;

	const int32 TexW = RT_SceneDepth->SizeX;
	const int32 TexH = RT_SceneDepth->SizeY;
	const float CameraHeight = MaxHeight;

	FBox Bounds = GetGeneratorBoundsWorldBox();
	if (!Bounds.IsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] DrawDebugHeightMapPoints: Invalid bounds"));
		return;
	}

	int32 PointCount = 0;
	const int32 SafeStride = FMath::Max(1, Stride);
	for (int32 Y = 0; Y < TexH; Y += SafeStride)
	{
		for (int32 X = 0; X < TexW; X += SafeStride)
		{
			const float Depth = Pixels[Y * TexW + X].R;
			if (!FMath::IsFinite(Depth) || Depth > CameraHeight + 5000.0f) continue;

			const float WorldZ = CameraHeight - Depth;
			const float U = (float(X) + 0.5f) / float(TexW);
			const float V = (float(Y) + 0.5f) / float(TexH);
			const FVector WorldPos(
				FMath::Lerp(Bounds.Min.X, Bounds.Max.X, U),
				FMath::Lerp(Bounds.Min.Y, Bounds.Max.Y, V),
				WorldZ);

			float NormDepth = FMath::Clamp(Depth / CameraHeight, 0.0f, 1.0f);
			FColor Color = FLinearColor(1.0f - NormDepth, NormDepth, 0.2f).ToFColor(true);
			DrawDebugPoint(GetWorld(), WorldPos, PointSize, Color, false, Duration);
			PointCount++;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[CSSW] DrawDebugHeightMapPoints: %d points, CameraHeight=%.1f, ActorZ=%.1f, MaxHeight=%.1f"),
		PointCount, CameraHeight, GetActorLocation().Z, MaxHeight);
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

void ACSShallowWaterCapture::StartSolver(float TimerRate, int32 InIteration)
{
	ClearSolverTimer();
	SolverTimerRate = FMath::Max(TimerRate, 0.0f);
	Iteration = FMath::Max(InIteration, 1);
	ResetSolverReadbackState(true, true);
	if (RT_VelocityHeight) UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_VelocityHeight, FLinearColor(0, 0, 0, 0));
	if (RT_ResultVelHeight) UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultVelHeight, FLinearColor(0, 0, -9999, 0));
	if (RT_SmoothHeight) UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_SmoothHeight, FLinearColor(0, 0, 0, 0));
	CaptureSceneDepthNow();
	if (bUseBakedResultMesh)
	{
		UseSimulationResultMesh();
	}
	else
	{
		UpdateSimulationPreviewMesh();
	}
	bSimVisActive = true;
	EnsureSimVisHISMReady();
	ScheduleSolverTimerTick();
	OnSolverStarted();
	UE_LOG(LogTemp, Log, TEXT("[CSSW] StartSolver: %s Iteration=%d CaptureSize=%.2f TextureSize=%.0f TimerRate=%.4f"),
		*GetName(), Iteration, CaptureSize, TextureSize, SolverTimerRate);
}

void ACSShallowWaterCapture::StopSolver()
{
	StopSimulationRuntime(false);
}

void ACSShallowWaterCapture::ToggleSimVisualization(int32 SimIterationsPerFrame)
{
	if (!IsSolverTimerActive())
	{
		StartSolver(SolverTimerRate, SimIterationsPerFrame);
		return;
	}
	Iteration = FMath::Max(SimIterationsPerFrame, 1);

	if (bUseBakedResultMesh)
	{
		UseSimulationResultMesh();
	}

	bSimVisActive = !bSimVisActive;
	
	if (bSimVisActive)
	{
		UpdateSimulationPreviewMesh();
		if (!EnsureSimVisHISMReady()) return;

		ResetSimVisTiles();

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
	bSimVisActive = false;
	if (SimVisHISM) SimVisHISM->SetVisibility(false);
	if (ReusltMesh) ReusltMesh->SetVisibility(true);
}

void ACSShallowWaterCapture::UseBakedResultMesh(UStaticMesh* InBakedMesh, UMaterialInterface* InWaterMaterial, UMaterialInterface* InDecalMaterial)
{
	if (!InBakedMesh || !ReusltMesh) return;

	Modify();
	ReusltMesh->Modify();
	if (CausticsDecal)
	{
		CausticsDecal->Modify();
	}

	if (!SimulationPreviewMesh)
	{
		SimulationPreviewMesh = ReusltMesh->GetStaticMesh();
	}
	if (!SimulationWaterMaterial)
	{
		SimulationWaterMaterial = WaterMaterial;
	}
	if (!SimulationDecalMaterial)
	{
		SimulationDecalMaterial = DecalMaterial;
	}

	BakedResultMesh = InBakedMesh;
	bUseBakedResultMesh = true;
	WaterMaterial = InWaterMaterial ? InWaterMaterial : WaterMaterial;
	DecalMaterial = InDecalMaterial ? InDecalMaterial : DecalMaterial;

	StopSolver();
	bSimVisActive = false;
	if (SimVisHISM)
	{
		ResetSimVisTiles();
		SimVisHISM->SetVisibility(false);
	}

	ReusltMesh->SetStaticMesh(BakedResultMesh);
	ReusltMesh->SetRelativeScale3D(FVector::OneVector);
	if (WaterMaterial)
	{
		ReusltMesh->SetMaterial(0, WaterMaterial);
	}
	ReusltMesh->SetVisibility(true);
	ReusltMesh->MarkRenderStateDirty();
	if (CausticsDecal && DecalMaterial)
	{
		CausticsDecal->SetDecalMaterial(DecalMaterial);
		CausticsDecal->SetVisibility(true);
		CausticsDecal->MarkRenderStateDirty();
	}
	MarkPackageDirty();
}

void ACSShallowWaterCapture::UseSimulationResultMesh()
{
	if (!ReusltMesh) return;

	Modify();
	ReusltMesh->Modify();

	bUseBakedResultMesh = false;
	bSimVisActive = false;

	UpdateSimulationPreviewMesh();
	SetMaterialParameter();
	MarkPackageDirty();
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

void ACSShallowWaterCapture::Destroyed()
{
	ClearSolverTimer();
	ReleaseTerrainVoxelGrid();
	Super::Destroyed();
}

void ACSShallowWaterCapture::BeginDestroy()
{
	ClearSolverTimer();
	ReleaseTerrainVoxelGrid();
	for (int32 i = 0; i < ReadbackBufferCount; i++)
	{
		delete TileMaskReadback[i];
		TileMaskReadback[i] = nullptr;
		delete ResultReadback[i];
		ResultReadback[i] = nullptr;
	}
	Super::BeginDestroy();
}

bool ACSShallowWaterCapture::CanRunShallowWaterGPUWork(const TCHAR* Context) const
{
	if (bSWConstructionGuardActive)
	{
		return false;
	}
	if (!GEngine || !GetWorld())
	{
		return false;
	}
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return false;
	}
	return true;
}

int32 ACSShallowWaterCapture::ResolveTextureSize() const
{
	const int32 Raw = FMath::CeilToInt32(CaptureSize / FMath::Max(WorldPixelSize, 1.0f));
	return FMath::RoundUpToPowerOfTwo(FMath::Max(16, Raw));
}

void ACSShallowWaterCapture::ReleaseTerrainVoxelGrid()
{
	VoxelColumnRunStartBuffer.SafeRelease();
	VoxelColumnRunCountBuffer.SafeRelease();
	VoxelRunsBuffer.SafeRelease();
	VoxelCoverageBuffer.SafeRelease();
	VoxelTotalRunCount = 0;
	VoxelGridSize = FIntVector::ZeroValue;
	bVoxelGridValid = false;
	bVoxelHeightMapInitialized = false;
}

void ACSShallowWaterCapture::ReleaseShallowWaterTransientResources(const TCHAR* Context)
{
	ReleaseTerrainVoxelGrid();
	for (int32 i = 0; i < ReadbackBufferCount; i++)
	{
		delete TileMaskReadback[i];
		TileMaskReadback[i] = nullptr;
		delete ResultReadback[i];
		ResultReadback[i] = nullptr;
	}
	CachedTileBits.Reset();
	CachedResultPixels.Reset();
	CachedResultWidth = 0;
	CachedResultHeight = 0;
}

void ACSShallowWaterCapture::ReleaseTransientRenderResources()
{
	ReleaseShallowWaterTransientResources(TEXT("ReleaseTransientRenderResources"));
}

void ACSShallowWaterCapture::VisualizeVoxelRuns(float Duration)
{
	UE_LOG(LogTemp, Warning, TEXT("[CSSW] VisualizeVoxelRuns: Not yet implemented"));
}

void ACSShallowWaterCapture::DrawDebugVoxelGrid(float Duration, float Thickness)
{
	UE_LOG(LogTemp, Warning, TEXT("[CSSW] DrawDebugVoxelGrid: Not yet implemented"));
}


