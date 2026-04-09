#include "ComputeShaderShallowWater.h"
#include "ComputeShaderGenerateHepler.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "ComputeShaderGenerateHepler.h"
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
#include "Misc/LowLevelTestAdapter.h"

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

	auto GetAPIFn = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(RDMod, "RENDERDOC_GetAPI"));
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
	using FPermutationDomain = TShaderPermutationDomain<FShallowWaterSimStep, FSplineRange>;

	static TShaderMapRef<FShallowWaterSim> CreatePermutation(EShallowWaterSimStep Permutation, bool UseSplineRange = false)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FSplineRange>(UseSplineRange);
		PermutationVector.Set<FShallowWaterSimStep>(Permutation);
		TShaderMapRef<FShallowWaterSim> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FShallowWaterSim);
	//Tells the engine that this shader uses a structure for its parameters
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
		// SHADER_PARAMETER(FVector2f, MaxCell)
		SHADER_PARAMETER(float, DT)
		SHADER_PARAMETER(float, Friction)
		SHADER_PARAMETER(float, SeaLevel)
		SHADER_PARAMETER(float, ActorLocationZ)
		SHADER_PARAMETER(float, AdvectFoam)
		SHADER_PARAMETER(float, FoamFadeSpeed)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()
public:
	//Called by the engine to determine which permutations to compile for this shader
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
	//Modifies the compilations environment of the shader
	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		//We're using it here to add some preprocessor defines. That way we don't have to change both C++ and HLSL code 
		// when we change the value for NUM_THREADS_PER_GROUP_DIMENSION
		
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_DIMENSION_X);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_DIMENSION_Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), NUM_THREADS_PER_GROUP_DIMENSION_Z);
		// OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_NAME"), "ShareGroupGeneral");
		// OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), "CreateFunc");
		
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
		//OutEnvironment.SetDefine(TEXT("FINDPIXELTHREADSIZE"), 256);

		if (PermutationVector.Get<FShallowWaterSimStep>() == EShallowWaterSimStep::SW_VelocityHeightSim)
		{
			OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), TEXT("CalVelocityHeight_SmoothData"));
			OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_EXTENT"), 5);
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
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CaptureRoot"));
	//SceneComponent->SetupAttachment(GetRootComponent(), TEXT("CaptureRoot"));
	SetRootComponent(SceneComponent);
	
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(SceneComponent, TEXT("Box"));
	Box->SetBoxExtent(FVector(50,50,50));
	VisualizeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualizationMesh"));
	VisualizeMesh->BoundsScale = 100;
	VisualizeMesh->SetupAttachment(SceneComponent, TEXT("VisualizationMesh"));
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


void ACSShallowWaterCapture::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ConstructionComponent();
}


void ACSShallowWaterCapture::ConstructionComponent()
{
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
	TextureSize = CaptureSize / WorldPixelSize;
	VisualizeMesh->SetRelativeScale3D(FVector::OneVector * CaptureSize / 100);
	VisualizeMesh->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

	SetActorScale3D(FVector::OneVector);

	// CheckAndCreateTexture_SWSourcePoint();

}

void ACSShallowWaterCapture::ShallowWaterSolverSoucePoint(int32 InIteration)
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);

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
 
	// CleanupAttachedActors();
	
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

	if (bAutoCapture)
	{
		CaptureSceneDepthNow();
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
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalVelocityHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_VelocityHeightSim);
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

			FRDGTextureUAVRef VelUAVs[2] = { RDGUAV_VelHeightSimA, RDGUAV_VelHeightSimB };
			FRDGTextureUAVRef SmoothUAVs[2] = { RDGUAV_SmoothHeightA, RDGUAV_SmoothHeightB };
			int32 ReadIdx = 0;
			int32 WriteIdx = 1;
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

#if PLATFORM_WINDOWS
		if (bDoCapture && GRenderDocAPI) GRenderDocAPI->EndFrameCapture(nullptr, nullptr);
#endif
	});
}

void ACSShallowWaterCapture::ShallowWaterSolverSplineRange(UTextureRenderTarget2D* RT_SplineScaleDist, UTextureRenderTarget2D* RT_CopyLandscape, FVector SourceLocation, FVector2f ValidUV, int32 TarIteration, float SourceSize)
{
	// if (!CheckAndCreateTexture_SWSourcePoint()) return;
	// SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);
	// if (RT_SplineScaleDist == nullptr || RT_CopyLandscape == nullptr) return;
	//
	// FBoxSphereBounds Bounds = Box->Bounds;
	// FVector RelativeSourceLocation = SourceLocation - (Bounds.Origin - Bounds.BoxExtent);
	// FVector SourceUV = RelativeSourceLocation / ( Bounds.BoxExtent * 2 );
	// SourceUV.Z = SourceLocation.Z;
	// FVector4f SourceUVRad = FVector4f( SourceUV.X, SourceUV.Y, SourceUV.Z, SourceSize);
	// const float ActorLocationZ = GetActorLocation().Z;
	//
	// RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	// RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	// RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	// RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);
	// RT_ResultDepthWet->ResizeTarget(TextureSize, TextureSize);
	// RT_SmoothHeight->ResizeTarget(TextureSize, TextureSize);
	// // RT_SplineScaleDist->ResizeTarget(TextureSize, TextureSize);
	//
	// FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_SplineScaleDist = RT_SplineScaleDist->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_CopyLandscape = RT_CopyLandscape->GameThread_GetRenderTargetResource();
	//
	// TarIteration = FMath::Max(TarIteration, 1);
	// ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	// [=, this ](FRHICommandListImmediate& RHICmdList)
	// {
	// 	FRDGBuilder GraphBuilder(RHICmdList);
	// 	{
	// 		float SizeX = R_SceneDepth->GetSizeXY().X;
	// 		float SizeY = R_SceneDepth->GetSizeXY().Y;
	// 		FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
	// 		
	// 		TShaderMapRef<FShallowWaterSim> ComputeShader_CompactActiveTiles = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_CompactActiveTiles, true);
	// 		TShaderMapRef<FShallowWaterSim> ComputeShader_FinalizeCompact = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_FinalizeCompact, true);
	// 		TShaderMapRef<FShallowWaterSim> ComputeShader_CalVelocityHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_VelocityHeightSim);
	// 		TShaderMapRef<FShallowWaterSim> ComputeShader_CalShallowIntegrate = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_ShallowIntegrate, true);
	// 		TShaderMapRef<FShallowWaterSim> ComputeShader_CalResult = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_Result);
	// 		
	// 		FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
	// 		FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
	// 		const FIntVector SingleGroupCount(1, 1, 1);
	// 		
	// 		
	// 		FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("UAV_DebugView")); 
	// 		FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
	// 		FRDGTextureRef TmpRDG_Result = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));
	// 		FRDGTextureUAVRef RDGUAV_Result = GraphBuilder.CreateUAV(TmpRDG_Result);
	// 		FRDGTextureRef TmpRDG_ResultDepthWet = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_ResultDepthWet"));
	// 		FRDGTextureUAVRef RDGUAV_ResultDepthWet = GraphBuilder.CreateUAV(TmpRDG_ResultDepthWet);
	// 		FRDGTextureRef TmpRDG_ResultSmoothHeight = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_ResultSmoothHeight"));
	// 		FRDGTextureUAVRef RDGUAV_ResultSmoothHeight = GraphBuilder.CreateUAV(TmpRDG_ResultSmoothHeight);
	// 		
	// 		FRDGTextureRef RDG_VelHeightSimA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Sim_A"));
	// 		FRDGTextureUAVRef RDGUAV_VelHeightSimA = GraphBuilder.CreateUAV(RDG_VelHeightSimA);
	// 		FRDGTextureRef RDG_VelHeightSimB = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Sim_B"));
	// 		FRDGTextureUAVRef RDGUAV_VelHeightSimB = GraphBuilder.CreateUAV(RDG_VelHeightSimB);
	// 		FRDGTextureRef RDG_SmoothHeightA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightA"));
	// 		FRDGTextureUAVRef RDGUAV_SmoothHeightA = GraphBuilder.CreateUAV(RDG_SmoothHeightA);
	// 		FRDGTextureRef RDG_SmoothHeightB = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightB"));
	// 		FRDGTextureUAVRef RDGUAV_SmoothHeightB = GraphBuilder.CreateUAV(RDG_SmoothHeightB);
	// 		
	// 		FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("SceneDepth_RT"));
	// 		FRDGTextureRef RDG_CopyLandscape = RegisterExternalTexture(GraphBuilder, R_CopyLandscape->GetRenderTargetTexture(), TEXT("CopyLandscape_RT"));
	// 		FRDGTextureRef RDG_VelocityHeight = RegisterExternalTexture(GraphBuilder, R_VelocityHeight->GetRenderTargetTexture(), TEXT("VelocityHeight_RT"));
	// 		FRDGTextureRef RDG_SplineScaleDist = RegisterExternalTexture(GraphBuilder, R_SplineScaleDist->GetRenderTargetTexture(), TEXT("SplineScaleDist_RT"));
	// 		FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
	// 		FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_ResultVelHeight->GetRenderTargetTexture(), TEXT("Result_RT"));
	// 		FRDGTextureRef RDG_ResultDepthWet = RegisterExternalTexture(GraphBuilder, R_ResultDepthWet->GetRenderTargetTexture(), TEXT("ResultDepthWet_RT"));
	// 		FRDGTextureRef RDG_ResultSmoothHeight = RegisterExternalTexture(GraphBuilder, R_ResultSmoothHeight->GetRenderTargetTexture(), TEXT("ResultSmoothHeight_RT"));
	//
	// 		FCompactTileBuffers CompactBuffers = CreateCompactTileBuffers(
	// 			GraphBuilder, (uint32)SizeX, (uint32)SizeY,
	// 			NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);
	//
	// 		PassParameters->T_SceneDepth = RDG_SceneDepth;
	// 		PassParameters->T_CopyLandscape = RDG_CopyLandscape;
	// 		PassParameters->T_VelocityHeight = RDG_VelocityHeight;
	// 		PassParameters->T_SplineScaleDist = RDG_SplineScaleDist;
	// 		PassParameters->T_ResultDepthWet = RDG_ResultDepthWet;
	// 		PassParameters->T_ResultSmoothHeight = RDG_ResultSmoothHeight;
	// 		PassParameters->DT = DT;
	// 		PassParameters->Friction = Friction;
	// 		PassParameters->CopyValidUV = ValidUV;
	// 		PassParameters->SeaLevel = SeaLevel;
	// 		PassParameters->ActorLocationZ = ActorLocationZ;
	// 		PassParameters->AdvectFoam = AdvectFoam;
	// 		PassParameters->FoamFadeSpeed = FoamFadeSpeed;
	// 		PassParameters->CloseBound = CloseBound;
	// 		PassParameters->BCount_SourceUVRads = 0;
	// 		PassParameters->SourceUVRad = SourceUVRad;
	// 		PassParameters->RW_ResultVelHeight = RDGUAV_Result;
	// 		PassParameters->RW_ResultDepthWet = RDGUAV_ResultDepthWet;
	// 		PassParameters->RW_ResultSmoothHeight = RDGUAV_ResultSmoothHeight;
	// 		PassParameters->RW_DebugView = RDGUAV_DebugView;
	// 		PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimA;
	// 		PassParameters->RW_VelHeightSimB = RDGUAV_VelHeightSimB;
	// 		PassParameters->RW_SmoothHeightA = RDGUAV_SmoothHeightA;
	// 		PassParameters->RW_SmoothHeightB = RDGUAV_SmoothHeightB;
	// 		PassParameters->RWB_SourceUVRads = nullptr;
	// 		PassParameters->DispatchExpandPixels = ComputeDispatchExpandPixels(TarIteration, TextureSize.X);
	// 		BindCompactTileBuffers(PassParameters, CompactBuffers);
	// 		PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
	//
	// 		AddCopyTexturePass(GraphBuilder, RDG_VelocityHeight, RDG_VelHeightSimA, FRHICopyTextureInfo());
	// 		AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, RDG_SmoothHeightA, FRHICopyTextureInfo());
	//
	// 		// --- Compact active tiles ---
	// 		ResetCompactCounter(GraphBuilder, CompactBuffers);
	// 		const FIntVector FullTileGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
	// 		{
	// 			FShallowWaterSim::FParameters* CompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
	// 			*CompactPassParameters = *PassParameters;
	// 			CompactPassParameters->B_CompactTileCoords = nullptr;
	// 			CompactPassParameters->B_CompactCounter = nullptr;
	// 			CompactPassParameters->CompactIndirectArgs = nullptr;
	// 			CompactPassParameters->RWB_CompactIndirectArgs = nullptr;
	// 			GraphBuilder.AddPass(
	// 				RDG_EVENT_NAME("CompactActiveTiles"),
	// 				CompactPassParameters,
	// 				ERDGPassFlags::AsyncCompute,
	// 				[CompactPassParameters, ComputeShader_CompactActiveTiles, FullTileGroupCount](FRHIComputeCommandList& RHICmdList)
	// 				{
	// 					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_CompactActiveTiles, *CompactPassParameters, FullTileGroupCount);
	// 				});
	// 		}
	// 		{
	// 			FShallowWaterSim::FParameters* FinalizeCompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
	// 			*FinalizeCompactPassParameters = *PassParameters;
	// 			NullifyAllCompactTileBindings(FinalizeCompactPassParameters);
	// 			FinalizeCompactPassParameters->RWB_CompactCounter = CompactBuffers.CounterUAV;
	// 			FinalizeCompactPassParameters->RWB_CompactIndirectArgs = CompactBuffers.IndirectArgsUAV;
	// 			GraphBuilder.AddPass(
	// 				RDG_EVENT_NAME("FinalizeCompact"),
	// 				FinalizeCompactPassParameters,
	// 				ERDGPassFlags::AsyncCompute,
	// 				[FinalizeCompactPassParameters, ComputeShader_FinalizeCompact, SingleGroupCount](FRHIComputeCommandList& RHICmdList)
	// 				{
	// 					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_FinalizeCompact, *FinalizeCompactPassParameters, SingleGroupCount);
	// 				});
	// 		}
	//
	// 		FRDGTextureRef CurrentVelHeightTextureA = RDG_VelHeightSimA;
	// 		FRDGTextureRef CurrentVelHeightTextureB = RDG_VelHeightSimB;
	// 		FRDGTextureRef CurrentSmoothHeightTextureA = RDG_SmoothHeightA;
	// 		FRDGTextureRef CurrentSmoothHeightTextureB = RDG_SmoothHeightB;
	// 		FRDGTextureUAVRef CurrentVelHeightSimA = RDGUAV_VelHeightSimA;
	// 		FRDGTextureUAVRef CurrentVelHeightSimB = RDGUAV_VelHeightSimB;
	// 		FRDGTextureUAVRef CurrentSmoothHeightA = RDGUAV_SmoothHeightA;
	// 		FRDGTextureUAVRef CurrentSmoothHeightB = RDGUAV_SmoothHeightB;
	// 		for (int32 i = 0 ; i < TarIteration; i++)
	// 		{
	// 			FShallowWaterSim::FParameters* VelocityHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
	// 			*VelocityHeightPassParameters = *PassParameters;
	// 			VelocityHeightPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
	// 			VelocityHeightPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
	// 			VelocityHeightPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
	// 			VelocityHeightPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
	// 			NullifyCompactTileUAVs(VelocityHeightPassParameters);
	// 			GraphBuilder.AddPass(
	// 				RDG_EVENT_NAME("CalVelocityHeight"),
	// 				VelocityHeightPassParameters,
	// 				ERDGPassFlags::AsyncCompute,
	// 				[VelocityHeightPassParameters, ComputeShader_CalVelocityHeight](FRHIComputeCommandList& RHICmdList)
	// 				{
	// 					FComputeShaderUtils::DispatchIndirect(
	// 						RHICmdList,
	// 						ComputeShader_CalVelocityHeight,
	// 						*VelocityHeightPassParameters,
	// 						VelocityHeightPassParameters->CompactIndirectArgs->GetIndirectRHICallBuffer(),
	// 						0);
	// 				});
	//
	// 			Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
	// 			Swap(CurrentSmoothHeightTextureA, CurrentSmoothHeightTextureB);
	// 			Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
	// 			Swap(CurrentSmoothHeightA, CurrentSmoothHeightB);
	//
	// 			FShallowWaterSim::FParameters* ShallowIntegratePassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
	// 			*ShallowIntegratePassParameters = *PassParameters;
	// 			ShallowIntegratePassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
	// 			ShallowIntegratePassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
	// 			ShallowIntegratePassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
	// 			ShallowIntegratePassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
	// 			NullifyCompactTileUAVs(ShallowIntegratePassParameters);
	// 			GraphBuilder.AddPass(
	// 				RDG_EVENT_NAME("CalShallowIntegrate"),
	// 				ShallowIntegratePassParameters,
	// 				ERDGPassFlags::AsyncCompute,
	// 				[ShallowIntegratePassParameters, ComputeShader_CalShallowIntegrate](FRHIComputeCommandList& RHICmdList)
	// 				{
	// 					FComputeShaderUtils::DispatchIndirect(
	// 						RHICmdList,
	// 						ComputeShader_CalShallowIntegrate,
	// 						*ShallowIntegratePassParameters,
	// 						ShallowIntegratePassParameters->CompactIndirectArgs->GetIndirectRHICallBuffer(),
	// 						0);
	// 				});
	//
	// 			Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
	// 			Swap(CurrentSmoothHeightTextureA, CurrentSmoothHeightTextureB);
	// 			Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
	// 			Swap(CurrentSmoothHeightA, CurrentSmoothHeightB);
	// 		}
	//
	// 		FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
	// 		*ResultPassParameters = *PassParameters;
	// 		ResultPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
	// 		ResultPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
	// 		ResultPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
	// 		ResultPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
	// 		NullifyCompactTileUAVs(ResultPassParameters);
	// 		GraphBuilder.AddPass(
	// 			RDG_EVENT_NAME("Result"),
	// 			ResultPassParameters,
	// 			ERDGPassFlags::AsyncCompute,
	// 			[ResultPassParameters, ComputeShader_CalResult](FRHIComputeCommandList& RHICmdList)
	// 			{
	// 				FComputeShaderUtils::DispatchIndirect(
	// 					RHICmdList,
	// 					ComputeShader_CalResult,
	// 					*ResultPassParameters,
	// 					ResultPassParameters->CompactIndirectArgs->GetIndirectRHICallBuffer(),
	// 					0);
	// 			});
	// 		AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
	// 		AddCopyTexturePass(GraphBuilder, TmpRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
	// 		AddCopyTexturePass(GraphBuilder, TmpRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
	// 		AddCopyTexturePass(GraphBuilder, CurrentVelHeightTextureA, RDG_VelocityHeight, FRHICopyTextureInfo());
	// 		AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
	// 		
	// 		
	// 	}
	// 	GraphBuilder.Execute();
	// });
	// FlushRenderingCommands();
}

void ACSShallowWaterCapture::ShallowWaterSolverSouceTexture()
{
	// if (!IsParameterValidMult_SourceTexture()) return;
	// RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	// RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	// RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	// RT_Result->ResizeTarget(TextureSize, TextureSize);
	// RT_Source->ResizeTarget(TextureSize, TextureSize);
	//
	// FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_ResultVelHeight = RT_Result->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_Source = RT_Source->GameThread_GetRenderTargetResource();
	//
	//
	// Iteration = FMath::Min(Iteration, 1);
	// ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	// [=, this ](FRHICommandListImmediate& RHICmdList)
	// {
	// 	FRDGBuilder GraphBuilder(RHICmdList);
	// 	{
	// 		float SizeX = R_SceneDepth->GetSizeXY().X;
	// 		float SizeY = R_SceneDepth->GetSizeXY().Y;
	// 		FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
	// 		
	// 		TShaderMapRef<FShallowWaterSim> ComputeShader_CalVelocityHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_VelocityHeightSim);
	// 		TShaderMapRef<FShallowWaterSim> ComputeShader_CalShallowIntegrate = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_ShallowIntegrate);
	// 		
	// 		FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
	// 		FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 32);
	// 		
	// 		
	// 		FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_A32B32G32R32F, TEXT("DebugView_Texture")); 
	// 		FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
	// 		
	//
	// 		FRDGTextureRef RDG_VelHeightSimA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("TmpSim_A"));
	// 		FRDGTextureUAVRef RDGUAV_VelHeightSimA = GraphBuilder.CreateUAV(RDG_VelHeightSimA);
	// 		FRDGTextureRef RDG_VelHeightSimB = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("TmpSim_B"));
	// 		FRDGTextureUAVRef RDGUAV_VelHeightSimB = GraphBuilder.CreateUAV(RDG_VelHeightSimB);
	// 		
	// 		FRDGTextureRef SceneDepthTexture = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("SceneDepth_RT"));
	// 		FRDGTextureRef RDG_VelocityHeight = RegisterExternalTexture(GraphBuilder, R_VelocityHeight->GetRenderTargetTexture(), TEXT("VelocityHeight_RT"));
	// 		FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
	// 		FRDGTextureRef RDG_Source = RegisterExternalTexture(GraphBuilder, R_Source->GetRenderTargetTexture(), TEXT("Source_RT"));
	// 		FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_ResultVelHeight->GetRenderTargetTexture(), TEXT("Result_RT"));
	// 		
	// 		PassParameters->T_SceneDepth = SceneDepthTexture;
	// 		PassParameters->T_VelocityHeight = RDG_VelocityHeight;
	// 		PassParameters->T_Source = RDG_Source;
	// 		PassParameters->DT = DT;
	// 		PassParameters->Friction = Friction;
	// 		
	// 		PassParameters->RW_DebugView = RDGUAV_DebugView;
	// 		PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimA;
	// 		PassParameters->RW_VelHeightSimB = RDGUAV_VelHeightSimB;
	// 		PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
	//
	// 		AddCopyTexturePass(GraphBuilder, RDG_VelocityHeight, RDG_VelHeightSimA, FRHICopyTextureInfo());
	// 		for (int32 i = 0 ; i < Iteration; i++)
	// 		{
	// 			GraphBuilder.AddPass(
	// 				RDG_EVENT_NAME("CalVelocityHeight"),
	// 				PassParameters,
	// 				ERDGPassFlags::AsyncCompute,
	// 				[&PassParameters, ComputeShader_CalVelocityHeight, GroupCount, i, RDGUAV_VelHeightSimA, RDGUAV_VelHeightSimB](FRHIComputeCommandList& RHICmdList)
	// 				{
	// 					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_CalVelocityHeight, *PassParameters, GroupCount);
	// 					if ( i % 2 == 0)
	// 					{
	// 						PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimB;
	// 						PassParameters->RW_VelHeightSimB = RDGUAV_VelHeightSimA;
	// 						
	// 					}
	// 					else
	// 					{
	// 						PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimA;
	// 						PassParameters->RW_VelHeightSimB = RDGUAV_VelHeightSimB;
	// 					}
	// 				});
	// 			if ( i % 2 == 0)
	// 			{
	// 				AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimB, RDG_VelocityHeight, FRHICopyTextureInfo());
	// 			}
	// 			else
	// 			{
	// 				AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimA, RDG_VelocityHeight, FRHICopyTextureInfo());
	// 			}
	// 			GraphBuilder.AddPass(
	// 				RDG_EVENT_NAME("CalShallowIntegrate"),
	// 				PassParameters,
	// 				ERDGPassFlags::AsyncCompute,
	// 				[&PassParameters, ComputeShader_CalShallowIntegrate, GroupCount, i, RDGUAV_VelHeightSimA, RDGUAV_VelHeightSimB](FRHIComputeCommandList& RHICmdList)
	// 				{
	// 					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_CalShallowIntegrate, *PassParameters, GroupCount);
	// 					if ( i % 2 == 0)
	// 					{
	// 						PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimA;
	// 						PassParameters->RW_VelHeightSimB = RDGUAV_VelHeightSimB;
	// 					}
	// 					else
	// 					{
	// 						PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimB;
	// 						PassParameters->RW_VelHeightSimB = RDGUAV_VelHeightSimA;
	// 					}
	// 				});
	// 			if ( i % 2 == 0)
	// 			{
	// 				AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimA, RDG_VelocityHeight, FRHICopyTextureInfo());
	// 			}
	// 			else
	// 			{
	// 				AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimB, RDG_VelocityHeight, FRHICopyTextureInfo());
	// 			}
	// 		}
	// 		
	// 		// if ( Iteration % 2 == 0)
	// 		// {
	// 		// 	AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimB, RDG_VelocityHeight, FRHICopyTextureInfo());
	// 		// }
	// 		// else
	// 		// {
	// 		// 	AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimA, RDG_VelocityHeight, FRHICopyTextureInfo());
	// 		// }
	// 		AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
	//
	// 		
	// 	}
	// 	GraphBuilder.Execute();
	// });
	
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
			// PassParameters->MaxCell = FVector2f(SizeX, SizeY);
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
	CaptureSceneDepth->CaptureScene();
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
	VisualizeMesh->SetVisibility(true);
	CausticsDecal->SetVisibility(true);
}

// void ACSShallowWaterCapture::Initialize_Implementation()
// {
//
// }

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
	
}

ACSSHallowWaterSource::ACSSHallowWaterSource()
{
	
}

ACSSHallowWaterContainer::ACSSHallowWaterContainer()
{
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneComponent);

	VisualizeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualizationMesh"));
	VisualizeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VisualizeMesh->SetupAttachment(SceneComponent, TEXT("VisualizationMesh"));
	CausticsDecal =CreateDefaultSubobject<UDecalComponent>(TEXT("CausticsDecal"));
	CausticsDecal->DecalSize = FVector(500, 50, 50);
	CausticsDecal->SetupAttachment(SceneComponent, TEXT("CausticsDecal"));

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

