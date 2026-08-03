#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "RenderGraphResources.h"
#include "CSGpuDebugDraw.h"
#include "CSMeshGeneratorDebugComponent.generated.h"

struct FCSSurfaceVoxelGPUBuffers;

enum class ECSMeshGeneratorDebugMode : uint8
{
	Directions,
	IsolatedQuads,
};

/**
 * Render-thread-safe snapshot of a debug request. Two independent sources may be present
 * at once and both are drawn: a GPU surface-voxel source (counts stay on the GPU) and any
 * number of CPU-supplied primitive batches (points / lines / arrows / boxes).
 */
struct FCSMeshGeneratorDebugData
{
	// CPU-supplied primitives, already expanded by FCSGpuDebugDraw::BuildBatches into one
	// shared position/index pair plus one draw record (slice + colour) per batch.
	TArray<FVector3f> BatchPositions;
	TArray<uint32> BatchIndices;
	TArray<FCSGpuDebugBatchDraw> BatchDraws;
	FBox BatchBounds = FBox(ForceInit);

	TRefCountPtr<FRDGPooledBuffer> Positions;
	TRefCountPtr<FRDGPooledBuffer> Normals;
	TRefCountPtr<FRDGPooledBuffer> Counter;
	int32 VoxelCapacity = 0;
	int32 MaxVoxelsToDraw = 0;
	float VoxelSize = 0.0f;
	float DirectionLength = 0.0f;
	float QuadScale = 1.0f;
	float NormalOffsetScale = 0.0f;
	FLinearColor DirectionColor = FLinearColor::Blue;
	FLinearColor PointColor = FLinearColor::Yellow;
	FBox WorldBounds = FBox(ForceInit);
	ECSMeshGeneratorDebugMode Mode = ECSMeshGeneratorDebugMode::Directions;
	bool bDrawPoints = true;
	bool bReverseOrientation = false;

	/** Carry the already-submitted CPU primitives over when a voxel source is (re)submitted. */
	void TakeBatchGeometryFrom(const FCSMeshGeneratorDebugData& Other)
	{
		BatchPositions = Other.BatchPositions;
		BatchIndices = Other.BatchIndices;
		BatchDraws = Other.BatchDraws;
		BatchBounds = Other.BatchBounds;
	}

	bool HasVoxelSource() const
	{
		return Positions.IsValid() && Normals.IsValid() && Counter.IsValid() && VoxelCapacity > 0;
	}

	bool HasBatchGeometry() const
	{
		return !BatchDraws.IsEmpty() && !BatchPositions.IsEmpty() && !BatchIndices.IsEmpty();
	}

	bool IsValid() const
	{
		return HasVoxelSource() || HasBatchGeometry();
	}
};

/**
 * Draws surface-voxel diagnostics directly from retained GPU buffers. The proxy reads
 * the valid voxel count from Counter[0], generates render geometry with a compute shader,
 * and issues indirect draws. No GPU resource is mapped or copied to CPU memory.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSMeshGeneratorDebugComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UCSMeshGeneratorDebugComponent();

	/** Submits GPU normal lines and optional point primitives. Returns the submitted capacity. */
	int32 SetDirectionSource(
		const FCSSurfaceVoxelGPUBuffers& Source,
		float DirectionLength,
		FLinearColor DirectionColor,
		bool bDrawPoints,
		FLinearColor PointColor,
		int32 MaxDirectionsToDraw,
		float Duration,
		bool bPersistent);

	/** Replaces the CPU-supplied primitives with Batches. Returns the number of items drawn.
	 *  This is the GPU-submitted stand-in for DrawDebugPoint / DrawDebugLine /
	 *  DrawDebugDirectionalArrow / DrawDebugBox: one draw per batch, no per-frame CPU work. */
	int32 SetPrimitiveBatches(TArray<FCSGpuDebugBatch> Batches, float Duration, bool bPersistent);

	/** Appends one batch, keeping whatever this component is already drawing. */
	int32 AddPrimitiveBatch(FCSGpuDebugBatch Batch, float Duration, bool bPersistent);

	/** World-space points. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly))
	int32 DrawGpuDebugPoints(const TArray<FVector>& Points, FLinearColor Color,
		float Duration = 5.0f, bool bPersistent = false, int32 MaxItems = 0);

	/** World-space (start, end) pairs. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly))
	int32 DrawGpuDebugLines(const TArray<FVector>& LineEndpoints, FLinearColor Color,
		float Duration = 5.0f, bool bPersistent = false, int32 MaxItems = 0);

	/** World-space (start, end) pairs; HeadSize <= 0 uses 10% of each shaft. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly))
	int32 DrawGpuDebugArrows(const TArray<FVector>& ArrowEndpoints, FLinearColor Color, float HeadSize = 0.0f,
		float Duration = 5.0f, bool bPersistent = false, int32 MaxItems = 0);

	/** World-space (min, max) corner pairs of axis-aligned boxes. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly))
	int32 DrawGpuDebugBoxes(const TArray<FVector>& BoxCorners, FLinearColor Color,
		float Duration = 5.0f, bool bPersistent = false, int32 MaxItems = 0);

	/** Submits one GPU-generated isolated quad per valid surface voxel. */
	bool SetIsolatedQuadSource(
		const FCSSurfaceVoxelGPUBuffers& Source,
		float QuadScale,
		float NormalOffsetScale,
		bool bReverseOrientation);

	/** Releases the retained voxel buffers and removes the debug proxy. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug")
	void ClearDebug();

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	void SubmitData(FCSMeshGeneratorDebugData&& InData);
	void ScheduleClear(float Duration, bool bPersistent);
	/** Re-expands PendingBatches into PendingData's geometry and resubmits. Returns the item count. */
	int32 RebuildBatchGeometry(float Duration, bool bPersistent);

	// Game-thread source of truth for the CPU-supplied primitives; the render-thread snapshot in
	// PendingData only ever carries the expanded geometry.
	TArray<FCSGpuDebugBatch> PendingBatches;
	FCSMeshGeneratorDebugData PendingData;
	FTimerHandle ClearTimerHandle;
};
