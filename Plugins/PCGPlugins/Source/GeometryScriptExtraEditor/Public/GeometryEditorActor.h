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

class AStaticMeshActor;

USTRUCT(BlueprintType, meta = (DisplayName = "SC Options"))
struct GEOMETRYSCRIPTEXTRAEDITOR_API FSpaceColonizationOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Iteration = 55;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Activetime = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 ExtentPlus = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float RandGrow = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float Seed = .5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float BackGrowRange = .8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float VoxelSize = 2.5f;
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

struct FVineLinePointScaleData
{
	TArray<float> Values;
};

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API AVineContainer : public AComputeShaderMeshGenerator
{
	GENERATED_BODY()
	
	// Sets default values for this actor's properties
public:
	AVineContainer(const FObjectInitializer& ObjectInitializer);
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* GrowTarget;
	
	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* TubeVineSource;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* PlaneVineSource;
	
	FGeometryScriptDynamicMeshBVH BVH;
	
	FBox InstanceBound;
	
	UPROPERTY(Transient)
	TObjectPtr<UDynamicMesh> PrefixMesh;

	UPROPERTY(Transient)
	TObjectPtr<UDynamicMesh> OutTubeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UDynamicMesh> OutPlaneMesh;

	UPROPERTY(Transient)
	TObjectPtr<AStaticMeshActor> GeneratedStaticMeshActor;

	TArray<FGeometryScriptPolyPath> TubeLines;

	TArray<FGeometryScriptPolyPath> PlaneLines;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TargetType;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TubeType;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* PlaneType;

	// Per-line source instance scale kept for compatibility with old generated data.
	TArray<float> TubeLineSourceScales;
	TArray<float> PlaneLineSourceScales;

	// Per-line source instance location used by VisVine for deterministic temporary SC-point jitter.
	TArray<FVector> TubeLineSourceLocations;
	TArray<FVector> PlaneLineSourceLocations;

	// Per-line/per-point scale from SpaceColonization: TargetPointScale * SourcePointScale.
	// GPU visualization interpolates these through preprocessing and multiplies vine thickness by them.
	TArray<FVineLinePointScaleData> TubeLinePointScales;
	TArray<FVineLinePointScaleData> PlaneLinePointScales;

	//bool MainVine = true;

	UPROPERTY(BlueprintReadWrite, Category = "VisVine")
	bool bUseGPUMode = true;

	UPROPERTY(BlueprintReadWrite)
	FVineVisualization VV;
	
	UPROPERTY(BlueprintReadWrite)
	FSpaceColonizationOptions SC;
	
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool VisVine(bool MainVine, bool bUseGPU = true);

private:
	bool VisVineCPU(bool MainVine);
	bool VisVineGPUInternal(bool MainVine);

public:
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	UDynamicMesh* GenerateVines(float ExtrudeScale = 50, bool Result = true, bool MultThread = false);

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void Clean();

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void RebuildDisplayInstancesFromTransformArrays();

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ImportFoliageToTransformArray(UFoliageType* InFoliageType);
	UFUNCTION(BlueprintPure, Category = ContainerCheck)
	UDynamicMesh* GetTubeMesh();

	UFUNCTION(BlueprintPure, Category = ContainerCheck)
	UDynamicMesh* GetPlaneMesh();

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ExportTransformArrayToFoliage(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, BlueprintCallable, Category = "VineActions")
	void FetchFoliage();


	UFUNCTION(BlueprintCallable, BlueprintCallable, Category = "VineActions")
	void RevertFoliage();



	UFUNCTION(BlueprintCallable, BlueprintCallable, Category = "VineActions")
	void GenerateVineAction();

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void SaveStaticmesh();

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void ClearAttachedStaticMeshActors();
};




// UCLASS(hidecategories=Object, editinlinenew)
// class GEOMETRYSCRIPTEXTRAEDITOR_API UMyFoliageType : public UFoliageType_InstancedStaticMesh
// {
// 	GENERATED_UCLASS_BODY()
// 	
// };
