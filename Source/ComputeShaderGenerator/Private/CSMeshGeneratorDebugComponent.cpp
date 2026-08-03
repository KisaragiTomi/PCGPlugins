#include "CSMeshGeneratorDebugComponent.h"
#include "CSGpuDebugDraw.h"
#include "CSMeshGeneratorDebugSceneProxy.h"
#include "ComputeShaderMeshGenerator.h"

#include "Engine/World.h"
#include "TimerManager.h"

namespace
{
// Unity/jumbo builds share a translation unit, so file-local helpers carry a unique prefix.
FCSGpuDebugBatch CSMGD_MakeBatch(ECSGpuDebugPrimitive Primitive, const TArray<FVector>& Positions,
	const FLinearColor& Color, float ArrowHeadSize, int32 MaxItems)
{
	FCSGpuDebugBatch Batch;
	Batch.Primitive = Primitive;
	Batch.Color = Color;
	Batch.ArrowHeadSize = ArrowHeadSize;
	Batch.MaxItems = MaxItems;
	Batch.Positions.Reserve(Positions.Num());
	for (const FVector& Position : Positions) Batch.Positions.Add(FVector3f(Position));
	return Batch;
}
} // namespace

UCSMeshGeneratorDebugComponent::UCSMeshGeneratorDebugComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
	CastShadow = false;
	bReceivesDecals = false;
}

int32 UCSMeshGeneratorDebugComponent::SetDirectionSource(
	const FCSSurfaceVoxelGPUBuffers& Source,
	float DirectionLength,
	FLinearColor DirectionColor,
	bool bDrawPoints,
	FLinearColor PointColor,
	int32 MaxDirectionsToDraw,
	float Duration,
	bool bPersistent)
{
	if (!Source.IsValid())
	{
		ClearDebug();
		return 0;
	}

	FCSMeshGeneratorDebugData NewData;
	NewData.Positions = Source.Positions;
	NewData.Normals = Source.Normals;
	NewData.Counter = Source.Counter;
	NewData.VoxelCapacity = Source.VoxelCapacity;
	NewData.MaxVoxelsToDraw = MaxDirectionsToDraw > 0
		? FMath::Min(MaxDirectionsToDraw, Source.VoxelCapacity)
		: Source.VoxelCapacity;
	NewData.VoxelSize = FMath::Max(Source.VoxelSize, UE_KINDA_SMALL_NUMBER);
	NewData.DirectionLength = FMath::Max(DirectionLength, UE_KINDA_SMALL_NUMBER);
	NewData.DirectionColor = DirectionColor;
	NewData.PointColor = PointColor;
	NewData.WorldBounds = Source.WorldBounds;
	NewData.Mode = ECSMeshGeneratorDebugMode::Directions;
	NewData.bDrawPoints = bDrawPoints;
	NewData.TakeBatchGeometryFrom(PendingData);
	SubmitData(MoveTemp(NewData));
	ScheduleClear(Duration, bPersistent);
	return PendingData.MaxVoxelsToDraw;
}

bool UCSMeshGeneratorDebugComponent::SetIsolatedQuadSource(
	const FCSSurfaceVoxelGPUBuffers& Source,
	float QuadScale,
	float NormalOffsetScale,
	bool bReverseOrientation)
{
	if (!Source.IsValid())
	{
		ClearDebug();
		return false;
	}

	FCSMeshGeneratorDebugData NewData;
	NewData.Positions = Source.Positions;
	NewData.Normals = Source.Normals;
	NewData.Counter = Source.Counter;
	NewData.VoxelCapacity = Source.VoxelCapacity;
	NewData.MaxVoxelsToDraw = Source.VoxelCapacity;
	NewData.VoxelSize = FMath::Max(Source.VoxelSize, UE_KINDA_SMALL_NUMBER);
	NewData.QuadScale = FMath::Max(QuadScale, UE_KINDA_SMALL_NUMBER);
	NewData.NormalOffsetScale = NormalOffsetScale;
	NewData.WorldBounds = Source.WorldBounds;
	NewData.Mode = ECSMeshGeneratorDebugMode::IsolatedQuads;
	NewData.bReverseOrientation = bReverseOrientation;
	NewData.TakeBatchGeometryFrom(PendingData);
	SubmitData(MoveTemp(NewData));
	ScheduleClear(0.0f, true);
	return true;
}

int32 UCSMeshGeneratorDebugComponent::SetPrimitiveBatches(TArray<FCSGpuDebugBatch> Batches, float Duration, bool bPersistent)
{
	PendingBatches = MoveTemp(Batches);
	return RebuildBatchGeometry(Duration, bPersistent);
}

int32 UCSMeshGeneratorDebugComponent::AddPrimitiveBatch(FCSGpuDebugBatch Batch, float Duration, bool bPersistent)
{
	PendingBatches.Add(MoveTemp(Batch));
	return RebuildBatchGeometry(Duration, bPersistent);
}

int32 UCSMeshGeneratorDebugComponent::DrawGpuDebugPoints(const TArray<FVector>& Points, FLinearColor Color,
	float Duration, bool bPersistent, int32 MaxItems)
{
	return AddPrimitiveBatch(CSMGD_MakeBatch(ECSGpuDebugPrimitive::Points, Points, Color, 0.0f, MaxItems),
		Duration, bPersistent);
}

int32 UCSMeshGeneratorDebugComponent::DrawGpuDebugLines(const TArray<FVector>& LineEndpoints, FLinearColor Color,
	float Duration, bool bPersistent, int32 MaxItems)
{
	return AddPrimitiveBatch(CSMGD_MakeBatch(ECSGpuDebugPrimitive::Lines, LineEndpoints, Color, 0.0f, MaxItems),
		Duration, bPersistent);
}

int32 UCSMeshGeneratorDebugComponent::DrawGpuDebugArrows(const TArray<FVector>& ArrowEndpoints, FLinearColor Color,
	float HeadSize, float Duration, bool bPersistent, int32 MaxItems)
{
	return AddPrimitiveBatch(CSMGD_MakeBatch(ECSGpuDebugPrimitive::Arrows, ArrowEndpoints, Color, HeadSize, MaxItems),
		Duration, bPersistent);
}

int32 UCSMeshGeneratorDebugComponent::DrawGpuDebugBoxes(const TArray<FVector>& BoxCorners, FLinearColor Color,
	float Duration, bool bPersistent, int32 MaxItems)
{
	return AddPrimitiveBatch(CSMGD_MakeBatch(ECSGpuDebugPrimitive::Boxes, BoxCorners, Color, 0.0f, MaxItems),
		Duration, bPersistent);
}

int32 UCSMeshGeneratorDebugComponent::RebuildBatchGeometry(float Duration, bool bPersistent)
{
	FCSMeshGeneratorDebugData NewData = PendingData;
	FCSGpuDebugDraw::BuildBatches(PendingBatches, NewData.BatchPositions, NewData.BatchIndices, NewData.BatchDraws);

	// Bounds come from the expanded geometry, so arrow heads and box corners are covered.
	NewData.BatchBounds = FBox(ForceInit);
	for (const FVector3f& Position : NewData.BatchPositions) NewData.BatchBounds += FVector(Position);

	if (!NewData.HasBatchGeometry() && !NewData.HasVoxelSource())
	{
		ClearDebug();
		return 0;
	}

	int32 ItemCount = 0;
	for (const FCSGpuDebugBatch& Batch : PendingBatches) ItemCount += Batch.NumItems();
	SubmitData(MoveTemp(NewData));
	ScheduleClear(Duration, bPersistent);
	return ItemCount;
}

void UCSMeshGeneratorDebugComponent::ClearDebug()
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(ClearTimerHandle);
	PendingBatches.Reset();
	PendingData = FCSMeshGeneratorDebugData();
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

FPrimitiveSceneProxy* UCSMeshGeneratorDebugComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5 || !PendingData.IsValid()) return nullptr;
	return new FCSMeshGeneratorDebugSceneProxy(this, PendingData);
}

FBoxSphereBounds UCSMeshGeneratorDebugComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// The voxel source only bounds the voxel centres, so it still needs the direction / quad
	// expansion; the batch bounds are already exact (built from the expanded geometry).
	FBox DebugBounds(ForceInit);
	if (PendingData.HasVoxelSource() && PendingData.WorldBounds.IsValid)
	{
		const float Expansion = PendingData.Mode == ECSMeshGeneratorDebugMode::Directions
			? FMath::Max(PendingData.DirectionLength, PendingData.VoxelSize)
			: PendingData.VoxelSize * FMath::Max(PendingData.QuadScale, 1.0f);
		DebugBounds = PendingData.WorldBounds.ExpandBy(Expansion);
	}
	if (PendingData.BatchBounds.IsValid)
		DebugBounds = DebugBounds.IsValid ? (DebugBounds + PendingData.BatchBounds) : PendingData.BatchBounds;
	if (!DebugBounds.IsValid) DebugBounds = FBox(FVector(-100.0), FVector(100.0));
	return FBoxSphereBounds(DebugBounds.TransformBy(LocalToWorld));
}

void UCSMeshGeneratorDebugComponent::SubmitData(FCSMeshGeneratorDebugData&& InData)
{
	PendingData = MoveTemp(InData);
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

void UCSMeshGeneratorDebugComponent::ScheduleClear(float Duration, bool bPersistent)
{
	UWorld* World = GetWorld();
	if (!World) return;
	World->GetTimerManager().ClearTimer(ClearTimerHandle);
	if (bPersistent) return;
	if (Duration <= 0.0f)
	{
		ClearTimerHandle = World->GetTimerManager().SetTimerForNextTick(this, &UCSMeshGeneratorDebugComponent::ClearDebug);
		return;
	}
	World->GetTimerManager().SetTimer(ClearTimerHandle, this, &UCSMeshGeneratorDebugComponent::ClearDebug,
		Duration, false);
}
