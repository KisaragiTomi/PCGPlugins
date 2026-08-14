#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGpuInstancedMeshComponent.generated.h"

class UCSMesh;
class UStaticMesh;
class UMaterialInterface;

/** Number of LOD levels the GPU LOD-selection pass can pick between (the cull shader keeps the
 *  thresholds in a float4). Extra LODs on the source mesh are ignored. */
#define CS_GPU_INSTANCED_MAX_LODS 4

/** One LOD of the base mesh inside the shared GPU vertex/index buffers. */
struct FCSGpuInstancedLODRange
{
	uint32 FirstIndex = 0;  // into the shared index buffer
	uint32 NumIndices = 0;
	uint32 BaseVertex = 0;  // added to every index by DrawIndexedIndirect
	float ScreenSize = 1.0f; // switch to this LOD at or below this screen size (LOD0 is largest)
};

/**
 * CPU snapshot of the base mesh. The proxy uploads it once into the GPU streams owned by
 * FCSGpuMeshSceneProxy; from then on the geometry is GPU-resident and only the per-instance
 * data changes. All LODs live in one vertex buffer and one index buffer, addressed by
 * FCSGpuInstancedLODRange.
 */
struct FCSGpuInstancedBaseMesh
{
	TArray<FVector3f> Positions;
	TArray<uint32> TangentBasis;  // 2 packed 8888 SNORM per vertex (TangentX, TangentZ)
	TArray<FVector2f> TexCoords;  // 1 per vertex
	TArray<uint32> Colors;        // 1 packed RGBA8 per vertex
	TArray<uint32> Indices;
	TArray<FCSGpuInstancedLODRange> LODs;

	/** Local bounds of LOD0, used as the per-instance culling sphere. */
	FBox LocalBounds = FBox(ForceInit);

	bool IsValid() const
	{
		return Positions.Num() >= 3 && Indices.Num() >= 3 && LODs.Num() > 0
			&& TangentBasis.Num() == Positions.Num() * 2
			&& TexCoords.Num() == Positions.Num()
			&& Colors.Num() == Positions.Num();
	}

	void Reset()
	{
		Positions.Reset();
		TangentBasis.Reset();
		TexCoords.Reset();
		Colors.Reset();
		Indices.Reset();
		LODs.Reset();
		LocalBounds = FBox(ForceInit);
	}
};

/**
 * GPU-produced instance source: a compute pass wrote the instances straight into GPU buffers and
 * the CPU never sees them. Layout matches the CPU path's packed source buffer — 5 float4 per
 * instance:
 *   [0..2] rows of the instance-to-component 3x3 (.w = 0)
 *   [3]    origin.xyz in component space, .w = per-instance random (0..1)
 *   [4]    culling sphere: centre.xyz in component space, .w = radius
 * Counter[0] holds the live instance count, so the count never round-trips to the CPU.
 */
struct FCSGpuInstanceSourceGPU
{
	TRefCountPtr<FRDGPooledBuffer> PackedInstances; // Buffer<float4>, 5 per instance
	TRefCountPtr<FRDGPooledBuffer> Counter;         // Buffer<uint>, [0] = instance count
	uint32 Capacity = 0;                            // instances the buffer can hold
	FBox LocalBounds = FBox(ForceInit);             // conservative bounds of the whole scatter

	bool IsValid() const { return PackedInstances.IsValid() && Counter.IsValid() && Capacity > 0; }
	void Reset() { *this = FCSGpuInstanceSourceGPU(); }
};

/**
 * Point-cloud form of the GPU instance source: world-space positions + normals instead of packed
 * instance rows. The proxy builds the rows itself at the start of every cull, so a producer that
 * already owns a (position, normal) point buffer — the depth-sampling point brush — can drive the
 * instanced display without knowing the instance layout, and without a readback.
 *
 * Each point becomes one instance whose +Z is its normal, uniformly scaled by InstanceScale.
 * Counter[0] is the live point count, same contract as FCSGpuInstanceSourceGPU.
 */
struct FCSGpuInstancePointSourceGPU
{
	TRefCountPtr<FRDGPooledBuffer> Positions; // Buffer<float4>, xyz = world position
	TRefCountPtr<FRDGPooledBuffer> Normals;   // Buffer<float4>, xyz = world normal
	TRefCountPtr<FRDGPooledBuffer> Counter;   // Buffer<uint>, [0] = live count
	uint32 Capacity = 0;
	float InstanceScale = 1.0f;
	FBox WorldBounds = FBox(ForceInit);

	bool IsValid() const { return Positions.IsValid() && Normals.IsValid() && Counter.IsValid() && Capacity > 0; }
	void Reset() { *this = FCSGpuInstancePointSourceGPU(); }
};

/**
 * What the GPU stream layout was built for.
 *
 * Derived once on the game thread whenever the base mesh or the instance set changes, then copied
 * wholesale into the scene proxy. It is a copy rather than a second derivation because
 * MaxInstancesPerLod is the *stride* of one LOD's region in the visible-instance buffers: a proxy
 * that culled with a different number than the one those buffers were sized from would compact
 * survivors past the end of a region, which is a device fault or silent garbage rather than an
 * error anybody can trace.
 */
struct FCSGpuInstancedGpuLayout
{
	/** LOD levels drawn, one DrawIndexedIndirect arg set each. */
	uint32 NumLODs = 1;

	/** Instances the source and visible buffers are sized for — the region stride, not the live
	 *  count. It ratchets with hysteresis (see UCSGpuInstancedMeshComponent::ResolveInstanceCapacity):
	 *  changing it reallocates six of the mesh's streams, and tracking the live count exactly would
	 *  do that on every single AddInstance. */
	uint32 InstanceCapacity = 0;

	/** Live rows in the source buffer. The GPU sources carry their own counter and leave this at the
	 *  capacity; the CPU array knows it exactly. */
	uint32 NumSourceInstances = 0;

	/** Coarse cull level. Zero means there is none — a GPU instance source has no cluster table, so
	 *  every instance goes straight through the fine cull. */
	uint32 NumClusters = 0;
	uint32 ClusterSize = 0;

	bool IsValid() const { return InstanceCapacity > 0 && NumLODs > 0; }
};

/**
 * HISM done on the GPU, on top of UCSGpuMeshComponent.
 *
 * One GPU-resident copy of the base mesh (all LODs concatenated) plus a per-instance transform
 * buffer. Every frame a compute pass culls a two-level hierarchy — clusters first, then the
 * instances inside surviving clusters — picks a LOD per instance from its screen size, compacts
 * the survivors into per-LOD regions of the visible-instance buffers and writes one
 * DrawIndexedIndirect arg set per LOD. The draw then reads the instance transform in the vertex
 * shader by SV_InstanceID (see FCSGpuInstancedMeshVertexFactory), so geometry is stored once no
 * matter how many instances there are and the visible set never touches the CPU.
 *
 * Relative to UHierarchicalInstancedStaticMeshComponent:
 *   - the cluster tree is a flat Morton-ordered cluster list rebuilt on the game thread, and the
 *     culling/LOD decision itself runs on the GPU instead of on the game thread;
 *   - instances can come from the CPU (AddInstance & co, serialized like HISM's) or straight from
 *     a compute shader (SetInstanceSourceGPU) — the render path is the same either way;
 *   - no per-instance collision, no ray tracing, no static lighting, no per-instance custom data.
 *
 * The material must have "Used with Instanced Static Meshes" enabled, exactly as for HISM.
 *
 * Every buffer — the base mesh, the per-LOD indirect args, the instance source and the
 * visible-instance buffers the cull compacts into — lives in a UCSMesh this component owns, not in
 * its scene proxy. A render-state recreation is therefore a rebind: the base mesh and the packed
 * instance rows are uploaded once, when they change, instead of once per proxy. That is the whole
 * reason the mesh object exists here, since a proxy rebuild used to re-upload the entire base mesh
 * and the entire instance array.
 *
 * The one thing that does NOT go through UCSMesh::EditMeshSync is the per-frame cull, which has to
 * run inside the renderer's own graph and can neither build a graph of its own nor block on a
 * flush. It uses the mesh's other sanctioned entry point instead — FCSMeshRenderThreadEdit, scoped
 * around the passes in FCSGpuInstancedMeshSceneProxy::RunCulling — so the resident streams are
 * registered and restored by the same code the game-thread path uses rather than by a second copy
 * of the rule kept in step by hand.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSGpuInstancedMeshComponent : public UCSGpuMeshComponent
{
	GENERATED_BODY()

public:
	UCSGpuInstancedMeshComponent();

	// -------------------------------------------------------------------------
	// Base mesh
	// -------------------------------------------------------------------------

	/** Mesh instanced by this component. Its LODs (up to CS_GPU_INSTANCED_MAX_LODS) become the
	 *  GPU LOD levels, using the asset's own screen sizes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS GPU Instanced Mesh")
	TObjectPtr<UStaticMesh> BaseMesh;

	/** Material drawn for every instance. Null uses the engine default surface material.
	 *  Must have bUsedWithInstancedStaticMeshes set or it will fall back to the default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh")
	TObjectPtr<UMaterialInterface> InstanceMaterial;

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh")
	void SetBaseMesh(UStaticMesh* InMesh);

	/** Feed a GPU-generated mesh (e.g. a readback from another UCSGpuMeshComponent) as the single
	 *  LOD0 base mesh instead of a UStaticMesh. Positions are taken as component-local. */
	void SetBaseMeshFromGpuData(const FCSGpuMeshCPUData& InMeshData);

	// -------------------------------------------------------------------------
	// Instances — CPU source (HISM-shaped API, transforms are component-local)
	//
	// Every one of these repacks the whole instance array and re-uploads it, which now includes a
	// blocking render flush. That was always the shape of this API (the sort alone is O(N log N) per
	// call), but the flush makes the difference visible: use AddInstances / SetInstances for more
	// than a handful, or the batching form of UpdateInstanceTransform below.
	// -------------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	int32 AddInstance(const FTransform& InstanceTransform, bool bWorldSpace = false);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	TArray<int32> AddInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace = false);

	/** Removes by swapping the last instance into the hole, so indices after InstanceIndex are
	 *  not stable — same contract as UInstancedStaticMeshComponent::RemoveInstance. */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool RemoveInstance(int32 InstanceIndex);

	/** bMarkRenderStateDirty=false is the batching form: the CPU-side instance array is updated but
	 *  neither the GPU buffers nor the render state are, so a run of edits costs one upload instead
	 *  of one per edit. The display keeps showing the previous set until a call that does update
	 *  (any other mutator, or this one with the flag set) lands. */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool UpdateInstanceTransform(int32 InstanceIndex, const FTransform& NewInstanceTransform, bool bWorldSpace = false, bool bMarkRenderStateDirty = true);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool GetInstanceTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace = false) const;

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	void ClearInstances();

	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Instances")
	int32 GetInstanceCount() const { return PerInstanceTransforms.Num(); }

	/** Replace the whole instance set in one go — one proxy rebuild instead of N. */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	void SetInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace = false);

	// -------------------------------------------------------------------------
	// Instances — GPU source
	// -------------------------------------------------------------------------

	/** Draw from compute-written instance buffers instead of the CPU array. While a GPU source is
	 *  set the CPU array is ignored (cluster culling is skipped — the source has no cluster table —
	 *  and per-instance frustum/distance culling and LOD selection still run). */
	void SetInstanceSourceGPU(const FCSGpuInstanceSourceGPU& InSource);

	/** Same, but from a GPU point cloud: the proxy turns each point into an instance whose +Z is
	 *  the point normal. Mutually exclusive with SetInstanceSourceGPU. */
	void SetInstanceSourceFromPoints(const FCSGpuInstancePointSourceGPU& InSource);

	void ClearInstanceSourceGPU();
	bool HasInstanceSourceGPU() const { return GpuInstanceSource.IsValid() || GpuPointSource.IsValid(); }

	// -------------------------------------------------------------------------
	// Culling / LOD
	// -------------------------------------------------------------------------

	/** Instances per cluster in the coarse cull level. Larger clusters make the cluster pass
	 *  cheaper but reject less. Only used by the CPU instance source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 InstancesPerCluster = 64;

	/** Instances farther than this from the view are dropped. 0 disables the distance cull. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "0"))
	float InstanceEndCullDistance = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling")
	bool bGpuFrustumCulling = true;

	/** Off pins every instance to LOD0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling")
	bool bGpuLODSelection = true;

	/** Multiplies the source mesh's LOD screen sizes; > 1 keeps higher LODs longer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "0.01"))
	float LODScreenSizeScale = 1.0f;

	// -------------------------------------------------------------------------
	// Render-thread accessors (used by the scene proxy at creation time)
	// -------------------------------------------------------------------------

	const FCSGpuInstancedBaseMesh& GetBaseMeshSnapshot() const { return BaseMeshSnapshot; }
	const TArray<FVector4f>& GetPackedInstances() const { return PackedInstances; }
	const TArray<FVector4f>& GetClusterBounds() const { return ClusterBounds; }
	const FCSGpuInstanceSourceGPU& GetInstanceSourceGPU() const { return GpuInstanceSource; }
	const FCSGpuInstancePointSourceGPU& GetInstancePointSourceGPU() const { return GpuPointSource; }

	/** The buffer set the proxy binds and the cull writes. Null / unallocated until the component
	 *  has both a base mesh and instances to draw. */
	UCSMesh* GetGpuMesh() const { return InstancedGpuMesh; }

	/** The numbers the current buffer set was sized from. The proxy culls with these and must not
	 *  re-derive them — see FCSGpuInstancedGpuLayout. */
	const FCSGpuInstancedGpuLayout& GetGpuLayout() const { return GpuLayout; }

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	/** Builds the GPU mesh that the mutators skipped while the component was unregistered. This runs
	 *  before CreateRenderState_Concurrent, which is the whole point: the render state may be
	 *  created off the game thread during the end-of-frame update, where the build's render flush
	 *  would not be legal, so proxy creation is only ever allowed to read what already exists. */
	virtual void OnRegister() override;

	//~ UObject interface
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	//~ UCSGpuMeshComponent interface
	virtual UMaterialInterface* GetRenderMaterial() const override { return InstanceMaterial; }

private:
	/** Re-extracts BaseMeshSnapshot from BaseMesh (or leaves an externally supplied snapshot
	 *  alone) and recreates the render state. */
	void RebuildBaseMeshSnapshot();

	/**
	 * Morton-sorts the instances into clusters, packs the GPU source layout, recomputes LocalBounds
	 * and hands the result to the GPU mesh. Every mutator ends here, so no path can repack the
	 * instances and forget to upload them — the two used to be separated by a proxy rebuild, and
	 * with a retained mesh nothing else would ever notice the omission.
	 *
	 * bRebuildGpuMesh=false does the CPU half only, for a caller that is batching edits
	 * (UpdateInstanceTransform's bMarkRenderStateDirty). The GPU half blocks on a render flush, so
	 * it must not run once per instance in a loop.
	 */
	void RebuildInstanceData(bool bRebuildGpuMesh = true);

	/** The CPU half: Morton sort, packed rows, cluster spheres, LocalBounds. */
	void RebuildInstancePacking();

	/** Declares the stream layout, sizes the mesh and uploads the base mesh + instance source.
	 *  Blocks (render flushes). Releases the mesh instead when there is nothing to draw. */
	void RebuildGpuMesh();

	/** Hands the GPU buffers back and forgets the layout. The live proxy keeps its own references
	 *  to the pooled buffers, so it goes on drawing correctly until its render state is recreated. */
	void ReleaseGpuMesh();

	/** Instance-buffer capacity for a live count, with hysteresis. Grows to 1.5x when the count
	 *  passes what is held and shrinks only once three quarters of it are unused.
	 *
	 *  A change here no longer drags the base mesh through a reallocation — UCSMesh::ResizeStreamsSync
	 *  touches the instance-sized streams and nothing else — but it still throws away and re-clears
	 *  six buffers, which at large instance counts is tens of megabytes of visible-instance region
	 *  per call. So the ratchet stays; what it protects against just got much smaller. */
	uint32 ResolveInstanceCapacity(uint32 LiveInstanceCount) const;

	/** Instance transforms in component space, in insertion order. This is the serialized,
	 *  user-facing order; PackedInstances holds the same set in cluster order. */
	UPROPERTY()
	TArray<FTransform> PerInstanceTransforms;

	/** Base mesh uploaded to the GPU. Filled from BaseMesh, or directly by
	 *  SetBaseMeshFromGpuData (in which case bBaseMeshIsExternal suppresses re-extraction). */
	FCSGpuInstancedBaseMesh BaseMeshSnapshot;
	bool bBaseMeshIsExternal = false;

	// GPU source layout, cluster order. 5 float4 per instance — see FCSGpuInstanceSourceGPU.
	// Clusters are fixed-size runs of this array, so the cull shader derives an instance's cluster
	// arithmetically and no explicit range table is needed.
	TArray<FVector4f> PackedInstances;
	TArray<FVector4f> ClusterBounds; // centre.xyz + radius per cluster

	FCSGpuInstanceSourceGPU GpuInstanceSource;
	FCSGpuInstancePointSourceGPU GpuPointSource;

	/** The retained buffer set. Transient because GPU data does not survive a level reload; the
	 *  property exists to hold the object against GC, not to serialize it.
	 *
	 *  No OnMeshChanged subscription, unlike UCSMeshRenderComponent: this component is the only
	 *  thing that ever edits this mesh, so it already knows when to recreate its render state.
	 *  Subscribing would only re-enter the rebuild it is itself in the middle of. */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> InstancedGpuMesh;

	/** What InstancedGpuMesh's streams are currently sized for. Reset when the mesh is released, so
	 *  the ratchet does not survive the buffers it describes. */
	FCSGpuInstancedGpuLayout GpuLayout;
};
