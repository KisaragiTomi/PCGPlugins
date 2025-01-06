// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "Noise.h"
#include "PolyLine.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Curve/CurveUtil.h"
#include "Curves/CurveLinearColor.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "FoliageCluster/Public/FoliageConverter.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshSpatialFunctions.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

#include "GeometryEditorActor.generated.h"

USTRUCT(BlueprintType, meta = (DisplayName = "VV Options"))
struct GEOMETRYSCRIPTEXTRA_API FVineVisualization
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CurlNoiseScale = 2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CurlNoiseFre = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float PerlinNoiseScale = 11;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float PerlinNoiseFre = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float ResampleLength = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float LineScale = 0.1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CircleScale = 0.2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float MergeDistMult = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UCurveLinearColor* CurveControl;
};

UCLASS()
class GEOMETRYSCRIPTEXTRA_API AVineContainer : public AConvertFoliageInstanceContainer
{
	GENERATED_BODY()
	
	// Sets default values for this actor's properties
public:
	AVineContainer(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(BlueprintReadWrite, Category = "StartInstance")
	UInstancedStaticMeshComponent* TubeFoliageInstanceContainer;

	UPROPERTY(BlueprintReadWrite, Category = "StartInstance")
	UInstancedStaticMeshComponent* PlaneFoliageInstanceContainer;
	
	TArray<AActor*> PickActors;
	
	FGeometryScriptDynamicMeshBVH BVH;
	
	FBox InstanceBound;
	
	UDynamicMesh* PrefixMesh;

	UDynamicMesh* OutTubeMesh;

	UDynamicMesh* OutPlaneMesh;
	
	TArray<FGeometryScriptPolyPath> TubeLines;

	TArray<FGeometryScriptPolyPath> PlaneLines;

	//bool MainVine = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Adjust")
	FVineVisualization VV;
	
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool CheckActors(TArray<AActor*> CheckActors);
	
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void VisVine(bool MainVine);

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void Clean();
	// void AddMesh(TArray<FGeometryScriptPolyPath> Lines);
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void AddInstance(UFoliageType* InFoliageType);
	
	virtual void AddInstanceFromFoliageType(UFoliageType* InFoliageType) override;

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void ConvertInstance(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintPure, Category = ContainerCheck)
	UDynamicMesh* GetTubeMesh();

	UFUNCTION(BlueprintPure, Category = ContainerCheck)
	UDynamicMesh* GetPlaneMesh();

	virtual void ConvertInstanceToFoliage(UFoliageType* InFoliageType) override;
	

};




// UCLASS(hidecategories=Object, editinlinenew)
// class GEOMETRYSCRIPTEXTRA_API UMyFoliageType : public UFoliageType_InstancedStaticMesh
// {
// 	GENERATED_UCLASS_BODY()
// 	
// };
