#include "RoadMeshComponent.h"
#include "RoadMeshSceneProxy.h"
#include "Materials/Material.h"
#include "RHI.h"

URoadMeshComponent::URoadMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// GPU-Scene instance culling overrides custom indirect args in the
	// Virtual Shadow Map / cube-shadow passes, so indirect-drawn roads cannot
	// cast VSM shadows. Roads hug the ground anyway; keep shadows off.
	CastShadow = false;
}

void URoadMeshComponent::SetBuildInput(FRoadBuildInput&& Input)
{
	LocalBounds = Input.LocalBounds;
	PendingInput = MoveTemp(Input);
	MarkRenderStateDirty();
	UpdateBounds();
}

FPrimitiveSceneProxy* URoadMeshComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;
	if (PendingInput.Splines.Num() == 0 || PendingInput.MaxVertices == 0) return nullptr;
	return new FRoadMeshSceneProxy(this, PendingInput);
}

FBoxSphereBounds URoadMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FBox Box = LocalBounds.IsValid ? LocalBounds : FBox(FVector(-100.0), FVector(100.0));
	return FBoxSphereBounds(Box.TransformBy(LocalToWorld));
}

void URoadMeshComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (RoadMaterial) OutMaterials.Add(RoadMaterial);
}
