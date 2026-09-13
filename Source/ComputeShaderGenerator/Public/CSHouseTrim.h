#pragma once

#include "CoreMinimal.h"
#include "CSHouseFrame.h"     // FBrickParams / FElement / AppendFlatRun / NextBrickSlot
#include "CSHouseProfile.h"   // FCSWallOpening / CSHouse_GetEdge —— 墙在哪儿、洞在哪儿只能有一个真源

/**
 * 包边石（TinyGladeHouse D7 的第三样；合卷卷一 A8 / 卷五 A11）。
 *
 * 两条带，同一套机制、只差一个高度：
 *
 *   · **墙顶包边（压顶石）**：骑在墙顶那一圈，盖住墙板顶面与屋面底之间那条 T 型接缝的横向断纹。
 *   · **墙脚包边（勒脚石）**：骑在房底那一圈，把墙板与地面之间那条硬交线压掉。
 *
 * 计划 D11 的实拍参考图注（`img/tiny-glade-ref-crenellation-trim.jpg`）把它和角石归成一族：
 * 「墙顶城齿、转角角石、勒脚石排 —— **同一种「离散块沿线累积」**」。TG 侧同向且更硬
 * （合卷卷五 §1、§3.3）：那边转角、墙裙、缝砖、雉堞全是同一个 `brick` × 逐实例非均匀缩放，
 * 138 个网格里砖只有一个，每砖 96 字节的记录**没有 mesh 索引字段**。
 *
 * -----------------------------------------------------------------------------
 * 与角石的分工：一个竖着摆、一个横着摆，共用一个 `SolveRun`
 * -----------------------------------------------------------------------------
 * 角石走 `CSHouseFrame::AppendColumn`（竖直段），包边走 `CSHouseFrame::AppendFlatRun`（平顶段）。
 * 砖数、铺装缩放、负缝（`FrameBrickBloat`）、容量截断、随机数基**四样全部共用** ——
 * 这正是卷五 A11 那条"别各写一套"的落点。
 *
 * -----------------------------------------------------------------------------
 * ⚠️ 必须避开洞，否则勒脚会横穿门洞
 * -----------------------------------------------------------------------------
 * 墙脚那条带的 Z 就是门洞的底（门 `Z0 = 0`）。不切的话，勒脚石会从门口正中一路铺过去 ——
 * 而且**砖数、零阻塞、三角形数所有断言都不会红**，只有出图看得见。
 *
 * 切法是把边的 S 区间减去"与本带 Z 区间相交"的洞的 S 区间。判据带 Z：高窗不该切断勒脚，
 * 落地门不该切断压顶。门樘那一侧的收口由门框砖负责、窗的洞缘由附属物自带的预制框盖住，
 * 所以这里只管让开，不管补。
 */
namespace CSHouseTrim
{
/** 一条包边带。`HalfHeight` 只参与"这个洞挡不挡得住我"的判据，不参与几何。 */
struct FBand
{
	/** 课程中心的墙空间 Z（从房底起算）。 */
	float CenterZ = 0.0f;
	/** 课程竖向半高 = `FrameBrickDepth / 2`（见 `AppendFlatRun` 的轴向说明）。 */
	float HalfHeight = 10.0f;

	float LowZ() const { return CenterZ - HalfHeight; }
	float HighZ() const { return CenterZ + HalfHeight; }
};

/** 一条边上的一段可铺区间（墙空间 S）。 */
struct FRun
{
	int32 EdgeIndex = 0;
	float S0 = 0.0f;
	float S1 = 0.0f;
	/**
	 * 两端各自的**剪切量** cm（≥ 0，0 = 这一头不挨着洞或洞不收窄）：端头那块砖的**顶边**
	 * 朝洞的方向多伸出这么多，把①按"带内最宽处"裁出来的那条缝补上（洞缘四级的③）。
	 * 底边不动 —— 底边正落在切口上，动了就把砖推进洞里。
	 */
	float ShearS0 = 0.0f;
	float ShearS1 = 0.0f;

	float Span() const { return S1 - S0; }
};

/**
 * 洞在这一带上挡掉的 S 区间（返回 false = 挡不住）。
 *
 * ⚠️ **2026-09-06：从"洞的包围盒"改成"洞的剪影"。** 原来是 `[S0(), S1()]` 整条全挡，对压顶 /
 * 勒脚那两条薄带无所谓（它们要么在拱脚以下、洞本来就是满宽，要么根本够不着洞），但砖层
 * （D4 两层之 A）是从房底摞到檐口的**一整摞**带 —— 按包围盒裁的话，拱洞会被切成一个矩形缺口，
 * 拱顶两侧本该有砖的地方全空了。判据换成 `CSHouse_OpeningSpanForBand`：每一带只挡"这个高度上
 * 洞真正有多宽"，拱形于是自然从砖里长出来。
 *
 * 对原有两条带**逐位无变化**：勒脚带落在拱脚以下 ⇒ 剪影就是满宽；压顶带够不着洞 ⇒ 照旧不挡。
 */
inline bool BlockedSpan(const FCSWallOpening& Opening, const FBand& Band, float& OutS0, float& OutS1)
{
	return CSHouse_OpeningSpanForBand(Opening, Band.LowZ(), Band.HighZ(), OutS0, OutS1);
}

/** 洞挡不挡得住这一带（不关心挡在哪儿的调用方用它）。 */
inline bool BlocksBand(const FCSWallOpening& Opening, const FBand& Band)
{
	float S0 = 0.0f, S1 = 0.0f;
	return BlockedSpan(Opening, Band, S0, S1);
}

/**
 * 一条边 → 减去挡路的洞之后的可铺段（**纯函数**，追加写，返回本次追加的段数）。
 *
 * `EdgeOpenings` 必须是**同一条边、按 `CenterS` 升序**的连续片段 —— 与
 * `CSHouseFrame::BuildEdgeElements` 同一个前置（`ACSHouseActor::ComputeDoors` 末尾那次排序
 * 已经保证）。乱序会让下面这个单游标扫掠漏掉洞，症状是勒脚从某个门口穿过去。
 *
 * `Clearance` 是洞两侧额外让开的距离 cm（让门樘砖有地方站，别和包边挤在一起）。
 */
inline int32 SplitEdge(int32 EdgeIndex, float SpanS0, float SpanS1, const FBand& Band, float Clearance,
	TArrayView<const FCSWallOpening> EdgeOpenings, float MinRun, TArray<FRun>& Out)
{
	const int32 Before = Out.Num();
	if (SpanS1 - SpanS0 <= MinRun) return 0;
	// 下面沿用原来的变量名：`EdgeLen` 现在是这条边**可铺区间的终点**，不再恒等于墙长。
	const float EdgeLen = SpanS1;

	float Cursor = SpanS0;
	// 上一个洞留给**下一段起点**的剪切量（那一段的 S0 挨着的正是它）。
	float PendingShear = 0.0f;
	for (const FCSWallOpening& Opening : EdgeOpenings)
	{
		// 剪影而不是包围盒：拱洞在高处只挡住窄窄一条，砖因此能跟着拱圈收进去。
		float BlockedS0 = 0.0f, BlockedS1 = 0.0f;
		if (!BlockedSpan(Opening, Band, BlockedS0, BlockedS1)) continue;
		const float Lo = FMath::Max(BlockedS0 - Clearance, SpanS0);
		const float Hi = FMath::Min(BlockedS1 + Clearance, EdgeLen);
		if (Hi <= Cursor) continue;                     // 已经被前一个洞吞掉（洞可能互相重叠）
		// 端头的剪切量：本段的 S1 与下一段的 S0 都挨着**这个**洞，所以两处同一个数。
		const float Shear = CSHouse_OpeningTopShear(Opening, Band.LowZ(), Band.HighZ());
		if (Lo - Cursor >= MinRun) Out.Add({ EdgeIndex, Cursor, Lo, PendingShear, Shear });
		PendingShear = Shear;
		Cursor = FMath::Max(Cursor, Hi);
	}
	if (EdgeLen - Cursor >= MinRun) Out.Add({ EdgeIndex, Cursor, EdgeLen, PendingShear, 0.0f });
	return Out.Num() - Before;
}

/**
 * 可铺段 → 砖路元素（追加写，返回本次追加的砖数）。
 *
 * 世界框架与门框砖**逐字同源**：原点取墙厚正中（`F.Start + F.In * T/2`），`AxisU` 沿边、
 * `AxisV` 竖直。换一个口径就会在有 yaw 的房子上与拱缘砖错开半个墙厚。
 *
 * 逐实例随机数从 **房子身份 + 家族盐 + 段序号** 派生，不从槽位派生：包边排在砖序最后，
 * 前面任何一段（开一扇门、来一个邻居）都会把它的槽位整体推走。
 */
inline int32 BuildTrimElements(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	const TArray<FRun>& Runs, const FBand& Band, uint32 Seed, uint32 FamilySalt,
	const CSHouseFrame::FBrickParams& Params, TArray<CSHouseFrame::FElement>& InOutElements)
{
	int32 Cursor = CSHouseFrame::NextBrickSlot(InOutElements);
	const int32 Before = Cursor;
	const double T = FMath::Max(double(WallThickness), 0.0);

	for (int32 Index = 0; Index < Runs.Num(); ++Index)
	{
		const FRun& Run = Runs[Index];
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Run.EdgeIndex, Footprint, float(T));
		const FVector Mid(F.Start.X + F.In.X * (T * 0.5), F.Start.Y + F.In.Y * (T * 0.5), 0.0);

		CSHouseFrame::FWallFrame Frame;
		Frame.Origin = FVector3f(World.TransformPosition(Mid));
		Frame.AxisU = FVector3f(World.TransformVectorNoScale(FVector(F.U.X, F.U.Y, 0.0))).GetSafeNormal();
		Frame.AxisV = FVector3f(World.TransformVectorNoScale(FVector::UpVector)).GetSafeNormal();
		Frame.AxisN = (-FVector3f(World.TransformVectorNoScale(FVector(F.In.X, F.In.Y, 0.0)))).GetSafeNormal();

		CSHouseFrame::AppendFlatRun(Frame, Run.S0, Run.S1, Band.CenterZ,
			CSHouseFrame::PathRandomBase(Seed, FamilySalt, Index), Params, InOutElements, Cursor,
			Run.ShearS0, Run.ShearS1);
	}
	return Cursor - Before;
}

/**
 * 一条带的完整算法：四条边各自按洞切段、再逐段出砖。返回砖数，`OutRuns` 供哈希与断言用。
 *
 * `AllOpenings` 是整栋房子的洞表（已按 `(边, CenterS)` 排序）。
 */
inline int32 BuildBand(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	const FBand& Band, float Clearance, TArrayView<const FCSWallOpening> AllOpenings,
	uint32 Seed, uint32 FamilySalt, const CSHouseFrame::FBrickParams& Params,
	TArray<FRun>& OutRuns, TArray<CSHouseFrame::FElement>& InOutElements)
{
	OutRuns.Reset();
	const float MinRun = FMath::Max(Params.Length, 1.0f) * 0.5f;

	for (int32 Edge = 0; Edge < Footprint.NumEdges(); ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, WallThickness);
		if (F.Len <= MinRun) continue;

		// 砖路走墙厚正中（`BuildTrimElements` 的原点），所以可铺区间取**中线处的斜接区间**：
		// 两条边的砖在角平分线上相遇，谁都不吃下整个转角方块。转角外侧那一小块缺口由角石盖住
		// （TG 同构：墙角是 `add_wall_corners` 的角石柱，不是两排砖互相搭接）。
		// ⚠️ 关掉角石时那块缺口会露出来 —— 那是斜接的代价，不是 bug。直角对接时代是偶数边吃下
		// 整个转角、奇数边缩进去，它只在偶数条直角边上成立，折线化之后没有等价物。
		float SpanS0 = 0.0f, SpanS1 = 0.0f;
		F.SpanAtDepth(WallThickness * 0.5f, WallThickness, SpanS0, SpanS1);

		// 同一条边的洞是洞表里的连续片段（调用方保证排序）。这里线性挑出来，不重排。
		int32 Begin = 0;
		while (Begin < AllOpenings.Num() && AllOpenings[Begin].EdgeIndex != Edge) ++Begin;
		int32 End = Begin;
		while (End < AllOpenings.Num() && AllOpenings[End].EdgeIndex == Edge) ++End;

		SplitEdge(Edge, SpanS0, SpanS1, Band, Clearance,
			AllOpenings.Slice(Begin, End - Begin), MinRun, OutRuns);
	}
	return BuildTrimElements(World, Footprint, WallThickness, OutRuns, Band, Seed, FamilySalt, Params, InOutElements);
}
}
