#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "LocalVertexFactory.h"
#include "CSGpuDebugDraw.h"
#include "CSMeshGeneratorDebugComponent.h"

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
	bool IsDirectionsMode() const { return Data.Mode == ECSMeshGeneratorDebugMode::Directions; }

	// -------------------------------------------------------------------------
	// One entry trio per debug shape: size and allocate what that shape draws with, record its
	// compute passes, submit its draws. The mode is read exactly three times — once per override
	// above — so a new shape is a new trio, not another branch inside these.
	// -------------------------------------------------------------------------

	/** Directions: 2 vertices / 2 line indices / 1 point index per voxel (AddVoxelDirectionsPass). */
	void AllocateDirectionBuffers(FRHICommandListBase& RHICmdList);
	void BuildDirectionGeometry(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
		const FCSGpuDebugVoxelSource& Source, FRDGBufferRef DebugPositions, FRDGBufferRef DebugIndices,
		FRDGBufferRef DebugArgs);
	void SubmitDirectionDraws(const TArray<const FSceneView*>& Views, uint32 VisibilityMap,
		FMeshElementCollector& Collector) const;

	/** Isolated quads: 4 vertices / 6 indices per voxel (AddVoxelQuadsPass). */
	void AllocateQuadBuffers(FRHICommandListBase& RHICmdList);
	void BuildQuadGeometry(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
		const FCSGpuDebugVoxelSource& Source, FRDGBufferRef DebugPositions, FRDGBufferRef DebugIndices,
		FRDGBufferRef DebugArgs);
	void SubmitQuadDraws(const TArray<const FSceneView*>& Views, uint32 VisibilityMap,
		FMeshElementCollector& Collector) const;

	/** Registers the buffers every shape writes, then hands off to the active shape's build entry. */
	void BuildGeometry(FRHICommandListBase& RHICmdList);

	FCSMeshGeneratorDebugData Data;
	FLocalVertexFactory VertexFactory;
	FCSGpuDebugPositionStream Positions;
	// Main = the shape's own primitive (lines or triangles); Point = the direction-mode centres.
	FCSPooledIndexBuffer MainIndices;
	FCSPooledIndexBuffer PointIndices;
	TRefCountPtr<FRDGPooledBuffer> MainIndirectArgs;
	TRefCountPtr<FRDGPooledBuffer> PointIndirectArgs;
	FMaterialRelevance MaterialRelevance;
	uint32 PositionCapacity = 0;
};
