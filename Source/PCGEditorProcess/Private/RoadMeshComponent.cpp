#include "RoadMeshComponent.h"
#include "RoadMeshSceneProxy.h"
#include "RHI.h"

#if WITH_EDITOR
#include "CSGpuMeshSave.h"
#endif

URoadMeshComponent::URoadMeshComponent()
{
	// GPU-Scene instance culling overrides custom indirect args in the
	// Virtual Shadow Map / cube-shadow passes, so indirect-drawn roads cannot
	// cast VSM shadows. Roads hug the ground anyway; keep shadows off.
	CastShadow = false;
}

void URoadMeshComponent::SetBuildInput(FRoadBuildInput&& Input)
{
	LocalBounds = Input.LocalBounds;
	PendingInput = MoveTemp(Input);
	// Immediate recreate (not the deferred MarkRenderStateDirty) so a synchronous SaveToStaticMesh
	// right after a rebuild sees the freshly built proxy after FlushRenderingCommands.
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

FPrimitiveSceneProxy* URoadMeshComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;
	if (PendingInput.Splines.Num() == 0 || PendingInput.MaxVertices == 0) return nullptr;
	return new FRoadMeshSceneProxy(this, PendingInput);
}

#if WITH_EDITOR
UStaticMesh* URoadMeshComponent::SaveToStaticMesh(const FString& AssetPathAndName, bool bReplaceExistingAsset,
	bool bSaveAsset, bool bConvertToActorLocalSpace)
{
	return CSGpuMeshSave::SaveGpuMeshComponentToStaticMesh(
		this, AssetPathAndName, RoadMaterial, GetComponentTransform(),
		bConvertToActorLocalSpace, bReplaceExistingAsset, bSaveAsset);
}
#endif
