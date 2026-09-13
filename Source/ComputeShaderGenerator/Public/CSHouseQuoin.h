#pragma once

#include "CoreMinimal.h"
#include "CSHouseFrame.h"     // FBrickParams / FElement / AppendColumn / NextBrickSlot —— 角石是砖路的一条路径类型
#include "CSHouseProfile.h"   // FCSHouseFootprint / CSHouse_GetEdge / CSHouse_GetCorner —— 角在哪只有一个真源

/**
 * 转角角石（TinyGladeHouse D7 的**墙自身转角**那一半；合卷卷一 A7 / 卷五 A11）。
 *
 * -----------------------------------------------------------------------------
 * 它盖的是什么：不是穿模，是 UV 岛断裂
 * -----------------------------------------------------------------------------
 * 相邻两面墙在转角上**斜接**（`CSHouse_GetEdge`，footprint 折线化 3a-2 起）—— 零重叠、零缝隙，
 * 所以这里**没有任何穿模要遮**。真正的破绽是外角那条竖直棱两侧是两块各自起算 UV 的墙面，
 * 砖纹与灰泥纹**到角就断**。角石就是盖这个的，顺带把硬棱压掉。
 *
 * ⇒ **判据不是"有没有穿模"**（那条恒真），而是"外角那条竖直棱有没有被砖遮住"。
 *
 * -----------------------------------------------------------------------------
 * 哪些角出角石
 * -----------------------------------------------------------------------------
 * **转角在 (0°, 90°] 的凸角**，即内角不小于 90°（矩形、五边形、六边形、八边形的每个角）：
 *   · 凹角不出：外角那条棱朝里，是一道凹缝，没有要盖的外棱。
 *   · 锐角（内角 < 90°）不出：每层砖是一个**方正的盒子**、只能与一面墙对齐，另一面墙在锐角上
 *     往回折，砖的内侧后角会从那面墙里戳出 `−cos(转角) · 短边` —— 与 3.85 cm 的浅浅包角不是一回事。
 * 判据集中在 `IsQuoinCorner`。
 *
 * -----------------------------------------------------------------------------
 * 为什么不另起一套排布
 * -----------------------------------------------------------------------------
 * TG 侧的实证（合卷卷五 §1、§3.3）：`assets/meshes` 138 个网格里**砖只有一个** `brick`，
 * 而每砖 96 字节的 `InstancedWallData` **没有 mesh 索引字段** —— 转角、墙裙、缝砖、雉堞、
 * 承重柱在那边全是同一个砖 × 逐实例非均匀缩放，连独立资产都没有。对应到本项目：角石与
 * 接缝柱共用 `CSHouseFrame::AppendColumn`，与门框砖共用组件、容量、交接、剔除球、
 * `SaveToStaticMesh` 出口，以及"母材质勾没勾 `bUsedWithInstancedStaticMeshes`"那条执行面判据。
 *
 * 曲线是**一条竖直线段**，天然等弧长 ⇒ 不踩已删旧路那个「弧长比例 ≠ 样条参数」的坑
 * （门框的 U 形路径踩过，见合卷卷零「踩过的坑」末条）。
 *
 * -----------------------------------------------------------------------------
 * 与接缝柱（`CSHouseSeam::FCorner`）的区别
 * -----------------------------------------------------------------------------
 * 同名不同物，别混：
 *
 * | | 接缝柱 | 角石 |
 * | --- | --- | --- |
 * | 触发 | 两栋房 footprint 真相交 | 恒有，与邻居无关 |
 * | 位置 | 轮廓交点（数量随摆位变） | 自己 footprint 的每个合格凸角（矩形恒 4 根） |
 * | 随机数基 | 接缝身份（两房必须算出同一个） | 自己的 id + 角序号 |
 * | 归属 | 无（两房各画一份重叠的） | 这栋房独有 |
 */
namespace CSHouseQuoin
{
/** 一根角石柱。世界空间，`Outward` 是角平分线方向（单位）。 */
struct FQuoin
{
	FVector2D Point = FVector2D::ZeroVector;
	FVector2D Outward = FVector2D(1.0, 0.0);
	float BottomZ = 0.0f;
	float TopZ = 0.0f;
	/** 这根柱立在 footprint 的几号角上（`CSHouse_GetCorner` 的号）。不出角石的角被跳过，所以它不等于数组下标。 */
	int32 CornerIndex = 0;
	/**
	 * `cos(转角 / 2)`：GPU 靠它把平分线转回两面墙的外法线（`CSHouseQuoinLayout::PlaceQuoin`）。
	 * 直角 = 1/√2 —— 2026-09-13 之前 shader 里写死的那个常量。
	 */
	float HalfTurnCos = UE_INV_SQRT_2;
	/**
	 * 世界 Z 低于它的那截砖在材质里被 discard（`CSHouseFrame::FElement::CullBelowZ`）。
	 * `<= 0` = 整根照砌。
	 *
	 * **`BuildQuoins` 不填它**：这一根柱子该不该让路是"当前洞集合"的函数，而洞是房子那一层
	 * 才知道的事（`ACSHouseActor::BuildQuoinBricks` 按转角门的起拱线填）。放进纯函数里就得
	 * 把整张洞表传进来，纯函数与单测都要跟着变胖，换不来任何东西。
	 */
	float CullBelowZ = 0.0f;
};

/**
 * 矩形 footprint（`FCSHouseFootprint::MakeRect`）的四个角，局部 XY 的符号。
 * 顺序 = 边序（0:−Y, 1:+X, 2:+Y, 3:−X）之间的角，与 `CSHouse_GetCorner` 同号。单测拿它写矩形上的期望值。
 */
inline FVector2D CornerSign(int32 Index)
{
	switch (Index & 3)
	{
	case 0:  return FVector2D(+1.0, -1.0);   // 0 号墙与 1 号墙之间
	case 1:  return FVector2D(+1.0, +1.0);
	case 2:  return FVector2D(-1.0, +1.0);
	default: return FVector2D(-1.0, -1.0);
	}
}

/**
 * 出角石的角的下限：`cos(转角/2) ≥ 1/√2` 即转角不超过 90°。容差 1e-3 约合 0.16° 转角，
 * 给「本该是直角、顶点被旋转舍入过」的折线留余量。理由见文件头「哪些角出角石」。
 */
inline bool IsQuoinCorner(const FCSHouseCornerFrame& Corner)
{
	return Corner.IsConvex() && Corner.HalfTurnCos >= double(UE_INV_SQRT_2) - 1.0e-3;
}

/**
 * footprint 的角 → 角石柱（**纯函数**，单测直接吃它）。追加写，返回本次追加的柱数。
 *
 * `Inset` 是包角外棱沿角平分线向内缩的距离 cm。Point 是外角锚点，不是砖心。
 * GPU 按两面墙的法线把砖心移入墙体，使两张外表面只突出 3.85 × QuoinScale cm。
 *
 * ⚠️ **退化 footprint 直接不出**：任一边的外皮放不下两头的斜接让出量时，相邻两角互相吃掉，
 * 柱心会跑到房子外面去（矩形上就是「任一边短于两个墙厚」）。
 */
inline int32 BuildQuoins(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	float BaseZ, float WallHeight, float Inset, TArray<FQuoin>& Out)
{
	if (!Footprint.IsValidFootprint() || WallHeight <= 0.0f) return 0;
	const float T = FMath::Max(WallThickness, 0.0f);
	const int32 N = Footprint.NumEdges();
	for (int32 Edge = 0; Edge < N; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, T);
		if (F.Len <= 0.0f || F.Len < FMath::Max(F.InsetStart, 0.0f) + FMath::Max(F.InsetEnd, 0.0f)) return 0;
	}

	const int32 Before = Out.Num();
	for (int32 Corner = 0; Corner < N; ++Corner)
	{
		const FCSHouseCornerFrame C = CSHouse_GetCorner(Corner, Footprint);
		if (!IsQuoinCorner(C)) continue;
		const FVector2D LocalCorner = C.Point - C.Outward * double(Inset);

		const FVector WorldPoint = World.TransformPosition(FVector(LocalCorner.X, LocalCorner.Y, 0.0));
		const FVector WorldOut = World.TransformVectorNoScale(FVector(C.Outward.X, C.Outward.Y, 0.0));

		FQuoin Q;
		Q.Point = FVector2D(WorldPoint.X, WorldPoint.Y);
		Q.Outward = FVector2D(WorldOut.X, WorldOut.Y).GetSafeNormal();
		Q.BottomZ = BaseZ;
		Q.TopZ = BaseZ + WallHeight;
		Q.CornerIndex = Corner;
		Q.HalfTurnCos = float(C.HalfTurnCos);
		Out.Add(Q);
	}
	return Out.Num() - Before;
}

/** 矩形口径的便捷重载，走同一个核（单测的老夹具都是 `FVector2D` 尺寸）。 */
inline int32 BuildQuoins(const FTransform& World, const FVector2D& Footprint, float WallThickness,
	float BaseZ, float WallHeight, float Inset, TArray<FQuoin>& Out)
{
	return BuildQuoins(World, FCSHouseFootprint::MakeRect(Footprint), WallThickness, BaseZ, WallHeight, Inset, Out);
}

/**
 * 角石柱 → 砖路元素（追加写，返回本次追加的砖数）。
 *
 * 逐实例随机数从 **房子身份 + 角序号** 派生，不从槽位派生 —— 与接缝砖同一条理由的另一半：
 * 槽位会被"这栋房多开一扇门"推走，于是将来谁给砖材质接上 `PerInstanceRandom` 色差，
 * 开一扇门就会让四个角的角石整体换色，而所有几何断言全绿。
 */
inline int32 BuildQuoinElements(const TArray<FQuoin>& Quoins, uint32 Seed,
	const CSHouseFrame::FBrickParams& Params, TArray<CSHouseFrame::FElement>& InOutElements,
	const FVector2f& MeshInvSize)
{
	int32 Cursor = CSHouseFrame::NextBrickSlot(InOutElements);
	const int32 Before = Cursor;
	for (int32 Index = 0; Index < Quoins.Num(); ++Index)
	{
		const FQuoin& Q = Quoins[Index];
		// `AppendColumn` 要么追加**恰好一条**元素、要么一条都不加（半块砖摆不下 / 容量用尽），
		// 返回砖数即可分辨 —— 剔除高度粘在刚追加的那一条上。
		const int32 Added = CSHouseFrame::AppendColumn(Q.Point, Q.Outward, Q.BottomZ, Q.TopZ,
			CSHouseFrame::PathRandomBase(Seed, CSHouseFrame::EPathFamily::Quoin, Index),
			Params, InOutElements, Cursor);
		if (Added > 0 && !InOutElements.IsEmpty())
		{
			CSHouseFrame::FElement& E = InOutElements.Last();
			E.CullBelowZ = Q.CullBelowZ;
			E.QuoinMeshInvSize = MeshInvSize;
			E.QuoinHalfTurnCos = Q.HalfTurnCos;
			E.QuoinScale = FMath::Max(Params.Length, 1.0f) / 69.0f;
			// TG 的基准厘米按本项目层高折算；Jitter 改长边，SplitJitter 改分层。
			E.Jitter = FMath::Clamp(Params.Jitter, 0.0f, 16.0f) * E.QuoinScale;
			E.SplitJitter = FMath::Max(Params.SplitJitter, 0.0f) * E.QuoinScale;
		}
	}
	return Cursor - Before;
}
}
