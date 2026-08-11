#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshSceneProxy.h"
#include "CSGpuInstancedMeshComponent.h"

class FRDGBuilder;
class FSceneView;
class UCSGpuInstancedMeshComponent;

/**
 * Which extra buffer an AuxVertex stream is. FCSGpuMeshSceneProxy::FindStream() keys on
 * (Role, TexCoordIndex), so the index doubles as the aux-slot id — the same trick the base
 * already uses to tell TexCoord0 from TexCoord1.
 */
enum class ECSGpuInstancedAuxSlot : uint8
{
	SourceInstances = 0,    // Buffer<float4>, 5 per instance (see FCSGpuInstanceSourceGPU)
	ClusterBounds = 1,      // Buffer<float4>, centre.xyz + radius per cluster
	ClusterVisible = 2,     // Buffer<uint>,   coarse-cull result per cluster
	VisibleTransforms = 3,  // Buffer<float4>, 3 per visible slot -> VertexFetch_InstanceTransformBuffer
	VisibleOrigins = 4,     // Buffer<float4>, 1 per visible slot -> VertexFetch_InstanceOriginBuffer
	VisibleLightmap = 5,    // Buffer<float4>, 1 per visible slot -> VertexFetch_InstanceLightmapBuffer
	LodCounters = 6,        // Buffer<uint>,   compaction cursor per LOD
};

/**
 * Scene proxy for UCSGpuInstancedMeshComponent.
 *
 * The base mesh is uploaded once into the standard triangle streams owned by
 * FCSGpuMeshSceneProxy (all LODs concatenated into one vertex + one index buffer). On top of
 * that this proxy owns the instance buffers and, every frame, runs the cluster/instance cull +
 * LOD selection passes of CSGpuInstancedMesh.usf, which compact the visible instances into
 * per-LOD regions and fill one DrawIndexedIndirect arg set per LOD.
 *
 * The per-frame work has to happen inside the renderer's own graph and before the base pass, so
 * a shared FSceneViewExtension drives it; the proxy registers itself for the duration of its
 * render-thread resources. The cull runs once per frame for the first view — with split screen
 * every view therefore draws the primary view's visible set, which over-draws rather than
 * under-draws.
 */
class FCSGpuInstancedMeshSceneProxy final : public FCSGpuMeshSceneProxy
{
public:
	explicit FCSGpuInstancedMeshSceneProxy(UCSGpuInstancedMeshComponent* Component);
	virtual ~FCSGpuInstancedMeshSceneProxy() override;

	/** Creates the shared cull view extension if it does not exist yet. Game thread. */
	static void EnsureCullServiceStarted();

	//~ FPrimitiveSceneProxy interface
	virtual SIZE_T GetTypeHash() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

	/** Adds this frame's cull + LOD + indirect-args passes to the renderer's graph. Render thread. */
	void RunCulling(FRDGBuilder& GraphBuilder, const FSceneView& View);

protected:
	//~ FCSGpuMeshSceneProxy interface
	virtual void RegisterStreams() override;
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override;
	virtual TUniquePtr<FLocalVertexFactory> CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const override;
	virtual void OnStreamsAllocated(FRHICommandListBase& RHICmdList) override;

private:
	uint32 GetAuxElementCount(ECSGpuInstancedAuxSlot Slot) const;

	// Game-thread snapshot; the proxy is recreated whenever any of it changes.
	FCSGpuInstancedBaseMesh BaseMesh;
	TArray<FVector4f> PackedInstances;
	TArray<FVector4f> ClusterBounds;
	FCSGpuInstanceSourceGPU GpuSource;
	FCSGpuInstancePointSourceGPU GpuPointSource;

	int32 ClusterSize = 64;
	float EndCullDistance = 0.0f;
	float LodScreenSizeScale = 1.0f;
	bool bFrustumCull = true;
	bool bLodSelect = true;

	// Derived once in the constructor.
	uint32 NumLODs = 1;
	uint32 MaxInstances = 0;  // capacity of one LOD region in the visible buffers
	uint32 NumClusters = 0;

	// Constant across frames; built once so GetDynamicMeshElements stays allocation-free.
	FUniformBufferRHIRef InstancedLooseUniformBuffer;

	bool bRegisteredForCulling = false;

	// One-shot diagnostics. Nothing about this path is visible from the game thread — if the cull
	// writes a zero instance count the draw is simply a no-op and the log stays silent — so the
	// first frame's indirect args are copied back and logged once per proxy.
	enum class EDiagnosticState : uint8 { Pending, Waiting, Done };
	EDiagnosticState DiagnosticState = EDiagnosticState::Pending;
	class FRHIGPUBufferReadback* DiagnosticReadback = nullptr;
	mutable bool bLoggedFirstDraw = false;
};
