// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderDebugParams.generated.h"

// =============================================================================
// VisVine Debug Parameter Structs (moved from GeometryScriptExtraEditor)
// =============================================================================

USTRUCT(BlueprintType, meta = (DisplayName = "GPU Projection Voxel Debug"))
struct COMPUTESHADERGENERATOR_API FVisVineGPUProjectionDebugOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bDrawGPUProjectionVoxelDebugPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0"))
	int32 GPUProjectionVoxelDebugPointLimit = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float GPUProjectionVoxelDebugDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bGPUProjectionVoxelDebugPointsPersistent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float GPUProjectionVoxelCenterPointSize = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float GPUProjectionVoxelTargetPointSize = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor GPUProjectionVoxelCenterColor = FLinearColor(1.0f, 0.85f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor GPUProjectionVoxelTargetColor = FLinearColor(1.0f, 0.0f, 0.1f, 1.0f);
};

USTRUCT(BlueprintType, meta = (DisplayName = "SC Stage Debug"))
struct COMPUTESHADERGENERATOR_API FVisVineSCStageDebugOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSCStageDrawTube = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SCStageDebugPointDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SCStageDebugPointSize = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSCStageDebugPointsPersistent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0"))
	int32 SCStageDebugPointLimit = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SCStageTubeDebugPointColor = FLinearColor(0.0f, 1.0f, 0.2f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SCStagePlaneDebugPointColor = FLinearColor(0.1f, 0.55f, 1.0f, 1.0f);
};

USTRUCT(BlueprintType, meta = (DisplayName = "Surface Voxel Debug"))
struct COMPUTESHADERGENERATOR_API FVisVineSurfaceVoxelDebugOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelArrowLength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelArrowColor = FLinearColor::Blue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelArrowDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelArrowThickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelArrowPersistentLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelDrawVoxelCenters = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelCenterColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelCenterPointSize = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelDrawWeightedTargets = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelWeightedTargetColor = FLinearColor::Red;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelWeightedTargetPointSize = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelDrawCenterToTargetLines = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelCenterToTargetColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0"))
	int32 SurfaceVoxelMaxArrowsToDraw = 0;
};

// Which intermediate stage of the VisVineGPU pipeline to read back and visualize as the
// center line. The SurfaceTarget center line evolves across these GPU passes:
//   FP  : after the final surface projection (FinalProject)
//   RS  : after the arc-length resample (ResampleSurface)
//   B   : after the smoothing ping-pong (final geometry center line)
// Only the selected stage is copied back from the GPU, so switching stages keeps the
// readback cost minimal.
UENUM(BlueprintType)
enum class EVisVineGPUDebugStage : uint8
{
	FinalProject UMETA(DisplayName = "FP - 最终投射后"),
	Resample     UMETA(DisplayName = "RS - 弧长重采样后"),
	Smooth       UMETA(DisplayName = "B - 平滑后 (最终中心线)"),
	None         UMETA(DisplayName = "无 - 只输出最终网格 (不画任何调试线)")
};

USTRUCT(BlueprintType, meta = (DisplayName = "VisVine Spline Debug"))
struct COMPUTESHADERGENERATOR_API FVisVineSplineDebugOptions
{
	GENERATED_BODY()
public:
	// When enabled, VisVineGPU draws each vine's selected-stage center line with DrawDebugLine.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bDrawDebugLines = true;

	// Which intermediate pipeline stage to read back and draw. Only this stage is copied
	// back from the GPU.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	EVisVineGPUDebugStage DebugStage = EVisVineGPUDebugStage::Smooth;

	// Skip vines whose center line ends up with fewer than this many points.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "2"))
	int32 MinPointsPerSpline = 2;

	// Color of the drawn center-line segments.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SplineColor = FLinearColor(0.0f, 1.0f, 0.35f, 1.0f);

	// Center-line segment thickness.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float LineThickness = 2.0f;

	// How long (seconds) the debug lines persist. Ignored when bPersistentLines is true.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float DebugDuration = 5.0f;

	// Keep debug lines until explicitly flushed instead of expiring after DebugDuration.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bPersistentLines = false;
};

// =============================================================================
// ComputeShaderMeshGenerator Debug Parameter Structs
// =============================================================================

/** Debug draw options for DrawDebugLastSurfaceVoxelDirections. */
USTRUCT(BlueprintType, meta = (DisplayName = "CS Last Voxel Direction Debug"))
struct COMPUTESHADERGENERATOR_API FCSDebugLastVoxelDirectionOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float DirectionLength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor DirectionColor = FLinearColor::Blue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Duration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Thickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bPersistentLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bDrawPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor PointColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float PointSize = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0"))
	int32 MaxDirectionsToDraw = 0;
};

/** Debug draw options for DrawDebugBoxSceneSurfaceVoxelDirections. */
USTRUCT(BlueprintType, meta = (DisplayName = "CS Box Voxel Direction Debug"))
struct COMPUTESHADERGENERATOR_API FCSDebugBoxVoxelDirectionOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.001"))
	float VoxelSize = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float DirectionLength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor DirectionColor = FLinearColor::Blue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Duration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Thickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bPersistentLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bDrawPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor PointColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float PointSize = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0"))
	int32 MaxDirectionsToDraw = 0;
};

/** Debug draw options for DrawDebugCachedSurfaceTriangles. */
USTRUCT(BlueprintType, meta = (DisplayName = "VisVine Triangle Debug"))
struct COMPUTESHADERGENERATOR_API FVisVineTriangleDebugOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bDrawTriangles = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float TriangleDebugDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bTriangleDebugPersistent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float TriangleLineThickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor TriangleLineColor = FLinearColor(0.0f, 0.75f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bDrawTriangleVertices = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float TriangleVertexPointSize = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor TriangleVertexColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bDrawTriangleNormals = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float TriangleNormalLength = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor TriangleNormalColor = FLinearColor::Red;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0"))
	int32 TriangleDebugCountLimit = 0;
};

/** Debug draw options for DrawDebugActiveVoxels. */
USTRUCT(BlueprintType, meta = (DisplayName = "CS Active Voxel Debug"))
struct COMPUTESHADERGENERATOR_API FCSDebugActiveVoxelOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Active Voxels")
	FName RequestId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Active Voxels")
	FLinearColor DebugColor = FLinearColor::Green;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Active Voxels", meta = (ClampMin = "0.0"))
	float Duration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Active Voxels", meta = (ClampMin = "0.0"))
	float Thickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Active Voxels")
	bool bPersistentLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Active Voxels")
	bool bDrawCacheBounds = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Active Voxels", meta = (ClampMin = "0"))
	int32 MaxVoxelsToDraw = 0;
};
