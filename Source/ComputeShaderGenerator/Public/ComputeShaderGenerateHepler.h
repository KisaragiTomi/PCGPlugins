#pragma once

#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "components/InstancedStaticMeshComponent.h"
#include "IntVectorTypes.h"
#include "Components/BoxComponent.h"
#include "ComputeShaderSceneCapture.h"

#include "ComputeShaderGenerateHepler.generated.h"

// ConvertToUAVRWBuffer(GraphBuilder, TRDGB_##Name, RDGUAVB_##Name, Name, EPixelFormat::PF_A32B32G32R32F, TEXT("UAV_SourceData"));
#define CREATE_UAVB(Format, Name) \
FRDGBufferRef TRDGB_##Name = nullptr; \
FRDGBufferUAVRef RDGUAVB_##Name = nullptr; \
ConvertToUAVRWBuffer(GraphBuilder, TRDGB_##Name, RDGUAVB_##Name, Name, Format, TEXT("UAV_"#Name)); \
PassParameters->BCount_##Name = Name.Num();

#define CREATE_UAVB_16(Name) \
CREATE_UAVB(EPixelFormat::PF_A16B16G16R16, Name) \
PassParameters->RWB_##Name = RDGUAVB_##Name;

#define CREATE_UAVB_32(Name) \
CREATE_UAVB(EPixelFormat::PF_A32B32G32R32F, Name) \
PassParameters->RWB_##Name = RDGUAVB_##Name;

#define CREATE_UAV(Size, Format, Name) \
FRDGTextureRef TRDG_##Name = nullptr; \
FRDGTextureUAVRef RDGUAV_##Name = nullptr; \
ConvertToUVATextureFormat(GraphBuilder, TRDG_##Name, RDGUAV_##Name, Size, Format, TEXT("UAV_"#Name)); \
PassParameters->RW_##Name = RDGUAV_##Name;

#define CREATE_TEXTURE_UAV_32(Name) \
CREATE_UAV(TextureSize, PF_A32B32G32R32F, Name)

#define CREATE_TEXTURE_UAV_16(Name) \
CREATE_UAV(TextureSize, PF_FloatRGBA, Name)

#define CREATE_TEXTURE_UAV_16_OUT(Name) \
CREATE_UAV(TextureSize, PF_FloatRGBA, Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name)); 


#define CREATE_TEXTURE_UAV_16_OUTP(Name) \
CREATE_UAV(TextureSize, PF_FloatRGBA, Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name)); \
PassParameters->T_##Name = RDG_##Name;

#define CREATE_TEXTURE_UAV_32_OUT(Name) \
CREATE_UAV(TextureSize, PF_A32B32G32R32F, Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name)); 


#define CREATE_TEXTURE_UAV_32_OUTP(Name) \
CREATE_UAV(TextureSize, PF_A32B32G32R32F, Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name)); \
PassParameters->T_##Name = RDG_##Name;

#define CREATE_RDG(Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name));\
PassParameters->T_##Name = RDG_##Name;


DECLARE_STATS_GROUP(TEXT("CSTest"), STATGROUP_CSTest, STATCAT_Advanced);

using namespace UE::Geometry;

namespace CSHepler
{
	static FRDGTextureRef ConvertToUVATexture(FTextureRenderTargetResource* RenderTarget, FRDGBuilder& GraphBuilder, FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0), const TCHAR* Name = TEXT("TempTexture") )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(RenderTarget->GetSizeXY(), PF_FloatRGBA, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, TmpTexture, ClearColor);
		return TmpTexture;
	}

	static FRDGTextureRef ConvertToUVATextureFormat(FRDGBuilder& GraphBuilder, FTextureRenderTargetResource* RenderTarget,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(RenderTarget->GetSizeXY(), Format, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, TmpTexture, ClearColor);
		return TmpTexture;
	}

	static FRDGTextureRef ConvertToUVATextureFormat(FRDGBuilder& GraphBuilder, FIntPoint Size,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(Size, Format, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, TmpTexture, ClearColor);
		return TmpTexture;
	}

	static void ConvertToUVATextureFormat(FRDGBuilder& GraphBuilder, FRDGTextureRef& OutTmpRDG, FRDGTextureUAVRef& OutRDGUAV, FIntPoint Size,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(Size, Format, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		OutTmpRDG = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, OutTmpRDG, ClearColor);

		OutRDGUAV = GraphBuilder.CreateUAV(OutTmpRDG);

	}

	static void CreateUVATextureArrayFormat(FRDGBuilder& GraphBuilder, int32 ArraySize, FRDGTextureRef& OutTmpRDG, FRDGTextureUAVRef& OutRDGUAV, FIntPoint Size,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc TextureArrayDesc = FRDGTextureDesc::Create2DArray(
		Size,   // 单层纹理尺寸
		Format,                // 像素格式（如 PF_R8G8B8A8）
		FClearValueBinding::Black,  // 默认值
		TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV, // 支持 UAV 读写
		ArraySize                   // 数组层数（即 RenderTarget 数量）
		);
		OutTmpRDG = GraphBuilder.CreateTexture(TextureArrayDesc, Name);
		AddClearRenderTargetPass(GraphBuilder, OutTmpRDG, ClearColor);

		OutRDGUAV = GraphBuilder.CreateUAV(OutTmpRDG);
	}
	
	static int32 GenerateTextureSize(int32 Iteration)
	{
		for (int i = 0; i < 12; i ++)
		{
			if (FMath::Pow(2.0, i) - 2 > Iteration)
				return FMath::Pow(2.0, i);
		}
		return -1;
	}

	inline void CreateRWBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& RDGBuffer, FRDGBufferUAVRef& RDGUAVBuffer, uint32 NumElements, uint32 BytesPerElement, EPixelFormat Format = PF_A16B16G16R16, const TCHAR* Name = TEXT("UAV_Buffer"))
	{
		if (NumElements == 0 || BytesPerElement == 0) return;
		RDGBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
		RDGUAVBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RDGBuffer, Format));
		AddClearUAVPass(GraphBuilder,RDGUAVBuffer, 0);
	}

	template<typename T>
	void ConvertToUAVRWBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& RDGBuffer, FRDGBufferUAVRef& RDGUAVBuffer, TArray<T> ArrayData, EPixelFormat Format = PF_A16B16G16R16, const TCHAR* Name = TEXT("UAV_Buffer"))
	{
		if (ArrayData.Num() == 0 || sizeof (T) == 0) return;
		RDGBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof (T), ArrayData.Num()), Name);
		GraphBuilder.QueueBufferUpload(RDGBuffer, ArrayData.GetData(), ArrayData.Num() * sizeof(T));
		RDGUAVBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RDGBuffer, Format));
	}

				
}


UCLASS()
class COMPUTESHADERGENERATOR_API ACSInstanceContainer : public AActor
{
	GENERATED_BODY()
public:
	
	ACSInstanceContainer(const FObjectInitializer& ObjectInitializer)
	{
		Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Instances"));
	}
	UPROPERTY(BlueprintReadWrite, Category = "CubeAttrib")
	UInstancedStaticMeshComponent* Instances;
};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSRangeGenerator : public AActor
{
	GENERATED_BODY()
public:
	ACSRangeGenerator();

	USceneComponent* SceneComponent;
	
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	UBoxComponent* Box;

	UBoxComponent* CollisionBox;

	UPROPERTY(BlueprintReadWrite, Category = "ComputeShader")
	UInstancedStaticMeshComponent* Instances;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	FIntVector DivdeCount = FIntVector(10, 10, 10);
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float CaptureSize = 1024;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float MaxDepth = 10000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	int32 GeneratorCount = 0;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	int32 MultGenerateCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	bool DoGenerate = false;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	UStaticMesh* CollisionMesh;

	UPROPERTY(BlueprintReadWrite, Category = "ComputeShader")
	TArray<AActor*> ActorsInBox;

	UPROPERTY(BlueprintReadWrite, Category = "ComputeShader")
	TArray<FTransform> StoreCaptureTransforms;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	TArray<ACSGenerateCaptureScene*> CaptureSceneGenerators;

	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void Tick(float DeltaTime) override;

	virtual void GenerateInternal();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void Generate();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	TArray<FTransform> GenerateTransformsCount();

	virtual TArray<FTransform> GenerateTransformsInternal();


};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSBoxRangeGenerator : public ACSRangeGenerator
{
	GENERATED_BODY()
public:
	
	ACSBoxRangeGenerator();

	
	virtual TArray<FTransform> GenerateTransformsInternal() override;
};


UCLASS()
class COMPUTESHADERGENERATOR_API ACSPlaneRangeGenerator : public ACSRangeGenerator
{
	GENERATED_BODY()
public:
	
	ACSPlaneRangeGenerator();

	virtual void Tick(float DeltaTime) override;

	virtual void GenerateInternal() override;
	
	virtual TArray<FTransform> GenerateTransformsInternal() override;
};

