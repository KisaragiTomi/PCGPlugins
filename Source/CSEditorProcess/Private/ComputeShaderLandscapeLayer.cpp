#include "ComputeShaderLandscapeLayer.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "LandscapeExtra.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeComponent.h"
#include "TextureResource.h"

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
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerAlpha)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerGeneratedHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerBlendResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER(FVector4f, ValidUVRange)
		SHADER_PARAMETER(float, GlobalAlpha)
		SHADER_PARAMETER(float, HeightModStrength)
		SHADER_PARAMETER(float, NormalStrength)
		SHADER_PARAMETER(float, FalloffWidth)
		SHADER_PARAMETER(float, NoiseFrequency)
		SHADER_PARAMETER(float, NoiseAmplitude)
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
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("CaptureRoot"));
	SetRootComponent(SceneComponent);

	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(SceneComponent, TEXT("Box"));
	Box->SetBoxExtent(FVector(50, 50, 50));

	AffectHeightmap = true;
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
	if (!RT_LayerBlendResult) return;

	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	FReadLandscapeData LandscapeData;
	ULandscapeExtra::CreateLandscapeTextureData(LandscapeData, Center, Extent);
	if (LandscapeData.TextureSize.X + LandscapeData.TextureSize.Y < 32) return;

	RT_LayerBlendResult->ResizeTarget(LandscapeData.TextureSize.X, LandscapeData.TextureSize.Y);
	RT_LayerAlpha->ResizeTarget(LandscapeData.TextureSize.X, LandscapeData.TextureSize.Y);
	RT_LayerGeneratedHeight->ResizeTarget(LandscapeData.TextureSize.X, LandscapeData.TextureSize.Y);
	RT_DebugView->ResizeTarget(LandscapeData.TextureSize.X, LandscapeData.TextureSize.Y);

	FVector TextureMin = Center - Extent;
	FVector TextureMax = Center + Extent;
	FVector Range = LandscapeData.MapMax - LandscapeData.MapMin + FVector(0, 0, 1);
	FVector MinUV = (TextureMin - LandscapeData.MapMin) / Range;
	FVector MaxUV = (TextureMax - LandscapeData.MapMin) / Range;

	LandscapeTexMinUV = MinUV;
	LandscapeTexUVRange = MaxUV - MinUV;
	MapMin = LandscapeData.MapMin;
	MapMax = LandscapeData.MapMax;
	Orig_LandscapeData = LandscapeData;
}

void ACSLandscapeLayer::GenerateLayerAlphaAndHeight()
{
	FTextureRenderTargetResource* R_Alpha = RT_LayerAlpha->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Height = RT_LayerGeneratedHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();

	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;

	ENQUEUE_RENDER_COMMAND(GenerateLayerAlphaHeight)(
	[=, this](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize = R_Alpha->GetSizeXY();

			FCSLandscapeLayer::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSLandscapeLayer::FParameters>();
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

			PassParameters->RW_LayerAlpha = RDGUAV_Alpha;
			PassParameters->RW_LayerGeneratedHeight = RDGUAV_Height;
			PassParameters->RW_DebugView = RDGUAV_Debug;
			PassParameters->ValidUVRange = FVector4f(LandscapeTexMinUV, LandscapeTexMinUV + LandscapeTexUVRange);
			PassParameters->GlobalAlpha = GlobalAlpha;
			PassParameters->HeightModStrength = HeightModStrength;
			PassParameters->NormalStrength = NormalStrength;
			PassParameters->FalloffWidth = FalloffWidth;
			PassParameters->NoiseFrequency = NoiseFrequency;
			PassParameters->NoiseAmplitude = NoiseAmplitude;
			PassParameters->BoxOrigin = FVector3f(Center);
			PassParameters->BoxExtent = FVector3f(Extent);
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			{
				TShaderMapRef<FCSLandscapeLayer> ComputeShader_GenAlpha(FCSLandscapeLayer::CreatePermutation(FCSLandscapeLayer::ELandscapeLayerFunction::L_GenerateLayerAlpha));
				PassParameters->RW_LayerAlpha = RDGUAV_Alpha;
				PassParameters->RW_LayerGeneratedHeight = nullptr;
				PassParameters->RW_DebugView = nullptr;
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GenerateLayerAlpha"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[&PassParameters, ComputeShader_GenAlpha, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_GenAlpha, *PassParameters, GroupCount);
					});
				AddCopyTexturePass(GraphBuilder, TmpRDG_Alpha, RegisterExternalTexture(GraphBuilder, R_Alpha->GetRenderTargetTexture(), TEXT("R_Alpha")), FRHICopyTextureInfo());
			}

			{
				TShaderMapRef<FCSLandscapeLayer> ComputeShader_GenHeight(FCSLandscapeLayer::CreatePermutation(FCSLandscapeLayer::ELandscapeLayerFunction::L_GenerateLayerHeight));
				PassParameters->RW_LayerAlpha = nullptr;
				PassParameters->RW_LayerGeneratedHeight = RDGUAV_Height;
				PassParameters->RW_DebugView = RDGUAV_Debug;
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("GenerateLayerHeight"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[&PassParameters, ComputeShader_GenHeight, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_GenHeight, *PassParameters, GroupCount);
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
	FTextureRenderTargetResource* R_Orig = RT_LayerBlendResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Alpha = RT_LayerAlpha->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Height = RT_LayerGeneratedHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();

	FReadLandscapeData LandscapeData;
	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	ULandscapeExtra::CreateLandscapeTextureData(LandscapeData, Center, Extent);
	if (LandscapeData.TextureSize.X + LandscapeData.TextureSize.Y < 32) return;
	Orig_LandscapeData = LandscapeData;

	ENQUEUE_RENDER_COMMAND(BlendLayerAlpha)(
	[=, this](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize = R_Orig->GetSizeXY();

			FCSLandscapeLayer::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSLandscapeLayer::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureSize.X, TextureSize.Y, 1), 32);

			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));

			FRDGTextureRef TmpRDG_Debug = nullptr;
			FRDGTextureUAVRef RDGUAV_Debug = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Debug, RDGUAV_Debug, TextureSize, PF_FloatRGBA, TEXT("UAV_Debug"));

			FRDGTextureRef RDG_Orig = RegisterExternalTexture(GraphBuilder, R_Orig->GetRenderTargetTexture(), TEXT("R_Orig"));
			FRDGTextureRef RDG_Alpha = RegisterExternalTexture(GraphBuilder, R_Alpha->GetRenderTargetTexture(), TEXT("R_Alpha"));
			FRDGTextureRef RDG_Height = RegisterExternalTexture(GraphBuilder, R_Height->GetRenderTargetTexture(), TEXT("R_Height"));

			PassParameters->T_OrigLandscapeData = RDG_Orig;
			PassParameters->T_MaterialBlendTexture = RDG_Alpha;
			PassParameters->RW_LayerBlendResult = RDGUAV_Result;
			PassParameters->RW_DebugView = RDGUAV_Debug;
			PassParameters->ValidUVRange = FVector4f(LandscapeTexMinUV, LandscapeTexMinUV + LandscapeTexUVRange);
			PassParameters->GlobalAlpha = GlobalAlpha;
			PassParameters->HeightModStrength = HeightModStrength;
			PassParameters->NormalStrength = NormalStrength;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			TShaderMapRef<FCSLandscapeLayer> ComputeShader_Blend = FCSLandscapeLayer::CreatePermutation(
				BlendMode == ELandscapeLayerBlendMode::MaterialDrive
					? FCSLandscapeLayer::ELandscapeLayerFunction::L_BlendMaterialDrive
					: FCSLandscapeLayer::ELandscapeLayerFunction::L_BlendAlpha);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("BlendLayerWithAlpha"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[&PassParameters, ComputeShader_Blend, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Blend, *PassParameters, GroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RegisterExternalTexture(GraphBuilder, R_Orig->GetRenderTargetTexture(), TEXT("R_Result")), FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_Debug, RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_Debug")), FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
}

void ACSLandscapeLayer::CreateLandscapeLayer()
{
	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
	{
		Landscape = *It;
		break;
	}
	if (!Landscape)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACSLandscapeLayer: No Landscape found in world."));
		return;
	}

	if (!LayerMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACSLandscapeLayer: LayerMaterial is not set."));
		return;
	}

	if (bLayerCreated)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACSLandscapeLayer: Layer already created. Call RemoveLandscapeLayer first."));
		return;
	}

	int32 NewLayerIndex = Landscape->GetLayerCount();
	Landscape->CreateLayer(NewLayerIndex);

	if (ULandscapeLayerInfo* LayerInfo = Landscape->GetLayerInfo(NewLayerIndex))
	{
		LayerGuid = LayerInfo->GetGuid();
		LayerIndex = NewLayerIndex;
		bLayerCreated = true;

		FName UsedLayerName = LayerName;
		if (UsedLayerName.IsNone())
		{
			UsedLayerName = FName(*FString::Printf(TEXT("PCG_TempLayer_%d"), NewLayerIndex));
		}

		LayerInfo->LayerNameObj = UsedLayerName;
		LayerInfo->bVisible = true;

		UE_LOG(LogTemp, Log, TEXT("ACSLandscapeLayer: Created layer '%s' at index %d, Guid: %s"),
			*UsedLayerName.ToString(), LayerIndex, *LayerGuid.ToString());
	}
}

void ACSLandscapeLayer::RemoveLandscapeLayer()
{
	if (!bLayerCreated) return;

	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
	{
		Landscape = *It;
		break;
	}
	if (!Landscape) return;

	Landscape->DeleteLayer(LayerIndex, nullptr);
	bLayerCreated = false;
	LayerGuid = FGuid();
	LayerIndex = -1;

	UE_LOG(LogTemp, Log, TEXT("ACSLandscapeLayer: Removed layer at index %d"), LayerIndex + 1);
}

void ACSLandscapeLayer::ApplyBlendedLayerToLandscape()
{
	if (RT_LayerBlendResult == nullptr) return;

	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
	{
		Landscape = *It;
		break;
	}
	if (!Landscape) return;

	if (!bLayerCreated)
	{
		CreateLandscapeLayer();
	}

	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);

	FReadLandscapeData LandscapeData;
	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;
	ULandscapeExtra::CreateLandscapeTextureData(LandscapeData, Center, Extent);
	if (LandscapeData.TextureSize.X + LandscapeData.TextureSize.Y < 32) return;

	int32 XNum = RT_LayerBlendResult->SizeX;
	TArray<FLinearColor> ResultColors;
	UKismetRenderingLibrary::ReadRenderTargetRaw(this, RT_LayerBlendResult, ResultColors, false);

	TArray<uint16> HeightData;
	TArray<uint16> HeightAlphaBlendData;
	TArray<uint8> HeightFlagsData;
	HeightData.Reserve(LandscapeData.TextureValidSize.X * LandscapeData.TextureValidSize.Y);
	HeightAlphaBlendData.Reserve(LandscapeData.TextureValidSize.X * LandscapeData.TextureValidSize.Y);
	HeightFlagsData.Reserve(LandscapeData.TextureValidSize.X * LandscapeData.TextureValidSize.Y);

	float LandscapeScaleZ = Landscape->GetActorScale3D().Z;

	for (int32 Y = 0; Y < LandscapeData.TextureValidSize.Y; Y++)
	{
		for (int32 X = 0; X < LandscapeData.TextureValidSize.X; X++)
		{
			float ResultHeight = ResultColors[X + Y * XNum].A / LandscapeScaleZ;
			HeightAlphaBlendData.Add(0);
			HeightFlagsData.Add(0);
			HeightData.Add(LandscapeDataAccess::GetTexHeight(ResultHeight));
		}
	}

	const FLandscapeLayer* Layer = Landscape->GetLayerConst(LayerIndex >= 0 ? LayerIndex : 0);
	FGuid TargetLayerGuid = Layer ? Layer->EditLayer->GetGuid() : FGuid();

	Landscape->ClearEditLayer(LayerIndex >= 0 ? LayerIndex : 0, nullptr, ELandscapeToolTargetTypeFlags::Heightmap);
	FScopedSetLandscapeEditingLayer Scope(Landscape, TargetLayerGuid,
		[=] { Landscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All); });

	LandscapeEdit.SetHeightData(
		LandscapeData.ReadRange.X, LandscapeData.ReadRange.Y,
		LandscapeData.ReadRange.Z, LandscapeData.ReadRange.W,
		(uint16*)HeightData.GetData(), 0, true, nullptr,
		(uint16*)HeightAlphaBlendData.GetData(), (uint8*)HeightFlagsData.GetData());
}

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
