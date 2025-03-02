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
#include "DynamicMeshActor.h"

#include "FoliageConverter.generated.h"

/**
 * 
 */
UCLASS()
class FOLIAGECLUSTER_API UFoliageConverter : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void ConvertFoliageToInstanceComponent(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void ConvertInstanceComponentToFoliage(UFoliageType* InFoliageType, UClass* ContainerClass);

	UFUNCTION(BlueprintPure, Category = FoliageExtra)
	static UStaticMesh* GetStaticMesh(UFoliageType* InFoliageType);
	
	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static UHierarchicalInstancedStaticMeshComponent* FindFoliageInstanceComponent(UStaticMesh* StaticMesh);

	UFUNCTION(BlueprintPure, Category = FoliageExtra, meta=(ScriptMethod))
	static void GetAllInstancesTransfrom(UFoliageType* InFoliageType, TArray<FTransform>& OutTransforms);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra, meta=(ScriptMethod))
	static void RefreshFoliage(UFoliageType* InFoliageType);
	
	UFUNCTION(BlueprintPure, Category = FoliageExtra, meta=(ScriptMethod))
	static UPARAM(DisplayName = "Get Foliage Type") const UFoliageType* GetFoliageType(UStaticMesh* StaticMesh);

	UFUNCTION(BlueprintPure, Category = FoliageExtra, meta=(ScriptMethod))
	static const UFoliageType* AddFoliageType(const UFoliageType* InType);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool RemoveFoliageInstance(UStaticMesh* StaticMesh, const int32 InstanceIndex);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool SetFoliageInstanceTransform(UStaticMesh* StaticMesh, const int32 InstanceIndex, const FTransform& Transform);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool AddFoliageInstance(UStaticMesh* StaticMesh, UPrimitiveComponent* AttachComponent, const FTransform& Transforms);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void DistanceSort(TMap<float, int32> InMap, TMap<float, int32>& OutMap);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool SetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32 BaseID);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool GetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32& BaseID);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool GetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float& CustomDataValue);
	
	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool SetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float CustomDataValue);
	
	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void AddFoliage();


};

UCLASS()
class FOLIAGECLUSTER_API AConvertFoliageInstanceContainer : public ADynamicMeshActor
{
	GENERATED_BODY()

public:
	AConvertFoliageInstanceContainer(const FObjectInitializer& ObjectInitializer);
	// {
	// 	FoliageInstanceContainer = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("FoliageInstanceContainer"));
	// 	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	// 	FoliageInstanceContainer->SetStaticMesh(Mesh);
	// 	FoliageInstanceContainer->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	// 	FoliageInstanceContainer->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	// 	FoliageInstanceContainer->SetVisibility(false, false);
	// 	FoliageInstanceContainer->SetHiddenInGame(true);
	// 	SetRootComponent(FoliageInstanceContainer);
	// }
	UPROPERTY(BlueprintReadWrite, Category = "CubeAttrib")
	UInstancedStaticMeshComponent* FoliageInstanceContainer;

	UFUNCTION(BlueprintCallable, Category = Convert)
	virtual void AddInstanceFromFoliageType(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = Convert)
	virtual void ConvertInstanceToFoliage(UFoliageType* InFoliageType);
};

