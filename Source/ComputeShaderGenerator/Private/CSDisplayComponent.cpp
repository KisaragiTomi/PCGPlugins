#include "CSDisplayComponent.h"

#include "CSDisplayPointArrowSceneProxy.h"
#include "CSDisplayTriangleSceneProxy.h"
#include "CSDisplayVoxelSceneProxy.h"
#include "CSGpuDebugDraw.h"

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

void UCSDisplayComponent::ShowTriangleSoup(const FCSBoxScenePreparedData& InPrepared, uint32 InVertexCapacity,
	const FBox& InWorldBounds, float Lifetime)
{
	if (!InPrepared.IsValid() || !InPrepared.HasAnyTriangles() || InVertexCapacity == 0)
	{
		ClearDisplay();
		return;
	}

	PendingVoxelData = FCSDisplayVoxelData();
	PendingPrepared = InPrepared;
	PendingVertexCapacity = InVertexCapacity;
	Mode = ECSDisplayMode::TriangleSoup;
	LocalBounds = InWorldBounds; // 绝对变换 => local bounds 即 world bounds

	// 提交后紧接着存盘的调用方必须看到新代理。MarkRenderRenderStateDirty 会把重建推迟到
	// game thread 的帧末更新，而单靠 FlushRenderingCommands 并不会处理那一步。
	RecreateRenderState_Concurrent();
	UpdateBounds();
	ScheduleClear(Lifetime);
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

int32 UCSDisplayComponent::ShowPointArrows(
	const FCSGpuDebugPooledSource& Source,
	int32 MaxArrowsToDraw,
	float ArrowLength,
	FLinearColor ArrowColor,
	float Lifetime)
{
	if (!Source.IsValid())
	{
		ClearDisplay();
		return 0;
	}

	FCSDisplayPointArrowData NewData;
	NewData.Positions = Source.Positions;
	NewData.Normals = Source.Normals;
	NewData.Counter = Source.Counter;
	NewData.MaxArrowsToDraw = MaxArrowsToDraw > 0
		? FMath::Min(MaxArrowsToDraw, Source.Capacity)
		: Source.Capacity;
	NewData.ArrowLength = FMath::Max(ArrowLength, UE_KINDA_SMALL_NUMBER);
	// 柱身/锥头比例按总长派生，调用方只需给一个长度就能得到形状合理的箭头。
	NewData.ShaftRadius = NewData.ArrowLength * 0.06f;
	NewData.HeadRadius = NewData.ArrowLength * 0.16f;
	NewData.HeadFraction = 0.35f;
	NewData.ArrowColor = ArrowColor;
	NewData.WorldBounds = Source.WorldBounds;

	// 切换到箭头模式：清掉其它两种源，保持"一个实例一次显示一种"的约定。
	PendingPrepared = FCSBoxScenePreparedData();
	PendingVertexCapacity = 0;
	PendingVoxelData = FCSDisplayVoxelData();
	PendingArrowData = MoveTemp(NewData);
	Mode = ECSDisplayMode::PointArrows;

	// 箭头会沿法线伸出总长，包围盒需按此外扩，否则会被过早剔除。
	LocalBounds = PendingArrowData.WorldBounds.IsValid
		? PendingArrowData.WorldBounds.ExpandBy(PendingArrowData.ArrowLength)
		: FBox(FVector(-100.0), FVector(100.0));

	RecreateRenderState_Concurrent();
	UpdateBounds();
	ScheduleClear(Lifetime);
	return PendingArrowData.MaxArrowsToDraw;
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
	PendingPrepared = FCSBoxScenePreparedData();
	PendingVertexCapacity = 0;
	PendingVoxelData = FCSDisplayVoxelData();
	PendingArrowData = FCSDisplayPointArrowData();
	LocalBounds = FBox(ForceInit);
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

FPrimitiveSceneProxy* UCSDisplayComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;

	switch (Mode)
	{
	case ECSDisplayMode::TriangleSoup:
		if (!PendingPrepared.IsValid() || !PendingPrepared.HasAnyTriangles() || PendingVertexCapacity == 0) return nullptr;
		return new FCSDisplayTriangleSceneProxy(this, PendingPrepared, PendingVertexCapacity);

	case ECSDisplayMode::PointArrows:
		if (!PendingArrowData.IsValid()) return nullptr;
		return new FCSDisplayPointArrowSceneProxy(this, PendingArrowData);

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
	// 三角汤的包围盒由 LocalBounds 承载，交给基类；体素形状需按其绘制尺寸外扩。
	if (Mode != ECSDisplayMode::VoxelDirections && Mode != ECSDisplayMode::VoxelQuads)
	{
		return Super::CalcBounds(LocalToWorld);
	}

	FBox DisplayBounds = PendingVoxelData.WorldBounds;
	if (!DisplayBounds.IsValid) DisplayBounds = FBox(FVector(-100.0), FVector(100.0));
	const float Expansion = Mode == ECSDisplayMode::VoxelDirections
		? FMath::Max(PendingVoxelData.DirectionLength, PendingVoxelData.VoxelSize)
		: PendingVoxelData.VoxelSize * FMath::Max(PendingVoxelData.QuadScale, 1.0f);
	return FBoxSphereBounds(DisplayBounds.ExpandBy(Expansion).TransformBy(LocalToWorld));
}

void UCSDisplayComponent::SubmitVoxelData(FCSDisplayVoxelData&& InData, float Lifetime)
{
	PendingPrepared = FCSBoxScenePreparedData();
	PendingVertexCapacity = 0;
	PendingArrowData = FCSDisplayPointArrowData();
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
