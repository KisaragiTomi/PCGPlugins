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
 * -----------------------------------------------------------------------------
 * 实例路的顶点色通道字典（**只管这条路上的基础网格**）
 * -----------------------------------------------------------------------------
 *   RGB = 基础网格资产自带的顶点色，原样透传（`M_TinyGladeDecor` 就靠它当反照率）
 *   A   = **逐实例随机 0..1**，语义名 `CSInstanceRandom`
 *
 * 这不是 `ACSHouseActor` 那本 P2 冻结字典的扩充，两者管的是**不同的网格**：那本管房体三角汤
 * （非实例路、`UCSMeshRenderComponent` 画），这份管实例路的基础网格；两条路上的资产没有一件
 * 是共用的，扩这一份不会动到那一份。
 *
 * 为什么非要新占一条通道 —— 裁决六第三句「材质不能依赖只有 gpumesh 代理才提供的逐图元数据」：
 * 烘成 StaticMesh 之后**没有实例了**，`PerInstanceRandom` 在 GPU-Scene 里恒等于 0
 * （非实例图元的 `FInstanceSceneData::RandomID` 就是 0），于是 `lerp(0.88, 1.12, 0)` 让整片
 * 石头/藤/摆件烘成**同一个色**，而且**一条断言都不会红** —— 本项目的经典失效形状。
 *
 * 两条路读同一个语义，靠的是「两者恰好互斥地为零」：
 *   · GPU 实例路：上传基础网格时把 A **清零**（就在 `RebuildBaseMeshSnapshot` /
 *     `SetBaseMeshFromGpuData` 里，各一处），随机数由 packed 行的 `Origin.w` 送到
 *     `PerInstanceRandom`；
 *   · 烘焙路：`PerInstanceRandom` 恒 0，随机数写进该实例**全部顶点**的 A。
 * ⇒ 材质两条路一律写 `CSInstanceRandom = PerInstanceRandom + VertexColor.A`，取值逐位相同，
 *    材质图里没有任何分支、也不需要两份材质实例。
 * ⚠️ 代价说清楚：**实例路的基础网格顶点色 alpha 从此不可用**（现有消费者只有
 *    `M_TinyGladeDecor`，它只读 RGB）。要用 alpha 做别的（叶片遮罩之类）得先改这份字典。
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
	// 诊断 / 验收（**阻塞**，绝对不许出现在交互路径上）
	//
	// 存在的理由：本项目三次栽在"CPU 侧断言全绿、画面是错的"上，而三次都是靠人看图才发现。
	// 其中最难看见的一类是**陈旧的 GPU 计数器** —— 生产者把实例数记成 0 了，而 GPU 上那个
	// 被剔除 pass 消费的 Counter 还留着上一代的值，于是画面上东西还立着，所有无头断言照绿。
	// 下面这两条是那一类 bug 唯一的无头判据：一条读 GPU 上的真值，一条问"画的是不是我们
	// 以为的那张网格 / 那份材质"。
	//
	// ⚠️ 两条都会阻塞（回读就是阻塞），且都走 `UCSMesh::CountedBlockingFlush()` 让计数器
	// 数到它们 —— 十一条 `flushes=0` 断言因此会在它们被误用到交互路径上时**立刻报红**，
	// 这正是想要的结果。名字里的 `Debug...Sync` / `Diagnostics` 分类就是那道警戒线。
	// -------------------------------------------------------------------------

	/**
	 * 回读 GPU 上那个**真正被剔除 pass 与 indirect draw 消费**的实例计数器。
	 *
	 * 不是 CPU 侧的镜像、也不是生产者记的期望值 —— 读的就是
	 * `FCSGpuInstancedMeshSceneProxy::RunCulling` 绑成 `InstanceCount` 的那个 buffer。
	 * 「CPU 说 0、GPU 说 12」这类 bug 只有拿这个数才比得出来。
	 *
	 * 返回 −1 表示**读不到**（没分配 / 回读没完成），与"真的是 0 个实例"区分开：
	 * 把读失败当 0 会让守着"擦掉之后必须归零"的断言在管线坏掉时假绿。
	 * CPU 实例数组那条路上没有 GPU 计数器，剔除 pass 用的是常量 `NumSourceInstances`，
	 * 这里如实返回它（那也确实是 GPU 那一侧看到的数）。
	 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadDrawnInstanceCountSync() const;

	/**
	 * GPU 上画的基础网格 / 材质，是不是我们以为的那两样。空串 = 是。
	 *
	 * `IsXxxDrawable` 那一族只查"非空"，查不到**引擎有没有静默换掉它**：母材质没勾
	 * `bUsedWithInstancedStaticMeshes` 时实例路径会退回默认材质，症状与"没绑材质"逐像素
	 * 相同；基础网格那一半更隐蔽 —— 换掉 `BaseMesh` 资产时组件不一定重建，画面上还是旧网格。
	 * 所以这里把上传到 GPU 的那份网格**回读出来**跟资产对，而不是信任 CPU 侧的快照。
	 *
	 * ⚠️ 材质那一半只做得到"引擎会不会换"，做不到"这一帧画出来的像素用的是哪份着色器" ——
	 * 后者要有真的一帧渲染才存在。判不了的情形会在原因串里说明白，不会假装成通过。
	 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Diagnostics", meta = (DevelopmentOnly))
	FString DebugGetDrawnAssetMismatchSync() const;

	// -------------------------------------------------------------------------
	// 烘焙出口（裁决六 ①）—— **离线、阻塞，绝不许出现在交互路径上**
	// -------------------------------------------------------------------------

	/**
	 * 把这一族实例展开成一张 StaticMesh 资产。
	 *
	 * 为什么必须有：裁决六 ① 要求每一类 GPU 生成物都有一条走得通的 `SaveToStaticMesh`，
	 * 而实例路此前**一个出口都没有** —— 变换只活在 GPU buffer 里，网格路那条
	 * （`UCSMeshRenderComponent::SaveToStaticMesh`）读的是常驻三角流，这里根本没有那种流：
	 * 常驻的只有**一份**基础网格 + 一张逐实例变换表。所以这条出口做的是**展开**：
	 * 回读实例行 → 每个实例把 LOD0 的顶点乘上自己的变换 → 拼成一张三角汤 → 走
	 * `UCSGpuMeshComponent::BuildStaticMesh`（与网格路同一条落盘实现，材质槽/多组 UV/
	 * 顶点色的装配规则因此不会两条路各写一份）。
	 *
	 * 阻塞（回读 + StaticMesh 构建）是**有意**的：这是用户主动发起的离线操作。它同样会被
	 * `UCSMesh::GetBlockingFlushCount()` 数到，于是十一条 `flushes=0` 断言会在它被误接到
	 * 交互路径上的那一刻报红 —— 与 `Debug...Sync` 那一族同一道警戒线。
	 *
	 * BakeSpace：烘出来的局部空间。实例原点是**组件空间**的，这里先乘组件变换升到世界，
	 * 再由落盘层烘回 BakeSpace 的局部 —— 与岩壳/道路那两条出口同一口径（传 actor 变换即可）。
	 *
	 * ⚠️ 覆盖面：GPU 实例源（`SetInstanceSourceGPU`，门框砖/接缝砖/藤/摆件/石阶走这条）与
	 * CPU 实例数组都能烘。**点云源（`SetInstanceSourceFromPoints`）烘不出来** —— 它的实例行
	 * 是代理每帧在渲染图里现打的（`PackPointInstancesCS` 写进 aux 槽），组件手上只有点位与法线，
	 * 没有渲染过就压根不存在那批行。要么在 CPU 上把那段打包数学再写一遍（两份会分叉），
	 * 要么给它一条离线 pass —— 都超出本条的范围，故如实返回 nullptr 并落一条日志。
	 */
#if WITH_EDITOR
	UStaticMesh* SaveToStaticMesh(const FTransform& BakeSpace, const FString& AssetPathAndName = TEXT(""),
		bool bReplaceExistingAsset = true, bool bSaveAsset = false, bool bEnableNanite = false);
#endif

	/**
	 * 烘焙时喂给每个实例的随机数。GPU 源上它就是 packed 行里 `Origin.w`（也就是画面上
	 * `PerInstanceRandom` 读到的那个值），CPU 源上是按插入序算的同一个哈希。
	 *
	 * 暴露出来只为一件事：让断言能拿"烘出来的顶点色 A"和"GPU 上那一行的 .w"逐位对，
	 * 而不是各算各的。返回空 = 读不到（与"真的是 0 个实例"区分开）。
	 */
	bool DebugReadInstanceRandomsSync(TArray<float>& OutRandoms) const;

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
	/**
	 * 取本组件这一族**活着**的 packed 实例行（5 float4/实例，布局见 FCSGpuInstanceSourceGPU）。
	 * **阻塞**（GPU 源要回读两次：先计数器、再按计数取行）。
	 *
	 * 返回 false = **读不到**，与"真的是 0 个实例"分开 —— 把读失败当空集会让烘焙出口在回读
	 * 管线自己坏掉时安静地产出一张空网格，而空网格与"这一族本来就没实例"在资产里长得一模一样。
	 */
	bool ReadLiveInstanceRowsSync(TArray<FVector4f>& OutRows) const;

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

	/**
	 * 有变更在未注册期间被跳过，重注册时必须重建一次。
	 *
	 * 踩过的坑：这条旗子以前不存在，未注册期的 RebuildGpuMesh 直接 ReleaseGpuMesh() 了事 ——
	 * 而 Release 自己就是一次阻塞刷新，还把常驻缓冲整套扔掉，于是 OnRegister 必须重新
	 * AllocateSync + EditMeshSync。编辑器里改一个 actor 属性就会 RerunConstructionScripts
	 * （卸载全部组件 → 跑构造脚本 → 重注册），所以**拖尺寸的每一帧都要付这 3 次阻塞刷新**，
	 * 而且与容量、包围盒、砖数全都无关，纯粹是这一轮卸载/重注册的开销（实测 12 帧 36 次）。
	 * 现在未注册期只记脏、什么都不动，重注册时按这面旗子决定要不要真重建。
	 */
	bool bGpuMeshDirty = false;
};
