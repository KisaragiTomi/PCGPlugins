#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

/**
 * GPU 实例源的**共用容器与容量管理**（TinyGladeHouse D6/D9/D12/D13）。
 *
 * 文件名与命名空间是历史遗留：这里原本是塑形物石阶（`ACSGroundShaperActor::RebuildSteps`）
 * 的 GPU 散布 —— CPU 解等高线与铺装、`RDG_SmoothSpline` 求值 B 样条、`CSGroundSteps.usf`
 * 逐记录组装实例行。**那条路已随 2026-08-30「裁决一」整条删除**（石阶现在只有地面自己那条
 * `CSGroundStairs.usf`，门框砖走 `CSHouseFrame.usf` 的解析推导）。
 *
 * 剩下的是四条 GPU 实例路（门框砖 / 藤枝 / 藤叶 / 摆件）都在用的三样东西：
 *   · `FPaletteBuffers` —— 一个 palette 条目的 packed 实例行 + counter + 基础网格尺寸；
 *   · `ReserveCapacity` —— 注册期一次付清的定容预留（交互期因此永远走不到扩容那次阻塞）；
 *   · `ReleaseOnRenderThread` —— 把最后一份引用交回渲染线程释放。
 *
 * 另外还留着一个 `ResampleUniform`：它**没有产线消费者**，只服务于单测
 * `House.FrameAnalyticMatchesLegacy` 里那份旧路 CPU 镜像（见它自己的注释）。
 */
namespace CSShaperSteps
{
/**
 * 把折线重采样成等弧长间距的点列（首末点逐位保留）。
 *
 * ⚠️ **产线消费者为零，这是有意的**（2026-08-30 裁决一）：它当初是门框旧路
 * （`ACSHouseActor::BuildFramePlan`，B 样条 + 逐砖记录）的必需品 —— 均匀三次 B 样条的参数
 * 按控制点序号走，而排块算的是弧长占比，两者只有在控制点等距时才等价；不重采样的话
 * 「左门樘底 -> 拱缘 -> 右门樘底」那条 U 形路径疏密比 16.9:1，19 块砖有 17 块挤在拱上、
 * 互相穿模成薄鳍片。旧路删掉之后这条约束随之作废：解析推导直接从拱参数算砖心，没有样条。
 *
 * **留着它是因为等价性判据要它**：单测 `House.FrameAnalyticMatchesLegacy` 里那份旧路
 * CPU 镜像（`CSHouseTest_LegacyFrameBricks`，不调任何产线代码）必须逐字复现旧路的组路方式，
 * 而这段重采样正是其中一步。它是"解析推导没跑偏"这条唯一守卫的一部分，**不是死代码**。
 * 另有 `House.FrameCurveUniform` 直接钉它自己的契约。
 *
 * 首末点必须逐位保留 —— 镜像靠"两端各外延一格控制点 + 只取 [1/(N-1), (N-2)/(N-1)]
 * 参数区间"把端点钉在设计位置上，端点一动这条纪律就失效。
 */
inline void ResampleUniform(const TArray<FVector>& InPoints, int32 SegmentCount, TArray<FVector>& OutPoints)
{
	if (InPoints.Num() < 2 || SegmentCount < 1)
	{
		OutPoints = InPoints;
		return;
	}

	TArray<double> Cumulative;
	Cumulative.SetNumUninitialized(InPoints.Num());
	Cumulative[0] = 0.0;
	for (int32 Index = 1; Index < InPoints.Num(); ++Index)
	{
		Cumulative[Index] = Cumulative[Index - 1] + FVector::Dist(InPoints[Index - 1], InPoints[Index]);
	}
	const double Total = Cumulative.Last();
	if (Total <= UE_KINDA_SMALL_NUMBER)
	{
		OutPoints = InPoints;
		return;
	}

	OutPoints.Reset(SegmentCount + 1);
	OutPoints.Add(InPoints[0]);
	// 游标只向前走：目标弧长单调递增，重扫一遍是 O(N·M) 而这里 O(N+M)。
	int32 Segment = 1;
	for (int32 Step = 1; Step < SegmentCount; ++Step)
	{
		const double Target = Total * Step / SegmentCount;
		while (Segment < InPoints.Num() - 1 && Cumulative[Segment] < Target) ++Segment;
		const double Span = Cumulative[Segment] - Cumulative[Segment - 1];
		const double T = Span > UE_KINDA_SMALL_NUMBER
			? FMath::Clamp((Target - Cumulative[Segment - 1]) / Span, 0.0, 1.0) : 0.0;
		OutPoints.Add(FMath::Lerp(InPoints[Segment - 1], InPoints[Segment], T));
	}
	OutPoints.Add(InPoints.Last());
}

// --- 交互期的稳态化：把"随尺寸连续变化的量"吸成阶梯 -------------------------------------
//
// 起因（实测，2026-08-30 拉尺寸那一轮）：门框砖实例源的包围盒直接由 FootprintSize / WallHeight
// 算出，而交接的包围盒阈值只有 1 cm ⇒ 拖动中每一帧都重走一次阻塞的 SetInstanceSourceGPU；
// 房体的 EnsureCapacitySync 按精确顶点数要容量 ⇒ 每一帧重新分配 + 拷贝。两条都是
// "值连续变 ⇒ 资源每帧重来"的同一个失败模式。解法同型：先留余量、再对齐到台阶、并且只涨不缩。
//
// ⚠️ **这两条必须只有一份**（2026-08-31 从 `CSHouseActor.cpp` 的匿名命名空间搬到这里）：
// 地面那一侧的裙边摆件走的是同一条纪律，各抄一份的话两组常数迟早分叉，
// 而症状是"房子那边零阻塞、地面这边每帧一次"——两边的断言各自都绿。
//
// 可陈述的保证是**一段把尺寸涨大不到 25% 的拖动，一次交接都不付**；越过时也只付一次，
// 随后又有一整段余量。代价只是剔除保守一点、显存多留一点，全量重建时会重新收紧。

inline constexpr double BoundsHeadroom = 1.25;   // 25% 余量：够吃下任何一段帧级拖动
inline constexpr double BoundsQuantum = 200.0;   // cm，包围盒台阶
inline constexpr int32 CapacityStep = 4096;      // 顶点/索引/实例容量的台阶

/** 留余量 + 向上对齐到台阶。只用于**保守**量（包围盒、容量），不能用在几何尺寸上。 */
inline double QuantizeUp(double Value)
{
	return FMath::CeilToDouble(FMath::Max(Value, 0.0) * BoundsHeadroom / BoundsQuantum) * BoundsQuantum;
}

/** 同一条纪律的容量版。`UCSMesh::EnsureCapacitySync` 只涨不缩，但"涨"本身就是一次阻塞刷新。 */
inline int32 ReserveCount(int32 Exact)
{
	const int64 Want = int64(FMath::Max(Exact, 1)) * 3 / 2;
	return int32(FMath::Max<int64>(CapacityStep, FMath::DivideAndRoundUp<int64>(Want, CapacityStep) * CapacityStep));
}

/** 一个 palette 条目对应的 GPU 实例源（直接喂 UCSGpuInstancedMeshComponent）。 */
struct FPaletteBuffers
{
	TRefCountPtr<FRDGPooledBuffer> PackedInstances;   // Buffer<float4>，5 个 / 实例
	TRefCountPtr<FRDGPooledBuffer> Counter;           // Buffer<uint>，[0] = 活跃数
	uint32 Capacity = 0;
	FVector3f BaseSphereCentre = FVector3f::ZeroVector;   // 基础网格局部包围球（未缩放）
	float BaseSphereRadius = 0.0f;

	/**
	 * 三轴块尺寸。基础网格自带真实尺寸时留 (1,1,1)；
	 * 喂"单位立方体字典 mesh"时由它给出实际尺寸 —— Tiny Glade 的 brick 正是一块
	 * 100³ 的居中立方体，**非均匀缩放本身就是砖的尺寸**（逆向报告 §1.4：
	 * "Affine3Packed transform，非均匀缩放 = 砖尺寸"）。
	 * 轴向约定同 kernel（`CSHouseFrame.usf`）：X = 面内径向、Y = 沿曲线、Z = 平面法线。
	 */
	FVector3f BlockSize = FVector3f(1.0f, 1.0f, 1.0f);

	bool IsValid() const { return PackedInstances.IsValid() && Counter.IsValid() && Capacity > 0; }
	void Reset() { *this = FPaletteBuffers(); }
};

/**
 * 按下限预留容量。
 *
 * 存在的理由是**时机**：分配必须在渲染线程，因此扩容那一趟一定阻塞。把它挪到注册/加载期
 * 一次付清，交互期（画笔刷、拖房子）就永远走不到扩容分支 —— 那正是"交互热路径零设备同步"
 * 这条纪律要求的。不预留的话，第一次真的长出砖的那一笔会当场付一次 flush，而且它出现在
 * 哪一笔完全取决于用户画到哪里，最难复现也最难归因。
 *
 * 只涨不缩；已经够大就一次 enqueue 都不发。
 */
bool ReserveCapacity(TArray<FPaletteBuffers>& Palettes, uint32 MinCapacityPerPalette);

/** 把最后一份引用交回渲染线程释放，避免在游戏线程上把在途帧正在读的显存抽走。 */
void ReleaseOnRenderThread(TArray<FPaletteBuffers>& Palettes);

/**
 * 把每个 palette 的 counter 清零。**不阻塞**（只录一趟 pass）。
 *
 * 存在的理由是本项目栽过的那一类 bug：生产者判定"这一轮一件都没有"于是**提前返回、
 * 顺手撤掉实例源**，counter 却还留着上一代的数。撤实例源同时会清空交接缓存，于是下一次
 * `EnsureXxxComponent` 把**同一批 buffer** 又交回组件 —— 剔除 pass 照着陈旧计数器再画一遍
 * 上一代的实例，而 CPU 侧的计数、三角形数、零阻塞断言**全部照绿**。
 *
 * 每条散布 kernel 自己的空表分支都已经会显式清零，但那要求 kernel **真的被调到**；
 * 提前返回的那几条路走不到它。所以那些路必须在撤实例源**之前**显式调这一条。
 */
void ZeroCounters(TArray<FPaletteBuffers>& Palettes);
}
