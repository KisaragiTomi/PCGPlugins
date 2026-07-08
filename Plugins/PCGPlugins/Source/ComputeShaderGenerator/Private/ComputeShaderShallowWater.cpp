#include "ComputeShaderShallowWater.h"
#include "ComputeShaderGenerateHepler.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "SparseTileDispatchHelper.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ObjectSaveContext.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

DECLARE_STATS_GROUP(TEXT("CSSW"), STATGROUP_CSSW, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("CSSW Execute"), STAT_CSSW_Execute, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Capture"), STAT_CSSW_Capture, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Total"), STAT_CSSW_Total, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW CollectSources"), STAT_CSSW_CollectSources, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW EnqueueRender"), STAT_CSSW_EnqueueRender, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW BuildHISM"), STAT_CSSW_BuildHISM, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW SetMaterial"), STAT_CSSW_SetMaterial, STATGROUP_CSSW);
DECLARE_GPU_STAT_NAMED(Stat_ShallowWater, TEXT("ShallowWater"));
DECLARE_GPU_STAT_NAMED(Stat_SW_Compact, TEXT("SW_Compact"));
DECLARE_GPU_STAT_NAMED(Stat_SW_VelHeight, TEXT("SW_VelHeight"));
DECLARE_GPU_STAT_NAMED(Stat_SW_Integrate, TEXT("SW_Integrate"));
DECLARE_GPU_STAT_NAMED(Stat_SW_Result, TEXT("SW_Result"));


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
		SHADER_PARAMETER(float, SpikeClampHeight)
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
	ReusltMesh->SetCastShadow(false);
	ReusltMesh->SetupAttachment(SceneRoot, TEXT("VisualizationMesh"));
	SimVisHISM = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("SimVisHISM"));
	SimVisHISM->SetupAttachment(SceneRoot, TEXT("SimVisHISM"));
#if WITH_EDITOR
	SimVisHISM->SetIsVisualizationComponent(true);
#endif
	SimVisHISM->SetVisibility(false);
	SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SimVisHISM->SetCastShadow(false);
	SimVisHISM->bVisibleInRayTracing = false;
	SimVisHISM->NumCustomDataFloats = 0;
	SimVisHISM->bNeverDistanceCull = true;
	SimVisHISM->bAllowCullDistanceVolume = false;
	CausticsDecal = CreateDefaultSubobject<UDecalComponent>(TEXT("CausticsDecal"));
	CausticsDecal->SetupAttachment(SceneRoot, TEXT("CausticsDecal"));
}

void ACSShallowWaterCapture::ClearSolverTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SolverTimerHandle);
	}
	SolverTimerHandle.Invalidate();
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

	const int32 ExpectedSolverGeneration = SolverGeneration;
	FTimerDelegate SolverTimerDelegate = FTimerDelegate::CreateWeakLambda(this, [this, ExpectedSolverGeneration]()
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_Total);
		if (ExpectedSolverGeneration != SolverGeneration || IsActorBeingDestroyed())
		{
			return;
		}

		ScheduleSolverTimerTick();
		ShallowWaterSolverSoucePoint(Iteration);
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

void ACSShallowWaterCapture::StopSimulationRuntime()
{
	ClearSolverTimer();
	ResetSolverState(true);
}

void ACSShallowWaterCapture::ResetSolverState(bool bAdvanceGeneration)
{
	if (bAdvanceGeneration)
	{
		SolverGeneration++;
		if (SolverGeneration <= 0) SolverGeneration = 1;
	}
	LastSolverFrameNumber = 0;
}

void ACSShallowWaterCapture::Clean()
{
	ClearSolverTimer();
	ResetSolverState(true);
	if (!CleanRenderTargets())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Clean failed on %s: render targets could not be initialized."), *GetName());
	}
}

bool ACSShallowWaterCapture::CleanRenderTargets()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return false;

	const int32 SafeTextureSize = FMath::Max(16, TextureSize);
	const FLinearColor DebugViewClearColor = FLinearColor::Black;
	const FLinearColor VelocityHeightClearColor(0, 0, 0, 0);
	const FLinearColor ResultVelHeightClearColor(0, 0, -9999, 0);
	const FLinearColor ResultDepthWetClearColor(-9999, -9999, -9999, -9999);
	const FLinearColor SmoothHeightClearColor(0, 0, 0, 0);
	const FLinearColor SceneDepthClearColor(GetActorLocation().Z + MaxHeight + 9000.0f, 0, 0, 1);

	if (!RT_DebugView || !RT_VelocityHeight || !RT_ResultVelHeight || !RT_ResultDepthWet || !RT_SmoothHeight)
	{
		return false;
	}

	RT_DebugView->ClearColor = DebugViewClearColor;
	RT_VelocityHeight->ClearColor = VelocityHeightClearColor;
	RT_ResultVelHeight->ClearColor = ResultVelHeightClearColor;
	RT_ResultDepthWet->ClearColor = ResultDepthWetClearColor;
	RT_SmoothHeight->ClearColor = SmoothHeightClearColor;

	RT_DebugView->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_VelocityHeight->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_ResultVelHeight->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_ResultDepthWet->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_SmoothHeight->ResizeTarget(SafeTextureSize, SafeTextureSize);

	if (!RT_SceneDepth)
	{
		RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(
			this,
			SafeTextureSize,
			SafeTextureSize,
			ETextureRenderTargetFormat::RTF_RGBA32f,
			SceneDepthClearColor,
			true,
			false);
		if (!RT_SceneDepth) return false;
	}
	RT_SceneDepth->ClearColor = SceneDepthClearColor;
	RT_SceneDepth->ResizeTarget(SafeTextureSize, SafeTextureSize);

	const int32 TileMaskWidth = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
	EnsureTileMask(TileMaskWidth, TileMaskHeight);
	if (!RT_TileMask) return false;

	auto ClearRenderTarget = [this](UTextureRenderTarget2D* RenderTarget, const FLinearColor& ClearColor)
	{
		if (RenderTarget)
		{
			UKismetRenderingLibrary::ClearRenderTarget2D(this, RenderTarget, ClearColor);
		}
	};

	ClearRenderTarget(RT_DebugView, DebugViewClearColor);
	ClearRenderTarget(RT_VelocityHeight, VelocityHeightClearColor);
	ClearRenderTarget(RT_ResultVelHeight, ResultVelHeightClearColor);
	ClearRenderTarget(RT_ResultDepthWet, ResultDepthWetClearColor);
	ClearRenderTarget(RT_SmoothHeight, SmoothHeightClearColor);
	ClearRenderTarget(RT_SceneDepth, SceneDepthClearColor);
	ClearRenderTarget(RT_TileMask, FLinearColor::Black);

	return true;
}

void ACSShallowWaterCapture::PostLoad()
{
	Super::PostLoad();

	StopSimulationRuntime();

}

void ACSShallowWaterCapture::BeginPlay()
{
	Super::BeginPlay();

	StopSimulationRuntime();
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
	// FVector CLocation = CausticsDecal->GetComponentLocation();
	// CausticsDecal->SetWorldLocation(FVector(CLocation.X, CLocation.Y, 0));
}


void ACSShallowWaterCapture::ConstructionComponent()
{
	bool bRestartSolverAfterConstruction = SolverTimerHandle.IsValid();
	if (UWorld* World = GetWorld())
	{
		bRestartSolverAfterConstruction = World->GetTimerManager().TimerExists(SolverTimerHandle);
	}
	if (bRestartSolverAfterConstruction)
	{
		ClearSolverTimer();
	}

	RefreshConstructionLayout();

	if (bRestartSolverAfterConstruction)
	{
		CaptureAll();
		ScheduleSolverTimerTick();
	}
	else
	{
		ClearSolverTimer();
	}
}

void ACSShallowWaterCapture::RefreshConstructionLayout()
{
	FVector RelativeScale = FVector(CaptureSize / 100, CaptureSize / 100, MaxHeight / 100);
	GeneratorBounds->SetRelativeScale3D(RelativeScale);
	GeneratorBounds->SetRelativeLocation(FVector(0, 0, Scale3DZ * 50));

	FVector DecalRelativeScale = FVector(MaxHeight / 100, CaptureSize / 100, CaptureSize / 100);
	CausticsDecal->SetRelativeScale3D(DecalRelativeScale);
	CausticsDecal->SetRelativeRotation(FRotator(-90, 0, 0));
	CausticsDecal->DecalSize = FVector(500, 50, 50);

	TextureSize = FMath::RoundUpToPowerOfTwo(
		FMath::Max(16, FMath::CeilToInt32(CaptureSize / FMath::Max(WorldPixelSize, 1.0f))));

	UpdateSimUV();
	SetActorScale3D(FVector::OneVector);
}

void ACSShallowWaterCapture::UpdateSimUV()
{
	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = CaptureSize;
	SimUVInvSize = CaptureSize > 0.f ? 1.f / CaptureSize : 0.f;
}

void ACSShallowWaterCapture::EnsureTileMask(int32 Width, int32 Height)
{
	if (!RT_TileMask)
	{
		RT_TileMask = NewObject<UTextureRenderTarget2D>(this);
	}

	const bool bNeedsRebuild =
		RT_TileMask->SizeX != Width ||
		RT_TileMask->SizeY != Height ||
		RT_TileMask->RenderTargetFormat != ETextureRenderTargetFormat::RTF_RGBA16f ||
		!RT_TileMask->bCanCreateUAV;

	RT_TileMask->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
	RT_TileMask->bCanCreateUAV = true;
	RT_TileMask->ClearColor = FLinearColor::Black;

	if (bNeedsRebuild)
	{
		RT_TileMask->InitAutoFormat(Width, Height);
		RT_TileMask->UpdateResourceImmediate(true);
	}
}

void ACSShallowWaterCapture::EnsureRTSizes()
{
	RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultDepthWet->ResizeTarget(TextureSize, TextureSize);
	RT_SmoothHeight->ResizeTarget(TextureSize, TextureSize);
}

void ACSShallowWaterCapture::RebuildSimulationVisualization()
{
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_BuildHISM);
		if (SimVisHISM)
		{
			UStaticMesh* PreviewMesh = SimulationPreviewMesh;
			if (!PreviewMesh && ReusltMesh) PreviewMesh = ReusltMesh->GetStaticMesh();
			if (!PreviewMesh && DebugMesh) PreviewMesh = DebugMesh;
			if (PreviewMesh && SimVisHISM->GetStaticMesh() != PreviewMesh)
			{
				SimVisHISM->SetStaticMesh(PreviewMesh);
			}
			if (!SimVisHISM->GetStaticMesh())
			{
				SimVisHISM->ClearInstances();
				return;
			}
			if (WaterMaterial)
			{
				SimVisHISM->SetMaterial(0, WaterMaterial);
			}

			constexpr int32 SimVisSubsampleFactor = 2;
			const int32 SafeTextureSize = FMath::Max(16, TextureSize);
			const int32 TileMaskWidth = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
			const int32 TileMaskHeight = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
			const int32 GridCountX = FMath::DivideAndRoundUp(TileMaskWidth, SimVisSubsampleFactor);
			const int32 GridCountY = FMath::DivideAndRoundUp(TileMaskHeight, SimVisSubsampleFactor);
			const int32 TotalSlots = GridCountX * GridCountY;
			const float TileWorldSize = CaptureSize / (float)GridCountX;
			const float HalfCapture = CaptureSize * 0.5f;
			const FVector TileScale(TileWorldSize / 100.0f, TileWorldSize / 100.0f, 1.0f);

			SimVisHISM->ClearInstances();

			TArray<FTransform> Transforms;
			Transforms.Reserve(TotalSlots);
			for (int32 Slot = 0; Slot < TotalSlots; Slot++)
			{
				const int32 TX = Slot % GridCountX;
				const int32 TY = Slot / GridCountX;
				const float CenterX = (TX + 0.5f) * TileWorldSize - HalfCapture;
				const float CenterY = (TY + 0.5f) * TileWorldSize - HalfCapture;
				Transforms.Emplace(FQuat::Identity, FVector(CenterX, CenterY, 0.0f), TileScale);
			}

			SimVisHISM->AddInstances(Transforms, false, false);
			SimVisHISM->BuildTreeIfOutdated(false, true);
			SimVisHISM->MarkRenderStateDirty();
		}
	}
}

void ACSShallowWaterCapture::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	StopSimulationRuntime();
	Super::PreSave(ObjectSaveContext);
}

void ACSShallowWaterCapture::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSimulationRuntime();
	Super::EndPlay(EndPlayReason);
}

void ACSShallowWaterCapture::ShallowWaterSolverSoucePoint(int32 InIteration)
{
	if (GFrameCounter == LastSolverFrameNumber) return;
	LastSolverFrameNumber = GFrameCounter;

	if (!CheckAndCreateTexture_SWSourcePoint()) return;

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);

	UpdateSimUV();

	TArray<FVector4> SourceData;
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_CollectSources);
		UWorld* World = GetWorld();
		if (!World)
		{
			return;
		}

		const FBox SourceBounds = FBox::BuildAABB(
			GeneratorBounds->Bounds.Origin,
			GeneratorBounds->Bounds.BoxExtent * FVector(1.2, 1.2, 9999));

		for (TActorIterator<ACSSHallowWaterSource> It(World, ACSSHallowWaterSource::StaticClass()); It; ++It)
		{
			ACSSHallowWaterSource* Actor = *It;
			const FVector Location = Actor->GetActorLocation();
			if (!SourceBounds.IsInsideOrOn(Location))
			{
				continue;
			}

			SourceData.Add(FVector4(Location.X, Location.Y, Location.Z, Actor->GetActorScale3D().X / CaptureSize * 500));
		}
	}
	if (SourceData.Num() == 0)
	{
		return;
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
		CaptureAll();
		if (!RT_SceneDepth)
		{
			UE_LOG(LogTemp, Error, TEXT("[CSSW] RT_SceneDepth is null after CaptureAll — cannot run solver"));
			return;
		}
	}
	EnsureRTSizes();

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();

	const int32 TileMaskWidth = FMath::DivideAndRoundUp(TextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp(TextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
	EnsureTileMask(TileMaskWidth, TileMaskHeight);
	FTextureRenderTargetResource* R_TileMask = RT_TileMask->GameThread_GetRenderTargetResource();

	InIteration = FMath::Max(InIteration, 1);
	const EWaterfallExpansion CapturedWaterfallExpansionIterations = WaterfallExpansionIterations;
	const float CapturedDT = DT;
	const float CapturedFriction = Friction;
	const float CapturedSeaLevel = SeaLevel;
	const float CapturedAdvectFoam = AdvectFoam;
	const float CapturedFoamFadeSpeed = FoamFadeSpeed;
	const float CapturedMaxWaterRisePerFrame = MaxWaterRisePerFrame;
	const float CapturedSpikeClampHeight = SpikeClampHeight;
	const int32 CapturedCloseBound = CloseBound;
	const int32 CapturedInIteration = InIteration;

	SCOPE_CYCLE_COUNTER(STAT_CSSW_EnqueueRender);
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight, R_ResultDepthWet, R_ResultSmoothHeight, R_TileMask,
	 SourceUVRads = MoveTemp(SourceUVRads), CapturedWaterfallExpansionIterations,
	 CapturedDT, CapturedFriction, CapturedSeaLevel, ActorLocation, CapturedAdvectFoam, CapturedFoamFadeSpeed,
	 CapturedMaxWaterRisePerFrame, CapturedSpikeClampHeight, CapturedCloseBound, CapturedInIteration, TileMaskWidth, TileMaskHeight
	 ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_ShallowWater);
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

			FCompactTileBuffers CompactBuffers = CreateCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);
			FCompactTileBuffers VisualTileMaskBuffers = CreateCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);

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
			PassParameters->SpikeClampHeight = CapturedSpikeClampHeight;
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
			{
				RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_Compact);
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
				RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_VelHeight);
				FShallowWaterSim::FParameters* VelocityHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*VelocityHeightPassParameters = *PassParameters;
				VelocityHeightPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				VelocityHeightPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				VelocityHeightPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				VelocityHeightPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(VelocityHeightPassParameters);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("CalVelocityHeight"),
					ComputePassFlags,
					ComputeShader_CalVelocityHeight,
					VelocityHeightPassParameters,
					CompactBuffers.IndirectArgsBuffer,
					0);

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentSmoothHeightTextureA, CurrentSmoothHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
				Swap(CurrentSmoothHeightA, CurrentSmoothHeightB);

				RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_Integrate);
				FShallowWaterSim::FParameters* ShallowIntegratePassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*ShallowIntegratePassParameters = *PassParameters;
				ShallowIntegratePassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				ShallowIntegratePassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				ShallowIntegratePassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				ShallowIntegratePassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(ShallowIntegratePassParameters);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("CalShallowIntegrate"),
					ComputePassFlags,
					ComputeShader_CalShallowIntegrate,
					ShallowIntegratePassParameters,
					CompactBuffers.IndirectArgsBuffer,
					0);

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
			}

			RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_Result);
			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			ResultPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
			ResultPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
			ResultPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
			ResultPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
			NullifyCompactTileUAVs(ResultPassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Result"),
				ComputePassFlags,
				ComputeShader_CalResult,
				ResultPassParameters,
				CompactBuffers.IndirectArgsBuffer,
				0);

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
	});
}


void ACSShallowWaterCapture::SetHeight()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	if (!RT_SceneDepth) { UE_LOG(LogTemp, Error, TEXT("[CSSW] RT_SceneDepth is null in SetHeight")); return; }
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);
	
	EnsureRTSizes();

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
			PassParameters->SpikeClampHeight = 0.0f;
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

void ACSShallowWaterCapture::SetMaterialParameter_Implementation()
{
	SCOPE_CYCLE_COUNTER(STAT_CSSW_SetMaterial);

	auto ApplySimParams = [this](UMaterialInstanceDynamic* MID)
	{
		if (!MID) return;
		MID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		MID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) MID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) MID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
	};

	auto EnsureMID = [this](UPrimitiveComponent* Comp, int32 SlotIndex, UMaterialInterface* ParentMat) -> UMaterialInstanceDynamic*
	{
		if (!Comp || !ParentMat) return nullptr;
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Comp->GetMaterial(SlotIndex));
		if (!MID || MID->Parent != ParentMat)
		{
			MID = UMaterialInstanceDynamic::Create(ParentMat, this);
			Comp->SetMaterial(SlotIndex, MID);
		}
		return MID;
	};

	ApplySimParams(EnsureMID(ReusltMesh, 0, WaterMaterial));
	ApplySimParams(EnsureMID(SimVisHISM, 0, WaterMaterial));

	if (CausticsDecal && DecalMaterial)
	{
		UMaterialInstanceDynamic* DecalMID = Cast<UMaterialInstanceDynamic>(CausticsDecal->GetDecalMaterial());
		if (!DecalMID || DecalMID->Parent != DecalMaterial)
		{
			DecalMID = UMaterialInstanceDynamic::Create(DecalMaterial, this);
			CausticsDecal->SetDecalMaterial(DecalMID);
		}
		ApplySimParams(DecalMID);
	}
}

void ACSShallowWaterCapture::CaptureAll()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;

	const int32 SafeTextureSize = FMath::Max(16, TextureSize);
	const FLinearColor SceneDepthClearColor(GetActorLocation().Z + MaxHeight + 9000.0f, 0, 0, 1);

	if (!RT_SceneDepth)
	{
		RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(
			this, SafeTextureSize, SafeTextureSize,
			ETextureRenderTargetFormat::RTF_RGBA32f,
			SceneDepthClearColor, true, false);
		if (!RT_SceneDepth) return;
	}
	RT_SceneDepth->ClearColor = SceneDepthClearColor;

	const int32 TileMaskWidth = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
	EnsureTileMask(TileMaskWidth, TileMaskHeight);

	EnsureRTSizes();
	UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_SceneDepth, SceneDepthClearColor);

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Capture);
	UWorld* World = GetWorld();
	if (!World) return;

	FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid) return;

	const float ActorZ = GetActorLocation().Z;
	QueryBox.Min.Z = FMath::Min(QueryBox.Min.Z, ActorZ - MaxHeight);
	QueryBox.Max.Z = FMath::Max(QueryBox.Max.Z, ActorZ + MaxHeight);

	FCSBoxScenePreparedData Prepared = PrepareBoxSceneTriangles(World, QueryBox, -1, TArray<FVector>(), 0.0f, SWCaptureTag);

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	const float CapturedCameraHeight = ActorZ + MaxHeight;
	const FBox CapturedBounds = QueryBox;

	ENQUEUE_RENDER_COMMAND(CaptureAllSceneDepth)(
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

void ACSShallowWaterCapture::StartSolver(float TimerRate, int32 InIteration)
{
	ClearSolverTimer();
	SolverTimerRate = FMath::Max(TimerRate, 0.0f);
	Iteration = FMath::Max(InIteration, 1);
	ResetSolverState(true);
	if (SimulationPreviewMesh && ReusltMesh)
	{
		ReusltMesh->SetStaticMesh(SimulationPreviewMesh);
		ReusltMesh->SetRelativeScale3D(FVector::OneVector);
	}
	bUseBakedResultMesh = false;
	RefreshConstructionLayout();
	RebuildSimulationVisualization();
	if (SimVisHISM) SimVisHISM->SetVisibility(true);
	if (ReusltMesh) ReusltMesh->SetVisibility(false);
	CaptureAll();
	ScheduleSolverTimerTick();
	OnSolverStarted();
	UE_LOG(LogTemp, Log, TEXT("[CSSW] StartSolver: %s Iteration=%d CaptureSize=%.2f TextureSize=%d TimerRate=%.4f"),
		*GetName(), Iteration, CaptureSize, TextureSize, SolverTimerRate);
}

void ACSShallowWaterCapture::StopSolver()
{
	StopSimulationRuntime();
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

void ACSShallowWaterCapture::BrowseBakedAssets()
{
#if WITH_EDITOR
	ULevel* CurrentLevel = GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName, LevelPath, Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetFolderPath = LevelPath / TEXT("CSSWData");

	TArray<UObject*> ObjectsToSync;

	if (BakedResultMesh)
	{
		ObjectsToSync.Add(BakedResultMesh);
	}

	if (SWUniqueID >= 0)
	{
		FString MICPath = AssetFolderPath / FString::Printf(TEXT("MI_CSSW_Water_%d"), SWUniqueID);
		UObject* MIC = StaticLoadObject(UObject::StaticClass(), nullptr, *MICPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (MIC) ObjectsToSync.Add(MIC);
	}

	if (ObjectsToSync.Num() > 0 && GEditor)
	{
		GEditor->SyncBrowserToObjects(ObjectsToSync);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] No baked assets found for %s (SWUniqueID=%d)"), *GetName(), SWUniqueID);
	}
#endif
}

void ACSShallowWaterCapture::OnBakeComplete_Implementation()
{
	ShowBakedResult();
}

void ACSShallowWaterCapture::ShowBakedResult()
{
	StopSolver();
	if (SimVisHISM)
	{
		SimVisHISM->ClearInstances();
		SimVisHISM->SetVisibility(false);
	}
	if (ReusltMesh)
	{
		if (BakedResultMesh)
		{
			ReusltMesh->SetStaticMesh(BakedResultMesh);
			ReusltMesh->SetRelativeScale3D(FVector::OneVector);
			bUseBakedResultMesh = true;
		}
		ReusltMesh->SetVisibility(true);
	}
	if (CausticsDecal && DecalMaterial) CausticsDecal->SetVisibility(true);
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
	const float PlaneScale = CaptureSize / 100.0f;
	FTransform SpawnTransform(GetActorRotation(), GetActorLocation(), FVector(PlaneScale, PlaneScale, 1.0f));
	AStaticMeshActor* Spawned = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		SpawnTransform,
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
	Super::Destroyed();
}

void ACSShallowWaterCapture::BeginDestroy()
{
	ClearSolverTimer();
	ReleaseTransientRenderResources();
	Super::BeginDestroy();
}

void ACSShallowWaterCapture::ReleaseTransientRenderResources()
{
	ClearMeshGeneratorCache();
	ClearGeneratedDataTextureCache();
}
