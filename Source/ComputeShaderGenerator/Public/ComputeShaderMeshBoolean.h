#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshTypes.h"
#include "ComputeShaderMeshGenerator.h"
#include "ComputeShaderMeshBoolean.generated.h"

class UCSMesh;
class UStaticMesh;

/** Stage B 布尔运算。当前用 self-winding（整个 soup 一起求缠绕数）实现，见各枚举说明。 */
UENUM(BlueprintType)
enum class ECSMeshBooleanOp : uint8
{
	/** 只做 Stage A：消除穿插，不做 inside/outside 删面（= SplitInterpenetratingBoxScene）。 */
	ArrangementOnly,
	/** self-union：删掉埋在实体内部的面，保留并集外壳（阈值 = WindingIsoThreshold，默认 0.5）。 */
	Union,
	/** 保留 |winding| >= WindingIsoThreshold 区域的边界；两实体交集把阈值设 ~1.5。 */
	KeepInside,
	/** 反：保留 |winding| < WindingIsoThreshold 区域的边界（外部外壳）。 */
	KeepOutside,
};

/**
 * Boolean policy, lifted out of the actor so the operator library can run the same pipeline
 * without one.
 *
 * The actor keeps every UPROPERTY it ever had and packs them into this struct on the way in
 * (MakeBooleanOptions), so serialized Blueprint values and existing call sites are untouched;
 * this is the parameter set, not a second source of truth.
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshBooleanOptions
{
	GENERATED_BODY()

	/** Ceiling on source triangles collected from the box. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "1"))
	int32 MaxSourceTriangles = 5000000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean")
	bool bReadLandscape = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "1"))
	int32 CutSegmentsPerTriangle = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "1024"))
	int32 MaxCutSegmentsHardCap = 200000000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean")
	float SideEpsilon = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean")
	float MinCutSegmentLength = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float CoplanarAngleDegrees = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "0.0"))
	float CoplanarOffsetEpsilon = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean")
	float WindingIsoThreshold = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean", meta = (ClampMin = "0.001"))
	float WindingSampleOffset = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean", meta = (ClampMin = "1.1"))
	float WindingBeta = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "1"))
	int32 VisibilityRayCount = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "90.0", ClampMax = "180.0"))
	float VisibilityHalfAngleDegrees = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "0.0"))
	float VisibilityShellRadius = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "0.0"))
	float VisibilitySampleDensity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "0.0"))
	float VisibilityRayBiasEpsilon = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility")
	bool bKeepBackFacingVisible = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|PostProcess", meta = (ClampMin = "0.0"))
	float RetainedTriangleExpansionDistance = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|PostProcess", meta = (ClampMin = "0.0"))
	float VertexWeldDistance = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Output")
	bool bPreserveSourceMaterialSlots = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|GPU Arrangement", meta = (ClampMin = "0.0"))
	float SnapRoundQuantum = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|GPU Arrangement", meta = (ClampMin = "2"))
	int32 ArrangementOutputTrianglesPerSource = 8;
};

/**
 * One Boolean pipeline run's raw output, held in the two representations the two output
 * rebuilds consume.
 *
 * Why this exists: **the pipeline is not run-to-run deterministic.** The source soup is filled
 * by a CAS bump allocator (ExtractStaticMeshTrianglesCS), so a given source triangle's soup
 * index moves between runs; the BSP orders each triangle's cuts by the *index* of the other
 * triangle that produced them (BSPCutOtherOwner), so the cut application order moves with it,
 * and with it the tessellation. Two runs of the same Boolean on the same scene return slightly
 * different triangle counts.
 *
 * That makes "run the pipeline twice and compare the two outputs" useless as a parity check —
 * it compares two different fragment sets, not two implementations. A capture is what lets both
 * rebuilds be handed the *same* fragments: the CPU arrays a consumer reads back and the pooled
 * buffers held here are the same bytes, so a comparison across them isolates the rebuild.
 *
 * Lifetime: pooled render buffers and raw material pointers, valid only for the synchronous
 * call that produced it. Not reflected, and not for storage.
 */
struct COMPUTESHADERGENERATOR_API FCSMeshBooleanCapture
{
	/** The arrangement's sub-triangle soup (3 float3 per fragment) and its per-fragment
	 *  encoded source (low bits source triangle, high bits Stage B classification). */
	TRefCountPtr<FRDGPooledBuffer> FragmentSoup;
	TRefCountPtr<FRDGPooledBuffer> FragmentSource;

	/** The source triangle soup's per-corner attributes, parallel to the source triangles. */
	TRefCountPtr<FRDGPooledBuffer> SourceVertices;
	TRefCountPtr<FRDGPooledBuffer> SourceNormals;
	TRefCountPtr<FRDGPooledBuffer> SourceUVs;
	TRefCountPtr<FRDGPooledBuffer> SourceColors;
	TRefCountPtr<FRDGPooledBuffer> SourceTangents;
	TRefCountPtr<FRDGPooledBuffer> SourceBiTangents;
	TRefCountPtr<FRDGPooledBuffer> SourceMaterialIds;

	/** The extraction's material registry; the per-triangle ids index it. */
	TArray<UMaterialInterface*> MaterialRegistry;

	uint32 SourceTriangleCount = 0;
	uint32 FragmentCount = 0;
	/** Fragments the accept predicate keeps, i.e. the output triangle count. GPU-counted, so a
	 *  consumer can size a resident allocation without guessing. */
	uint32 OutputTriangleCount = 0;
	int32 SourceUVChannels = 1;
	/** Stage B ran, so MB_SRC_KEEP filtering applies. */
	bool bStageB = false;
	/** Conservative world bound, for a consumer that has nothing better until it reduces. */
	FBox QueryBox = FBox(ForceInit);

	bool IsValid() const;
};

/**
 * 场景三角形 GPU 穿插切分 / 布尔子类。设计见 Docs/SceneTriangleBooleanSplit.md。
 *
 * 场景 soup 提取、LBVH、三角求交、CSR、BSP 重三角化、inside/outside 分类、可见性救回与
 * 可选顶点焊接均在同一个 RDG 中完成。Stage A 不回读；最终 emit 才决定哪些子三角进入输出。
 * RDG 完成后只回读最终状态、有效输出前缀和构建 transient UStaticMesh 所需的源属性。
 *
 * LBVH、fast-winding preprocessing 和 position-weld passes 由父类的 protected
 * convenience API 转发到 CSGpuTriangleUtilities。这里仅保留阈值、采样、属性合并和
 * 拓扑清理等 Boolean policy，避免其他生成器反向依赖本子类。
 */
UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API AComputeShaderMeshBoolean : public AComputeShaderMeshGenerator
{
	GENERATED_BODY()

public:
	/** 是否把地形（landscape）也纳入切分的场景三角形。false 时只读 static mesh，不读地形。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean")
	bool bReadLandscape = false;

	/** 交线段全局列表容量倍率：容量 = 三角数 * 该值，再受 MaxCutSegmentsHardCap 限制。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "1"))
	int32 CutSegmentsPerTriangle = 4;

	/** 交线段全局列表硬上限（限制 GPU 常驻 buffer 体积）。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "1024"))
	int32 MaxCutSegmentsHardCap = 200000000;

	/** 判定顶点“全在平面一侧”的带宽（世界单位）。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean")
	float SideEpsilon = 0.01f;

	/** 交线段最短长度（世界单位）；更短的按退化/共享边丢弃。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean")
	float MinCutSegmentLength = 0.05f;

	/** 共面判定：两三角二面角小于该角度（度）即视为共面，沿彼此的边互相切割。归一化，与三角尺寸无关。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float CoplanarAngleDegrees = 1.0f;

	/** 共面判定：两三角平面偏移小于该距离（世界单位 cm）才算同一平面；平行但不同面的不切割。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean", meta = (ClampMin = "0.0"))
	float CoplanarOffsetEpsilon = 0.1f;

	/** 输出三角形反转绕序，以匹配基类 scene triangle-soup 的正面约定（默认 true）。 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean")
	/** Legacy compatibility flag; output triangles always preserve source winding. */
	bool bReverseOrientation = true;

	/**
	 * Stage B Boolean classification policy, not part of the shared winding builder.
	 * The common facility computes a field; this threshold decides which field values
	 * this particular Boolean operation treats as inside.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean")
	float WindingIsoThreshold = 0.5f;

	/**
	 * Boolean-specific two-sided sampling distance. It remains on the derived actor
	 * because other winding consumers may sample points, voxels, or only one side.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean", meta = (ClampMin = "0.001"))
	float WindingSampleOffset = 0.5f;

	/**
	 * Fast-winding accuracy requested by this consumer. The implementation is shared,
	 * but keeping the setting here avoids imposing Boolean tuning on every base actor.
	 * Larger beta is more accurate and slower; 2.5 is recommended for order 1.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean", meta = (ClampMin = "1.1"))
	float WindingBeta = 2.5f;

	/** Legacy compatibility only; the shared GPU LBVH always stores one triangle per leaf. */
	UPROPERTY(BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean", meta = (ClampMin = "1"))
	int32 WindingBVHLeafSize = 8;

	/** 射线可见性：每采样起点射出的方向数（球面 Fibonacci 截到锥内；命中一条前向逃逸即提前结束）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "1"))
	int32 VisibilityRayCount = 64;

	/** 射线可见性：碰撞球方向锥半角（度）。90=前向半球，180=全球；仅 bKeepBackFacingVisible=true 时 >90° 生效。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "90.0", ClampMax = "180.0"))
	float VisibilityHalfAngleDegrees = 120.0f;

	/** 射线可见性：碰撞球半径（世界单位 cm）；射线飞这么远仍未命中即逃逸=可见。0=按场景 bounds 自动。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "0.0"))
	float VisibilityShellRadius = 0.0f;

	/** 射线可见性：每单位面积的采样起点数（点/cm²）。起点数 = clamp(ceil(Density*TriArea), 1, 200)，正比于面积；0=只用质心。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "0.0"))
	float VisibilitySampleDensity = 0.0f;

	/** 射线可见性：射线起点沿方向的自相交避让偏移（世界单位 cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility", meta = (ClampMin = "0.0"))
	float VisibilityRayBiasEpsilon = 0.05f;

	/** 射线可见性救回：是否把「从外面只看得到反面」的三角形也视为外部可见。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility")
	bool bKeepBackFacingVisible = false;

	/** 旧版兼容字段，现已不参与输出绕序。所有救回三角形都保持其源三角形朝向。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility")
	bool bFlipBackFacingNormals = true;

	/** 旧版兼容字段，现已不参与分类。Ray Visibility 只救回可见的 winding 误删面，不会因不可见而删除三角形。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility")
	bool bDeleteRayVisibilityFailedTriangles = true;

	/** 最终 emit 时增加 winding 双侧采样偏移，救回扩展边界带。0 表示禁用。 */
	/** Applies only to BooleanBoxScene Stage B; SplitInterpenetratingBoxScene is ArrangementOnly and ignores it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|PostProcess", meta = (ClampMin = "0.0"))
	float RetainedTriangleExpansionDistance = 0.0f;

	/**
	 * Boolean output policy controlling whether the shared weld facility is invoked.
	 * The distance stays here because other generators may need different seam/material
	 * rules; only the position-to-representative algorithm is common. Zero disables it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|PostProcess", meta = (ClampMin = "0.0"))
	float VertexWeldDistance = 0.0f;

	/**
	 * 保留源网格的材质槽结构：源 mesh 有几个槽，输出就有几个槽——即使这些槽指向同一个材质，
	 * 或者全都没指定材质（输出得到同样数量的空槽，可事后逐槽填）。
	 * 关掉则按材质指针去重，材质列表最紧凑，但槽位结构会丢：同材质的槽会被合并，
	 * 所有空槽也会合并成一个。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Output")
	bool bPreserveSourceMaterialSlots = true;

	/**
	 * 输出的 StaticMesh 启用 Nanite。布尔结果通常是百万级三角的一次性产物，交给 Nanite 做
	 * LOD 与剔除比手工 LOD 现实得多，渲染开销也基本与三角数脱钩，因此默认开启。
	 * 代价是构建时多一步 Nanite 数据生成（大网格上是秒级）、资产体积变大。
	 * 需要给不支持 Nanite 的管线（半透明、部分自定义材质、移动端）用时关掉。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Output")
	bool bOutputNanite = true;

	/** Last generated transient StaticMesh. Material slots preserve the source triangle material IDs. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "CS Mesh Boolean|Output")
	TObjectPtr<UStaticMesh> OutputStaticMesh;

	/**
	 * GPU BSP 重三角化的 snap-round 量化（世界单位 cm）。为避免大世界坐标下 float32 ULP 不可表示，
	 * 实际量化 = max(SnapRoundQuantum, 场景 bounds 最大边 * 2^-18)，且在“平移到 QueryBox.Min 的帧”里做。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|GPU Arrangement", meta = (ClampMin = "0.0"))
	float SnapRoundQuantum = 0.01f;

	/**
	 * 子三角输出容量 = 源三角数 × 该倍率（再夹到 1536 MiB 硬预算）。切分后的子三角数实测约为
	 * 源三角数的 2.5 倍，默认 8 留了 3 倍余量。调大更耐受高度互切的场景，代价是显存；
	 * 若日志报 outOverflow 就把它调大。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|GPU Arrangement", meta = (ClampMin = "2"))
	int32 ArrangementOutputTrianglesPerSource = 8;

	/**
	 * Stage A：对 GeneratorBounds 盒内的场景三角形消除互相穿插，返回切分后的 transient StaticMesh。
	 * 不创建或覆盖 Content Browser 资产；需要持久化时由调用方显式保存。同步执行（内部 FlushRenderingCommands）。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Boolean")
	UStaticMesh* SplitInterpenetratingBoxScene();

	/**
	 * Stage A + Stage B：切分后再用 GPU 缠绕数分类筛面，得到布尔结果（self-winding）；输出绕序保持源三角形约定。
	 * @param Op 布尔运算；ArrangementOnly 时等价于 SplitInterpenetratingBoxScene。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Boolean")
	UStaticMesh* BooleanBoxScene(ECSMeshBooleanOp Op);

	/** Packs this actor's UPROPERTYs into the policy struct the pipeline actually reads. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Boolean")
	FCSMeshBooleanOptions MakeBooleanOptions() const;

	/**
	 * Runs the Boolean pipeline and returns the result as a CPU mesh snapshot, stopping short
	 * of building any asset. Splitting the one-shot entry point here is what lets the operator
	 * library put the result into a UCSMesh (where it can be welded, transformed, drawn or
	 * saved) instead of only ever producing a transient StaticMesh.
	 *
	 * Note what is and is not GPU-resident: the arrangement, classification and visibility
	 * work all run on the GPU, but the output attribute reconstruction (barycentric UV /
	 * normal / tangent / colour interpolation onto the sub-triangles, and the per-corner
	 * material-slot table) is CPU work reading a readback. So the *pipeline* is chainable on
	 * the GPU from here on, while the Boolean itself still round-trips once.
	 *
	 * Synchronous (internal FlushRenderingCommands). Game thread only.
	 *
	 * Passing OutCapture also hands back the pipeline output this rebuild consumed — the same
	 * fragments and source attributes, as the GPU buffers the CPU arrays were read back from.
	 * That is what lets a caller feed the GPU rebuild identical data instead of a second,
	 * differently-tessellated run of the pipeline (see FCSMeshBooleanCapture). It costs one
	 * extra counting dispatch and is otherwise inert; the default leaves every existing call
	 * site unchanged.
	 */
	bool RunBooleanToSnapshot(
		ECSMeshBooleanOp Op,
		const FCSMeshBooleanOptions& Options,
		FCSGpuMeshCPUData& OutMeshData,
		TArray<UMaterialInterface*>& OutMaterials,
		FCSMeshBooleanCapture* OutCapture = nullptr);

	/**
	 * The GPU output rebuild on its own: takes one captured pipeline run and writes Target's
	 * resident streams. This is the half that was ported from the CPU rebuild — the accept
	 * predicate and the barycentric attribute interpolation — with the pipeline factored out.
	 *
	 * Split out from RunBooleanToGpuMesh so it can be pointed at a capture somebody else
	 * produced. Since the pipeline is not run-to-run deterministic, that is the only way to
	 * compare this rebuild against the CPU one on equal terms.
	 *
	 * Replaces Target's contents and its material table. Game thread only; blocks.
	 */
	bool RebuildGpuMeshFromCapture(const FCSMeshBooleanCapture& Capture, UCSMesh* Target);

	/**
	 * Runs the same Boolean pipeline and writes the result straight into Target's resident
	 * streams, replacing its contents. The GPU counterpart of RunBooleanToSnapshot: no mesh
	 * data ever reaches the CPU.
	 *
	 * What the two share is the graph itself (one builder, one set of passes). What differs is
	 * only the consumer: the snapshot path reads the sub-triangle soup and the source attributes
	 * back and rebuilds the output attributes on the CPU; this one runs that same barycentric
	 * rebuild as a compute kernel over buffers the GPU already holds. The output is expected to
	 * agree to float precision — MeshBoolean.GpuParity is what holds them together.
	 *
	 * One small readback survives and cannot be removed: a ~26-uint status block carrying the
	 * source triangle count, the fragment count, the overflow flags and the output triangle
	 * count. UCSMesh capacity is a CPU-side allocation and only the GPU knows how big the result
	 * is, so the size has to come back before the streams can be sized. No attribute does.
	 *
	 * Target->Materials is replaced by the extraction's material registry plus one trailing
	 * empty slot, and the per-triangle id stream indexes that table: a source triangle with
	 * CS_NO_MATERIAL_ID (or an out-of-range registry id) lands in the trailing slot rather than
	 * in registry slot 0. Slot NUMBERING therefore differs from RunBooleanToSnapshot, which
	 * builds a deduplicated table in first-use order; the material each triangle resolves to
	 * does not.
	 *
	 * Returns false — leaving Target untouched — when the pipeline produces nothing, and also
	 * when Options.VertexWeldDistance > 0, which this path does not implement (see the comment
	 * on the implementation). Callers that must always produce a result fall back to
	 * RunBooleanToSnapshot. Synchronous (internal FlushRenderingCommands). Game thread only.
	 */
	bool RunBooleanToGpuMesh(
		ECSMeshBooleanOp Op,
		const FCSMeshBooleanOptions& Options,
		UCSMesh* Target);

private:
	/** Split / Boolean 的共用实现。Op 决定是否跑 Stage B 缠绕数分类。 */
	UStaticMesh* RunBooleanInternal(ECSMeshBooleanOp Op);

};
