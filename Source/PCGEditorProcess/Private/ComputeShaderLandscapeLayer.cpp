#include "ComputeShaderLandscapeLayer.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderMeshGenerator.h"
#include "LandscapeExtra.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeComponent.h"
#include "LandscapeEditLayerMergeRenderContext.h"
#include "LandscapeEditResourcesSubsystem.h"
#include "LandscapeUtils.h"
#include "TextureResource.h"
#include "EngineUtils.h"

DECLARE_CYCLE_STAT(TEXT("CSLandscapeLayer Execute"), STAT_CSLayer_Execute, STATGROUP_CSTest)

class FCSLandscapeLayer : public FGlobalShader
{
public:
	enum class ELandscapeLayerFunction : uint8
	{
		L_GenerateLayerAlpha,
		L_GenerateLayerHeight,
		L_BlendAlpha,
		L_BlendMaterialDrive,
		MAX
	};
	class FLandscapeLayerFunction : SHADER_PERMUTATION_ENUM_CLASS("LayerFunction", ELandscapeLayerFunction);
	using FPermutationDomain = TShaderPermutationDomain<FLandscapeLayerFunction>;

	static TShaderMapRef<FCSLandscapeLayer> CreatePermutation(ELandscapeLayerFunction Permutation)
	{
		typename FCSLandscapeLayer::FPermutationDomain PermutationVector;
		PermutationVector.Set<FCSLandscapeLayer::FLandscapeLayerFunction>(Permutation);
		TShaderMapRef<FCSLandscapeLayer> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}

	DECLARE_GLOBAL_SHADER(FCSLandscapeLayer);
	SHADER_USE_PARAMETER_STRUCT(FCSLandscapeLayer, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_OrigLandscapeData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_MaterialBlendTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_AlphaTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerAlpha)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerGeneratedHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerBlendResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_BlendResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER(FVector4f, ValidUVRange)
		SHADER_PARAMETER(float, GlobalAlpha)
		SHADER_PARAMETER(float, HeightModStrength)
		SHADER_PARAMETER(float, NormalStrength)
		SHADER_PARAMETER(float, FalloffWidth)
		SHADER_PARAMETER(float, NoiseFrequency)
		SHADER_PARAMETER(float, NoiseAmplitude)
		SHADER_PARAMETER(float, ErosionStrength)
		SHADER_PARAMETER(FVector3f, BoxOrigin)
		SHADER_PARAMETER(FVector3f, BoxExtent)
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

		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("L_LAYERGENERATEALPHA"),
			TEXT("L_LAYERGENERATEHEIGHT"),
			TEXT("L_LAYERBLENDALPHA"),
			TEXT("L_LAYERBLENDMATERIALDRIVE"),
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)ELandscapeLayerFunction::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FLandscapeLayerFunction>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSLandscapeLayer, "/Plugin/PCGPlugins/Shaders/Private/LandscapeLayer.usf", "CSLandscapeLayerFunction", SF_Compute);

using namespace CSHepler;

ACSLandscapeLayer::ACSLandscapeLayer()
{
	// Reuse the inherited AComputeShaderMeshGenerator components instead of creating duplicates.
	SceneComponent = SceneRoot;
	Box = GeneratorBounds;

	Box->SetBoxExtent(FVector(50, 50, 50));
	Box->bEditableWhenInherited = true;
}

void ACSLandscapeLayer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Box->SetRelativeScale3D(FVector(100, 100, 100));
	Box->SetRelativeLocation(FVector(0, 0, 50));
	Box->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
}

void ACSLandscapeLayer::InitRT()
{
	if (RT_OrigLandscapeData == nullptr)
		RT_OrigLandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_LayerAlpha == nullptr)
		RT_LayerAlpha = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_LayerGeneratedHeight == nullptr)
		RT_LayerGeneratedHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_LayerBlendResult == nullptr)
		RT_LayerBlendResult = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);
	if (RT_DebugView == nullptr)
		RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);
}

void ACSLandscapeLayer::ReadLandscapeDataToTexture()
{
	if (!RT_LayerBlendResult || !RT_OrigLandscapeData) return;

	ALandscape* Landscape = FindLandscape();
	if (!Landscape || !Box) return;

	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;

	if (!AComputeShaderMeshGenerator::RenderLandscapeToNormalHeightRT(
		Landscape, Center, Extent, RT_OrigLandscapeData))
	{
		return;
	}

	int32 TexW = RT_OrigLandscapeData->SizeX;
	int32 TexH = RT_OrigLandscapeData->SizeY;
	RT_LayerBlendResult->ResizeTarget(TexW, TexH);
	RT_LayerAlpha->ResizeTarget(TexW, TexH);
	RT_LayerGeneratedHeight->ResizeTarget(TexW, TexH);
	RT_DebugView->ResizeTarget(TexW, TexH);

	LandscapeTexMinUV = FVector::ZeroVector;
	LandscapeTexUVRange = FVector(1, 1, 0);
	MapMin = Center - Extent;
	MapMax = Center + Extent;
}

void ACSLandscapeLayer::GenerateLayerAlphaAndHeight()
{
	if (!RT_LayerAlpha || !RT_LayerGeneratedHeight || !RT_OrigLandscapeData) return;

	FTextureRenderTargetResource* R_Alpha = RT_LayerAlpha->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Height = RT_LayerGeneratedHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_OrigData = RT_OrigLandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();

	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	const FVector CapturedLandscapeTexMinUV = LandscapeTexMinUV;
	const FVector CapturedLandscapeTexUVRange = LandscapeTexUVRange;
	const float CapturedGlobalAlpha = GlobalAlpha;
	const float CapturedHeightModStrength = HeightModStrength;
	const float CapturedNormalStrength = NormalStrength;
	const float CapturedFalloffWidth = FalloffWidth;
	const float CapturedNoiseFrequency = NoiseFrequency;
	const float CapturedNoiseAmplitude = NoiseAmplitude;

	ENQUEUE_RENDER_COMMAND(GenerateLayerAlphaHeight)(
	[R_Alpha, R_Height, R_OrigData, R_Debug, Center, Extent, CapturedLandscapeTexMinUV, CapturedLandscapeTexUVRange,
	 CapturedGlobalAlpha, CapturedHeightModStrength, CapturedNormalStrength, CapturedFalloffWidth,
	 CapturedNoiseFrequency, CapturedNoiseAmplitude](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize = R_Alpha->GetSizeXY();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureSize.X, TextureSize.Y, 1), 32);

			FRDGTextureRef TmpRDG_Alpha = nullptr;
			FRDGTextureUAVRef RDGUAV_Alpha = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Alpha, RDGUAV_Alpha, TextureSize, PF_A32B32G32R32F, TEXT("UAV_Alpha"));

			FRDGTextureRef TmpRDG_Height = nullptr;
			FRDGTextureUAVRef RDGUAV_Height = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Height, RDGUAV_Height, TextureSize, PF_A32B32G32R32F, TEXT("UAV_Height"));

			FRDGTextureRef TmpRDG_Debug = nullptr;
			FRDGTextureUAVRef RDGUAV_Debug = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Debug, RDGUAV_Debug, TextureSize, PF_FloatRGBA, TEXT("UAV_Debug"));

			FRDGTextureRef RDG_OrigData = RegisterExternalTexture(GraphBuilder, R_OrigData->GetRenderTargetTexture(), TEXT("R_OrigLandscapeData"));

			auto FillCommonParams = [&](FCSLandscapeLayer::FParameters* Params)
			{
				auto UVEnd = CapturedLandscapeTexMinUV + CapturedLandscapeTexUVRange;
				Params->ValidUVRange = FVector4f(
					static_cast<float>(CapturedLandscapeTexMinUV.X), static_cast<float>(CapturedLandscapeTexMinUV.Y),
					static_cast<float>(UVEnd.X), static_cast<float>(UVEnd.Y));
				Params->GlobalAlpha = CapturedGlobalAlpha;
				Params->HeightModStrength = CapturedHeightModStrength;
				Params->NormalStrength = CapturedNormalStrength;
				Params->FalloffWidth = CapturedFalloffWidth;
				Params->NoiseFrequency = CapturedNoiseFrequency;
				Params->NoiseAmplitude = CapturedNoiseAmplitude;
				Params->BoxOrigin = FVector3f(Center);
				Params->BoxExtent = FVector3f(Extent);
				Params->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
			};

			{
				FCSLandscapeLayer::FParameters* AlphaParams = GraphBuilder.AllocParameters<FCSLandscapeLayer::FParameters>();
				FillCommonParams(AlphaParams);
				AlphaParams->RW_LayerAlpha = RDGUAV_Alpha;

				TShaderMapRef<FCSLandscapeLayer> CS_GenAlpha(FCSLandscapeLayer::CreatePermutation(FCSLandscapeLayer::ELandscapeLayerFunction::L_GenerateLayerAlpha));
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GenerateLayerAlpha"),
					AlphaParams,
					ERDGPassFlags::AsyncCompute,
					[AlphaParams, CS_GenAlpha, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, CS_GenAlpha, *AlphaParams, GroupCount);
					});
				AddCopyTexturePass(GraphBuilder, TmpRDG_Alpha, RegisterExternalTexture(GraphBuilder, R_Alpha->GetRenderTargetTexture(), TEXT("R_Alpha")), FRHICopyTextureInfo());
			}

			{
				FCSLandscapeLayer::FParameters* HeightParams = GraphBuilder.AllocParameters<FCSLandscapeLayer::FParameters>();
				FillCommonParams(HeightParams);
				HeightParams->T_OrigLandscapeData = RDG_OrigData;
				HeightParams->RW_LayerGeneratedHeight = RDGUAV_Height;
				HeightParams->RW_DebugView = RDGUAV_Debug;

				TShaderMapRef<FCSLandscapeLayer> CS_GenHeight(FCSLandscapeLayer::CreatePermutation(FCSLandscapeLayer::ELandscapeLayerFunction::L_GenerateLayerHeight));
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GenerateLayerHeight"),
					HeightParams,
					ERDGPassFlags::AsyncCompute,
					[HeightParams, CS_GenHeight, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, CS_GenHeight, *HeightParams, GroupCount);
					});
				AddCopyTexturePass(GraphBuilder, TmpRDG_Height, RegisterExternalTexture(GraphBuilder, R_Height->GetRenderTargetTexture(), TEXT("R_Height")), FRHICopyTextureInfo());
				AddCopyTexturePass(GraphBuilder, TmpRDG_Debug, RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_Debug")), FRHICopyTextureInfo());
			}
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
}

void ACSLandscapeLayer::BlendLayerWithAlpha()
{
	if (!RT_LayerBlendResult || !RT_OrigLandscapeData || !RT_LayerGeneratedHeight) return;

	FTextureRenderTargetResource* R_BlendResult = RT_LayerBlendResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_OrigData = RT_OrigLandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Height = RT_LayerGeneratedHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();

	const FVector CapturedLandscapeTexMinUV = LandscapeTexMinUV;
	const FVector CapturedLandscapeTexUVRange = LandscapeTexUVRange;
	const float CapturedGlobalAlpha = GlobalAlpha;
	const float CapturedHeightModStrength = HeightModStrength;
	const float CapturedNormalStrength = NormalStrength;
	const ECSLandscapeBlendMode CapturedBlendMode = BlendMode;
	FTextureRHIRef CapturedMaterialBlendRHI = nullptr;
	if (MaterialBlendTexture)
	{
		if (FTextureResource* MatRes = MaterialBlendTexture->GetResource())
			CapturedMaterialBlendRHI = MatRes->GetTextureRHI();
	}

	ENQUEUE_RENDER_COMMAND(BlendLayerAlpha)(
	[R_BlendResult, R_OrigData, R_Height, R_Debug, CapturedLandscapeTexMinUV, CapturedLandscapeTexUVRange,
	 CapturedGlobalAlpha, CapturedHeightModStrength, CapturedNormalStrength, CapturedBlendMode,
	 CapturedMaterialBlendRHI](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize = R_BlendResult->GetSizeXY();

			FCSLandscapeLayer::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSLandscapeLayer::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureSize.X, TextureSize.Y, 1), 32);

			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));

			FRDGTextureRef TmpRDG_Debug = nullptr;
			FRDGTextureUAVRef RDGUAV_Debug = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Debug, RDGUAV_Debug, TextureSize, PF_FloatRGBA, TEXT("UAV_Debug"));

			FRDGTextureRef RDG_OrigData = RegisterExternalTexture(GraphBuilder, R_OrigData->GetRenderTargetTexture(), TEXT("R_OrigLandscapeData"));
			FRDGTextureRef RDG_Height = RegisterExternalTexture(GraphBuilder, R_Height->GetRenderTargetTexture(), TEXT("R_Height"));

			PassParameters->T_OrigLandscapeData = RDG_OrigData;
			PassParameters->T_AlphaTexture = RDG_Height;
			PassParameters->RW_LayerBlendResult = RDGUAV_Result;
			PassParameters->RW_DebugView = RDGUAV_Debug;
			{
				auto UVEnd = CapturedLandscapeTexMinUV + CapturedLandscapeTexUVRange;
				PassParameters->ValidUVRange = FVector4f(
					static_cast<float>(CapturedLandscapeTexMinUV.X), static_cast<float>(CapturedLandscapeTexMinUV.Y),
					static_cast<float>(UVEnd.X), static_cast<float>(UVEnd.Y));
			}
			PassParameters->GlobalAlpha = CapturedGlobalAlpha;
			PassParameters->HeightModStrength = CapturedHeightModStrength;
			PassParameters->NormalStrength = CapturedNormalStrength;
			PassParameters->FalloffWidth = 500.0f;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			if (CapturedBlendMode == ECSLandscapeBlendMode::MaterialDrive && CapturedMaterialBlendRHI.IsValid())
			{
				PassParameters->T_MaterialBlendTexture = RegisterExternalTexture(GraphBuilder,
					CapturedMaterialBlendRHI, TEXT("R_MaterialBlend"), ERDGTextureFlags::None);
			}

			TShaderMapRef<FCSLandscapeLayer> ComputeShader_Blend = FCSLandscapeLayer::CreatePermutation(
				CapturedBlendMode == ECSLandscapeBlendMode::MaterialDrive
					? FCSLandscapeLayer::ELandscapeLayerFunction::L_BlendMaterialDrive
					: FCSLandscapeLayer::ELandscapeLayerFunction::L_BlendAlpha);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("BlendLayerWithAlpha"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_Blend, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Blend, *PassParameters, GroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RegisterExternalTexture(GraphBuilder, R_BlendResult->GetRenderTargetTexture(), TEXT("R_BlendResult")), FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_Debug, RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_Debug")), FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
}

void ACSLandscapeLayer::CreateLandscapeLayer()
{
	EnsureEditLayer();
}

void ACSLandscapeLayer::RemoveLandscapeLayer()
{
	RemoveEditLayer();
}

void ACSLandscapeLayer::ApplyBlendedLayerToLandscape()
{
	if (RT_LayerBlendResult == nullptr) return;
	bHasResult = true;
	RequestLandscapeUpdate(true);
}

void ACSLandscapeLayer::CommitToLandscape()
{
#if WITH_EDITOR
	if (RT_LayerBlendResult == nullptr || !bHasResult) return;

	FCSReadLandscapeData LandscapeData;
	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	ULandscapeExtra::CreateLandscapeTextureData(LandscapeData, Center, Extent);

	BakeResultToLandscape(RT_LayerBlendResult, LandscapeData, /*bClearLayerFirst*/ true, /*bWriteAlphaBlendAndFlags*/ true);

	bHasResult = false;
#endif
}

#if WITH_EDITOR

bool ACSLandscapeLayer::RenderLayer(
	UE::Landscape::EditLayers::FRenderParams& RenderParams,
	UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder)
{
	if (!bHasResult || !RT_LayerBlendResult) return false;
	if (!RenderParams.MergeRenderContext->IsHeightmapMerge()) return false;

	// [Step 1 - stop the crash] The previous implementation blitted RT_LayerBlendResult into the
	// heightmap blend render target with ULandscapeScratchRenderTarget::CopyFrom(). That is a raw
	// CopyTextureRegion with NO format/encoding conversion, so copying RGBA16f (RT_LayerBlendResult)
	// into the landscape's packed BGRA8 heightmap target is format-incompatible and crashes the RHI
	// thread (D3D12 E_INVALIDARG at CopyTextureRegion, confirmed via -d3ddebug).
	// TODO [Step 2 - correct write]: replace this with a shader pass that samples RT_LayerBlendResult,
	// encodes world-space height the landscape way (LandscapeDataAccess::GetTexHeight) and writes it
	// packed (LandscapeCommon.ush PackHeight -> RG) into WriteRT at the batch's DestPosition sub-rect.
	// Until then this layer simply contributes nothing to the live merge; CommitToLandscape() still
	// bakes the result into the landscape via the CPU SetHeightData path.
	return false;
}

void ACSLandscapeLayer::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (bHasResult)
	{
		RequestLandscapeUpdate(true);
	}
}
#endif

void ACSLandscapeLayer::FullPipeline()
{
	InitRT();
	ReadLandscapeDataToTexture();
	GenerateLayerAlphaAndHeight();
	BlendLayerWithAlpha();
	ApplyBlendedLayerToLandscape();
}

UTextureRenderTarget2D* ACSLandscapeLayer::CreateBlankAlphaTexture(UObject* WorldContextObject, int32 SizeX, int32 SizeY, FLinearColor ClearColor)
{
	return UKismetRenderingLibrary::CreateRenderTarget2D(
		WorldContextObject, SizeX, SizeY,
		ETextureRenderTargetFormat::RTF_RGBA8,
		ClearColor, true, false);
}
