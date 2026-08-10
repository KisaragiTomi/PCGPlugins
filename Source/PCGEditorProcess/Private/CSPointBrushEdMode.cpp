#include "CSPointBrushEdMode.h"

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
	Settings.PreviewPointSize = Target->PreviewPointSize;
	Settings.PreviewLifetime = Target->PreviewLifetime;
	Settings.PreviewColor = Target->PreviewPointColor;
	Settings.bExitAfterCommit = Target->bExitAfterCommit;
	return Settings;
}

void FCSPointBrushEdMode::CommitSamples(const TArray<FCSBrushSample>& Samples)
{
	ACSPointBrushActor* Target = TargetActor.Get();
	if (!Target) return;

	TArray<FCSBrushPoint> Points;
	Points.Reserve(Samples.Num());
	for (const FCSBrushSample& Sample : Samples)
	{
		FCSBrushPoint& Point = Points.AddDefaulted_GetRef();
		Point.Position = Sample.Location;
		Point.Normal = Sample.Normal;
	}

	const int32 AddedCount = Target->AppendBrushPoints(Points);
	UE_LOG(LogTemp, Log, TEXT("[CSPointBrush] Added %d points to %s (total %d)."),
		AddedCount, *GetNameSafe(Target), Target->GetBrushPointCount());
}

bool FCSPointBrushEdMode::IsTooCloseToCommitted(const FVector& Location, float MinSpacingSq) const
{
	const ACSPointBrushActor* Target = TargetActor.Get();
	if (!Target) return false;

	for (const FCSBrushPoint& Point : Target->PaintedPoints)
	{
		if (FVector::DistSquared(Point.Position, Location) < MinSpacingSq) return true;
	}
	return false;
}

bool FCSPointBrushEdMode::IsPointAllowed(const FVector& Location) const
{
	const ACSPointBrushActor* Target = TargetActor.Get();
	return Target && Target->IsBrushPointAllowed(Location);
}
