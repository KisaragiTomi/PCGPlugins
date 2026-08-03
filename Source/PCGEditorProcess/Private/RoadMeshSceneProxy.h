#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshSceneProxy.h"
#include "RoadTypes.h"

class URoadMeshComponent;

/**
 * Scene proxy that builds the road mesh on the GPU (compute passes translated
 * from RoadGenerator.hip) into the base-owned persistent buffers, then draws them
 * every frame through DrawIndexedIndirect. Vertex and index data never exist on the
 * CPU (except an explicit save-to-StaticMesh).
 *
 * The road is a proper INDEXED mesh (shared vertices), so IndexCapacity != VertexCapacity.
 * All the generic buffer-set / vertex-factory / draw / readback plumbing lives in
 * FCSGpuMeshSceneProxy; this class only registers the standard triangle streams and runs
 * the road compute build.
 */
class FRoadMeshSceneProxy final : public FCSGpuMeshSceneProxy
{
public:
	FRoadMeshSceneProxy(URoadMeshComponent* Component, const FRoadBuildInput& InInput);
	virtual ~FRoadMeshSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;

protected:
	//~ FCSGpuMeshSceneProxy interface
	virtual void RegisterStreams() override;
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override;

private:
	FRoadBuildInput Input;
};
