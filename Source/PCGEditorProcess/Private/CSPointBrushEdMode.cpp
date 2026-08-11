#include "CSPointBrushEdMode.h"

#include "CSDepthBrushSampleService.h"
#include "CSPointBrushActor.h"

const FEditorModeID FCSPointBrushEdMode::EM_CSPointBrush = TEXT("CSPointBrushEdMode");

void FCSPointBrushEdMode::SetTargetActor(ACSPointBrushActor* InTargetActor)
{
	TargetActor = InTargetActor;
	ResetBrushState();
}

AActor* FCSPointBrushEdMode::GetBrushTargetActor() const
{
	return TargetActor.Get();
}

FCSBrushSettings FCSPointBrushEdMode::GetBrushSettings() const
{
	FCSBrushSettings Settings;
	const ACSPointBrushActor* Target = TargetActor.Get();
	if (!Target) return Settings;

	Settings.Radius = Target->BrushRadius;
	Settings.TraceRadius = Target->TraceRadius;
	Settings.MinSpacing = Target->MinSpacing;
	Settings.SamplesPerMouseMove = Target->SamplesPerMouseMove;
	Settings.bExitAfterCommit = Target->bExitAfterCommit;
	return Settings;
}

void FCSPointBrushEdMode::SamplePendingPoints()
{
	ACSPointBrushActor* Target = TargetActor.Get();
	if (!Target || !IsBrushTraceValid()) return;

	// The pass appends into this buffer from the render thread, so it has to exist — and be big
	// enough — before the request goes out.
	if (!Target->EnsureGpuPointBuffer()) return;

	const FCSBrushSettings Settings = GetBrushSettings();
	const FVector BrushCentre = GetBrushLocation();
	const float BrushRadius = FMath::Max(1.0f, Settings.Radius);
	const int32 SampleCount = FMath::Max(1, Settings.SamplesPerMouseMove);

	FCSDepthBrushSampleRequest Request;
	Request.Output = Target->GetPointBuffers();
	Request.ViewState = GetBrushViewState();
	Request.BrushCentre = BrushCentre;
	Request.BrushRadius = BrushRadius;
	Request.MinSpacing = Settings.MinSpacing;
	Request.SampleCount = SampleCount;
	Request.RandomSeed = ++SampleSequence;
	Request.PaintBounds = Target->bUsePaintBounds ? Target->GetPaintBoundsWorldBox() : FBox(ForceInit);

	// Fired once the pass is in the frame's graph, so the display rebuild that follows is ordered
	// behind it on the render thread and picks up the points this update just added.
	Request.OnDispatched = [WeakTarget = TWeakObjectPtr<ACSPointBrushActor>(Target)]()
	{
		if (ACSPointBrushActor* RefreshTarget = WeakTarget.Get()) RefreshTarget->RefreshDebugDraw();
	};

	FCSDepthBrushSampleService::Get().EnqueueSample(MoveTemp(Request));

	// The GPU decides how many candidates survive; the CPU only gets to know the ceiling, which is
	// what the arrow geometry and the draw bounds are sized from.
	Target->NotifyGpuPointsRequested(SampleCount, BrushCentre, BrushRadius);
}

void FCSPointBrushEdMode::CommitSamples(const TArray<FCSBrushSample>& /*Samples*/)
{
	// Sampling already appended everything; committing is only about making sure the display shows
	// the last update even if its dispatch callback was dropped with the request.
	if (ACSPointBrushActor* Target = TargetActor.Get()) Target->RefreshDebugDraw();
}

void FCSPointBrushEdMode::ClearBrushTarget()
{
	TargetActor.Reset();
}

bool FCSPointBrushEdMode::IsPointAllowed(const FVector& Location) const
{
	const ACSPointBrushActor* Target = TargetActor.Get();
	return Target && Target->IsBrushPointAllowed(Location);
}
