#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGpuInstancedMeshComponent.generated.h"

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
	// -------------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	int32 AddInstance(const FTransform& InstanceTransform, bool bWorldSpace = false);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	TArray<int32> AddInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace = false);

	/** Removes by swapping the last instance into the hole, so indices after InstanceIndex are
	 *  not stable — same contract as UInstancedStaticMeshComponent::RemoveInstance. */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool RemoveInstance(int32 InstanceIndex);

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

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

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

	/** Morton-sorts the instances into clusters, packs the GPU source layout and recomputes
	 *  LocalBounds. Cheap enough to run on every instance-set change. */
	void RebuildInstanceData();

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
};
