// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "InstancedFoliageActor.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "FoliageInstanceBase.h"
#include "InstancedFoliage.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "FoliageConverter.generated.h"




/**
 * 
 */
UCLASS()
class TATOOLSPLUGIN_API UFoliageConverter : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:

	

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static UHierarchicalInstancedStaticMeshComponent* FindFoliageInstanceComponent(UStaticMesh* StaticMesh);

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static bool RemoveFoliageInstance(UStaticMesh* StaticMesh, const int32 InstanceIndex);

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static bool SetFoliageInstanceTransform(UStaticMesh* StaticMesh, const int32 InstanceIndex, const FTransform& Transform);

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static bool AddFoliageInstance(UStaticMesh* StaticMesh, UPrimitiveComponent* AttachComponent, const FTransform& Transforms);

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static void DistanceSort(TMap<float, int32> InMap, TMap<float, int32>& OutMap);

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static bool SetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32 BaseID);

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static bool GetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32& BaseID);

	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static bool GetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float& CustomDataValue);
	
	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static bool SetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float CustomDataValue);
	
	UFUNCTION(BlueprintCallable, Category = ProceduralFoliageSimulation)
	static void AddFoliage();


};
