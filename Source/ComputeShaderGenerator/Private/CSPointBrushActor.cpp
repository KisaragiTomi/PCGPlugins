#include "CSPointBrushActor.h"

#include "CSDisplayComponent.h"
#include "CSGpuInstancedMeshComponent.h"

#include "Engine/World.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "TimerManager.h"

FCSPointBrushEditorRequest ACSPointBrushActor::OnPointBrushEditorRequest;

namespace
{
// Allocates the pooled float4 / float4 / uint2 triple at Capacity and seeds the first Positions.Num()
// slots with the given points; the rest of the buffer is what the brush's compute pass appends into.
// Game thread: the allocation itself is render-thread only, so the work is enqueued and the call
// blocks until the pooled refs are valid — the same shape every other producer in this plugin uses.
bool UploadBrushPointsToGPU(
	TArray<FVector4f> Positions,
	TArray<FVector4f> Normals,
	int32 Capacity,
	FCSGpuDebugPooledSource& OutSource)
{
	OutSource.Reset();

	const int32 PointCount = Positions.Num();
	if (Capacity <= 0 || PointCount > Capacity || Normals.Num() != PointCount) return false;

	// [0] is what the debug pass and any indirect dispatch read as the live count, and what the
	// brush's append pass advances; [1] carries the allocated capacity so a consumer can clamp
	// without asking the CPU.
	const TArray<uint32> CounterData = { uint32(PointCount), uint32(Capacity) };

	TRefCountPtr<FRDGPooledBuffer> PositionBuffer;
	TRefCountPtr<FRDGPooledBuffer> NormalBuffer;
	TRefCountPtr<FRDGPooledBuffer> CounterBuffer;

	ENQUEUE_RENDER_COMMAND(CSPointBrushUpload)(
		[Positions = MoveTemp(Positions), Normals = MoveTemp(Normals), CounterData,
		 Capacity, &PositionBuffer, &NormalBuffer, &CounterBuffer](FRHICommandListImmediate& RHICmdList)
		{
			// CreateBufferDesc (not structured): the debug passes view these as typed
			// Buffer<float4> / Buffer<uint>, which a structured buffer cannot back.
			PositionBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), uint32(Capacity)), TEXT("CS.PointBrush.Positions"));
			NormalBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), uint32(Capacity)), TEXT("CS.PointBrush.Normals"));
			CounterBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("CS.PointBrush.Counter"));

			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSPointBrush.Upload"));
			FRDGBufferRef PositionRef = GraphBuilder.RegisterExternalBuffer(PositionBuffer);
			FRDGBufferRef NormalRef = GraphBuilder.RegisterExternalBuffer(NormalBuffer);
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(CounterBuffer);

			if (!Positions.IsEmpty())
			{
				GraphBuilder.QueueBufferUpload(PositionRef, Positions.GetData(), Positions.Num() * sizeof(FVector4f));
				GraphBuilder.QueueBufferUpload(NormalRef, Normals.GetData(), Normals.Num() * sizeof(FVector4f));
			}
			GraphBuilder.QueueBufferUpload(CounterRef, CounterData.GetData(), CounterData.Num() * sizeof(uint32));

			// Consumers register these into their own graph and read them as SRVs.
			GraphBuilder.SetBufferAccessFinal(PositionRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(NormalRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});

	FlushRenderingCommands();

	OutSource.Positions = MoveTemp(PositionBuffer);
	OutSource.Normals = MoveTemp(NormalBuffer);
	OutSource.Counter = MoveTemp(CounterBuffer);
	OutSource.Capacity = Capacity;
	return OutSource.IsValid();
}

// Hands the last reference to the render thread so the pool reclaims the memory behind any frame
// still reading it, instead of dropping it out from under an in-flight draw on the game thread.
void ReleasePooledSourceOnRenderThread(FCSGpuDebugPooledSource& Source)
{
	if (!Source.Positions.IsValid() && !Source.Normals.IsValid() && !Source.Counter.IsValid())
	{
		Source.Reset();
		return;
	}

	ENQUEUE_RENDER_COMMAND(CSPointBrushRelease)(
		[ReleasedSource = MoveTemp(Source)](FRHICommandListImmediate&) mutable
		{
			ReleasedSource.Reset();
		});

	Source.Reset();
}
}

ACSPointBrushActor::ACSPointBrushActor()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	PointInstanceComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("PointInstances"));
	PointInstanceComponent->SetupAttachment(Root);

	PointArrowComponent = CreateDefaultSubobject<UCSDisplayComponent>(TEXT("PointArrows"));
	PointArrowComponent->SetupAttachment(Root);
}

void ACSPointBrushActor::StartPointBrush()
{
	OnPointBrushEditorRequest.Broadcast(this);
}

int32 ACSPointBrushActor::AppendBrushPoints(const TArray<FCSBrushPoint>& NewPoints)
{
	const int32 Capacity = FMath::Max(1, MaxPointCount);
	const int32 AddCount = FMath::Clamp(NewPoints.Num(), 0, Capacity - PaintedPoints.Num());
	if (AddCount <= 0)
	{
		if (!NewPoints.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSPointBrush] %s is at its %d point cap; %d points dropped."),
				*GetActorNameOrLabel(), Capacity, NewPoints.Num());
		}
		return 0;
	}

	if (AddCount < NewPoints.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSPointBrush] %s hit its %d point cap; %d of %d points dropped."),
			*GetActorNameOrLabel(), Capacity, NewPoints.Num() - AddCount, NewPoints.Num());
	}

	Modify();
	PaintedPoints.Append(NewPoints.GetData(), AddCount);
	MarkPackageDirty();

	RebuildPointBuffer();
	return AddCount;
}

void ACSPointBrushActor::ClearBrushPoints()
{
	Modify();
	PaintedPoints.Reset();
	MarkPackageDirty();
	ReleasePointBuffer();
}

bool ACSPointBrushActor::RebuildPointBuffer()
{
	ReleasePointBuffer();

	// Allocated at the full cap rather than at PaintedPoints.Num(): the brush's compute pass appends
	// into the tail of this same buffer, and it cannot grow it from the render thread.
	const int32 Capacity = FMath::Max(1, MaxPointCount);
	const int32 PointCount = FMath::Min(PaintedPoints.Num(), Capacity);
	TArray<FVector4f> Positions;
	TArray<FVector4f> Normals;
	Positions.Reserve(PointCount);
	Normals.Reserve(PointCount);
	for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
	{
		const FCSBrushPoint& Point = PaintedPoints[PointIndex];
		const FVector3f Position(Point.Position);
		const FVector3f Normal = FVector3f(Point.Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector));
		Positions.Emplace(Position.X, Position.Y, Position.Z, 1.0f);
		Normals.Emplace(Normal.X, Normal.Y, Normal.Z, 0.0f);
	}

	if (!UploadBrushPointsToGPU(MoveTemp(Positions), MoveTemp(Normals), Capacity, PointBuffers))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSPointBrush] Failed to allocate a %d point buffer on %s."), Capacity, *GetActorNameOrLabel());
		return false;
	}

	GpuPointCountUpperBound = PointCount;
	PointBuffers.WorldBounds = ComputePaintedPointBounds();
	PointBuffers.ItemSize = FMath::Max(ArrowLength, 1.0f);
	RefreshDebugDraw();
	return true;
}

bool ACSPointBrushActor::EnsureGpuPointBuffer()
{
	if (PointBuffers.IsValid() && PointBuffers.Capacity >= FMath::Max(1, MaxPointCount)) return true;
	return RebuildPointBuffer();
}

void ACSPointBrushActor::NotifyGpuPointsRequested(int32 CandidateCount, const FVector& InBrushCentre, float InBrushRadius)
{
	if (!PointBuffers.IsValid()) return;

	GpuPointCountUpperBound = FMath::Min(GpuPointCountUpperBound + FMath::Max(0, CandidateCount), PointBuffers.Capacity);

	// Every sample lands inside the brush sphere, so unioning the sphere is the tightest bound the
	// CPU can still know without reading the points back.
	const FVector Extent(FMath::Max(1.0f, InBrushRadius));
	PointBuffers.WorldBounds += FBox(InBrushCentre - Extent, InBrushCentre + Extent);
}

int32 ACSPointBrushActor::GetDisplayPointCount() const
{
	const int32 Capacity = FMath::Max(1, PointBuffers.Capacity);
	return FMath::Clamp(FMath::Max(PaintedPoints.Num(), GpuPointCountUpperBound), 1, Capacity);
}

void ACSPointBrushActor::RefreshDebugDraw()
{
	// InstanceMesh 决定走哪条显示路径；另一条一定清空，避免两份几何叠在一起。
	if (!InstanceMesh)
	{
		if (PointInstanceComponent) PointInstanceComponent->ClearInstances();
		if (!PointArrowComponent) return;

		if (!PointBuffers.IsValid())
		{
			PointArrowComponent->ClearDisplay();
			return;
		}

		// 每个点一个沿法线朝向的箭头，由 compute pass 直接从 PointBuffers 生成。
		// 箭头几何按 CPU 侧的点数上界分配（真计数只在 GPU 上），画多少仍由 GPU 计数决定。
		const int32 DisplayCount = GetDisplayPointCount();
		PointArrowComponent->ShowPointArrows(
			PointBuffers,
			(MaxPointsToDraw > 0) ? FMath::Min(MaxPointsToDraw, DisplayCount) : DisplayCount,
			FMath::Max(ArrowLength, 1.0f),
			PointColor,
			/*Lifetime*/ -1.0f); // 常驻，跟随笔刷数据存在
		return;
	}

	if (PointArrowComponent) PointArrowComponent->ClearDisplay();
	if (!PointInstanceComponent) return;

	if (!PointBuffers.IsValid())
	{
		PointInstanceComponent->ClearInstances();
		return;
	}

	PointInstanceComponent->InstanceMaterial = InstanceMaterial;
	PointInstanceComponent->SetBaseMesh(InstanceMesh);

	// 驱动源是 PointBuffers 而不是 PaintedPoints：笔刷只往 GPU buffer 里追加，CPU 数组早已不是
	// 全集。组件每帧从这对 buffer 现打实例行，所以这里只需交出引用，追加多少它自己就画多少。
	FCSGpuInstancePointSourceGPU Source;
	Source.Positions = PointBuffers.Positions;
	Source.Normals = PointBuffers.Normals;
	Source.Counter = PointBuffers.Counter;
	Source.Capacity = uint32(FMath::Max(PointBuffers.Capacity, 0));
	Source.InstanceScale = FMath::Max(InstanceScale, UE_KINDA_SMALL_NUMBER);
	Source.WorldBounds = PointBuffers.WorldBounds;

	PointInstanceComponent->SetInstanceSourceFromPoints(Source);
}

void ACSPointBrushActor::ReleasePointBuffer()
{
	if (PointInstanceComponent) PointInstanceComponent->ClearInstances();
	if (PointArrowComponent) PointArrowComponent->ClearDisplay();
	ReleasePooledSourceOnRenderThread(PointBuffers);
	GpuPointCountUpperBound = 0;
}

bool ACSPointBrushActor::IsBrushPointAllowed(const FVector& WorldPosition) const
{
	if (!bUsePaintBounds) return true;
	const FBox Bounds = GetPaintBoundsWorldBox();
	return Bounds.IsValid && Bounds.IsInsideOrOn(WorldPosition);
}

FBox ACSPointBrushActor::GetPaintBoundsWorldBox() const
{
	const FVector Extent = PaintBoundsExtent.GetAbs();
	return FBox(GetActorLocation() - Extent, GetActorLocation() + Extent);
}

FBox ACSPointBrushActor::ComputePaintedPointBounds() const
{
	FBox Bounds(ForceInit);
	for (const FCSBrushPoint& Point : PaintedPoints) Bounds += Point.Position;
	return Bounds;
}

void ACSPointBrushActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	if (IsTemplate() || PaintedPoints.IsEmpty()) return;

	// Deferred by one tick: this runs during level load, where blocking on the render thread would
	// stall the load and the renderer may not be ready to serve the upload yet.
	UWorld* World = GetWorld();
	if (!World) return;
	World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		// The display is rebuilt from PaintedPoints either way; bRebuildBufferOnLoad only governs
		// the GPU mirror, which exists for compute consumers rather than for drawing.
		if (bRebuildBufferOnLoad && !PointBuffers.IsValid()) RebuildPointBuffer();
		else RefreshDebugDraw();
	}));
}

void ACSPointBrushActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ReleasePointBuffer();
	Super::EndPlay(EndPlayReason);
}

void ACSPointBrushActor::Destroyed()
{
	ReleasePointBuffer();
	Super::Destroyed();
}

void ACSPointBrushActor::BeginDestroy()
{
	// Last line of defence: closing the level and shutting the engine down both reach the actor
	// through GC rather than Destroyed(). The debug component is torn down by GC on its own, so
	// only the pooled refs are handed over here.
	ReleasePooledSourceOnRenderThread(PointBuffers);
	Super::BeginDestroy();
}

#if WITH_EDITOR
void ACSPointBrushActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	static const TSet<FName> RedrawProperties = {
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, PointColor),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, ArrowLength),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, MaxPointsToDraw),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, InstanceMesh),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, InstanceMaterial),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, InstanceScale),
	};

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (RedrawProperties.Contains(PropertyName)) RefreshDebugDraw();
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, MaxPointCount)) RebuildPointBuffer();
}
#endif
