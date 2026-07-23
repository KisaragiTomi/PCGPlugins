#include "ComputeShaderBasicFunctionEditor.h"

#include "ComputeShaderGenerateHepler.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "ComputeShaderGenerateHepler.h"
#include "EngineUtils.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "LandscapeExtra.h"
#include "EngineModule.h"
#include "PixelShaderUtils.h"
#include "Components/SplineComponent.h"
#include "Engine/Texture2DArray.h"
#include "GeometryScript/PolyPathFunctions.h"
#include "Slate/SceneViewport.h"


using namespace CSHepler;


UTextureRenderTarget2D* UComputeShaderBasicFunctionEditor::GenerateHeightNormal(FVector Center, FVector Extent, int32 OutSize)
{
	FCSReadLandscapeData LandscapeData;
	ULandscapeExtra::CreateLandscapeTextureData(LandscapeData, Center, Extent);
	int32 HeightDataSize = FMath::CeilToInt(FMath::Pow(LandscapeData.Colors16.Num(), .5));
	
	UTextureRenderTarget2D* RT_HeightData = UKismetRenderingLibrary::CreateRenderTarget2D(GWorld, HeightDataSize, HeightDataSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	UTextureRenderTarget2D* RT_HeightNormal = UKismetRenderingLibrary::CreateRenderTarget2D(GWorld, OutSize, OutSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);

	FTextureRenderTargetResource* R_HeightData = RT_HeightData->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_HeightNormal = RT_HeightNormal->GameThread_GetRenderTargetResource();

	FVector TextureMin = Center - Extent;
	FVector TextureMax = Center + Extent;
	FVector Range = LandscapeData.MapMax - LandscapeData.MapMin + FVector(0, 0, 1);
	FVector MinUV = (TextureMin - LandscapeData.MapMin) / Range;
	FVector MaxUV = (TextureMax - LandscapeData.MapMin) / Range;
	FVector UVRange = MaxUV - MinUV;
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{

			FIntPoint TextureSizeXY = R_HeightNormal->GetSizeXY();
			FIntVector GroupSize = FIntVector(TextureSizeXY.X, TextureSizeXY.Y, 1);
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(GroupSize, 32);
			
			FTextureRHIRef TextureRHI = R_HeightNormal->GetRenderTargetTexture();
			uint32 DestStride;
			void* DestData = RHILockTexture2D(TextureRHI, 0, RLM_WriteOnly, DestStride, false);
			if (!DestStride) return;
			
			FMemory::Memcpy(DestData, LandscapeData.Colors16.GetData(), HeightDataSize * HeightDataSize * sizeof(FFloat16Color));
			RHIUnlockTexture2D(TextureRHI, 0 ,false);
			
			TShaderMapRef<FGeneralFunctionShader> ComputeShader = FGeneralFunctionShader::CreateShaderPermutation(FGeneralFunctionShader::EGeneralShader::GTS_ConvertHeightDataToTexture);
			FGeneralFunctionShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FGeneralFunctionShader::FParameters>();

			FRDGTextureRef RDG_HeightData = RegisterExternalTexture(GraphBuilder, R_HeightData->GetRenderTargetTexture(), TEXT("RDG_HeightData"));
			FRDGTextureRef RDG_HeightNormal = RegisterExternalTexture(GraphBuilder, R_HeightNormal->GetRenderTargetTexture(), TEXT("RDG_HeightNormal"));

			FRDGTextureRef TmpRDG_HeightNormal = nullptr;
			FRDGTextureUAVRef RDGUAV_HeightNormal = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_HeightNormal, RDGUAV_HeightNormal, TextureSizeXY, PF_FloatRGBA, TEXT("UAV_HeightNormal"));

			PassParameters->T_ProcssTexture0 = RDG_HeightData;
			PassParameters->RW_ProcssTexture0 = RDGUAV_HeightNormal;
			PassParameters->InputVectorData0 = FVector3f(UVRange.X, UVRange.Y, UVRange.Z);
			PassParameters->InputVectorData1 = FVector3f(MinUV.X, MinUV.Y, MinUV.Z);
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ConvertHeightDataToTexture"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[&PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
				});

			AddCopyTexturePass(GraphBuilder, TmpRDG_HeightNormal, RDG_HeightNormal, FRHICopyTextureInfo());

			
			
		}
		GraphBuilder.Execute();
	});
	FlushRenderingCommands();
	
	return RT_HeightNormal;
}
