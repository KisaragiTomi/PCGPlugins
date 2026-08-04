#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * 显存预算预检：在启动一个按三角形数线性吃显存的 GPU 管线之前，先估算它会占多少显存、
 * 当前设备还剩多少，超限时给出（可选的）交互确认。
 *
 * 之所以做成无状态的公共 facility 而不是某个 generator 的成员：预算模型的输入只有
 * "每源三角多少字节" 和 "有多少三角"，与具体是 Boolean、Fill 还是 ShallowWater 无关。
 * 各 generator 只负责用自己的参数填 FTriangleSoupCostModel（哪些 buffer 会分配、
 * 倍率是多少），阈值判定、显存查询和弹窗策略在这里统一。
 *
 * 注意：这里只管**显存**。CPU 侧（MeshDescription 提取、回读数组、StaticMesh 构建）
 * 另有开销，AvailablePhysicalMemory 仅作为附带信息报告，不参与上限计算。
 */
namespace CSGpuMemoryBudget
{
	/** 某一时刻的设备内存快照。单位字节；0 表示该项未知。 */
	struct FMemorySnapshot
	{
		/** 显卡本地显存总量（DXGI DedicatedVideoMemory / RHI 报告值）。 */
		int64 TotalVideoMemory = 0;

		/** 当前还能用的显存。bAvailableIsMeasured=false 时是按 RHI 跟踪量推算的粗略值。 */
		int64 AvailableVideoMemory = 0;

		/** true = 来自适配器实时预算（含其他进程占用）；false = 由总量减 RHI 跟踪用量推算。 */
		bool bAvailableIsMeasured = false;

		/** >0 表示驱动已经在把本地显存赶到系统内存——此时预算实际已经爆了。 */
		int64 DemotedVideoMemory = 0;

		/** 附带报告的可用物理内存，不参与上限计算。 */
		int64 AvailablePhysicalMemory = 0;
	};

	/** 查询当前设备内存。可在任意线程调用；D3D12 下走 DXGI 实时预算，其余 RHI 走推算。 */
	COMPUTESHADERGENERATOR_API FMemorySnapshot QueryMemorySnapshot();

	/**
	 * 三角形 soup 管线的每源三角显存开销模型。
	 *
	 * 每个字段对应一类真实会分配的 buffer；系数在 .cpp 中按各 buffer 的
	 * CreateBuffer 容量式写死，改了分配式就要同步改这里。模型是线性的，不考虑管线内部
	 * 的硬上限（如 Boolean 的 1536 MiB 输出封顶）——那些封顶只在早已超预算的规模上才生效。
	 */
	struct FTriangleSoupCostModel
	{
		/** 每源三角预留的交线段数（Boolean 的 CutSegmentsPerTriangle）。0 = 不分配交线 buffer。 */
		int32 CutSegmentsPerTriangle = 0;

		/** 每源三角预留的输出子三角数（Boolean 的 ArrangementOutputTrianglesPerSource）。0 = 无输出 buffer。 */
		int32 OutputTrianglesPerSource = 0;

		/** 是否构建三角 LBVH（Nodes/Parent/Atomic/Morton 排序键值）。 */
		bool bBuildLBVH = false;

		/** 是否构建 fast-winding 多极矩场（两个 5×NodeCount 的 float4 buffer，很贵）。 */
		bool bBuildWindingField = false;

		/** 是否对输出跑顶点焊接（每输出三角额外 3 个 uint32 代表元）。 */
		bool bWeldOutput = false;

		/** 源 soup 里实际会分配的顶点属性。位置永远分配。 */
		bool bSourceNormals = true;
		bool bSourceTangents = true;
		bool bSourceColors = true;
		bool bSourceUVs = true;
		bool bSourceMaterialIds = true;

		/** 调用方自己的额外每三角开销，用于模型没覆盖的 buffer。 */
		double ExtraBytesPerSourceTriangle = 0.0;

		/** 该模型下每个源三角的显存字节数。 */
		COMPUTESHADERGENERATOR_API int64 BytesPerSourceTriangle() const;
	};

	/** 在给定快照和安全系数下，该 cost 模型能承受的最大源三角数。 */
	COMPUTESHADERGENERATOR_API int64 EstimateMaxSourceTriangles(
		const FTriangleSoupCostModel& Cost,
		const FMemorySnapshot& Snapshot,
		float SafetyRatio);

	/** 盒内场景三角形的**廉价**预估结果（不加载 MeshDescription，不做逐三角裁剪）。 */
	struct FBoxSceneTriangleEstimate
	{
		/** 判定用的估算值：按包围盒重叠比例加权后的三角数。 */
		int64 EstimatedTriangles = 0;

		/** 保守上界：所有与盒相交的实例的完整三角数之和（不做任何重叠折算）。 */
		int64 UpperBoundTriangles = 0;

		/** 参与统计的 static mesh 实例数（ISM 每实例算一个）。 */
		int32 StaticMeshInstances = 0;

		/** 其中来自 landscape 的估算三角数（已计入 EstimatedTriangles）。 */
		int64 LandscapeTriangles = 0;
	};

	/**
	 * 预估 QueryBox 内的场景三角数。只读渲染数据的三角计数和组件包围盒，不触碰
	 * MeshDescription——真正的提取（PrepareBoxSceneTriangles）本身就是重活，不能拿来预检。
	 *
	 * 必须在 game thread 调用（遍历 UObject）。EstimatedTriangles 按包围盒重叠比例折算：
	 * 一个只有一角伸进盒里的大 mesh，实际进入 soup 的三角远少于它的总数。
	 */
	COMPUTESHADERGENERATOR_API FBoxSceneTriangleEstimate EstimateBoxSceneTriangles(
		UWorld* World,
		const FBox& QueryBox,
		int32 LODIndex = 0,
		bool bIncludeLandscape = false);

	/** 预检结论。 */
	enum class EBudgetVerdict : uint8
	{
		/** 估算值在预算内，没有打扰用户。 */
		WithinBudget,
		/** 超预算，用户在弹窗里选择了继续。 */
		ConfirmedByUser,
		/** 超预算，用户选择了取消。 */
		CancelledByUser,
		/** 超预算，但无法弹窗（无人值守/commandlet/非 game thread），按设置放行。 */
		ProceededUnattended,
		/** 超预算，无法弹窗，按设置中止。 */
		CancelledUnattended,
		/** 显存信息不可用，无法判定；一律放行并留下日志。 */
		BudgetUnknown,
	};

	struct FBudgetCheckResult
	{
		EBudgetVerdict Verdict = EBudgetVerdict::WithinBudget;

		/** 预估的源三角数。 */
		int64 EstimatedTriangles = 0;

		/** 当前显存下能承受的源三角上限。 */
		int64 MaxTriangles = 0;

		/** 预估峰值显存占用。 */
		int64 EstimatedBytes = 0;

		FMemorySnapshot Snapshot;

		bool ShouldProceed() const
		{
			return Verdict != EBudgetVerdict::CancelledByUser
				&& Verdict != EBudgetVerdict::CancelledUnattended;
		}
	};

	struct FBudgetCheckSettings
	{
		/** 可用显存中允许本次操作占用的比例。留出的余量给 RDG 池化、碎片和驱动开销。 */
		float SafetyRatio = 0.7f;

		/** 超限时是否弹窗询问。false = 直接按 bProceedWhenUnattended 处理并打日志。 */
		bool bPromptOnExceed = true;

		/** 无法弹窗时（无人值守、commandlet、非 game thread）是否照常继续。 */
		bool bProceedWhenUnattended = false;
	};

	/**
	 * 用已知的三角数做预检。超限时按设置弹窗/中止，并把判定写进日志。
	 * OperationName 会出现在弹窗与日志里（如 TEXT("Mesh Boolean")）。
	 */
	COMPUTESHADERGENERATOR_API FBudgetCheckResult CheckSourceTriangleBudget(
		const TCHAR* OperationName,
		int64 EstimatedTriangles,
		const FTriangleSoupCostModel& Cost,
		const FBudgetCheckSettings& Settings);

	/** EstimateBoxSceneTriangles + CheckSourceTriangleBudget 的组合入口。game thread only。 */
	COMPUTESHADERGENERATOR_API FBudgetCheckResult CheckBoxSceneTriangleBudget(
		const TCHAR* OperationName,
		UWorld* World,
		const FBox& QueryBox,
		int32 LODIndex,
		bool bIncludeLandscape,
		const FTriangleSoupCostModel& Cost,
		const FBudgetCheckSettings& Settings);
}
