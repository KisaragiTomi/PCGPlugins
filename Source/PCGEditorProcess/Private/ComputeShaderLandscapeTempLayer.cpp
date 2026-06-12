#include "ComputeShaderLandscapeTempLayer.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderBasicFunction.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Landscape.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditLayerMergeRenderContext.h"
#include "LandscapeEditResourcesSubsystem.h"
#include "LandscapeUtils.h"
#endif
#include "LandscapeComponent.h"
#include "LandscapeExtra.h"

DECLARE_CYCLE_STAT(TEXT("CSTempLayer Execute"), STAT_CSTempLayer_Execute, STATGROUP_CSTest)

class FCSTempLandscapeLayer : public FGlobalShader
{
public:
	enum class ETempLayerFunction : uint8
	{
		TL_Blend,
		TL_BlendDebug,
		MAX
	};
	class FTempLayerFunction : SHADER_PERMUTATION_ENUM_CLASS("TempLayerFunc", ETempLayerFunction);
	using FPermutationDomain = TShaderPermutationDomain<FTempLayerFunction>;

	static TShaderMapRef<FCSTempLandscapeLayer> CreatePermutation(ETempLayerFunction Permutation)
	{
		typename FCSTempLandscapeLayer::FPermutationDomain PermutationVector;
		PermutationVector.Set<FCSTempLandscapeLayer::FTempLayerFunction>(Permutation);
		TShaderMapRef<FCSTempLandscapeLayer> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}

	DECLARE_GLOBAL_SHADER(FCSTempLandscapeLayer);
	SHADER_USE_PARAMETER_STRUCT(FCSTempLandscapeLayer, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_OrigLandscapeData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ExternalHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Result)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER(float, TempLayerAlpha)
		SHADER_PARAMETER(float, TempHeightOffset)
		SHADER_PARAMETER(float, TempNoiseFrequency)
		SHADER_PARAMETER(float, TempNoiseAmplitude)
		SHADER_PARAMETER(int32, TempNoiseOctaves)
		SHADER_PARAMETER(float, TempFalloffWidth)
		SHADER_PARAMETER(int32, TempBlendMode)
		SHADER_PARAMETER(int32, TempSourceMode)
		SHADER_PARAMETER(FVector3f, TempBoxOrigin)
		SHADER_PARAMETER(FVector3f, TempBoxExtent)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 32);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 32);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), 1);
		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);

		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("L_TEMPLAYERBLEND"),
			TEXT("L_TEMPLAYERDEBUG"),
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)ETempLayerFunction::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FTempLayerFunction>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSTempLandscapeLayer, "/Plugin/PCGPlugins/Shaders/Private/CSLandscape.usf", "CSLandscapeFunction", SF_Compute);

using namespace CSHepler;

ACSLandscapeTempLayer::ACSLandscapeTempLayer()
{
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	BrushBox = CreateDefaultSubobject<UBoxComponent>(TEXT("BrushBox"));
	BrushBox->SetupAttachment(SceneRoot);
	BrushBox->SetBoxExtent(FVector(50, 50, 50));
}

void ACSLandscapeTempLayer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BrushBox->SetRelativeScale3D(FVector(100, 100, 100));
	BrushBox->SetRelativeLocation(FVector(0, 0, 50));
}

#if WITH_EDITOR
void ACSLandscapeTempLayer::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RequestLandscapeUpdate(true);
}
#endif

void ACSLandscapeTempLayer::SetExternalHeightRT(UTextureRenderTarget2D* InRT)
{
	ExternalHeightRT = InRT;
	RequestLandscapeUpdate(true);
}

void ACSLandscapeTempLayer::RefreshLayer()
{
	RequestLandscapeUpdate(true);
}

void ACSLandscapeTempLayer::EnsureRTs(const FIntPoint& InSize)
{
	if (RT_InternalResult == nullptr || RT_InternalResult->SizeX != InSize.X || RT_InternalResult->SizeY != InSize.Y)
	{
		RT_InternalResult = UKismetRenderingLibrary::CreateRenderTarget2D(
			this, InSize.X, InSize.Y, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	}
	if (bShowDebugView && (RT_DebugView == nullptr || RT_DebugView->SizeX != InSize.X || RT_DebugView->SizeY != InSize.Y))
	{
		RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(
			this, InSize.X, InSize.Y, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	}
}

void ACSLandscapeTempLayer::RunBlendCS(
	UTextureRenderTarget2D* InCombinedResult,
	UTextureRenderTarget2D* OutResult,
	const FIntPoint& Size)
{
	FTextureRenderTargetResource* R_Combined = InCombinedResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Result = OutResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = (bShowDebugView && RT_DebugView) ? RT_DebugView->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* R_External = (SourceMode == ETempLayerSourceMode::ExternalRT && ExternalHeightRT)
		? ExternalHeightRT->GameThread_GetRenderTargetResource() : nullptr;

	const float CapturedAlpha = LayerAlpha;
	const float CapturedHeightOffset = HeightOffset;
	const float CapturedNoiseFreq = NoiseFrequency;
	const float CapturedNoiseAmp = NoiseAmplitude;
	const int32 CapturedNoiseOct = NoiseOctaves;
	const float CapturedFalloff = FalloffWidth;
	const int32 CapturedBlendMode = static_cast<int32>(BlendMode);
	const int32 CapturedSourceMode = static_cast<int32>(SourceMode);
	const FVector3f CapturedOrigin(BrushBox->Bounds.Origin);
	const FVector3f CapturedExtent(BrushBox->Bounds.BoxExtent);
	const bool CapturedShowDebug = bShowDebugView;

	ENQUEUE_RENDER_COMMAND(CSTempLayerBlend)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize(Size.X, Size.Y);
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureSize.X, TextureSize.Y, 1), 32);

			auto Func = CapturedShowDebug
				? FCSTempLandscapeLayer::ETempLayerFunction::TL_BlendDebug
				: FCSTempLandscapeLayer::ETempLayerFunction::TL_Blend;
			TShaderMapRef<FCSTempLandscapeLayer> CS = FCSTempLandscapeLayer::CreatePermutation(Func);

			FCSTempLandscapeLayer::FParameters* Params = GraphBuilder.AllocParameters<FCSTempLandscapeLayer::FParameters>();

			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_TempResult"));

			FRDGTextureRef RDG_Combined = RegisterExternalTexture(GraphBuilder, R_Combined->GetRenderTargetTexture(), TEXT("R_Combined"));
			FRDGTextureRef RDG_Output = RegisterExternalTexture(GraphBuilder, R_Result->GetRenderTargetTexture(), TEXT("R_Output"));

			Params->T_OrigLandscapeData = RDG_Combined;
			Params->RW_Result = RDGUAV_Result;
			Params->TempLayerAlpha = CapturedAlpha;
			Params->TempHeightOffset = CapturedHeightOffset;
			Params->TempNoiseFrequency = CapturedNoiseFreq;
			Params->TempNoiseAmplitude = CapturedNoiseAmp;
			Params->TempNoiseOctaves = CapturedNoiseOct;
			Params->TempFalloffWidth = CapturedFalloff;
			Params->TempBlendMode = CapturedBlendMode;
			Params->TempSourceMode = CapturedSourceMode;
			Params->TempBoxOrigin = CapturedOrigin;
			Params->TempBoxExtent = CapturedExtent;
			Params->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			if (R_External)
			{
				FRDGTextureRef RDG_External = RegisterExternalTexture(GraphBuilder, R_External->GetRenderTargetTexture(), TEXT("R_External"));
				Params->T_ExternalHeight = RDG_External;
			}
			else
			{
				Params->T_ExternalHeight = RDG_Combined;
			}

			if (CapturedShowDebug && R_Debug)
			{
				FRDGTextureRef TmpRDG_Debug = nullptr;
				FRDGTextureUAVRef RDGUAV_Debug = nullptr;
				ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Debug, RDGUAV_Debug, TextureSize, PF_FloatRGBA, TEXT("UAV_TempDebug"));
				Params->RW_DebugView = RDGUAV_Debug;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("CSTempLayerBlendDebug"),
					Params,
					ERDGPassFlags::AsyncCompute,
					[Params, CS, GroupCount](FRHIComputeCommandList& CmdList)
					{
						FComputeShaderUtils::Dispatch(CmdList, CS, *Params, GroupCount);
					});

				FRDGTextureRef RDG_Debug = RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_Debug"));
				AddCopyTexturePass(GraphBuilder, TmpRDG_Debug, RDG_Debug, FRHICopyTextureInfo());
			}
			else
			{
				Params->RW_DebugView = nullptr;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("CSTempLayerBlend"),
					Params,
					ERDGPassFlags::AsyncCompute,
					[Params, CS, GroupCount](FRHIComputeCommandList& CmdList)
					{
						FComputeShaderUtils::Dispatch(CmdList, CS, *Params, GroupCount);
					});
			}

			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Output, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
}

#if WITH_EDITOR

bool ACSLandscapeTempLayer::RenderLayer(
	UE::Landscape::EditLayers::FRenderParams& RenderParams,
	UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder)
{
	SCOPE_CYCLE_COUNTER(STAT_CSTempLayer_Execute);

	if (!RenderParams.MergeRenderContext->IsHeightmapMerge()) return false;
	if (FMath::IsNearlyZero(LayerAlpha)) return false;

	RenderParams.MergeRenderContext->CycleBlendRenderTargets(RDGBuilderRecorder);
	ULandscapeScratchRenderTarget* WriteRT = RenderParams.MergeRenderContext->GetBlendRenderTargetWrite();
	ULandscapeScratchRenderTarget* ReadRT = RenderParams.MergeRenderContext->GetBlendRenderTargetRead();

	WriteRT->TransitionTo(ERHIAccess::RTV, RDGBuilderRecorder);
	ReadRT->TransitionTo(ERHIAccess::SRVMask, RDGBuilderRecorder);

	UTextureRenderTarget2D* ReadRT2D = ReadRT->GetRenderTarget2D();
	FIntPoint Size = RenderParams.RenderAreaSectionRect.Size();

	EnsureRTs(Size);
	RunBlendCS(ReadRT2D, RT_InternalResult, Size);

	// Copy blended result to the write scratch RT
	WriteRT->CopyFrom(
		ULandscapeScratchRenderTarget::FCopyFromScratchRenderTargetParams(ReadRT),
		RDGBuilderRecorder);

	bHasResult = true;
	return true;
}
#endif

void ACSLandscapeTempLayer::CommitToLandscape()
{
#if WITH_EDITOR
	if (!RT_InternalResult) return;

	FVector Center = BrushBox->Bounds.Origin;
	FVector Extent = BrushBox->Bounds.BoxExtent;
	FCSReadLandscapeData LandscapeData;
	ULandscapeExtra::CreateLandscapeTextureData(LandscapeData, Center, Extent);

	BakeResultToLandscape(RT_InternalResult, LandscapeData, /*bClearLayerFirst*/ false, /*bWriteAlphaBlendAndFlags*/ false);

	Destroy();
#endif
}
