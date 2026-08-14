#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshSceneProxy.h"
#include "CSMesh.h"

class UCSMeshRenderComponent;

/**
 * Scene proxy for UCSMeshRenderComponent: the base's external-streams mode and nothing else.
 *
 * It allocates no buffers and runs no compute. RegisterStreams()/BuildGeometry() stay
 * pure-virtual on the base because every proxy-owning leaf still needs them; this one
 * implements them as no-ops that the adopt path never calls.
 *
 * What it does add is the section split. The base draws the whole mesh as one batch out of arg
 * set 0; this one submits a batch per arg set, each with its own material. Everything that makes
 * that split is decided on the game thread and frozen here — the batch list, the materials, the
 * relevance union — because the render thread may not touch a UObject and a section change is
 * therefore a new proxy, not a new frame.
 */
class FCSMeshRenderSceneProxy final : public FCSGpuMeshSceneProxy
{
public:
	FCSMeshRenderSceneProxy(UCSMeshRenderComponent* Component, const FCSMeshResidentRef& InResident,
		const TArray<TObjectPtr<UMaterialInterface>>& InBatchMaterials);

	virtual SIZE_T GetTypeHash() const override;

	/** One FMeshBatch per arg set instead of the base's single whole-mesh batch. */
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

protected:
	//~ FCSGpuMeshSceneProxy interface — unused in external mode.
	virtual void RegisterStreams() override {}
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override {}

private:
	/** One DrawIndexedIndirect arg set: IndexCountPerInstance, InstanceCount, StartIndexLocation,
	 *  BaseVertexLocation, StartInstanceLocation. FMeshBatchElement::IndirectArgsOffset counts
	 *  bytes, not sets, so batch i has to be scaled by this rather than indexed by it. */
	static constexpr uint32 IndirectArgsSetBytes = uint32(5 * sizeof(uint32));

	/** Material per draw batch, batch i drawing from arg set i. Raw pointers captured on the game
	 *  thread exactly as the base captures its single Material, and kept alive for this proxy's
	 *  whole life by the component's matching UPROPERTY: GetRenderProxy() is all the render thread
	 *  is allowed to ask a UMaterialInterface, and only if it is still there to ask. */
	TArray<UMaterialInterface*> BatchMaterials;
};
