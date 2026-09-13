#pragma once

#include "CoreMinimal.h"
#include "CSHouseProfile.h"   // FCSHouseFootprint / CSHouse_GetEdge —— 折线版推拉

/**
 * 拉尺寸（计划 D5）的纯函数层：单边推拉。
 *
 * **本文件不含任何交互设施**（抓手 actor / gizmo / EdMode / 命中体全部不在这一轮范围）。
 * 这里只解决「尺寸连续变化时派生物跟得住、不抖」的那半，尺寸从属性 / 蓝图 / 测试改都走同一条路。
 * 无 GPU、无 world、无编辑器依赖，可直接进 automation 测试。
 *
 * ── 为什么单边推拉要抽成纯函数（不是洁癖） ──────────────────────────────────────
 * 「对侧不动、中心随动」= `FootprintSize += Offset` 且 actor 中心沿该墙外法线移动 `Offset/2`。
 * 两个量必须**同时**改，分开写就会出现"尺寸改了、中心没跟上"的半步状态，而那个状态在画面上
 * 表现为**对侧墙也跟着走** —— 与"拖 1 m 墙走了 2 m"那个父子回路缺陷（计划 D5）逐像素相同，
 * 极易误诊。抽成一个函数 + 两条单测（Offset=0 幂等；Offset=Δ 后对侧墙世界位置逐位不变）
 * 就把这一类错误一次钉死。
 *
 * ── 尺寸禁带已删除（2026-08-31，四坡屋顶）────────────────────────────────────
 * 禁带（裁决四「房屋尺寸更换有最小距离」）与脊向滞回是一对，两者唯一的用途都是**挡住双坡
 * 屋顶在 X 穿过 Y 时那次 90° 原地翻面**。屋顶改成四坡以后脊向由长轴连续导出、正方形处脊长
 * 为 0，**翻轴这个事件不存在了**，禁带也就没有要挡的东西 —— 连同 `FCSHouseResizeBand` /
 * `CSHouseResize_ApplyBand` / `_RawMatches` / `_WouldFlipRidge` 与 `RawFootprintSize`
 * 累加器一并删除。硬下界 `MinFootprint` 保留，它与翻轴无关。
 */

/** 边号 → 被这条边推动的是哪一维（true = X，false = Y）。与 `CSHouse_GetEdge` 同号：0 南 1 东 2 北 3 西。 */
inline bool CSHouseResize_EdgeDrivesX(int32 EdgeIndex)
{
	return (EdgeIndex & 1) != 0;
}

/** 边号 → **局部**外法线（指离房子）。与 `CSHouse_GetEdge` 的 `In` 严格反号，别另写一套。 */
inline FVector2D CSHouseResize_EdgeOuterLocal(int32 EdgeIndex)
{
	switch (EdgeIndex & 3)
	{
	case 0:  return FVector2D(0, -1);
	case 1:  return FVector2D(1, 0);
	case 2:  return FVector2D(0, 1);
	default: return FVector2D(-1, 0);
	}
}

/** 边号 + yaw → **世界**外法线（Z 恒 0）。 */
inline FVector CSHouseResize_EdgeOuterWorld(int32 EdgeIndex, float YawDegrees)
{
	const FVector2D L = CSHouseResize_EdgeOuterLocal(EdgeIndex);
	return FRotator(0.0, double(YawDegrees), 0.0).RotateVector(FVector(L.X, L.Y, 0.0));
}

/**
 * 单边推拉：被推的墙沿外法线走 `Offset`，**对侧墙世界位置逐位不变**。
 *
 * 返回**实际**生效的位移（经 `MinFootprint` 下限修正之后）。调用方必须用返回值而不是传入的
 * Offset 去记账 —— 拖拽 handle 的累加器（计划 D5 的"记账量法"）一旦记成请求值而不是生效值，
 * 卡在下限上时残差就会一路累积，松手瞬间房子跳一大截。
 */
inline float CSHouse_ApplyEdgePush(FVector2D& InOutSize, FVector& InOutCenter, int32 EdgeIndex,
	float YawDegrees, float Offset, float MinFootprint)
{
	const bool bDrivesX = CSHouseResize_EdgeDrivesX(EdgeIndex);
	const double Current = bDrivesX ? InOutSize.X : InOutSize.Y;

	const double Desired = FMath::Max(Current + double(Offset), double(FMath::Max(MinFootprint, 1.0f)));
	const double Applied = Desired - Current;
	if (Applied == 0.0) return 0.0f;   // 幂等早退：Offset=0 连调 N 次不许改动任何量

	if (bDrivesX) InOutSize.X = Desired; else InOutSize.Y = Desired;
	InOutCenter += CSHouseResize_EdgeOuterWorld(EdgeIndex, YawDegrees) * (Applied * 0.5);
	return float(Applied);
}

/**
 * **折线版**单边推拉（footprint 折线化 3f）：第 `EdgeIndex` 条边沿自己的外法线平移，两端顶点沿
 * 相邻两条边滑动（相邻边所在的直线不变）；之后把包围盒中心移回局部原点、actor 中心反向补上。
 * 于是**除被推的那条边以外，每条边的世界位置都不变** —— 矩形版「对侧不动、中心随动」是它的特例。
 *
 * 下限三条，都只收住"往里推"或"推穿"：
 *  ① 外法线方向上的宽度不低于 `MinFootprint`（矩形上就是被推的那一维，与矩形版逐字同一条）；
 *  ② 两条相邻边不许被推到短于 1 cm（往里推时它们变短）；
 *  ③ 被推的边本身不许短于 1 cm（锐角折线往外推时它变短）。
 * 返回实际生效的位移，记账纪律与矩形版相同。
 *
 * 矩形房子（`FootprintShape` 为空）仍走上面的矩形版：那条的算术被 `House.EdgePush` 按位钉着；
 * 两者在矩形上一致由 `House.EdgePushPolyline` 在容差内钉住。
 */
inline float CSHouse_ApplyEdgePushPolyline(FCSHouseFootprint& InOutLocal, FVector& InOutCenter, int32 EdgeIndex,
	float YawDegrees, float Offset, float MinFootprint)
{
	const int32 N = InOutLocal.NumEdges();
	if (N < 3 || EdgeIndex < 0 || EdgeIndex >= N) return 0.0f;
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, InOutLocal, 0.0f);
	const FCSHouseEdgeFrame Prev = CSHouse_GetEdge((EdgeIndex + N - 1) % N, InOutLocal, 0.0f);
	const FCSHouseEdgeFrame Next = CSHouse_GetEdge((EdgeIndex + 1) % N, InOutLocal, 0.0f);
	if (F.Len <= 0.0f || Prev.Len <= 0.0f || Next.Len <= 0.0f) return 0.0f;
	const FVector2D Normal(-F.In.X, -F.In.Y);

	// ① 宽度：凸折线上被推的边就是外法线那一侧的支撑线，推 d 宽度恰好变 d。
	double MinProj = TNumericLimits<double>::Max(), MaxProj = -TNumericLimits<double>::Max();
	for (const FVector2D& V : InOutLocal.Verts)
	{
		const double P = FVector2D::DotProduct(V, Normal);
		MinProj = FMath::Min(MinProj, P);
		MaxProj = FMath::Max(MaxProj, P);
	}
	const double Current = MaxProj - MinProj;
	double Applied = FMath::Max(Current + double(Offset), double(FMath::Max(MinFootprint, 1.0f))) - Current;

	// 顶点沿相邻边滑动的速率：起点沿 Prev.U 走 d / CPrev，终点沿 Next.U 走 d / CNext。
	// 凸角上 CPrev = sin(起点转角) > 0、CNext = −sin(终点转角) < 0；共线顶点速率发散，退化成沿法线平移。
	const double CPrev = FVector2D::DotProduct(Prev.U, Normal);
	const double CNext = FVector2D::DotProduct(Next.U, Normal);
	constexpr double MinLen = 1.0;
	if (CPrev > 1.0e-6) Applied = FMath::Max(Applied, (MinLen - double(Prev.Len)) * CPrev);      // ② 上一条边
	if (CNext < -1.0e-6) Applied = FMath::Max(Applied, (MinLen - double(Next.Len)) * -CNext);    // ② 下一条边
	// ③ 本边：两端沿本边方向各走 d·cot(转角)，合起来缩短 d·(cotA + cotB)。转角小于 90° 时往外推变短，
	//    大于 90°（锐角折线）时往里推变短，两个方向各收一次。矩形上 cot 90° = 0，这条不起作用。
	const double Shrink = (CPrev > 1.0e-6 ? FVector2D::DotProduct(Prev.U, F.U) / CPrev : 0.0)
		- (CNext < -1.0e-6 ? FVector2D::DotProduct(Next.U, F.U) / CNext : 0.0);
	if (Shrink > 1.0e-9) Applied = FMath::Min(Applied, FMath::Max((double(F.Len) - MinLen) / Shrink, 0.0));
	else if (Shrink < -1.0e-9) Applied = FMath::Max(Applied, FMath::Min((double(F.Len) - MinLen) / Shrink, 0.0));
	if (Applied == 0.0) return 0.0f;   // 幂等早退：Offset=0 连调 N 次不许改动任何量

	const int32 Start = EdgeIndex;
	const int32 End = (EdgeIndex + 1) % N;
	InOutLocal.Verts[Start] += CPrev > 1.0e-6 ? Prev.U * (Applied / CPrev) : Normal * Applied;
	InOutLocal.Verts[End] += CNext < -1.0e-6 ? Next.U * (Applied / CNext) : Normal * Applied;

	// 包围盒中心回到局部原点，actor 中心补同一段（世界量：过 yaw）。
	const FVector2D Centre = InOutLocal.GetBounds().GetCenter();
	for (FVector2D& V : InOutLocal.Verts) V -= Centre;
	InOutCenter += FRotator(0.0, double(YawDegrees), 0.0).RotateVector(FVector(Centre.X, Centre.Y, 0.0));
	return float(Applied);
}
