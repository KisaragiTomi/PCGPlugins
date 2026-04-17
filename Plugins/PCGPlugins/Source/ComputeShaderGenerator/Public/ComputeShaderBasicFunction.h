#pragma once
//
#include "CoreMinimal.h"
#include "ComputeShaderGeneral.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Components/SplineComponent.h"
#include "TextureResource.h"
#include "RenderGraphBuilder.h"
#include "Util/ColorConstants.h"

class ACSCliffGenerateCapture;
class ACSPlaneRangeGenerator;

#include "ComputeShaderBasicFunction.generated.h"


struct FCSSPlinePointData
{
	TArray<TArray<FLinearColor>> PointData;
};

USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSReadLandscapeData
{
	GENERATED_BODY()
	TArray<FLinearColor> Colors;
	TArray<FFloat16Color> Colors16;
	TArray<FLinearColor> ValidColors;
	FIntVector4 ReadRange = FIntVector4(0, 0, 0, 0);
	FIntVector2 TextureSize = FIntVector2(0, 0);
	FIntVector2 TextureValidSize = FIntVector2(0, 0);
	FVector2f ValidUVRange = FVector2f::Zero();
	FVector MapMin = FVector::ZeroVector;
	FVector MapMax = FVector::ZeroVector;
	FVector ValidMapMin = FVector::ZeroVector;
	FVector ValidMapMax = FVector::ZeroVector;
	FTransform Transform = FTransform::Identity;
	FBoxSphereBounds ValidTextureBounds;
	FBoxSphereBounds TextureBounds;
};

UCLASS()
class COMPUTESHADERGENERATOR_API UComputeShaderBasicFunction : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawLinearColorsToRenderTarget32(UTextureRenderTarget2D* InTextureTarget, TArray<FLinearColor> Colors);


	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawLinearColorsToRenderTarget16(UTextureRenderTarget2D* InTextureTarget, TArray<FLinearColor> Colors);
	
	static void DrawFFloat16ColorsToRenderTarget(UTextureRenderTarget2D* InTextureTarget, TArray<FFloat16Color> Colors16);
	
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void ConnectivityPixel(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* InConnectivityMap, UTextureRenderTarget2D
	                              * InDebugView, int32 Channel = 2, int32 TextureSize = 256);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void BlurTexture(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutBlurTexture, float BlurScale = 1);

	static void BlurTextureRDG(FRDGBuilder& GraphBuilder, FRDGTextureRef& InTexture, FRDGTextureUAVRef& InTextureUAV, FRDGTextureRef& OutTexture, FIntVector
	                           GroupCount, FBlurTexture::EBlurType Type, float
	                           BlurScale = 1);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void BlurNormalTexture(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutBlurTexture, float BlurScale = .5);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void UpPixelsMask(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutUpTexture, float Threshold = .8, int32 Channel = 0);
	
	// UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	// static void UpPixelsMask(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutUpTexture, float Threshold = .8, int32 Channel = 0);
	//
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawTextureOut(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutTextureTarget);
	

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void ExtentMaskFast(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* InDebugView, int32 Channel = 0, int32 NumExtend = 1);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawLinearColorToRenderTarget(UTextureRenderTarget2D* InTextureTarget, TArray<FLinearColor> InColors);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CalTreeWindTexture(UTextureRenderTarget2D* InWindTexture_PivotIndex, UTextureRenderTarget2D* InWindTexture_DirExtent, UTextureRenderTarget2D
	                               * InDebugView, TArray<FVector4f> RootCenter4f, TArray<FVector4f> RootNormal4f, int32 TextureSizeX, int32 TextureSizeY);

	//SampleDistRotate: xy = dir, z = dist, w = rotate
	//SampleGradientHeight: xy = gradient, z = height
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static TArray<FTransform> SampleSpline(UTextureRenderTarget2D* InSampleDistRotate, UTextureRenderTarget2D* InSampleGradientHeight,
	                                       UTextureRenderTarget2D* InDebugView, TArray<USplineComponent*>
	                                       InSplineComponents, FBoxSphereBounds Bounds, int32 TextureSize = 256);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void GenerateMapCliff(TSubclassOf<ACSPlaneRangeGenerator> ManagerClass, TArray<TSubclassOf<ACSCliffGenerateCapture>> GeneratorClass, ACSPlaneRangeGenerator
	                             *& GeneratorManager);
	
	static void RDG_SampleSpline(FRDGBuilder& GraphBuilder, FRDGTextureRef& TmpRDG_DirMinDistRotate, FRDGTextureUAVRef& RDGUAV_GradientHeight, TArray<
	                             FLinearColor> SplinePoints, FIntPoint TextureSizeXY, FIntVector GroupCount
	);

	static void CalDistanceToNearestSurface(FSceneView* SceneView, UTextureRenderTarget2D* InDebugView);

	static void SampleGlobalDistanceFieldAtPositions(
		FSceneView* SceneView,
		const TArray<FVector>& WorldPositions,
		TArray<float>& OutDistances,
		TArray<FVector>& OutGradients);


	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CopyTexture(UTextureRenderTarget2D* InOrig, UTextureRenderTarget2D* InCopy);
	
	static void DrawCopyTexture(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef RDGUAV_CopySource, UTextureRenderTarget2D* RT_CopyTarget);
	
	static void DrawCopyTexture(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef RDGUAV_CopySource, FRDGTextureRef& RDG_CopyTarget);

	static void BuildTextureArray(FRDGBuilder& GraphBuilder, int32& Index, FRDGTextureRef& RDG_CopySource, FRDGTextureUAVRef
	                              & RDG_CopyTarget, FIntVector& GroupCount);

	static void GenerateWorldCaptureTransform(FRDGBuilder& GraphBuilder, int32& Index, FRDGTextureRef& RDG_CopySource, FRDGTextureUAVRef
							  & RDG_CopyTarget, FIntVector& GroupCount);


#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void Test(UTexture2DArray* InArray, UTexture2D* InTexture, UTextureRenderTarget2D* InDebugView);
	
	static void UpdateTextureArray(UTexture2DArray* Texture2DArray);
#endif

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void SyncRenderThread();
	
};

