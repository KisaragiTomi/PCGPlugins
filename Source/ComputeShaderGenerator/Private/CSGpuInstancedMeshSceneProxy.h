#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshSceneProxy.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSMesh.h" // FCSMeshResidentRef: the buffer set this proxy borrows

class FRDGBuilder;
class FSceneView;
class UCSGpuInstancedMeshComponent;

/**
 * Which extra buffer an AuxVertex stream is. A stream is addressed by (Role, TexCoordIndex) —
 * FCSGpuMeshSceneProxy::FindStream, FCSMeshResident::FindStream and FCSMeshEditContext::Find all
 * key on that pair — so the index doubles as the aux-slot id, the same trick the base already uses
 * to tell TexCoord0 from TexCoord1.
 *
 * THESE START AT 16, NOT 0, AND MUST NOT BE MOVED DOWN. AuxVertex slot 0 belongs to the retained
 * set's per-triangle material-id stream, which FCSMeshStreamLayout forces on and offers no way to
 * disable (every operator in CSMeshOps needs it). This leaf declared SourceInstances at slot 0 while
 * its buffers were proxy-owned, where no material-id stream existed; the moment the same declaration
 * went to UCSMesh the two collided. What a collision does is the reason for the gap: an aux stream
 * simply does not get registered — UCSMesh::SetStreamLayoutSync refuses the whole layout and returns
 * false, and FCSMeshResident::AddStream returns false for one stream — after which a null buffer is
 * bound at draw time, with nothing in the log pointing at the cause.
 *
 * The gap to 16 is so a later standard aux stream (they are numbered upwards from 0) cannot walk
 * into this block. TexCoordIndex is only a key for the AuxVertex role — nothing sizes an array by
 * it, and only the TexCoord role feeds FLocalVertexFactory::FDataType::NumTexCoords — so a high
 * value costs nothing.
 */
enum class ECSGpuInstancedAuxSlot : uint8
{
	SourceInstances = 16,   // Buffer<float4>, 5 per instance (see FCSGpuInstanceSourceGPU)
	ClusterBounds = 17,     // Buffer<float4>, centre.xyz + radius per cluster
	ClusterVisible = 18,    // Buffer<uint>,   coarse-cull result per cluster
	VisibleTransforms = 19, // Buffer<float4>, 3 per visible slot -> VertexFetch_InstanceTransformBuffer
	VisibleOrigins = 20,    // Buffer<float4>, 1 per visible slot -> VertexFetch_InstanceOriginBuffer
	VisibleLightmap = 21,   // Buffer<float4>, 1 per visible slot -> VertexFetch_InstanceLightmapBuffer
	LodCounters = 22,       // Buffer<uint>,   compaction cursor per LOD
	/**
	 * Buffer<float>, CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS per visible slot
	 * -> FInstancedStaticMeshVertexFactoryUniformShaderParameters::InstanceCustomDataBuffer。
	 * ⚠️ 格式**必须 PF_R32_FLOAT**：引擎按 `Buffer<float>` 取，照抄邻居那几条的
	 * float4 视图会读出错位数据，而画面上只是"数值不对"，不报错。
	 */
	VisibleCustomData = 23,
};

/** Appends this leaf's seven aux stream descriptors to OutStreams, sized from Layout.
 *  bExternalPackedSource means a producer supplies the packed rows in its own buffer, so the mesh
 *  only needs a placeholder for that slot. Lives with the enum because the slot assignment and the
 *  slot numbering must never be edited apart. */
void CSGpuInstancedBuildAuxStreamDescs(
	TArray<FCSGpuStreamDesc>& OutStreams,
	const FCSGpuInstancedGpuLayout& Layout,
	bool bExternalPackedSource);

/**
 * 点云源 -> packed 实例行（CSGpuInstancedMesh.usf 的 PackPointInstancesCS）。
 *
 * 两条路共用这一个入口：经典路每帧在剔除前打进 SourceInstances 槽，Nanite 路在 GPU-Scene 写入前
 * 打进一块临时行。抽出来是为了让"点怎么变成实例"只有一份 —— 两边各写一份的话，法线朝向或随机数
 * 哪天改了一边，另一边的画面就静默地对不上。
 *
 * WorldToComponent：点位是世界空间，行是组件局部空间。BaseSphere* 只决定第 5 行（剔除球），
 * 不读那一行的调用方传零即可。
 */
void CSGpuInstancedAddPackPointsPass(
	FRDGBuilder& GraphBuilder,
	class FGlobalShaderMap* ShaderMap,
	const FCSGpuInstancePointSourceGPU& Points,
	FRDGBufferRef InstanceCount,
	FRDGBufferRef OutPackedInstances,
	const FMatrix44f& WorldToComponent,
	const FVector3f& BaseSphereCentre,
	float BaseSphereRadius,
	uint32 MaxSourceInstances);

/**
 * Scene proxy for UCSGpuInstancedMeshComponent.
 *
 * It owns no buffers. The component's UCSMesh holds the base mesh (all LODs concatenated into one
 * vertex + one index buffer), the per-LOD indirect args and the seven aux streams; this proxy
 * adopts that set through FCSGpuMeshSceneProxy::SetExternalStreams and, every frame, runs the
 * cluster/instance cull + LOD selection passes of CSGpuInstancedMesh.usf, which compact the visible
 * instances into per-LOD regions and fill one DrawIndexedIndirect arg set per material section of
 * each LOD (all sections of a LOD share that LOD's region; each draws its own index run with its
 * own material).
 *
 * What it does NOT adopt is UCSMeshRenderComponent's proxy. That one hard-wires FLocalVertexFactory
 * through FCSMeshRenderSceneProxy, and this path exists precisely because the instance transform has
 * to be manual-fetched by SV_InstanceID out of compute-written buffers — see
 * FCSGpuInstancedMeshVertexFactory. Retention and the vertex factory are separable concerns, and the
 * base's external-streams mode is what separates them: RegisterStreams()/BuildGeometry() are never
 * called in that mode, but CreateVertexFactory() and OnStreamsAllocated() still are.
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
	/** InDrawMaterials：每个材质段（= 每个 draw，与快照 Sections 同序）一张，调用方已解析好覆盖 / 资产材质
	 *  并做完用途检查（CreateSceneProxy）；空项画引擎默认材质。 */
	FCSGpuInstancedMeshSceneProxy(UCSGpuInstancedMeshComponent* Component, const FCSMeshResidentRef& InResident,
		const TArray<UMaterialInterface*>& InDrawMaterials);
	virtual ~FCSGpuInstancedMeshSceneProxy() override;

	/** Creates the shared cull view extension if it does not exist yet. Game thread. */
	static void EnsureCullServiceStarted();

	//~ FPrimitiveSceneProxy interface
	virtual SIZE_T GetTypeHash() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

	/** Adds this frame's cull + LOD + indirect-args passes to the renderer's graph, through the
	 *  mesh's own FCSMeshRenderThreadEdit so the resident streams end the frame in the access states
	 *  CSGpuMeshStreams::FinalAccessForRole demands. Render thread. */
	void RunCulling(FRDGBuilder& GraphBuilder, const FSceneView& View);

protected:
	//~ FCSGpuMeshSceneProxy interface
	// Never called: the base skips both hooks in external-streams mode, where the buffers already
	// exist and already hold geometry. They stay pure-virtual on the base because every
	// proxy-owning leaf still needs them.
	virtual void RegisterStreams() override {}
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override {}
	virtual TUniquePtr<FLocalVertexFactory> CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const override;
	virtual void OnStreamsAllocated(FRHICommandListBase& RHICmdList) override;
	/** No BLAS: the resident streams hold the source mesh once and the instance transforms live in
	 *  a GPU buffer (FCSGpuInstanceSourceGPU), which a single-instance BLAS cannot express. Ray
	 *  tracing for this leaf needs GPU-side TLAS instance data and is a separate piece of work. */
	virtual bool WantsRayTracingGeometry() const override { return false; }
	/** 一个 draw 一张，与 Sections 同序（基座的 BLAS / card-capture 按下标配材质；本叶子两者都不开，补齐是为了口径一致）。 */
	virtual void GetBatchMaterials(TArray<FMaterialRenderProxy*, TInlineAllocator<8>>& OutMaterials) const override;

private:
	/** The buffer set this proxy borrows, held as the shared reference so either teardown order is
	 *  safe. The base holds the same object (SetExternalStreams) and derives its vertex-factory
	 *  bindings from it; RunCulling needs it directly, because an edit is declared over the resident
	 *  set rather than over a list of buffers somebody remembered to enumerate. */
	FCSMeshResidentRef Resident;

	/** Resident.AllocationGeneration at the moment this proxy adopted the buffers. The set can be
	 *  reallocated while a proxy is live (a capacity change, a stream resize), and the proxy then
	 *  holds the buffers it adopted while the resident set points at new ones — culling into those
	 *  would fill buffers the draw does not read, which looks exactly like a cull that found
	 *  nothing. The component marks its render state dirty on every such change, so refusing to
	 *  cull costs the frame or two before the replacement proxy arrives. */
	uint32 AdoptedAllocationGeneration = 0;

	// Game-thread snapshot; the proxy is recreated whenever any of it changes. Deliberately not the
	// whole FCSGpuInstancedBaseMesh or the packed instance array any more — those live in the mesh
	// object, and copying them per proxy is exactly the cost retention removes.
	TArray<FCSGpuInstancedLODRange> LODs;
	/** 快照的材质段，一段一个 draw（arg set 下标 = 段号）。截到 Layout.NumDraws。 */
	TArray<FCSGpuInstancedSection> Sections;
	/** 每段的材质，恒非空（空的已换成引擎默认材质）。与 Sections 同长。 */
	TArray<UMaterialInterface*> DrawMaterials;
	FVector3f BaseSphereCentre = FVector3f::ZeroVector; // base bounds, for the point-source packer
	float BaseSphereRadius = 0.0f;
	FCSGpuInstanceSourceGPU GpuSource;
	FCSGpuInstancePointSourceGPU GpuPointSource;

	float EndCullDistance = 0.0f;
	float LodScreenSizeScale = 1.0f;
	bool bFrustumCull = true;
	bool bLodSelect = true;

	/** Copied from the component, never re-derived — see FCSGpuInstancedGpuLayout. */
	FCSGpuInstancedGpuLayout Layout;

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
