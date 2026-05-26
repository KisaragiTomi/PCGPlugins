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
#include "ComputeShaderMeshGenerator.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshSpatialFunctions.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

#include "GeometryEditorActor.generated.h"

USTRUCT(BlueprintType, meta = (DisplayName = "SC Options"))
struct GEOMETRYSCRIPTEXTRAEDITOR_API FSpaceColonizationOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Iteration = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Activetime = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 ExtentPlus = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float RandGrow = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float Seed = .5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float BackGrowRange = .8;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float VoxelSize = 10;
};

USTRUCT(BlueprintType, meta = (DisplayName = "VV Options"))
struct GEOMETRYSCRIPTEXTRAEDITOR_API FVineVisualization
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
	float VinesOffset = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UCurveLinearColor* CurveControl;
};

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API AVineContainer : public AComputeShaderMeshGenerator
{
	GENERATED_BODY()
	
	// Sets default values for this actor's properties
public:
	AVineContainer(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* GrowTarget;
	
	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* TubeVineSource;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* PlaneVineSource;
	
	TArray<AActor*> PickActors;
	
	FGeometryScriptDynamicMeshBVH BVH;
	
	FBox InstanceBound;
	
	UPROPERTY(Transient)
	TObjectPtr<UDynamicMesh> PrefixMesh;

	UPROPERTY(Transient)
	TObjectPtr<UDynamicMesh> OutTubeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UDynamicMesh> OutPlaneMesh;
	
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
	bool VisVineGPU(bool MainVine);

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	UDynamicMesh* GenerateVines(FSpaceColonizationOptions SC, float ExtrudeScale = 50, bool Result = true, bool
	                            MultThread = false);

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void Clean();
	// void AddMesh(TArray<FGeometryScriptPolyPath> Lines);
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void AddInstance(UFoliageType* InFoliageType);
	
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void AddInstanceFromFoliageType(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void ConvertInstance(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintPure, Category = ContainerCheck)
	UDynamicMesh* GetTubeMesh();

	UFUNCTION(BlueprintPure, Category = ContainerCheck)
	UDynamicMesh* GetPlaneMesh();

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ConvertInstanceToFoliage(UFoliageType* InFoliageType);
	

};




// UCLASS(hidecategories=Object, editinlinenew)
// class GEOMETRYSCRIPTEXTRAEDITOR_API UMyFoliageType : public UFoliageType_InstancedStaticMesh
// {
// 	GENERATED_UCLASS_BODY()
// 	
// };
