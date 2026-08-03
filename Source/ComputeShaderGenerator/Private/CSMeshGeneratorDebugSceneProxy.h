#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "LocalVertexFactory.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"
#include "CSGpuDebugDraw.h"
#include "CSMeshGeneratorDebugComponent.h"

class UCSMeshGeneratorDebugComponent;

/** GPU-only scene proxy for surface-voxel directions, points, and isolated quads. */
class FCSMeshGeneratorDebugSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FCSMeshGeneratorDebugSceneProxy(const UCSMeshGeneratorDebugComponent* Component, const FCSMeshGeneratorDebugData& InData);
	virtual ~FCSMeshGeneratorDebugSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual bool CanBeOccluded() const override;

private:
	void BuildGeometry(FRHICommandListBase& RHICmdList);

	FCSMeshGeneratorDebugData Data;

	// GPU voxel source: buffers filled by the debug compute passes, counts decided on the GPU.
	FLocalVertexFactory VertexFactory;
	FCSGpuDebugPositionStream Positions;
	FCSPooledIndexBuffer MainIndices;
	FCSPooledIndexBuffer PointIndices;

	// CPU-supplied primitives: one uploaded position/index pair, one draw per batch.
	FLocalVertexFactory BatchVertexFactory;
	FCSGpuDebugPositionStream BatchPositions;
	FCSPooledIndexBuffer BatchIndices;
	TRefCountPtr<FRDGPooledBuffer> MainIndirectArgs;
	TRefCountPtr<FRDGPooledBuffer> PointIndirectArgs;
	FMaterialRelevance MaterialRelevance;
	uint32 PositionCapacity = 0;
	uint32 MainIndexCapacity = 0;
	uint32 PointIndexCapacity = 0;
};
