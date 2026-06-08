// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "PolyLine.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Curves/CurveLinearColor.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "ComputeShaderMeshGenerator.h"
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
	UCurveLinearColor* CurveControl = nullptr;
};

struct FVineLinePointScaleData
{
	TArray<float> Values;
};

struct FVineLinePointAxisData
{
	TArray<FVector> Values;
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
	
	
	FBox InstanceBound;
	
	// Triangle surface data used by the CPU/BVH vine visualization path.
	// Filled by GenerateVines().
	FCSTriangleMeshData CachedSurfaceTriangles;

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

	// Per-line source instance scale used by GPU input packing.
	TArray<float> TubeLineSourceScales;
	TArray<float> PlaneLineSourceScales;

	// Per-line source instance location used by VisVine for deterministic temporary SC-point jitter.
	TArray<FVector> TubeLineSourceLocations;
	TArray<FVector> PlaneLineSourceLocations;

	// Per-line/per-point scale from SpaceColonization: TargetPointScale * SourcePointScale.
	// GPU visualization multiplies vine thickness by them during input packing.
	TArray<FVineLinePointScaleData> TubeLinePointScales;
	TArray<FVineLinePointScaleData> PlaneLinePointScales;

	// Per-line/per-point smoothed line axes from SpaceColonization GPU finalization.
	TArray<FVineLinePointAxisData> TubeLinePointAxes;
	TArray<FVineLinePointAxisData> PlaneLinePointAxes;

	//bool MainVine = true;

	UPROPERTY(BlueprintReadWrite, Category = "VisVine")
	bool bUseGPUMode = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine", meta = (ClampMin = "0"))
	int32 GenerateVineVoxelNormalBlurIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine", meta = (ClampMin = "0"))
	int32 VisVineGPUAxisSmoothIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine", meta = (ClampMin = "0"))
	int32 VisVineGPUPostProjectionSmoothIterations = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bDrawGPUProjectionVoxelDebugPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0"))
	int32 GPUProjectionVoxelDebugPointLimit = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float GPUProjectionVoxelDebugDuration = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SCStageDebugPointDuration = 10.0f;

	UPROPERTY(BlueprintReadWrite)
	FVineVisualization VV;
	
	UPROPERTY(BlueprintReadWrite)
	FSpaceColonizationOptions SC;
	
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool VisVine(bool MainVine, bool bUseGPU = true);

private:
	// Voxelized surface data used by the GPU vine visualization path.
	// Filled by GenerateVines().
	FCSSurfaceVoxelData CachedSurfaceVoxels;

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

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ExportTransformArrayToFoliage(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, BlueprintCallable, Category = "VineActions")
	void FetchFoliage();


	UFUNCTION(BlueprintCallable, BlueprintCallable, Category = "VineActions")
	void RevertFoliage();



	UFUNCTION(BlueprintCallable, BlueprintCallable, Category = "VineActions")
	void GenerateVineAction();

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Cached Vine Surface Voxel Points"))
	int32 DrawDebugCachedVineSurfaceVoxelPoints();

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Cached Vine SC Stage Points"))
	int32 DrawDebugCachedVineSCStagePoints(bool bDrawTube = true, bool bDrawPlane = true);

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Debug Vine Surface Voxel Arrows"))
	int32 DrawDebugVineSurfaceVoxelArrows(
		float ArrowLength = 0.0f,
		FLinearColor ArrowColor = FLinearColor::Blue,
		float Duration = 5.0f,
		float Thickness = 2.0f,
		bool bPersistentLines = false,
		bool bDrawVoxelCenters = true,
		FLinearColor VoxelCenterColor = FLinearColor::Yellow,
		float VoxelCenterPointSize = 6.0f,
		bool bDrawWeightedTargets = true,
		FLinearColor WeightedTargetColor = FLinearColor::Red,
		float WeightedTargetPointSize = 8.0f,
		bool bDrawCenterToTargetLines = true,
		FLinearColor CenterToTargetColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f),
		int32 MaxArrowsToDraw = 0);

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
