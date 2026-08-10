#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshSceneProxy.h"
#include "ComputeShaderMeshGenerator.h" // FCSBoxScenePreparedData

class UCSDisplayComponent;

/**
 * Scene proxy for UCSDisplayComponent. On the render thread it runs the
 * generator's box-scene triangle extraction (AddPreparedBoxSceneTrianglesToRDG) into a
 * transient triangle soup, then a pack pass fills the base-owned LocalVertexFactory streams
 * (positions, tangent basis, zero UV, white colour) with an identity index buffer and builds
 * DrawIndexedIndirect args + the MeshCounters from the GPU triangle counter. The buffer set,
 * vertex-factory binding, draw path, and CPU readback all live in FCSGpuMeshSceneProxy; this
 * class only registers the standard triangle streams and runs the pack/args passes.
 *
 * The soup is a triangle list with a unique vertex per index, so IndexCapacity == VertexCapacity.
 */
class FCSDisplayTriangleSceneProxy final : public FCSGpuMeshSceneProxy
{
public:
	FCSDisplayTriangleSceneProxy(UCSDisplayComponent* Component, const FCSBoxScenePreparedData& InPrepared, uint32 InVertexCapacity);
	virtual ~FCSDisplayTriangleSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;

protected:
	//~ FCSGpuMeshSceneProxy interface
	virtual void RegisterStreams() override;
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override;

private:
	FCSBoxScenePreparedData Prepared;
	uint32 InputVertexCapacity = 0;
};
