#pragma once

#include "CoreMinimal.h"
#include "CSHouseFrame.h"    // FBrickParams / FElement / EPathFamily
#include "CSHouseTrim.h"     // FBand / FRun / BuildBand —— 一条按洞裁过的横带
#include "CSHouseProfile.h"  // FCSWallOpening / CSHouse_GetEdge

/**
 * **砖层**（两层墙之 A，用户裁决 2026-09-06「挖洞策略改成 Tiny Glade 的真两层」）。
 *
 * 一句话：**砖层 = 一摞包边带**。`CSHouseTrim::BuildBand` 本来就是"沿四条边铺一行砖、
 * 按洞把行切断"，把它从两条（压顶 / 勒脚）叠到从房底一直摞到檐口，就是整面砖墙。
 * 复用而不是另写，是卷五 A11 那条纪律的直接后果 —— TG 侧同向且更硬：转角、墙裙、缝砖、
 * 雉堞、承重柱在那边**全是同一个 `brick` × 逐实例非均匀缩放**，138 个网格里砖只有一个。
 *
 * -----------------------------------------------------------------------------
 * 洞缘的四级里，这里做掉了①②
 * -----------------------------------------------------------------------------
 * 计划 D4「洞缘的四级处理」（读 `_wall_wall_brick_lod0` 的 VS/PS 之后订正的那份）：
 *
 *   ① **删实例**：整块落在洞内的砖不发 —— `CSHouseTrim::SplitEdge` 把边的 S 区间减去
 *      "与本行 Z 区间相交"的洞的 S 区间。判据带 Z，所以高窗不切勒脚、落地门不切压顶。
 *      TG 对位物 `utils::trim_rows` 走 `split_heightmap_by_threshold<RowTrimmerSink>` `[PDB]`。
 *   ② **水平贴合**：`AppendFlatRun` 的 `SolveRun` 解出一个铺装缩放，让整数块砖**正好填满**
 *      切出来的那一段 ⇒ 洞两侧那一列砖天然抵住洞缘。TG 对位物是逐实例非均匀缩放
 *      （`InstancedWallData` 里没有 mesh 索引字段，改尺寸只能改 transform）。
 *
 * ③（洞上缘那些砖的局部 z 按洞高曲线缩放，TG 的 `flags & 32`）与 ④（普通砖逐像素裁）
 * **都在 GPU 侧**，不在本文件。③ 只服务门拱：窗的洞缘由附属物自带的预制框盖住
 * （2026-09-06 裁决，与 TG 同构）。
 *
 * -----------------------------------------------------------------------------
 * ⚠️ 容量是硬上限，只截断不扩容
 * -----------------------------------------------------------------------------
 * `FBrickParams::MaxBricks` 与门框 / 接缝 / 角石 / 包边**共用同一份常驻容量**：砖层只是同一个
 * 组件里排在最后的那些行。撞到上限就停发，**绝不扩容** —— 扩容是一次阻塞刷新，落在用户恰好
 * 画到的那一笔上（零阻塞纪律）。所以动工前先用 `EstimateBricks` 对一次预算：一面 6×4 m、
 * 檐高 3 m 的房子按 26 cm 砖 / 20 cm 层高约 **1400 块**，比默认容量大一个量级。
 */
namespace CSHouseBrickWall
{
/**
 * 砖层的分层方案：从房底往上均匀摞，最后一层的**顶**对齐檐口。
 *
 * 层高取砖的**进深**（`AppendFlatRun` 里砖的进深轴朝上，见那条注释），所以一层的竖向占位
 * 就是 `FrameBrickDepth`。层数向上取整之后回算实际层高，避免檐口下留一条半层的缝。
 */
struct FCourses
{
	int32 Count = 0;
	/** 实际层高 cm（= WallHeight / Count，可能略小于请求值）。 */
	float Height = 0.0f;

	/** 第 `Index` 层的中心 Z（墙空间，房底为 0）。 */
	float CenterZ(int32 Index) const { return (float(Index) + 0.5f) * Height; }
	/** 判据用的半高。取实际层高的一半 ⇒ 相邻层的 Z 区间首尾相接、不重不漏。 */
	float HalfHeight() const { return Height * 0.5f; }
};

/**
 * 分层。`RequestedHeight <= 0` 或墙高 <= 0 ⇒ 空方案（`Count == 0`），调用方据此整层跳过。
 */
inline FCourses PlanCourses(float WallHeight, float RequestedHeight)
{
	FCourses Out;
	if (WallHeight <= 0.0f || RequestedHeight <= 0.0f) return Out;

	// 向上取整：宁可层高比请求的略矮，也不要在檐口下留半层。
	Out.Count = FMath::Max(FMath::CeilToInt(WallHeight / RequestedHeight), 1);
	Out.Height = WallHeight / float(Out.Count);
	return Out;
}

/**
 * 砖数的**上界**估计（不减洞）：四条边的周长 / 砖长 × 层数。
 *
 * 只用来对预算与写断言 —— 真实值一定不大于它（洞只会让砖变少），而且每段不足半块砖时
 * `AppendFlatRun` 直接不出，所以短段还会再少一点。
 */
inline int32 EstimateBricks(const FCSHouseFootprint& Footprint, float WallThickness, const FCourses& Courses, float BrickLength)
{
	if (Courses.Count <= 0) return 0;
	const float Length = FMath::Max(BrickLength, 1.0f);

	float Perimeter = 0.0f;
	for (int32 Edge = 0; Edge < Footprint.NumEdges(); ++Edge)
	{
		Perimeter += FMath::Max(CSHouse_GetEdge(Edge, Footprint, WallThickness).Len, 0.0f);
	}
	// 每条边各自向上取整会更准，但这里要的是**上界**，一次取整就够，且与边数无关地偏大。
	return FMath::CeilToInt(Perimeter / Length) * Courses.Count;
}

/**
 * 铺整面砖墙：逐层调 `CSHouseTrim::BuildBand`，返回实际发出的砖数。
 *
 * `OutRuns` 是**每层复用**的暂存（`BuildBand` 内部会 `Reset`），传进来只为免掉逐层分配；
 * 想要哈希的话在回调里取，别指望它在返回后还留着最后一层以外的内容。
 *
 * 逐实例随机数从 **房子身份 + 家族盐 + 层号** 派生（`PathRandomBase` 的第三个参数换成层号），
 * 不从槽位派生 —— 砖层排在砖序最后，前面任何一段（开一扇门、来一个邻居）都会把它整体推走，
 * 跟着槽位走就会在改门的那一刻让整面墙的随机色重新洗一遍。
 */
template <typename FCourseSink>
inline int32 BuildWall(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	const FCourses& Courses, float Clearance, TArrayView<const FCSWallOpening> AllOpenings,
	uint32 Seed, const CSHouseFrame::FBrickParams& Params,
	TArray<CSHouseTrim::FRun>& OutRuns, TArray<CSHouseFrame::FElement>& InOutElements,
	FCourseSink&& OnCourse)
{
	int32 Total = 0;
	for (int32 Index = 0; Index < Courses.Count; ++Index)
	{
		CSHouseTrim::FBand Band;
		Band.CenterZ = Courses.CenterZ(Index);
		Band.HalfHeight = Courses.HalfHeight();

		// 家族盐固定、层号进随机数基 ⇒ 每层的随机序列不同，但只要层号不变就逐位可复现。
		const int32 Added = CSHouseTrim::BuildBand(World, Footprint, WallThickness, Band, Clearance,
			AllOpenings, Seed, CSHouseFrame::EPathFamily::BrickWall + uint32(Index), Params,
			OutRuns, InOutElements);
		Total += Added;
		OnCourse(Index, Band, Added, OutRuns);

		// 撞到容量上限之后再摞也只是空转（`AppendFlatRun` 会返回 0），提前收手省掉逐层扫洞表。
		if (Added == 0 && Index > 0 && Total >= Params.MaxBricks) break;
	}
	return Total;
}
}
