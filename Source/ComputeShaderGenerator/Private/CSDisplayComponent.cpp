#include "CSDisplayComponent.h"

#include "CSDisplayVoxelSceneProxy.h"
#include "CSGpuDebugDraw.h"
#include "ComputeShaderMeshGenerator.h" // FCSSurfaceVoxelGPUBuffers

#include "Engine/World.h"
#include "RHI.h"
#include "TimerManager.h"

UCSDisplayComponent::UCSDisplayComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// 显示的内容都是世界空间数据：以绝对变换渲染，使 local space == world space，
	// 位置 1:1 画出，不受宿主 Actor 变换影响。
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);

	// GPU-Scene 的实例剔除会覆盖 Virtual Shadow Map pass 里的自定义 indirect args，
	// 因此 indirect 绘制的网格无法投 VSM 阴影（road 也是同样的限制）。
	CastShadow = false;
	bReceivesDecals = false;
}

int32 UCSDisplayComponent::ShowVoxelDirections(
	const FCSGpuDebugPooledSource& Source,
	float DirectionLength,
	FLinearColor DirectionColor,
	bool bDrawPoints,
	FLinearColor PointColor,
	int32 MaxDirectionsToDraw,
	float Lifetime)
{
	if (!Source.IsValid())
	{
		ClearDisplay();
		return 0;
	}

	FCSDisplayVoxelData NewData;
	NewData.Positions = Source.Positions;
	NewData.Normals = Source.Normals;
	NewData.Counter = Source.Counter;
	NewData.VoxelCapacity = Source.Capacity;
	NewData.MaxVoxelsToDraw = MaxDirectionsToDraw > 0
		? FMath::Min(MaxDirectionsToDraw, Source.Capacity)
		: Source.Capacity;
	NewData.VoxelSize = FMath::Max(Source.ItemSize, UE_KINDA_SMALL_NUMBER);
	NewData.DirectionLength = FMath::Max(DirectionLength, UE_KINDA_SMALL_NUMBER);
	NewData.DirectionColor = DirectionColor;
	NewData.PointColor = PointColor;
	NewData.WorldBounds = Source.WorldBounds;
	NewData.Mode = ECSDisplayMode::VoxelDirections;
	NewData.bDrawPoints = bDrawPoints;
	SubmitVoxelData(MoveTemp(NewData), Lifetime);
	return PendingVoxelData.MaxVoxelsToDraw;
}

int32 UCSDisplayComponent::ShowVoxelDirections(
	const FCSSurfaceVoxelGPUBuffers& Source,
	float DirectionLength,
	FLinearColor DirectionColor,
	bool bDrawPoints,
	FLinearColor PointColor,
	int32 MaxDirectionsToDraw,
	float Lifetime)
{
	FCSGpuDebugPooledSource PooledSource;
	PooledSource.Positions = Source.Positions;
	PooledSource.Normals = Source.Normals;
	PooledSource.Counter = Source.Counter;
	PooledSource.Capacity = Source.VoxelCapacity;
	PooledSource.ItemSize = Source.VoxelSize;
	PooledSource.WorldBounds = Source.WorldBounds;
	return ShowVoxelDirections(PooledSource, DirectionLength, DirectionColor, bDrawPoints, PointColor,
		MaxDirectionsToDraw, Lifetime);
}

bool UCSDisplayComponent::ShowVoxelQuads(
	const FCSSurfaceVoxelGPUBuffers& Source,
	float QuadScale,
	float NormalOffsetScale,
	bool bReverseOrientation,
	float Lifetime)
{
	if (!Source.IsValid())
	{
		ClearDisplay();
		return false;
	}

	FCSDisplayVoxelData NewData;
	NewData.Positions = Source.Positions;
	NewData.Normals = Source.Normals;
	NewData.Counter = Source.Counter;
	NewData.VoxelCapacity = Source.VoxelCapacity;
	NewData.MaxVoxelsToDraw = Source.VoxelCapacity;
	NewData.VoxelSize = FMath::Max(Source.VoxelSize, UE_KINDA_SMALL_NUMBER);
	NewData.QuadScale = FMath::Max(QuadScale, UE_KINDA_SMALL_NUMBER);
	NewData.NormalOffsetScale = NormalOffsetScale;
	NewData.WorldBounds = Source.WorldBounds;
	NewData.Mode = ECSDisplayMode::VoxelQuads;
	NewData.bReverseOrientation = bReverseOrientation;
	SubmitVoxelData(MoveTemp(NewData), Lifetime);
	return true;
}

void UCSDisplayComponent::ClearDisplay()
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(ClearTimerHandle);
	Mode = ECSDisplayMode::None;
	PendingVoxelData = FCSDisplayVoxelData();
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

FPrimitiveSceneProxy* UCSDisplayComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;

	switch (Mode)
	{
	case ECSDisplayMode::VoxelDirections:
	case ECSDisplayMode::VoxelQuads:
		if (!PendingVoxelData.IsValid()) return nullptr;
		return new FCSDisplayVoxelSceneProxy(this, PendingVoxelData);

	default:
		return nullptr;
	}
}

FBoxSphereBounds UCSDisplayComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// 空闲时（Mode == None）以及源没有交出包围盒时，退回一个固定小盒 —— 组件仍然注册在场景里，
	// 交出一个无效包围盒会让渲染端每帧对它做无意义的处理。
	const bool bHasVoxelBounds = Mode != ECSDisplayMode::None && PendingVoxelData.WorldBounds.IsValid;
	if (!bHasVoxelBounds) return FBoxSphereBounds(FBox(FVector(-100.0), FVector(100.0)).TransformBy(LocalToWorld));

	// 体素形状按其绘制尺寸外扩：方向线伸出 DirectionLength，面片铺开 VoxelSize * QuadScale。
	const float Expansion = Mode == ECSDisplayMode::VoxelDirections
		? FMath::Max(PendingVoxelData.DirectionLength, PendingVoxelData.VoxelSize)
		: PendingVoxelData.VoxelSize * FMath::Max(PendingVoxelData.QuadScale, 1.0f);
	return FBoxSphereBounds(PendingVoxelData.WorldBounds.ExpandBy(Expansion).TransformBy(LocalToWorld));
}

void UCSDisplayComponent::SubmitVoxelData(FCSDisplayVoxelData&& InData, float Lifetime)
{
	Mode = InData.Mode;
	PendingVoxelData = MoveTemp(InData);
	RecreateRenderState_Concurrent();
	UpdateBounds();
	ScheduleClear(Lifetime);
}

void UCSDisplayComponent::ScheduleClear(float Lifetime)
{
	UWorld* World = GetWorld();
	if (!World) return;
	World->GetTimerManager().ClearTimer(ClearTimerHandle);
	if (Lifetime < 0.0f) return; // 常驻
	if (Lifetime == 0.0f)
	{
		// 一帧可视：下一帧清除。
		ClearTimerHandle = World->GetTimerManager().SetTimerForNextTick(this, &UCSDisplayComponent::ClearDisplay);
		return;
	}
	World->GetTimerManager().SetTimer(ClearTimerHandle, this, &UCSDisplayComponent::ClearDisplay, Lifetime, false);
}
