#include "CSInstanceBrushEdMode.h"

#include "MeshGeneratorBrushCache.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

const FEditorModeID FCSInstanceBrushEdMode::EM_CSInstanceBrush = TEXT("CSInstanceBrushEdMode");

void FCSInstanceBrushEdMode::SetTargetActor(AMeshGeneratorBrushCache* InTargetActor)
{
	TargetActor = InTargetActor;
	ResetBrushState();
}

AActor* FCSInstanceBrushEdMode::GetBrushTargetActor() const
{
	return TargetActor.Get();
}

FCSBrushSettings FCSInstanceBrushEdMode::GetBrushSettings() const
{
	FCSBrushSettings Settings;
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target) return Settings;

	Settings.Radius = Target->InstanceBrushRadius;
	Settings.TraceRadius = Target->InstanceBrushTraceRadius;
	Settings.MinSpacing = Target->InstanceBrushMinSpacing;
	Settings.SamplesPerMouseMove = Target->InstanceBrushSamplesPerMouseMove;
	Settings.PreviewPointSize = Target->InstanceBrushPreviewPointSize;
	Settings.PreviewLifetime = Target->InstanceBrushPreviewLifetime;
	Settings.bExitAfterCommit = Target->bInstanceBrushExitAfterCommit;
	return Settings;
}

void FCSInstanceBrushEdMode::CommitSamples(const TArray<FCSBrushSample>& Samples)
{
	AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target || !Target->InstanceBrushMesh) return;

	TArray<FTransform> Transforms;
	Transforms.Reserve(Samples.Num());
	for (const FCSBrushSample& Sample : Samples) Transforms.Add(BuildInstanceTransform(Sample));

	const int32 AddedCount = Target->CommitPaintInstances(Transforms, Target->InstanceBrushMesh);
	UE_LOG(LogTemp, Log, TEXT("[CSInstanceBrush] Added %d instances to %s."), AddedCount, *GetNameSafe(Target));
}

void FCSInstanceBrushEdMode::ClearBrushTarget()
{
	TargetActor.Reset();
}

bool FCSInstanceBrushEdMode::IsTooCloseToCommitted(const FVector& Location, float MinSpacingSq) const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target || !Target->InstanceBrushMesh) return false;

	const UHierarchicalInstancedStaticMeshComponent* PaintComponent = Target->FindPaintComponent(Target->InstanceBrushMesh);
	if (!PaintComponent) return false;

	for (int32 InstanceIndex = 0; InstanceIndex < PaintComponent->GetInstanceCount(); ++InstanceIndex)
	{
		FTransform InstanceTransform = FTransform::Identity;
		if (PaintComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true) &&
			FVector::DistSquared(InstanceTransform.GetLocation(), Location) < MinSpacingSq)
		{
			return true;
		}
	}

	return false;
}

bool FCSInstanceBrushEdMode::IsPointAllowed(const FVector& Location) const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	return Target && Target->IsInstanceBrushPointAllowed(Location);
}

bool FCSInstanceBrushEdMode::IsReadyToPaint() const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	return Target && Target->InstanceBrushMesh;
}

FTransform FCSInstanceBrushEdMode::BuildInstanceTransform(const FCSBrushSample& Sample) const
{
	const AMeshGeneratorBrushCache* Target = TargetActor.Get();
	if (!Target) return FTransform::Identity;

	const float RandomYawDegrees = FMath::FRandRange(0.0f, FMath::Max(0.0f, Target->InstanceBrushRandomYawDegrees));
	const FQuat Rotation = Target->bInstanceBrushAlignToNormal
		? FQuat(Sample.Normal, FMath::DegreesToRadians(RandomYawDegrees)) * FRotationMatrix::MakeFromZ(Sample.Normal).ToQuat()
		: FRotator(0.0f, RandomYawDegrees, 0.0f).Quaternion();

	const float MinScale = FMath::Min(Target->InstanceBrushUniformScaleRange.X, Target->InstanceBrushUniformScaleRange.Y);
	const float MaxScale = FMath::Max(Target->InstanceBrushUniformScaleRange.X, Target->InstanceBrushUniformScaleRange.Y);
	const float SafeMinScale = FMath::Max(UE_KINDA_SMALL_NUMBER, MinScale);
	const float SafeMaxScale = FMath::Max(SafeMinScale, MaxScale);
	const float UniformScale = FMath::FRandRange(SafeMinScale, SafeMaxScale);

	return FTransform(Rotation, Sample.Location, FVector(UniformScale));
}
