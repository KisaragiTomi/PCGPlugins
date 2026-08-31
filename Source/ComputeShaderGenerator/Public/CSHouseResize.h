#pragma once

#include "CoreMinimal.h"
#include "CSHouseRoof.h"   // ECSRidgeAxis / CSHouseRoof_ChooseRidgeAxis —— 禁带宽度由滞回比反解，不是独立调参量

/**
 * 拉尺寸（计划 D5）的纯函数层：单边推拉 + 尺寸禁带。
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
 * ── 禁带（2026-08-30 裁决四，口径在 D5 动工时定死 = 本文件） ────────────────────
 * 用户原话：「房屋尺寸更换时有最小距离，避免产生翻轴现象。」落成规格是**在 |X − Y| 上开一条
 * 禁带**：推拉一旦让两边长度靠得太近就跳到带外沿，长宽比因此**永远不会停在翻轴阈附近**。
 *
 * 与滞回的分工（两者都要，缺一不可）：
 *  · `RidgeSwitchRatio = 1.15` 的滞回管的是"已经站在阈上时别来回翻"；
 *  · 禁带管的是"根本站不到阈上" —— 它把整段模糊区 [A/R, A·R] 整个吞掉。
 * 被否掉的另一种读法是「拖动增量小于阈值不改尺寸」的死区：那个只消抖动，翻轴点本身仍可停留。
 *
 * ⚠️ **禁带外沿必须严格落在滞回阈之外**（`CSHouseResize_EffectiveBandFraction` 的那条下界）。
 * 等号会让跳带的落点恰好是 `Y == R·X`，而 `CSHouseRoof_ChooseRidgeAxis` 用的是严格 `>`，
 * 于是那一步不翻、下一个**连续**步才翻 —— 翻轴就这么从"跳带的那一瞬"漏回到平滑拖动里，
 * 正是本条要消掉的东西。
 */

/** 禁带参数。默认构造 = **关掉禁带**（Fraction 0），旧路径与只想测推拉的单测不受影响。 */
struct FCSHouseResizeBand
{
	/** 带半宽 / 不动那一维的长度。0 或负 = 关。默认值见 `ACSHouseActor::FootprintBandFraction`。 */
	float Fraction = 0.0f;

	/** 脊向滞回比，用来反解带宽下界（见下）。与房屋的 `RidgeSwitchRatio` 同一个量。 */
	float RidgeSwitchRatio = 1.15f;
};

/**
 * 实际生效的带半宽比例。
 *
 * 下界不是拍脑袋：滞回的模糊区是 `[A/R, A·R]`（这一段里脊向由记忆决定，不由尺寸决定），
 * 要让尺寸"停不进模糊区"，带必须**整段盖住**它 —— 线性带 `A(1±f)` 盖住 `A·R` 需要 `f > R−1`，
 * 盖住 `A/R` 只需要 `f > (R−1)/R`，所以 `f > R−1` 是那个紧的条件。
 *
 * 乘 1.25 是"外沿再往外 25%"的安全余量：等号处 `Y == R·A` 会被严格 `>` 判成不翻（见文件头警告），
 * 而 float 在 cm 量级上还会有半个 ulp 的抖动。留一整段余量比追求最紧的边界省事得多。
 */
inline float CSHouseResize_EffectiveBandFraction(const FCSHouseResizeBand& Band)
{
	if (!(Band.Fraction > 0.0f)) return 0.0f;
	const float FloorFraction = FMath::Max(Band.RidgeSwitchRatio, 1.0f) - 1.0f;
	return FMath::Max(Band.Fraction, FloorFraction * 1.25f);
}

/** 这个尺寸落在禁带里吗（带内 = 两边长度靠得太近）。Anchor 是不动的那一维。 */
inline bool CSHouseResize_IsInsideBand(double Desired, double Anchor, const FCSHouseResizeBand& Band)
{
	const float F = CSHouseResize_EffectiveBandFraction(Band);
	if (F <= 0.0f) return false;
	return FMath::Abs(Desired - Anchor) < Anchor * F;
}

/**
 * 把被推的那一维推出禁带。
 *
 * 跳哪一侧**只看 Desired 落在带心（= Anchor，即 X == Y 的穿越点）的哪一边**，不看拖动方向、
 * 不看上一帧 —— 因此它是幂等的纯函数：同一个 Desired 永远给同一个答案，反复调用不会漂。
 * 若改成"按拖动方向跳"，来回拖就会在两条外沿之间产生 2f·A 的迟滞环，那才是真的抖。
 *
 * 观感上它就是一个**卡口（detent）**：靠近正方形时墙被吸在外沿上不动，用户把光标推过
 * 穿越点的那一瞬，房子"啪"地换档到另一侧外沿 —— 翻轴（如果发生）与这一步的尺寸跳变同步，
 * 用户读到的是"房子换了个形状"，而不是"屋顶自己转了 90°"。
 */
inline double CSHouseResize_ApplyBand(double Desired, double Anchor, const FCSHouseResizeBand& Band)
{
	const float F = CSHouseResize_EffectiveBandFraction(Band);
	if (F <= 0.0f || Anchor <= 0.0) return Desired;
	const double Half = Anchor * F;
	if (FMath::Abs(Desired - Anchor) >= Half) return Desired;
	return Desired > Anchor ? Anchor + Half : Anchor - Half;
}

/** 原始诉求 → 生效尺寸：先过禁带、再过硬下界。**只有这一处**定义两者的先后，别在调用点各写一遍。 */
inline double CSHouseResize_ResolveSize(double RawDesired, double Anchor, float MinFootprint,
	const FCSHouseResizeBand& Band)
{
	return FMath::Max(CSHouseResize_ApplyBand(RawDesired, Anchor, Band), double(FMath::Max(MinFootprint, 1.0f)));
}

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
 * 返回**实际**生效的位移（经 MinFootprint 下限与禁带修正之后）。调用方必须用返回值而不是
 * 传入的 Offset 去记账 —— 拖拽 handle 的累加器（计划 D5 的"记账量法"）一旦记成请求值而不是
 * 生效值，卡在下限或外沿上时残差就会一路累积，松手瞬间房子跳一大截。
 *
 * MinFootprint 是**硬下界，禁带给它让路**：房子小到 `Anchor·(1−f) < MinFootprint` 时低侧
 * 根本没有合法解，硬顶回去只会让墙拖不动（比偶尔停在带内糟得多）。
 *
 * ⚠️ **`InOutRawSize` 不是可选的锦上添花，开了禁带就必须给**（踩过的坑，动工当天就撞上）：
 * 禁带把尺寸吸在外沿上时 `Applied == 0`，而生效尺寸又是下一次推拉的起点 ⇒ 不记原始诉求的话
 * 每一帧都从同一个外沿重新算，墙**永远跨不过带**，禁带从"卡口"退化成"硬墙"。
 * 传进来的那份累加的是**用户的手往哪儿走了多远**（未经带修正），跨带因此需要攒够一整个
 * 半带宽的拖动量 —— 这正是裁决四那句"尺寸更换有最小距离"的字面意思。
 * 传 nullptr = 不累加（禁带关掉时两者等价，计划 D5 那条 6 参签名就是这么用的）。
 */
inline float CSHouse_ApplyEdgePush(FVector2D& InOutSize, FVector& InOutCenter, int32 EdgeIndex,
	float YawDegrees, float Offset, float MinFootprint, const FCSHouseResizeBand& Band = FCSHouseResizeBand(),
	FVector2D* InOutRawSize = nullptr)
{
	const bool bDrivesX = CSHouseResize_EdgeDrivesX(EdgeIndex);
	const double Current = bDrivesX ? InOutSize.X : InOutSize.Y;
	const double Anchor = bDrivesX ? InOutSize.Y : InOutSize.X;

	const double RawBefore = InOutRawSize ? (bDrivesX ? InOutRawSize->X : InOutRawSize->Y) : Current;
	const double RawAfter = RawBefore + double(Offset);
	if (InOutRawSize)
	{
		if (bDrivesX) InOutRawSize->X = RawAfter; else InOutRawSize->Y = RawAfter;
	}

	const double Desired = CSHouseResize_ResolveSize(RawAfter, Anchor, MinFootprint, Band);
	const double Applied = Desired - Current;
	if (Applied == 0.0) return 0.0f;   // 幂等早退：Offset=0 连调 N 次不许改动任何量

	if (bDrivesX) InOutSize.X = Desired; else InOutSize.Y = Desired;
	InOutCenter += CSHouseResize_EdgeOuterWorld(EdgeIndex, YawDegrees) * (Applied * 0.5);
	return float(Applied);
}

/**
 * 累加器还认得当前尺寸吗。不认得就该重新同步 —— 别人直接设了 `FootprintSize`、换了一条边推、
 * 或者上一次拖动早已结束，这三种情况下留着旧诉求就是"手没动房子自己跳"。
 */
inline bool CSHouseResize_RawMatches(const FVector2D& RawSize, const FVector2D& Size, int32 EdgeIndex,
	float MinFootprint, const FCSHouseResizeBand& Band)
{
	const bool bDrivesX = CSHouseResize_EdgeDrivesX(EdgeIndex);
	const double Anchor = bDrivesX ? Size.Y : Size.X;
	const double RawAnchor = bDrivesX ? RawSize.Y : RawSize.X;
	const double Current = bDrivesX ? Size.X : Size.Y;
	const double Raw = bDrivesX ? RawSize.X : RawSize.Y;
	return FMath::IsNearlyEqual(RawAnchor, Anchor, 0.01)
		&& FMath::IsNearlyEqual(CSHouseResize_ResolveSize(Raw, Anchor, MinFootprint, Band), Current, 0.01);
}

/**
 * 这一次推拉会不会翻脊？给出图 / 断言用的**预判**，不改任何状态。
 *
 * 存在的理由：翻轴在禁带口径下**只可能与跳带同步发生**，而"跳带"这件事从外部看只是尺寸
 * 突然变了一截。没有这条谓词就没法在测试里区分"翻在跳带那一步（预期）"与"翻在某个连续步里
 * （缺陷）"，而两者的画面差别恰恰是本条裁决要管的全部内容。
 */
inline bool CSHouseResize_WouldFlipRidge(const FVector2D& SizeBefore, const FVector2D& SizeAfter,
	ECSRidgeAxis CurrentAxis, float RidgeSwitchRatio)
{
	const ECSRidgeAxis Before = CSHouseRoof_ChooseRidgeAxis(SizeBefore, CurrentAxis, RidgeSwitchRatio);
	return CSHouseRoof_ChooseRidgeAxis(SizeAfter, Before, RidgeSwitchRatio) != Before;
}
