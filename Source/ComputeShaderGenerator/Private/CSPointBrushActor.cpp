#include "CSPointBrushActor.h"

#include "CSDisplayComponent.h"

#include "Engine/World.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "TimerManager.h"

FCSPointBrushEditorRequest ACSPointBrushActor::OnPointBrushEditorRequest;

namespace
{
// Allocates the pooled float4 / float4 / uint2 triple and uploads the painted set into it.
// Game thread: the allocation itself is render-thread only, so the work is enqueued and the call
// blocks until the pooled refs are valid — the same shape every other producer in this plugin uses.
bool UploadBrushPointsToGPU(
	TArray<FVector4f> Positions,
	TArray<FVector4f> Normals,
	FCSGpuDebugPooledSource& OutSource)
{
	OutSource.Reset();

	const int32 PointCount = Positions.Num();
	if (PointCount <= 0 || Normals.Num() != PointCount) return false;

	// [0] is what the debug pass and any indirect dispatch read as the live count; [1] carries the
	// allocated capacity so a consumer can clamp without asking the CPU.
	const TArray<uint32> CounterData = { uint32(PointCount), uint32(PointCount) };

	TRefCountPtr<FRDGPooledBuffer> PositionBuffer;
	TRefCountPtr<FRDGPooledBuffer> NormalBuffer;
	TRefCountPtr<FRDGPooledBuffer> CounterBuffer;

	ENQUEUE_RENDER_COMMAND(CSPointBrushUpload)(
		[Positions = MoveTemp(Positions), Normals = MoveTemp(Normals), CounterData,
		 PointCount, &PositionBuffer, &NormalBuffer, &CounterBuffer](FRHICommandListImmediate& RHICmdList)
		{
			const uint32 Capacity = uint32(PointCount);
			// CreateBufferDesc (not structured): the debug passes view these as typed
			// Buffer<float4> / Buffer<uint>, which a structured buffer cannot back.
			PositionBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Capacity), TEXT("CS.PointBrush.Positions"));
			NormalBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Capacity), TEXT("CS.PointBrush.Normals"));
			CounterBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("CS.PointBrush.Counter"));

			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSPointBrush.Upload"));
			FRDGBufferRef PositionRef = GraphBuilder.RegisterExternalBuffer(PositionBuffer);
			FRDGBufferRef NormalRef = GraphBuilder.RegisterExternalBuffer(NormalBuffer);
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(CounterBuffer);

			GraphBuilder.QueueBufferUpload(PositionRef, Positions.GetData(), Positions.Num() * sizeof(FVector4f));
			GraphBuilder.QueueBufferUpload(NormalRef, Normals.GetData(), Normals.Num() * sizeof(FVector4f));
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
	OutSource.Capacity = PointCount;
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

	PointDebugComponent = CreateDefaultSubobject<UCSDisplayComponent>(TEXT("PointDebug"));
	PointDebugComponent->SetupAttachment(Root);
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
	if (PaintedPoints.IsEmpty()) return false;

	const int32 PointCount = FMath::Min(PaintedPoints.Num(), FMath::Max(1, MaxPointCount));
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

	if (!UploadBrushPointsToGPU(MoveTemp(Positions), MoveTemp(Normals), PointBuffers))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSPointBrush] Failed to upload %d points on %s."), PointCount, *GetActorNameOrLabel());
		return false;
	}

	PointBuffers.WorldBounds = ComputePaintedPointBounds();
	PointBuffers.ItemSize = FMath::Max(NormalLength, 1.0f);
	RefreshDebugDraw();
	return true;
}

void ACSPointBrushActor::RefreshDebugDraw()
{
	if (!PointDebugComponent) return;
	if (!PointBuffers.IsValid())
	{
		PointDebugComponent->ClearDisplay();
		return;
	}

	// The shape is always "one line plus one point per item"; a near-zero line length is how a
	// points-only visual is expressed, since the line then covers no pixels.
	const float DirectionLength = bDrawNormals ? FMath::Max(NormalLength, 1.0f) : UE_KINDA_SMALL_NUMBER;
	PointDebugComponent->ShowVoxelDirections(
		PointBuffers,
		DirectionLength,
		NormalColor,
		true,
		PointColor,
		MaxPointsToDraw,
		/*Lifetime*/ -1.0f); // 常驻，跟随笔刷数据存在
}

void ACSPointBrushActor::ReleasePointBuffer()
{
	if (PointDebugComponent) PointDebugComponent->ClearDisplay();
	ReleasePooledSourceOnRenderThread(PointBuffers);
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

	if (IsTemplate() || !bRebuildBufferOnLoad || PaintedPoints.IsEmpty() || PointBuffers.IsValid()) return;

	// Deferred by one tick: this runs during level load, where blocking on the render thread would
	// stall the load and the renderer may not be ready to serve the upload yet.
	UWorld* World = GetWorld();
	if (!World) return;
	World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		RebuildPointBuffer();
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
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, NormalColor),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, bDrawNormals),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, NormalLength),
		GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, MaxPointsToDraw),
	};

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (RedrawProperties.Contains(PropertyName)) RefreshDebugDraw();
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACSPointBrushActor, MaxPointCount)) RebuildPointBuffer();
}
#endif
