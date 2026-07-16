#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "LocalVertexFactory.h"
#include "RenderGraphResources.h"
#include "RoadTypes.h"

class URoadMeshComponent;
class UMaterialInterface;

/**
 * Scene proxy that builds the road mesh on the GPU (compute passes translated
 * from RoadGenerator.hip) into persistent pooled buffers, then draws them
 * every frame through DrawIndexedIndirect. Vertex and index data never exist
 * on the CPU.
 */
class FRoadMeshSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FRoadMeshSceneProxy(URoadMeshComponent* Component, const FRoadBuildInput& InInput);
	virtual ~FRoadMeshSceneProxy() override;

	//~ FPrimitiveSceneProxy interface
	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual bool CanBeOccluded() const override;

private:
	/** Runs the full compute pipeline once; results stay on the GPU. */
	void BuildRoadNetwork(FRHICommandListImmediate& RHICmdList);

	// Thin FRenderResource wrappers that expose a pooled buffer's RHI object
	// as a vertex/index buffer for the vertex factory streams.
	struct FPooledVertexBuffer final : public FVertexBuffer
	{
		TRefCountPtr<FRDGPooledBuffer> Pooled;
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			if (Pooled.IsValid()) VertexBufferRHI = Pooled->GetRHI();
		}
		virtual void ReleaseRHI() override
		{
			VertexBufferRHI.SafeRelease();
		}
	};
	struct FPooledIndexBuffer final : public FIndexBuffer
	{
		TRefCountPtr<FRDGPooledBuffer> Pooled;
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			if (Pooled.IsValid()) IndexBufferRHI = Pooled->GetRHI();
		}
		virtual void ReleaseRHI() override
		{
			IndexBufferRHI.SafeRelease();
		}
	};

	FRoadBuildInput Input;
	UMaterialInterface* Material;
	FMaterialRelevance MaterialRelevance;

	FLocalVertexFactory VertexFactory;

	TRefCountPtr<FRDGPooledBuffer> PositionPooled;
	TRefCountPtr<FRDGPooledBuffer> TangentPooled;
	TRefCountPtr<FRDGPooledBuffer> TexCoordPooled;
	TRefCountPtr<FRDGPooledBuffer> ColorPooled;
	TRefCountPtr<FRDGPooledBuffer> IndexPooled;
	TRefCountPtr<FRDGPooledBuffer> IndirectArgsPooled;

	FPooledVertexBuffer PositionVB;
	FPooledVertexBuffer TangentVB;
	FPooledVertexBuffer TexCoordVB;
	FPooledVertexBuffer ColorVB;
	FPooledIndexBuffer IndexBuf;

	FShaderResourceViewRHIRef PositionSRV;
	FShaderResourceViewRHIRef TangentSRV;
	FShaderResourceViewRHIRef TexCoordSRV;
	FShaderResourceViewRHIRef ColorSRV;
};
