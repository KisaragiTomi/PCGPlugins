#include "ComputeShaderLandscapeLayerRuntime.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderMeshGenerator.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#if WITH_EDITOR
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#endif
#include "LandscapeComponent.h"
#include "TextureResource.h"
#include "ComputeShaderMeshFill.h"
#include "ComputeShaderSceneCapture.h"
#include "ComputeShaderShallowWater.h"

DECLARE_CYCLE_STAT(TEXT("CSRuntimeLandscapeLayer Execute"), STAT_CSRuntimeLayer_Execute, STATGROUP_CSTest)

class FCSRuntimeLandscapeLayer : public FGlobalShader
{
public:
	enum class ERuntimeLayerFunction : uint8
	{
		RL_GenerateNoiseAlpha,
		RL_GenerateNoiseHeight,
		RL_BlendAlpha,
		RL_BlendMaterialDrive,
		RL_ThermalErosion,
		MAX
	};
	class FRuntimeLayerFunction : SHADER_PERMUTATION_ENUM_CLASS("RuntimeLayerFunction", ERuntimeLayerFunction);
	using FPermutationDomain = TShaderPermutationDomain<FRuntimeLayerFunction>;

	static TShaderMapRef<FCSRuntimeLandscapeLayer> CreatePermutation(ERuntimeLayerFunction Permutation)
	{
		typename FCSRuntimeLandscapeLayer::FPermutationDomain PermutationVector;
		PermutationVector.Set<FCSRuntimeLandscapeLayer::FRuntimeLayerFunction>(Permutation);
		TShaderMapRef<FCSRuntimeLandscapeLayer> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}

	DECLARE_GLOBAL_SHADER(FCSRuntimeLandscapeLayer);
	SHADER_USE_PARAMETER_STRUCT(FCSRuntimeLandscapeLayer, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_OrigLandscapeData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_AlphaTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_MaterialBlendTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerAlpha)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerGeneratedHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerBlendResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_BlendResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER(FVector4f, ValidUVRange)
		SHADER_PARAMETER(float, GlobalAlpha)
		SHADER_PARAMETER(float, HeightModStrength)
		SHADER_PARAMETER(float, NormalStrength)
		SHADER_PARAMETER(float, NoiseFrequency)
		SHADER_PARAMETER(float, NoiseAmplitude)
		SHADER_PARAMETER(float, FalloffWidth)
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
			TEXT("RL_GENERATENOISEALPHA"),
			TEXT("RL_GENERATENOISEHEIGHT"),
			TEXT("RL_BLENDALPHA"),
			TEXT("RL_BLENDMATERIALDRIVE"),
			TEXT("RL_THERMALEROSION"),
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)ERuntimeLayerFunction::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FRuntimeLayerFunction>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSRuntimeLandscapeLayer, "/Plugin/PCGPlugins/Shaders/Private/LandscapeLayer.usf", "CSRuntimeLandscapeLayerFunction", SF_Compute);

using namespace CSHepler;

ACSRuntimeLandscapeLayer::ACSRuntimeLandscapeLayer()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void ACSRuntimeLandscapeLayer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!bInitialized)
	{
		InitRuntimeRT();
		bInitialized = true;
	}

	FindLandscape();
}

void ACSRuntimeLandscapeLayer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

bool ACSRuntimeLandscapeLayer::FindLandscape()
{
	if (TargetLandscape)
	{
		LandscapeGuid = TargetLandscape->GetLandscapeGuid();
		return true;
	}

	for (TActorIterator<ALandscape> It(GetWorld(), ALandscape::StaticClass()); It; ++It)
	{
		TargetLandscape = *It;
		LandscapeGuid = TargetLandscape->GetLandscapeGuid();
		return true;
	}

	return false;
}

ALandscape* ACSRuntimeLandscapeLayer::FindOrCreateLandscape()
{
	FindLandscape();
	return TargetLandscape;
}

void ACSRuntimeLandscapeLayer::InitRuntimeRT()
{
	if (RT_OrigLandscapeData == nullptr)
		RT_OrigLandscapeData = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_LayerAlphaHeight == nullptr)
		RT_LayerAlphaHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
	if (RT_BlendResult == nullptr)
		RT_BlendResult = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);
	if (RT_DebugView == nullptr)
		RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, 0, 0), true, false);
}

void ACSRuntimeLandscapeLayer::ReadLandscapeData()
{
	if (!FindLandscape() || !Box) return;

	FVector Center = Box->Bounds.Origin;
	FVector Extent = Box->Bounds.BoxExtent;

	if (!AComputeShaderMeshGenerator::RenderLandscapeToNormalHeightRT(
		TargetLandscape, Center, Extent, RT_OrigLandscapeData))
	{
		return;
	}

	const int32 TexW = RT_OrigLandscapeData->SizeX;
	const int32 TexH = RT_OrigLandscapeData->SizeY;
	RT_LayerAlphaHeight->ResizeTarget(TexW, TexH);
	RT_BlendResult->ResizeTarget(TexW, TexH);
	RT_DebugView->ResizeTarget(TexW, TexH);

	LandscapeTexMinUV = FVector::ZeroVector;
	LandscapeTexUVRange = FVector(1, 1, 0);
	MapMin = Center - Extent;
	MapMax = Center + Extent;

	Orig_LandscapeData.MapMin = MapMin;
	Orig_LandscapeData.MapMax = MapMax;
	Orig_LandscapeData.ValidUVRange = FVector2f(1.0f, 1.0f);
	Orig_LandscapeData.TextureSize = FIntVector2(TexW, TexH);
	Orig_LandscapeData.TextureValidSize = FIntVector2(TexW, TexH);
	Orig_LandscapeData.Transform = TargetLandscape->GetTransform();

	UComputeShaderBasicFunction::CopyTexture(RT_OrigLandscapeData, RT_LayerAlphaHeight);
}

void ACSRuntimeLandscapeLayer::ReadRuntimeLandscapeData(FCSReadLandscapeData& LandscapeData, FVector Center, FVector Extent, int32 ExtentPlus)
{
#if WITH_EDITOR
	ALandscape* Landscape = TargetLandscape;
	if (!Landscape) return;
	if (Extent.Length() < 0.001f) return;

	const ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	if (!LandscapeInfo) return;

	const FVector Min = Center - Extent;
	const FVector Max = Center + Extent;
	const FVector MaxLocalPoint = Landscape->GetTransform().InverseTransformPosition(Max);
	const FVector MinLocalPoint = Landscape->GetTransform().InverseTransformPosition(Min);
	const FIntPoint KeyMax(FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus, FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus);
	const FIntPoint KeyMin(FMath::FloorToInt(MinLocalPoint.X) - ExtentPlus, FMath::FloorToInt(MinLocalPoint.Y) - ExtentPlus);

	const int32 XNum = KeyMax.X - KeyMin.X;
	const int32 YNum = KeyMax.Y - KeyMin.Y;
	if (XNum < 2 || YNum < 2) return;

	LandscapeData.TextureSize = FIntVector2(RT_BlendResult ? RT_BlendResult->SizeX : XNum, RT_BlendResult ? RT_BlendResult->SizeY : YNum);
	LandscapeData.TextureValidSize = FIntVector2(XNum, YNum);
	LandscapeData.ReadRange = FIntVector4(KeyMin.X, KeyMin.Y, KeyMax.X - 1, KeyMax.Y - 1);
	LandscapeData.Transform = Landscape->GetTransform();
	LandscapeData.MapMin = Min;
	LandscapeData.MapMax = Max;
#else
	UE_LOG(LogTemp, Warning, TEXT("ReadRuntimeLandscapeData: Landscape edit interface not available in packaged builds"));
#endif
}

void ACSRuntimeLandscapeLayer::GenerateLayerData()
{
	if (!RT_LayerAlphaHeight || !RT_BlendResult || !RT_OrigLandscapeData) return;

	FTextureRenderTargetResource* R_AlphaHeight = RT_LayerAlphaHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_OrigData = RT_OrigLandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();

	FVector Center = Box ? Box->Bounds.Origin : GetActorLocation();
	FVector Extent = Box ? Box->Bounds.BoxExtent : FVector(5000, 5000, 500);
	const FVector CapturedLandscapeTexMinUV = LandscapeTexMinUV;
	const FVector CapturedLandscapeTexUVRange = LandscapeTexUVRange;
	const float CapturedGlobalAlpha = LayerSettings.GlobalAlpha;
	const float CapturedNoiseFrequency = LayerSettings.NoiseFrequency;
	const float CapturedNoiseAmplitude = LayerSettings.NoiseAmplitude;
	const float CapturedFalloffWidth = LayerSettings.FalloffWidth;

	ENQUEUE_RENDER_COMMAND(RuntimeGenerateLayerData)(
	[R_AlphaHeight, R_OrigData, R_Debug, Center, Extent, CapturedLandscapeTexMinUV, CapturedLandscapeTexUVRange,
	 CapturedGlobalAlpha, CapturedNoiseFrequency, CapturedNoiseAmplitude, CapturedFalloffWidth](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize = R_AlphaHeight->GetSizeXY();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureSize.X, TextureSize.Y, 1), 32);

			FRDGTextureRef TmpRDG_AlphaHeight = nullptr;
			FRDGTextureUAVRef RDGUAV_AlphaHeight = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_AlphaHeight, RDGUAV_AlphaHeight, TextureSize, PF_FloatRGBA, TEXT("UAV_AlphaHeight"));

			FRDGTextureRef TmpRDG_Debug = nullptr;
			FRDGTextureUAVRef RDGUAV_Debug = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Debug, RDGUAV_Debug, TextureSize, PF_FloatRGBA, TEXT("UAV_Debug"));

			FRDGTextureRef RDG_OrigData = RegisterExternalTexture(GraphBuilder, R_OrigData->GetRenderTargetTexture(), TEXT("R_OrigLandscapeData"));

			FCSRuntimeLandscapeLayer::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSRuntimeLandscapeLayer::FParameters>();
			PassParameters->T_OrigLandscapeData = RDG_OrigData;
			PassParameters->RW_LayerAlpha = RDGUAV_AlphaHeight;
			PassParameters->RW_DebugView = RDGUAV_Debug;
			{
				auto UVEnd_tmp = CapturedLandscapeTexMinUV + CapturedLandscapeTexUVRange;
				PassParameters->ValidUVRange = FVector4f(
					static_cast<float>(CapturedLandscapeTexMinUV.X), static_cast<float>(CapturedLandscapeTexMinUV.Y),
					static_cast<float>(UVEnd_tmp.X), static_cast<float>(UVEnd_tmp.Y));
			}
			PassParameters->GlobalAlpha = CapturedGlobalAlpha;
			PassParameters->NoiseFrequency = CapturedNoiseFrequency;
			PassParameters->NoiseAmplitude = CapturedNoiseAmplitude;
			PassParameters->FalloffWidth = CapturedFalloffWidth;
			PassParameters->BoxOrigin = FVector3f(Center);
			PassParameters->BoxExtent = FVector3f(Extent);
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			TShaderMapRef<FCSRuntimeLandscapeLayer> ComputeShader_Gen(
				FCSRuntimeLandscapeLayer::CreatePermutation(FCSRuntimeLandscapeLayer::ERuntimeLayerFunction::RL_GenerateNoiseAlpha));
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("RuntimeGenerateNoiseAlphaHeight"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_Gen, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Gen, *PassParameters, GroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_AlphaHeight, RegisterExternalTexture(GraphBuilder, R_AlphaHeight->GetRenderTargetTexture(), TEXT("R_AlphaHeight")), FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_Debug, RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_Debug")), FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
}

void ACSRuntimeLandscapeLayer::BlendLayer()
{
	if (!RT_BlendResult || !RT_LayerAlphaHeight || !RT_OrigLandscapeData) return;

	FTextureRenderTargetResource* R_Blend = RT_BlendResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_AlphaHeight = RT_LayerAlphaHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_OrigData = RT_OrigLandscapeData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();
	const FVector CapturedLandscapeTexMinUV = LandscapeTexMinUV;
	const FVector CapturedLandscapeTexUVRange = LandscapeTexUVRange;
	const float CapturedGlobalAlpha = LayerSettings.GlobalAlpha;
	const ERuntimeLayerBlendMode CapturedBlendMode = LayerSettings.BlendMode;
	FTextureRHIRef CapturedMaterialBlendTextureRHI = nullptr;
	if (LayerSettings.MaterialBlendTexture)
	{
		if (FTextureResource* MaterialBlendResource = LayerSettings.MaterialBlendTexture->GetResource())
		{
			CapturedMaterialBlendTextureRHI = MaterialBlendResource->GetTextureRHI();
		}
	}

	ENQUEUE_RENDER_COMMAND(RuntimeBlendLayer)(
	[R_Blend, R_AlphaHeight, R_OrigData, R_Debug, CapturedLandscapeTexMinUV, CapturedLandscapeTexUVRange,
	 CapturedGlobalAlpha, CapturedBlendMode, CapturedMaterialBlendTextureRHI](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize = R_Blend->GetSizeXY();

			FCSRuntimeLandscapeLayer::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSRuntimeLandscapeLayer::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureSize.X, TextureSize.Y, 1), 32);

			FRDGTextureRef TmpRDG_Result = nullptr;
			FRDGTextureUAVRef RDGUAV_Result = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Result, RDGUAV_Result, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));

			FRDGTextureRef TmpRDG_Debug = nullptr;
			FRDGTextureUAVRef RDGUAV_Debug = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Debug, RDGUAV_Debug, TextureSize, PF_FloatRGBA, TEXT("UAV_Debug"));

			FRDGTextureRef RDG_OrigData = RegisterExternalTexture(GraphBuilder, R_OrigData->GetRenderTargetTexture(), TEXT("R_OrigLandscapeData"));
			FRDGTextureRef RDG_AlphaHeight = RegisterExternalTexture(GraphBuilder, R_AlphaHeight->GetRenderTargetTexture(), TEXT("R_AlphaHeight"));

			PassParameters->T_OrigLandscapeData = RDG_OrigData;
			PassParameters->T_AlphaTexture = RDG_AlphaHeight;
			PassParameters->RW_BlendResult = RDGUAV_Result;
			PassParameters->RW_DebugView = RDGUAV_Debug;
			{
				auto UVEnd_tmp = CapturedLandscapeTexMinUV + CapturedLandscapeTexUVRange;
				PassParameters->ValidUVRange = FVector4f(
					static_cast<float>(CapturedLandscapeTexMinUV.X), static_cast<float>(CapturedLandscapeTexMinUV.Y),
					static_cast<float>(UVEnd_tmp.X), static_cast<float>(UVEnd_tmp.Y));
			}
			PassParameters->GlobalAlpha = CapturedGlobalAlpha;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			if (CapturedBlendMode == ERuntimeLayerBlendMode::MaterialDrive && CapturedMaterialBlendTextureRHI.IsValid())
			{
				PassParameters->T_AlphaTexture = RegisterExternalTexture(GraphBuilder,
					CapturedMaterialBlendTextureRHI, TEXT("R_MaterialBlend"), ERDGTextureFlags::None);
			}

			TShaderMapRef<FCSRuntimeLandscapeLayer> ComputeShader_Blend = FCSRuntimeLandscapeLayer::CreatePermutation(
				(CapturedBlendMode == ERuntimeLayerBlendMode::MaterialDrive)
					? FCSRuntimeLandscapeLayer::ERuntimeLayerFunction::RL_BlendMaterialDrive
					: FCSRuntimeLandscapeLayer::ERuntimeLayerFunction::RL_BlendAlpha);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("RuntimeBlendLayer"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_Blend, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Blend, *PassParameters, GroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RegisterExternalTexture(GraphBuilder, R_Blend->GetRenderTargetTexture(), TEXT("R_Blend")), FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_Debug, RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_Debug")), FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
}

void ACSRuntimeLandscapeLayer::ApplyToLandscape()
{
	if (!RT_BlendResult) return;

	bHasRuntimeResult = true;

	if (bAutoCommit)
	{
		CommitToLandscape();
	}
}

void ACSRuntimeLandscapeLayer::CommitToLandscape()
{
#if WITH_EDITOR
	if (!RT_BlendResult || !FindLandscape()) return;

	FCSReadLandscapeData LandscapeData;
	FVector Center = Box ? Box->Bounds.Origin : GetActorLocation();
	FVector Extent = Box ? Box->Bounds.BoxExtent : FVector(5000, 5000, 500);
	ReadRuntimeLandscapeData(LandscapeData, Center, Extent);
	if (LandscapeData.TextureSize.X + LandscapeData.TextureSize.Y < 32) return;

	int32 XNum = RT_BlendResult->SizeX;
	TArray<FLinearColor> ResultColors;
	UKismetRenderingLibrary::ReadRenderTargetRaw(this, RT_BlendResult, ResultColors, false);

	TArray<uint16> HeightData;
	HeightData.Reserve(LandscapeData.TextureValidSize.X * LandscapeData.TextureValidSize.Y);

	float LandscapeScaleZ = TargetLandscape->GetActorScale3D().Z;
	for (int32 Y = 0; Y < LandscapeData.TextureValidSize.Y; Y++)
	{
		for (int32 X = 0; X < LandscapeData.TextureValidSize.X; X++)
		{
			float ResultHeight = ResultColors[X + Y * XNum].A / LandscapeScaleZ;
			HeightData.Add(LandscapeDataAccess::GetTexHeight(ResultHeight));
		}
	}

	FLandscapeEditDataInterface LandscapeEdit(TargetLandscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(true);

	LandscapeEdit.SetHeightData(
		LandscapeData.ReadRange.X, LandscapeData.ReadRange.Y,
		LandscapeData.ReadRange.Z, LandscapeData.ReadRange.W,
		(uint16*)HeightData.GetData(), 0, true, nullptr, nullptr, nullptr);

	TargetLandscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All);

	bHasRuntimeResult = false;
#else
	UE_LOG(LogTemp, Warning, TEXT("CommitToLandscape: Landscape editing not available in packaged builds"));
#endif
}

void ACSRuntimeLandscapeLayer::FullRuntimePipeline()
{
	if (!FindLandscape())
	{
		UE_LOG(LogTemp, Warning, TEXT("ACSRuntimeLandscapeLayer: No Landscape found."));
		return;
	}

	InitRuntimeRT();
	ReadLandscapeData();
	GenerateLayerData();
	BlendLayer();
	ApplyToLandscape();
}

FName ACSRuntimeLandscapeLayer::GetBlendModeName(ERuntimeLayerBlendMode Mode)
{
	switch (Mode)
	{
	case ERuntimeLayerBlendMode::Alpha: return TEXT("Alpha");
	case ERuntimeLayerBlendMode::Additive: return TEXT("Additive");
	case ERuntimeLayerBlendMode::Subtract: return TEXT("Subtract");
	case ERuntimeLayerBlendMode::Multiply: return TEXT("Multiply");
	case ERuntimeLayerBlendMode::MaterialDrive: return TEXT("MaterialDrive");
	default: return TEXT("Unknown");
	}
}

ACSRuntimeLandscapeLayerManager::ACSRuntimeLandscapeLayerManager()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ACSRuntimeLandscapeLayerManager::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoFindLandscape)
	{
		UWorld* World = GetWorld();
		if (!World) return;

		for (TActorIterator<ALandscape> It(World, ALandscape::StaticClass()); It; ++It)
		{
			TargetLandscape = *It;
			break;
		}
	}

	if (bAutoRunOnBeginPlay)
	{
		RunAllLayers();
	}
}

void ACSRuntimeLandscapeLayerManager::AddLayer(ACSRuntimeLandscapeLayer* Layer)
{
	if (!Layer) return;
	if (!ActiveLayers.Contains(Layer))
	{
		Layer->TargetLandscape = TargetLandscape;
		ActiveLayers.Add(Layer);
	}
}

void ACSRuntimeLandscapeLayerManager::RemoveLayer(ACSRuntimeLandscapeLayer* Layer)
{
	ActiveLayers.Remove(Layer);
}

void ACSRuntimeLandscapeLayerManager::RunAllLayers()
{
	for (ACSRuntimeLandscapeLayer* Layer : ActiveLayers)
	{
		if (Layer)
		{
			Layer->TargetLandscape = TargetLandscape;
			Layer->FullRuntimePipeline();
		}
	}
}

void ACSRuntimeLandscapeLayerManager::ClearAllLayers()
{
	ActiveLayers.Reset();
}

FLandscapeLayerBlendResult ACSRuntimeLandscapeLayerManager::BlendMultipleLayers(
	const TArray<FLandscapeLayerBlendResult>& InLayerResults,
	ERuntimeLayerBlendMode FinalBlendMode)
{
	FLandscapeLayerBlendResult Result;
	return Result;
}

void ACSRuntimeLandscapeLayer::GenerateLayerData_Noise(float InNoiseFrequency, float InNoiseAmplitude)
{
	LayerSettings.NoiseFrequency = InNoiseFrequency;
	LayerSettings.NoiseAmplitude = InNoiseAmplitude;
	GenerateLayerData();
}

void ACSRuntimeLandscapeLayer::GenerateLayerData_Erosion(int32 Iterations, float Strength)
{
	LayerSettings.ErosionIterations = Iterations;
	LayerSettings.ErosionStrength = Strength;
	GenerateLayerData();
}
