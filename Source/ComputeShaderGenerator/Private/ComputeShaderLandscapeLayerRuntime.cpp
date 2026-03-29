#include "ComputeShaderLandscapeLayerRuntime.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeComponent.h"
#include "TextureResource.h"
#include "DrawPrimtive.h"
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
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerAlpha)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_LayerHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_BlendResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER(FVector4f, ValidUVRange)
		SHADER_PARAMETER(float, GlobalAlpha)
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

	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
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

	FCSReadLandscapeData LandscapeData;
	ReadRuntimeLandscapeData(LandscapeData, Center, Extent);

	if (LandscapeData.TextureSize.X + LandscapeData.TextureSize.Y < 32) return;

	RT_LayerAlphaHeight->ResizeTarget(LandscapeData.TextureSize.X, LandscapeData.TextureSize.Y);
	RT_BlendResult->ResizeTarget(LandscapeData.TextureSize.X, LandscapeData.TextureSize.Y);
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

	UComputeShaderBasicFunction::DrawFFloat16ColorsToRenderTarget(RT_LayerAlphaHeight, LandscapeData.Colors16);
}

void ACSRuntimeLandscapeLayer::ReadRuntimeLandscapeData(FCSReadLandscapeData& LandscapeData, FVector Center, FVector Extent, int32 ExtentPlus)
{
	ALandscape* Landscape = TargetLandscape;
	if (!Landscape) return;

	const FVector Min = Center - Extent;
	const FVector Max = Center + Extent;
	if (Extent.Length() < 0.001f) return;

	const ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	if (!LandscapeInfo) return;

	const FVector MaxLocalPoint = Landscape->GetTransform().InverseTransformPosition(Max);
	const FVector MinLocalPoint = Landscape->GetTransform().InverseTransformPosition(Min);
	const FIntPoint KeyMax(FMath::CeilToInt(MaxLocalPoint.X) + ExtentPlus, FMath::CeilToInt(MaxLocalPoint.Y) + ExtentPlus);
	const FIntPoint KeyMin(FMath::FloorToInt(MinLocalPoint.X) - ExtentPlus, FMath::FloorToInt(MinLocalPoint.Y) - ExtentPlus);

	int32 XMin = KeyMin.X;
	int32 YMin = KeyMin.Y;
	int32 XMax = KeyMax.X;
	int32 YMax = KeyMax.Y;

	int32 ComponentSizeQuads = LandscapeInfo->ComponentSizeQuads;
	int32 XNum = XMax - XMin;
	int32 YNum = YMax - YMin;

	int32 NumPixelX = 0;
	for (int I = 5; I < 12; I++)
	{
		if (1 << I >= XNum)
		{
			NumPixelX = 1 << I;
			break;
		}
	}
	if (NumPixelX == 0) return;

	int32 NumPixelY = 0;
	for (int I = 5; I < 12; I++)
	{
		if (1 << I >= YNum)
		{
			NumPixelY = 1 << I;
			break;
		}
	}
	if (NumPixelY == 0) return;

	int32 X1 = KeyMin.X;
	int32 Y1 = KeyMin.Y;
	int32 X2 = KeyMax.X - 1;
	int32 Y2 = KeyMax.Y - 1;

	TArray<uint16> Values;
	Values.AddZeroed(XNum * YNum);

	FLandscapeEditDataInterface LandscapeEdit(Landscape->GetLandscapeInfo());
	LandscapeEdit.SetShouldDirtyPackage(false);
	LandscapeEdit.GetHeightDataFast(X1, Y1, X2, Y2, (uint16*)Values.GetData(), 0);

	TArray<FLinearColor> HeightNormals;
	TArray<FLinearColor> ValidHeightNormals;
	HeightNormals.AddZeroed(NumPixelX * NumPixelY);
	ValidHeightNormals.Reserve(XNum * YNum);

	for (int32 J = YMin; J < YMax; J++)
	{
		for (int32 I = XMin; I < XMax; I++)
		{
			FVector LandscapePosition = FVector(I, J, LandscapeDataAccess::GetLocalHeight(Values[(J - YMin) * XNum + (I - XMin)]));
			LandscapePosition = Landscape->GetTransform().TransformPosition(LandscapePosition);

			int xPrev = (I == X1) ? XMin : I - 1;
			int xNext = (I == X2) ? I : I + 1;
			int yPrev = (J == Y1) ? YMin : J - 1;
			int yNext = (J == Y2) ? J : J + 1;

			FVector LPXN = FVector(xNext, J, LandscapeDataAccess::GetLocalHeight(Values[(J - YMin) * XNum + (xNext - XMin)]));
			FVector LXPX = FVector(xPrev, J, LandscapeDataAccess::GetLocalHeight(Values[(J - YMin) * XNum + (xPrev - XMin)]));
			FVector LPYN = FVector(I, yNext, LandscapeDataAccess::GetLocalHeight(Values[(yNext - YMin) * XNum + (I - XMin)]));
			FVector LYPY = FVector(I, yPrev, LandscapeDataAccess::GetLocalHeight(Values[(yPrev - YMin) * XNum + (I - XMin)]));

			FVector DX = (Landscape->GetTransform().TransformPosition(LPXN) - Landscape->GetTransform().TransformPosition(LXPX)).GetSafeNormal();
			FVector DY = (Landscape->GetTransform().TransformPosition(LPYN) - Landscape->GetTransform().TransformPosition(LYPY)).GetSafeNormal();
			FVector Normal = FVector::CrossProduct(DX, DY).GetSafeNormal();

			FLinearColor Color = FLinearColor(Normal.X, Normal.Y, Normal.Z, LandscapePosition.Z);
			HeightNormals[I - XMin + (J - YMin) * NumPixelX] = Color;
			ValidHeightNormals.Add(Color);
		}
	}

	TArray<FFloat16Color> HeightNormals16;
	HeightNormals16.Reserve(HeightNormals.Num());
	for (const FLinearColor& C : HeightNormals)
	{
		HeightNormals16.Add(FFloat16Color(C));
	}

	LandscapeData.Colors16 = HeightNormals16;
	LandscapeData.ValidColors = ValidHeightNormals;
	LandscapeData.Colors = HeightNormals;
	LandscapeData.MapMin = Landscape->GetTransform().TransformPosition(FVector(XMin - 0.5f, YMin - 0.5f, 0.0f)) + Max * FVector(0, 0, 1);
	LandscapeData.MapMax = Landscape->GetTransform().TransformPosition(FVector(NumPixelX + XMin - 0.5f, NumPixelY + YMin - 0.5f, 0.0f)) + Min * FVector(0, 0, 1);
	LandscapeData.ValidMapMin = Landscape->GetTransform().TransformPosition(FVector(XMin - 0.5f, YMin - 0.5f, 0.0f));
	LandscapeData.ValidMapMax = Landscape->GetTransform().TransformPosition(FVector(XNum + XMin - 0.5f, YNum + YMin - 0.5f, 0.0f));
	LandscapeData.TextureSize = FIntVector2(NumPixelX, NumPixelY);
	LandscapeData.TextureValidSize = FIntVector2(XNum, YNum);
	LandscapeData.ValidUVRange = FVector2f(XNum / float(NumPixelX), YNum / float(NumPixelY));
	LandscapeData.ReadRange = FIntVector4(X1, Y1, X2, Y2);
	LandscapeData.Transform = Landscape->GetTransform();
	LandscapeData.TextureBounds = FBoxSphereBounds(FBox(LandscapeData.MapMin + 0.5f, LandscapeData.MapMax + 0.5f));
	LandscapeData.ValidTextureBounds = FBoxSphereBounds(FBox(LandscapeData.ValidMapMin + 0.5f, LandscapeData.ValidMapMax + 0.5f));
}

void ACSRuntimeLandscapeLayer::GenerateLayerData()
{
	if (!RT_LayerAlphaHeight || !RT_BlendResult) return;

	FTextureRenderTargetResource* R_AlphaHeight = RT_LayerAlphaHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();

	FVector Center = Box ? Box->Bounds.Origin : GetActorLocation();
	FVector Extent = Box ? Box->Bounds.BoxExtent : FVector(5000, 5000, 500);

	ENQUEUE_RENDER_COMMAND(RuntimeGenerateLayerData)(
	[=, this](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntPoint TextureSize = R_AlphaHeight->GetSizeXY();

			FCSRuntimeLandscapeLayer::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSRuntimeLandscapeLayer::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureSize.X, TextureSize.Y, 1), 32);

			FRDGTextureRef TmpRDG_Alpha = nullptr;
			FRDGTextureUAVRef RDGUAV_Alpha = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Alpha, RDGUAV_Alpha, TextureSize, PF_FloatRGBA, TEXT("UAV_Alpha"));

			FRDGTextureRef TmpRDG_Debug = nullptr;
			FRDGTextureUAVRef RDGUAV_Debug = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Debug, RDGUAV_Debug, TextureSize, PF_FloatRGBA, TEXT("UAV_Debug"));

			PassParameters->RW_LayerAlpha = RDGUAV_Alpha;
			PassParameters->RW_DebugView = RDGUAV_Debug;
			PassParameters->ValidUVRange = FVector4f(LandscapeTexMinUV, LandscapeTexMinUV + LandscapeTexUVRange);
			PassParameters->GlobalAlpha = LayerSettings.GlobalAlpha;
			PassParameters->NoiseFrequency = LayerSettings.NoiseFrequency;
			PassParameters->NoiseAmplitude = LayerSettings.NoiseAmplitude;
			PassParameters->FalloffWidth = LayerSettings.FalloffWidth;
			PassParameters->BoxOrigin = FVector3f(Center);
			PassParameters->BoxExtent = FVector3f(Extent);
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			{
				TShaderMapRef<FCSRuntimeLandscapeLayer> ComputeShader_GenAlpha(
					FCSRuntimeLandscapeLayer::CreatePermutation(FCSRuntimeLandscapeLayer::ERuntimeLayerFunction::RL_GenerateNoiseAlpha));
				PassParameters->RW_LayerAlpha = RDGUAV_Alpha;
				PassParameters->RW_LayerHeight = nullptr;
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("RuntimeGenerateNoiseAlpha"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[&PassParameters, ComputeShader_GenAlpha, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_GenAlpha, *PassParameters, GroupCount);
					});
				AddCopyTexturePass(GraphBuilder, TmpRDG_Alpha, RegisterExternalTexture(GraphBuilder, R_AlphaHeight->GetRenderTargetTexture(), TEXT("R_Alpha")), FRHICopyTextureInfo());
			}

			{
				TShaderMapRef<FCSRuntimeLandscapeLayer> ComputeShader_GenHeight(
					FCSRuntimeLandscapeLayer::CreatePermutation(FCSRuntimeLandscapeLayer::ERuntimeLayerFunction::RL_GenerateNoiseHeight));
				PassParameters->RW_LayerAlpha = nullptr;
				PassParameters->RW_LayerHeight = RDGUAV_Alpha;
				PassParameters->RW_DebugView = RDGUAV_Debug;
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("RuntimeGenerateNoiseHeight"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[&PassParameters, ComputeShader_GenHeight, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_GenHeight, *PassParameters, GroupCount);
					});
				AddCopyTexturePass(GraphBuilder, TmpRDG_Alpha, RegisterExternalTexture(GraphBuilder, R_AlphaHeight->GetRenderTargetTexture(), TEXT("R_Alpha")), FRHICopyTextureInfo());
				AddCopyTexturePass(GraphBuilder, TmpRDG_Debug, RegisterExternalTexture(GraphBuilder, R_Debug->GetRenderTargetTexture(), TEXT("R_Debug")), FRHICopyTextureInfo());
			}
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
}

void ACSRuntimeLandscapeLayer::BlendLayer()
{
	if (!RT_BlendResult || !RT_LayerAlphaHeight) return;

	FTextureRenderTargetResource* R_Blend = RT_BlendResult->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_AlphaHeight = RT_LayerAlphaHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Debug = RT_DebugView->GameThread_GetRenderTargetResource();

	ENQUEUE_RENDER_COMMAND(RuntimeBlendLayer)(
	[=, this](FRHICommandListImmediate& RHICmdList)
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

			FRDGTextureRef RDG_Alpha = RegisterExternalTexture(GraphBuilder, R_AlphaHeight->GetRenderTargetTexture(), TEXT("R_AlphaHeight"));

			PassParameters->T_OrigLandscapeData = RDG_Alpha;
			PassParameters->T_AlphaTexture = RDG_Alpha;
			PassParameters->RW_BlendResult = RDGUAV_Result;
			PassParameters->RW_DebugView = RDGUAV_Debug;
			PassParameters->ValidUVRange = FVector4f(LandscapeTexMinUV, LandscapeTexMinUV + LandscapeTexUVRange);
			PassParameters->GlobalAlpha = LayerSettings.GlobalAlpha;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

			ERuntimeLayerBlendMode ActiveBlend = LayerSettings.BlendMode;

			if (ActiveBlend == ERuntimeLayerBlendMode::MaterialDrive && LayerSettings.MaterialBlendTexture)
			{
				PassParameters->T_AlphaTexture = RegisterExternalTexture(GraphBuilder,
					LayerSettings.MaterialBlendTexture->GetResource()->GetTexture2DResource()->GetTextureRHI(), TEXT("R_MaterialBlend"));
			}

			TShaderMapRef<FCSRuntimeLandscapeLayer> ComputeShader_Blend = FCSRuntimeLandscapeLayer::CreatePermutation(
				(ActiveBlend == ERuntimeLayerBlendMode::MaterialDrive)
					? FCSRuntimeLandscapeLayer::ERuntimeLayerFunction::RL_BlendMaterialDrive
					: FCSRuntimeLandscapeLayer::ERuntimeLayerFunction::RL_BlendAlpha);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("RuntimeBlendLayer"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[&PassParameters, ComputeShader_Blend, GroupCount](FRHIComputeCommandList& RHICmdList)
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
	LandscapeEdit.SetShouldDirtyPackage(false);

	FLandscapeLayer* EditLayer = TargetLandscape->GetLayer(0);
	FGuid LayerGuid = EditLayer ? EditLayer->GetGuid() : FGuid();

	LandscapeEdit.SetHeightData(
		LandscapeData.ReadRange.X, LandscapeData.ReadRange.Y,
		LandscapeData.ReadRange.Z, LandscapeData.ReadRange.W,
		(uint16*)HeightData.GetData(), 0, true, nullptr, nullptr, nullptr);

	TargetLandscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All);
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
		for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
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
