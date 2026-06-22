// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "PolyLine.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Curves/CurveLinearColor.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "ComputeShaderMeshGenerator.h"
#include "ComputeShaderDebugParams.h"
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

	// SpaceColonization-specific parameters
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 BackGrowCount = 3;
	// Fork Tapering starts shrinking the branch once it backtracks past the Nth non-primary
	// fork (1 = the first fork, 2 = the second, ...). Forks before the Nth are passed through
	// at full scale; from the Nth fork onward the retained ancestor points (BackGrowCount of
	// them) taper continuously down toward the end scale and may cross further forks without
	// resetting or stopping the taper.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1"))
	int32 ForkTaperForkOrdinal = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	float InfluenceRadius = 200.0f;
};

USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTEXTRAEDITOR_API FSpaceColonizationAttribute
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	bool Attractor = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	bool End = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	bool Startpt = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	float CurveU = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 SpawnCount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 Startid = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 PrePt = -1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 NextPt = -1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 Infaction = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 BranchCount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 BackCount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	FVector N = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	TArray<int32> Associates;
};

USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTEXTRAEDITOR_API FSpaceColonizationLineResult
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	FGeometryScriptPolyPath Path;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	TArray<float> PointScales;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	TArray<FVector> PointAxes;
};

USTRUCT(BlueprintType, meta = (DisplayName = "VV Options"))
struct GEOMETRYSCRIPTEXTRAEDITOR_API FVV
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UVScaleInfluence = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float UVScaleFloor = 0.08f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float UVScalePower = 1.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.000001"))
	float UVLengthScale = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UCurveLinearColor* CurveControl = nullptr;

	// --- moved from FVisVineParameters ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	bool bUseGPUMode = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 GenerateVineVoxelNormalBlurIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUPostProjectionSmoothIterations = 3;

	// Half-width (in path points) of each post-projection smoothing pass. Larger values round
	// sharp folds harder per iteration; 1 reproduces the legacy immediate-neighbor smoothing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1"))
	int32 VisVineGPUPostProjectionSmoothKernelRadius = 4;

	// Light radius-1 (immediate prev/next only) smoothing passes applied after the wide-kernel
	// smoothing, as a final local cleanup.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUPostProjectionSmallSmoothIterations = 2;

	// How strongly the local corner angle modulates the path smoothing. The angle between the
	// incoming/outgoing segments at each point drives the strength: a smaller angle (sharper fold)
	// smooths harder, while near-straight points are left almost untouched. 0 == legacy uniform
	// smoothing (every interior point fully averaged); 1 == full angle-driven modulation.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0", ClampMax = "1"))
	float VisVineGPUPostProjectionSmoothAngleStrength = 1.0f;

	// Before the final tangent smoothing, redistribute each vine's surface points to uniform
	// arc-length spacing along the post-projection surface polyline. Point count is preserved.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	bool bVisVineGPUResampleSurfaceEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUNoiseIterations = 10;

	// Pass C 扫掠截面（Tube）的圆周段数：每个路径点环上的截面顶点数。3 = 三角形（原始行为），
	// 数值越大截面越接近圆。仅影响 Tube 截面，Plane 截面始终为 2 点。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "3"))
	int32 VisVineGPUTubeSegments = 3;

	// Temporarily run the post-projection passes (RS/B/FT/AS/C) on the CPU instead of the GPU.
	// The GPU still runs the voxel-dependent stages (N/A/P/FP) and reads back the projected
	// surface; the CPU then resamples, smooths, rebuilds tangents and sweeps the mesh.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	bool bVisVineGPUUseCPUForPostPasses = false;

};

USTRUCT()
struct FVineLinePointScaleData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<float> Values;
};

USTRUCT()
struct FVineLinePointAxisData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVector> Values;
};

// VisVine debug parameter structs moved to ComputeShaderDebugParams.h

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


	FBox InstanceBound;

	UPROPERTY(Transient)
	TObjectPtr<AStaticMeshActor> GeneratedStaticMeshActor;

	UPROPERTY(Transient)
	TObjectPtr<AActor> DebugVineSplineActor;

	UPROPERTY(Transient)
	TArray<FGeometryScriptPolyPath> TubeLines;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TargetType;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TubeType;

	// Per-line source instance scale used by GPU input packing.
	UPROPERTY(Transient)
	TArray<float> TubeLineSourceScales;

	// Per-line source instance location used by VisVine for deterministic temporary SC-point jitter.
	UPROPERTY(Transient)
	TArray<FVector> TubeLineSourceLocations;

	// Per-line/per-point scale from SpaceColonization: TargetPointScale * SourcePointScale.
	// GPU visualization multiplies vine thickness by them during input packing.
	UPROPERTY(Transient)
	TArray<FVineLinePointScaleData> TubeLinePointScales;

	// Per-line/per-point smoothed line axes from SpaceColonization GPU finalization.
	UPROPERTY(Transient)
	TArray<FVineLinePointAxisData> TubeLinePointAxes;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineGPUProjectionDebugOptions GPUProjectionDebug;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineSCStageDebugOptions SCStageDebug;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineSurfaceVoxelDebugOptions SurfaceVoxelDebug;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineTriangleDebugOptions TriangleDebug;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineSplineDebugOptions SplineDebug;

	UPROPERTY(BlueprintReadWrite)
	FVV VV;
	
	UPROPERTY(BlueprintReadWrite)
	FSpaceColonizationOptions SC;
	
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool VisVine();

private:
	// Voxelized surface data used by the GPU vine visualization path.
	// Filled by GenerateVines().
	FCSSurfaceVoxelData CachedSurfaceVoxels;

	bool VisVineGPUInternal();

	// Draws each vine's selected-stage center line directly with DrawDebugLine (no spawned
	// actor). Lines are split per vine via PathPointMeta (BaseIndex / PointCount).
	// No-op unless SplineDebug.bDrawDebugLines.
	void DrawDebugVineCenterLines(
		const TArray<FVector4f>& CenterPoints,
		const TArray<FIntVector4>& PathPointMeta);

public:
	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug")
	void ClearDebugVineSplineActor();

public:
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	UDynamicMesh* GenerateVines(float ExtrudeScale = 50, bool Result = true);

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
	int32 DrawDebugCachedVineSCStagePoints();

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Debug Vine Surface Voxel Arrows"))
	int32 DrawDebugVineSurfaceVoxelArrows();

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Debug Cached Surface Triangles"))
	int32 DrawDebugCachedSurfaceTriangles();

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void SaveStaticmesh();

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void ClearAttachedStaticMeshActors();

	// ---- SpaceColonization (moved from UGenerateVines, params from SC member) ----

	UFUNCTION(BlueprintCallable, Category = "SpaceColonization")
	bool BuildSpaceColonizationQueue(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms,
		TArray<FVector>& OutTargetLocations, TArray<FSpaceColonizationAttribute>& OutSCAttributes, bool bUseComputeShader = false);

	UFUNCTION(BlueprintCallable, Category = "SpaceColonization")
	TArray<FSpaceColonizationLineResult> SpaceColonizationWithScales(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, bool bUseComputeShader = false);
};




// UCLASS(hidecategories=Object, editinlinenew)
// class GEOMETRYSCRIPTEXTRAEDITOR_API UMyFoliageType : public UFoliageType_InstancedStaticMesh
// {
// 	GENERATED_UCLASS_BODY()
// 	
// };
