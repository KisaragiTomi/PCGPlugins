#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "LocalVertexFactory.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"
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
	struct FPooledVertexBuffer final : public FVertexBuffer
	{
		TRefCountPtr<FRDGPooledBuffer> Pooled;
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
		virtual void ReleaseRHI() override;
	};

	struct FPooledIndexBuffer final : public FIndexBuffer
	{
		TRefCountPtr<FRDGPooledBuffer> Pooled;
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
		virtual void ReleaseRHI() override;
	};

	void BuildGeometry(FRHICommandListBase& RHICmdList);

	FCSMeshGeneratorDebugData Data;
	FLocalVertexFactory VertexFactory;
	FPooledVertexBuffer Positions;
	FPooledIndexBuffer MainIndices;
	FPooledIndexBuffer PointIndices;
	TRefCountPtr<FRDGPooledBuffer> MainIndirectArgs;
	TRefCountPtr<FRDGPooledBuffer> PointIndirectArgs;
	FMaterialRelevance MaterialRelevance;
	uint32 PositionCapacity = 0;
	uint32 MainIndexCapacity = 0;
	uint32 PointIndexCapacity = 0;
};
