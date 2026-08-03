#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderMeshGenerator.h"
#include "ComputeShaderMeshBoolean.generated.h"

class UMaterialInterface;
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
	AComputeShaderMeshBoolean();

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

	/** Debug：用 DrawDebugPoint 画碰撞球采样点 + 面内起点，核对形态与密度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility|Debug")
	bool bDrawDebugSamples = false;

	/** Debug：最多为多少个三角画采样点（防刷爆视口）；<=0 表示不限。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|RayVisibility|Debug")
	int32 DebugDrawMaxTriangles = 8;

	/**
	 * Debug 上色：勾选后**不删**内部面，而是保留全部面、把"埋在实体内部"的三角形涂成红色
	 * vertex color，并把组件材质切换成 /PCGPlugins/MeshBoolean/M_VertexColor。取消勾选则正常
	 * 布尔（删内部面）并回退默认材质。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean")
	bool bDebugColor = false;

	/**
	 * Debug 上色时用的材质（默认 M_VertexColor，显示红/白 vertex color）。
	 * 置空(None)则 debug 几何（保留全部面）改用**源 mesh 原材质**，不套 debug 材质。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Boolean")
	TObjectPtr<UMaterialInterface> DebugMaterial;

	/**
	 * 保留源网格的材质槽结构：源 mesh 有几个槽，输出就有几个槽——即使这些槽指向同一个材质，
	 * 或者全都没指定材质（输出得到同样数量的空槽，可事后逐槽填）。
	 * 关掉则按材质指针去重，材质列表最紧凑，但槽位结构会丢：同材质的槽会被合并，
	 * 所有空槽也会合并成一个。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Boolean|Output")
	bool bPreserveSourceMaterialSlots = true;

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
	 * @param bRecomputeNormals 输出后重算法线
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Boolean")
	UStaticMesh* SplitInterpenetratingBoxScene(bool bRecomputeNormals = false);

	/**
	 * Stage A + Stage B：切分后再用 GPU 缠绕数分类筛面，得到布尔结果（self-winding）；输出绕序保持源三角形约定。
	 * @param Op 布尔运算；ArrangementOnly 时等价于 SplitInterpenetratingBoxScene。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Boolean")
	UStaticMesh* BooleanBoxScene(ECSMeshBooleanOp Op, bool bRecomputeNormals = false);

private:
	/** Split / Boolean 的共用实现。Op 决定是否跑 Stage B 缠绕数分类。 */
	UStaticMesh* RunBooleanInternal(ECSMeshBooleanOp Op, bool bRecomputeNormals);

};
