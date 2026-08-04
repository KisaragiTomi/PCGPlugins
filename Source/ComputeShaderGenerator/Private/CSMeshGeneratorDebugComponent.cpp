#include "CSMeshGeneratorDebugComponent.h"
#include "CSMeshGeneratorDebugSceneProxy.h"
#include "ComputeShaderMeshGenerator.h"

#include "Engine/World.h"
#include "TimerManager.h"

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
	SubmitData(MoveTemp(NewData));
	ScheduleClear(0.0f, true);
	return true;
}

void UCSMeshGeneratorDebugComponent::ClearDebug()
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(ClearTimerHandle);
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
	FBox DebugBounds = PendingData.WorldBounds;
	if (!DebugBounds.IsValid) DebugBounds = FBox(FVector(-100.0), FVector(100.0));
	const float Expansion = PendingData.Mode == ECSMeshGeneratorDebugMode::Directions
		? FMath::Max(PendingData.DirectionLength, PendingData.VoxelSize)
		: PendingData.VoxelSize * FMath::Max(PendingData.QuadScale, 1.0f);
	return FBoxSphereBounds(DebugBounds.ExpandBy(Expansion).TransformBy(LocalToWorld));
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
