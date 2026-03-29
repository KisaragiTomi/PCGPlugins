// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ComputeShaderShallowWater.h"
#include "CSAssetProcess.generated.h"

/**
 * 
 */
UCLASS()
class CSEDITORPROCESS_API UCSAssetProcess : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CaptureMeshHeight(AStaticMeshActor* InMeshActor, UTextureRenderTarget2D*& OutRenderTarget2D, TArray<AActor*>& OutActors, int32
	                              InTextureSize = 256);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CalculateMeshHeight(AStaticMeshActor* InMeshActor, UTextureRenderTarget2D* NewRenderTarget2D);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void SaveSWData(ACSShallowWaterCapture* InCSSWActor);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static UTexture2D* SaveTextureData(UTextureRenderTarget2D* RenderTarget, FString AssetName);
	
	template<typename T>
	static T* FindOrCreateAsset(FString AssetName, FString AssetPath, TFunction<T*()> Func);
	
	static UTexture2D* ConveretAndSaveRTAsset(FString AssetName, FString AssetPath, UTextureRenderTarget2D* RT);

	static UMaterialInstance* FindOrDuplicateMaterialInstanceAsset(FString SourcePath, FString TargetPath, bool Overwirte = false);

	static UMaterialInstance* FindOrCreateMaterialInstanceAsset(FString AssetName, FString AssetPath, UMaterialInterface* Parent);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void GetDistanceToNearestSurface(UTextureRenderTarget2D* InDebugView);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CreateDebugTexture(AActor* TargetActor, UTextureRenderTarget2D* InDebugView, FString DebugName);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static ACSShallowWaterCapture* CSSW_GetSelectActor();
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void MaterialTest(ACSShallowWaterCapture* InCSSWActor);
};


