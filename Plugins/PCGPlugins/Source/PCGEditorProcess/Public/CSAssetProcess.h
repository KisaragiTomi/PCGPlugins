// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSAssetProcess.generated.h"

class AStaticMeshActor;
class UTextureRenderTarget2D;
class UTexture2D;
class UMaterialInstance;
class UMaterialInterface;
class UStaticMesh;

UCLASS()
class PCGEDITORPROCESS_API UCSAssetProcess : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CaptureMeshHeight(AStaticMeshActor* InMeshActor, UTextureRenderTarget2D*& OutRenderTarget2D, TArray<AActor*>& OutActors, int32
	                              InTextureSize = 256);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CalculateMeshHeight(AStaticMeshActor* InMeshActor, UTextureRenderTarget2D* NewRenderTarget2D);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static UTexture2D* SaveTextureData(UTextureRenderTarget2D* RenderTarget, FString AssetName);
	
	template<typename T>
	static T* FindOrCreateAsset(FString AssetName, FString AssetPath, TFunction<T*()> Func);
	
	static UTexture2D* ConveretAndSaveRTAsset(FString AssetName, FString AssetPath, UTextureRenderTarget2D* RT);

	static UMaterialInstance* FindOrDuplicateMaterialInstanceAsset(FString SourcePath, FString TargetPath, bool Overwirte = false);

	static UMaterialInstance* FindOrCreateMaterialInstanceAsset(FString AssetName, FString AssetPath, UMaterialInterface* Parent);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void GetDistanceToNearestSurface(UTextureRenderTarget2D* InDebugView);

	UFUNCTION(BlueprintCallable, Category = "DistanceField", meta = (WorldContext = "WorldContextObject"))
	static void SampleGlobalDistanceField(
		UObject* WorldContextObject,
		const TArray<FVector>& WorldPositions,
		TArray<float>& OutDistances,
		TArray<FVector>& OutGradients);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CreateDebugTexture(AActor* TargetActor, UTextureRenderTarget2D* InDebugView, FString DebugName);

	/**
	 * 读取 16bit RT 的 B 通道对 Plane 顶点做 Z 位移，
	 * 同时将 RT 的 RG 归一化写入顶点色 RG，A 通道直接写入顶点色 A。
	 * @param InRenderTarget		16bit RT（RTF_RGBA16f）
	 * @param InStaticMesh			目标 Plane StaticMesh
	 * @param DisplaceScale			B 通道位移缩放系数
	 */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DisplaceMeshByRTBlueChannel(UTextureRenderTarget2D* InRenderTarget, UStaticMesh* InStaticMesh, float DisplaceScale = 100.0f);

};


