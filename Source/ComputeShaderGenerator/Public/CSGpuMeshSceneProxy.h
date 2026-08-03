#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "LocalVertexFactory.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"
#include "CSGpuMeshTypes.h"

class UPrimitiveComponent;
class UMaterialInterface;
class FRHIGPUBufferReadback;

/**
 * Base scene proxy that draws a GPU-resident mesh directly through the render
 * pipeline: it owns an FLocalVertexFactory over GPU buffers and submits one
 * FMeshBatch per view in GetDynamicMeshElements. Vertex/index data lives only on
 * the GPU — nothing is read back to the CPU (except an explicit, opt-in save) and
 * no UDynamicMesh is involved.
 *
 * The buffer set is DESCRIPTOR-DRIVEN. A subclass supplies geometry by implementing
 * two hooks:
 *   RegisterStreams()  — push FCSGpuStreamDesc entries (usually AddStandardTriangleStreams())
 *                        and set VertexCapacity/IndexCapacity.
 *   BuildGeometry()    — register the base's pooled buffers into an FRDGBuilder and run
 *                        the leaf's compute passes that fill them (incl. MeshCounters).
 * The base then owns everything shared by every GPU mesh (roads, the compute-shader mesh
 * generator, ...): buffer allocation, vertex-factory binding, the draw path, teardown,
 * and the CPU readback used by save-to-StaticMesh. Adding a new buffer is one more
 * AddStream(...) in RegisterStreams(); the alloc / VF-bind / readback code is untouched.
 */
class COMPUTESHADERGENERATOR_API FCSGpuMeshSceneProxy : public FPrimitiveSceneProxy
{
public:
	FCSGpuMeshSceneProxy(const UPrimitiveComponent* Component, UMaterialInterface* InMaterial, const char* DebugName);
	virtual ~FCSGpuMeshSceneProxy() override;

	// Heap-owned and never copied; the buffer registry (TArray<TUniquePtr<...>>) is move-only,
	// so deleting the copy operations keeps MSVC from trying to synthesise a copy-assignment.
	FCSGpuMeshSceneProxy(const FCSGpuMeshSceneProxy&) = delete;
	FCSGpuMeshSceneProxy& operator=(const FCSGpuMeshSceneProxy&) = delete;

	//~ FPrimitiveSceneProxy interface
	virtual uint32 GetMemoryFootprint() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual bool CanBeOccluded() const override;
	// GetTypeHash() stays pure-virtual: each concrete proxy must return its own unique hash.

	// -------------------------------------------------------------------------
	// Readback API (used by UCSGpuMeshComponent::ReadbackMeshSync). Render thread only.
	// -------------------------------------------------------------------------

	/** Conservative GPU-buffer capacities (the actual counts are GPU-decided and read
	 *  back from the MeshCounters buffer). */
	uint32 GetVertexCapacity() const { return VertexCapacity; }
	uint32 GetIndexCapacity() const { return IndexCapacity; }

	/** Fills OutDescs with the descriptors of every stream that participates in the CPU
	 *  mesh readback (bReadback && CpuSemantic != None), in registration order. */
	void GetMeshReadbackDescs(TArray<FCSGpuStreamDesc>& OutDescs) const;

	/** Enqueues a copy of the 2-uint MeshCounters buffer ([0]=vertexCount, [1]=indexCount). */
	bool EnqueueCountersReadback(FRHICommandListImmediate& RHICmdList, FRHIGPUBufferReadback* Readback) const;

	/** Enqueues one readback per mesh-readback stream (same order as GetMeshReadbackDescs),
	 *  sizing each copy by the stream's CountSource against VertexCount/IndexCount. */
	bool EnqueueMeshReadback(FRHICommandListImmediate& RHICmdList, uint32 VertexCount, uint32 IndexCount,
		const TArray<FRHIGPUBufferReadback*>& Readbacks) const;

	/** Submit an indexed GPU-buffer draw for every visible view. This is shared by the base
	 *  triangle path and leaf-owned debug geometry; it never maps or reads either buffer. */
	static void SubmitGpuBufferDraw(
		const FPrimitiveSceneProxy& SceneProxy,
		const TArray<const FSceneView*>& Views,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector,
		const FVertexFactory& InVertexFactory,
		FMaterialRenderProxy& MaterialProxy,
		const FIndexBuffer& IndexBuffer,
		EPrimitiveType PrimitiveType,
		uint32 NumPrimitives,
		uint32 MaxVertexIndex,
		bool bCastShadow = false,
		FRHIBuffer* IndirectArgsBuffer = nullptr,
		uint32 IndirectArgsOffset = 0,
		uint32 FirstIndex = 0);

protected:
	// The pooled-buffer render-resource wrappers live in CSGpuMeshTypes.h so debug geometry
	// (FCSGpuDebugDraw) and non-derived proxies can use the same types; these aliases keep the
	// long-standing member/leaf spelling.
	using FPooledVertexBuffer = FCSPooledVertexBuffer;
	using FPooledIndexBuffer = FCSPooledIndexBuffer;

	// One entry in the descriptor-driven buffer set. Heap-allocated (TUniquePtr in the
	// Streams array) so &VB / &IB stay stable for the vertex-factory streams even as the
	// array grows — FVertexStreamComponent stores the FVertexBuffer pointer.
	struct FCSGpuStreamRuntime
	{
		FCSGpuStreamDesc Desc;
		TRefCountPtr<FRDGPooledBuffer> Pooled;
		FShaderResourceViewRHIRef SRV;
		FPooledVertexBuffer VB;
		FPooledIndexBuffer IB;
	};

	// Everything GetDynamicMeshElements needs to describe one draw. AllocateStreamsAndBindVF
	// fills this from the registered streams; the base reads it every frame. Set bValid last.
	struct FDrawDesc
	{
		// Index buffer to draw with. Null => the base skips the draw (this proxy path
		// always draws indexed; non-indexed geometry supplies an identity index buffer).
		FIndexBuffer* IndexBuffer = nullptr;
		// When set, the draw uses DrawIndexedIndirect with these args; NumPrimitives is
		// ignored. When null, the base issues a direct indexed draw of NumPrimitives.
		FRHIBuffer* IndirectArgsBuffer = nullptr;
		uint32 IndirectArgsOffset = 0;
		uint32 NumPrimitives = 0;
		uint32 FirstIndex = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		bool bValid = false;
	};

	// -------------------------------------------------------------------------
	// Leaf hooks (pure-virtual)
	// -------------------------------------------------------------------------

	/** Push the buffer descriptors (typically AddStandardTriangleStreams()) and set
	 *  VertexCapacity / IndexCapacity. Called first from InitGpuGeometry(). */
	virtual void RegisterStreams() = 0;

	/** Register the base's pooled buffers into an FRDGBuilder and run the compute passes
	 *  that fill them (positions/tangents/uv/color/indices/indirect args/MeshCounters).
	 *  Called after the base has allocated the buffers and bound the vertex factory. */
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) = 0;

	// -------------------------------------------------------------------------
	// Base services callable by leaves
	// -------------------------------------------------------------------------

	/** Register one stream. Call from RegisterStreams(). */
	void AddStream(const FCSGpuStreamDesc& Desc);

	/** Register the standard triangle-mesh set: Position / TangentBasis / TexCoord0 /
	 *  Color / Index / IndirectArgs + the MeshCounters carrier. Call after setting the
	 *  capacities; leaves may AddStream(...) extra buffers before or after. */
	void AddStandardTriangleStreams();

	/** Pooled buffer for a registered stream (for BuildGeometry to register external / dispatch). */
	TRefCountPtr<FRDGPooledBuffer> GetStreamBuffer(ECSGpuStreamRole Role, uint8 Index = 0) const;

	// Shared vertex factory; configured by the base from the registered streams.
	FLocalVertexFactory VertexFactory;

	UMaterialInterface* Material = nullptr;
	FMaterialRelevance MaterialRelevance;
	FDrawDesc DrawDesc;

	// Set by the leaf in RegisterStreams(). Soup: IndexCapacity == VertexCapacity;
	// indexed (road): independent.
	uint32 VertexCapacity = 0;
	uint32 IndexCapacity = 0;

	// Batch-level flag mirrored from the component; subclasses may flip it before the
	// proxy is registered. Actual shadow casting is still gated by the component's
	// CastShadow / the proxy's shadow relevance.
	bool bBatchCastShadow = true;

private:
	/** Base orchestrator: RegisterStreams() -> AllocateStreamsAndBindVF() -> BuildGeometry(). */
	void InitGpuGeometry(FRHICommandListBase& RHICmdList);
	/** Release SRVs then pooled buffers for every stream (after the VF is released). */
	void ReleaseGpuGeometry();

	/** Allocate pooled buffers + SRVs for every stream, bind the vertex-factory streams,
	 *  and fill DrawDesc's index/indirect handles. */
	void AllocateStreamsAndBindVF(FRHICommandListBase& RHICmdList);

	/** Find a stream runtime by role (+ TexCoord index for the TexCoord role). */
	const FCSGpuStreamRuntime* FindStream(ECSGpuStreamRole Role, uint8 Index = 0) const;

	TArray<TUniquePtr<FCSGpuStreamRuntime>> Streams;
};
