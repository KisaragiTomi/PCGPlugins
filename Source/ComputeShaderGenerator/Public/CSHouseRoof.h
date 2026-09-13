#pragma once

#include "CoreMinimal.h"
#include "CSHouseProfile.h"   // FCSHouseFootprint / CSHouse_GetEdge —— 屋面长在 footprint 上，边在哪只有一个真源
#include "CSHouseRoof.generated.h"

/**
 * 屋面的共享求值器（TinyGladeHouse_Plan.md D4「屋面：抽一个共享求值器，脊向要显式」）。
 *
 * 为什么必须共享：Tiny Glade 的屋面是被瓦、梁、尖顶、雪、老虎窗**共同引用的单一求值器**
 * （逆向报告 §3.3【确凿】：支撑梁沿 circle_normal 按 roof_profile 平移并施加与瓦片**完全
 * 相同**的屋面凹陷噪声；雪 mesh 加与瓦片一致的抖动噪声保证贴合）。不共享就会脱开 —— 屋面
 * 方程一旦散在各自的生成函数里，铺瓦/铺梁/落窗谓词就会各写一份，彼此差一点点就穿帮。
 *
 * -----------------------------------------------------------------------------
 * 2026-08-31：双坡 + 山墙 → **四坡（hip）**，屋面本体交给瓦片
 * -----------------------------------------------------------------------------
 * 实拍俯视（用户提供）：TG 的屋顶是**四个坡面** + 四条角斜脊 + 中间一条短脊，且整面**全由瓦
 * 铺成**。逆向侧对得上的是 `roof_shape::ridge_length_01_from_rectangle_ratio` —— 脊长是矩形
 * 长宽比的连续函数，越接近正方形脊越短，正方形处连续退化成金字塔。
 *
 * 据此删掉的三样（都不是"暂时不做"，是**在四坡下不存在**）：
 *  · **山墙**：四坡的四面墙全是檐墙，墙顶一律平在 `EaveZ`。
 *  · **翻轴事件**：脊向由形状连续导出，正方形处脊长为 0，「脊朝哪」这个问题根本不出现。
 *  · **实体屋面板**：屋顶是瓦片实例，房体三角汤里一片屋面都不产，墙顶就是平的 `EaveZ`。
 *
 * -----------------------------------------------------------------------------
 * 2026-09-13：矩形 → **凸折线**（footprint 折线化 3e）
 * -----------------------------------------------------------------------------
 * 「每个坡面同一个坡度」的屋面就是 footprint 的**直骨架**屋面。凸多边形上它有一行闭式：
 *
 *     Z(p) = EaveZ + tan(pitch) · min_k ((p − Start_k) · In_k)
 *
 * 即到**每条边所在直线**的有向内距取最小。矩形上这就是原来的 `min(HalfX − |x|, HalfY − |y|)`，
 * 一个字的特例都没有。min 在哪条边上取到，那一点就属于哪个坡面；两条边并列取到的轨迹是骨架的
 * 弧（角斜脊 / 脊）。外挑段内距为负，屋面照同一个式子继续往下走，角上沿平分线外延。
 *
 * ⚠️ **只对凸折线成立**：凹折线的内部不再是半平面的交，「到直线的距离」会被远处的边抢走 min。
 * 凹 footprint 落地时这里要换成真正的直骨架（含分裂事件）。
 *
 * 骨架本身（弧的端点、最高那段脊）由 `CSHouseRoof_BuildSkeleton` 按边塌缩事件求出 —— 脊瓦沿弧铺、
 * 尖顶立在最高那段脊的两端、鸟窝沿它排。**脊长与角斜脊仍然都是推论，不是参数**，别给它们加旋钮。
 *
 * 全部是无 GPU 依赖的纯函数，可直接进 automation 测试。
 */

/** 一座四坡屋面的完整描述。房屋 actor 每次生成时现组，不序列化（脊向也不是状态）。 */
USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSRoofDesc
{
	GENERATED_BODY()

	/** 底面轮廓（局部 XY，逆时针闭合折线）= 墙外皮。与 `ACSHouseActor::GetFootprint()` 同一份。 */
	UPROPERTY() FCSHouseFootprint Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));

	/** 檐口高 = 墙高（局部 Z）。屋面在 footprint 边界处恰好等于它。 */
	UPROPERTY() float EaveZ = 300.0f;

	/** 坡度（度）。**每个坡面同一个坡度** —— 脊长与角斜脊都是它的推论。 */
	UPROPERTY() float Pitch = 35.0f;

	/** 屋檐外挑 cm（每条边都挑同样多，沿边的法线量）。 */
	UPROPERTY() float Overhang = 25.0f;

	float TanPitch() const { return FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	/** cos(pitch)。铺瓦时"沿坡量的长度 → 竖直/水平分量"都按它换算，别在生成器里再写一遍三角函数。 */
	float CosPitch() const { return FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	float SinPitch() const { return FMath::Sin(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	/**
	 * 到每条边所在直线的**最小有向内距**（边界上为 0、内部为正、外挑段为负）—— 四坡高度场的核心。
	 * 退化 footprint（少于 3 个顶点）返回 0。
	 */
	double InsetDistance(const FVector2D& LocalXY) const
	{
		const int32 N = Footprint.NumEdges();
		if (N < 3) return 0.0;
		double Min = TNumericLimits<double>::Max();
		for (int32 Edge = 0; Edge < N; ++Edge)
		{
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
			if (F.Len <= 0.0f) continue;
			Min = FMath::Min(Min, FVector2D::DotProduct(LocalXY - F.Start, F.In));
		}
		return Min == TNumericLimits<double>::Max() ? 0.0 : Min;
	}

	/** 屋面最高处的内距（= 最大内切圆半径；矩形 = 半短边）。现算一次骨架，N 很小。 */
	double MaxInset() const;
};

/** 直骨架的一条弧 = 两个坡面的交线（角斜脊或脊）。 */
struct FCSRoofSkeletonArc
{
	/** 两端（局部 XY）与各自的内距；高度 = EaveZ + tan(pitch) · 内距。 */
	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;
	double InsetA = 0.0;
	double InsetB = 0.0;
	/** 两侧坡面的边号。 */
	int32 FaceA = INDEX_NONE;
	int32 FaceB = INDEX_NONE;
};

struct FCSRoofSkeleton
{
	/**
	 * 前 `NumHips` 条是角斜脊，**按角号**排（角 k = `CSHouse_GetCorner(k)`，夹在 k 号边与 k+1 号边之间），
	 * A 端在檐口外角（内距 −Overhang）、B 端在它汇入的骨架节点；其后是节点之间的脊，按生成顺序，
	 * 最后一条是最高的那段脊（金字塔时长度为 0，调用方按长度过滤）。矩形上恰好是「四条角斜脊 + 一条脊」。
	 */
	TArray<FCSRoofSkeletonArc> Arcs;
	int32 NumHips = 0;
	/** 屋面最高处的内距。 */
	double MaxInset = 0.0;
	/** 最高那段脊的两端（字典序小的在 A：先比 X 再比 Y），金字塔尖时两端重合。 */
	FVector2D TopA = FVector2D::ZeroVector;
	FVector2D TopB = FVector2D::ZeroVector;
};

/**
 * 凸折线的直骨架（边塌缩事件，O(N²)）。`Overhang` 只用来把角斜脊的起点外延到檐口外角。
 * 退化 footprint 输出空骨架。
 */
COMPUTESHADERGENERATOR_API void CSHouseRoof_BuildSkeleton(const FCSHouseFootprint& Footprint, double Overhang, FCSRoofSkeleton& Out);

inline double FCSRoofDesc::MaxInset() const
{
	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(Footprint, double(Overhang), Skeleton);
	return Skeleton.MaxInset;
}

/**
 * 第 `Edge` 个坡面在内距 `Inset` 处那一排的沿边区间 `[OutT0, OutT1]`（从边起点 `Start` 沿 `U` 量）。
 *
 * 坡面 k 在内距 d 处的点 = 本边内偏 d 的那条直线上、同时满足「到其余每条边的内距 ≥ d」的那一段。
 * 于是逐条边做一次一维半平面裁剪即可 —— 短边先收成尖、两条原本不相邻的边在尖之后变成相邻，
 * 这些骨架事件全都自动包含在内，不需要知道骨架长什么样。矩形上它给出 `[d, Len − d]`，
 * 就是原来「半宽 = 半边长 − 内距」那条式子。
 *
 * 返回 false = 这一排已经没有这个坡面了（或退化输入）。
 */
inline bool CSHouseRoof_FaceSpanAtInset(const FCSHouseFootprint& Footprint, int32 Edge, double Inset,
	double& OutT0, double& OutT1)
{
	const int32 N = Footprint.NumEdges();
	if (N < 3 || Edge < 0 || Edge >= N) return false;
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
	if (F.Len <= 0.0f) return false;

	const FVector2D Base = F.Start + F.In * Inset;
	const double Unbounded = 1.0e30;
	double T0 = -Unbounded, T1 = Unbounded;
	for (int32 Other = 0; Other < N; ++Other)
	{
		if (Other == Edge) continue;
		const FCSHouseEdgeFrame G = CSHouse_GetEdge(Other, Footprint, 0.0f);
		if (G.Len <= 0.0f) continue;
		// 「(Base + U·t − G.Start)·G.In ≥ d」⇔「Q + C·t ≥ 0」。
		const double C = FVector2D::DotProduct(F.U, G.In);
		const double Q = FVector2D::DotProduct(Base - G.Start, G.In) - Inset;
		if (FMath::Abs(C) <= 1.0e-9)
		{
			if (Q < 0.0) return false;   // 平行边已经把这一排整条吃掉
			continue;
		}
		const double R = -Q / C;
		if (C > 0.0) T0 = FMath::Max(T0, R);
		else T1 = FMath::Min(T1, R);
	}
	if (T0 <= -Unbounded || T1 >= Unbounded || T1 <= T0) return false;
	OutT0 = T0;
	OutT1 = T1;
	return true;
}

/** 屋面在局部 XY 处的高度。不判是否落在轮廓内（那是 `CSHouseRoof_IsUnderRoof` 的事）。 */
inline float CSHouseRoof_EvalZ(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	return Desc.EaveZ + Desc.TanPitch() * float(Desc.InsetDistance(LocalXY));
}

/** 屋脊高（局部 Z）= 最高处的内距 × 坡度。矩形上只由**短边**决定。 */
inline float CSHouseRoof_RidgeZ(const FCSRoofDesc& Desc)
{
	return Desc.EaveZ + Desc.TanPitch() * float(Desc.MaxInset());
}

/** 檐口外沿高（局部 Z）——外挑最外一圈。四面同高（同坡度、同外挑）。 */
inline float CSHouseRoof_EaveOuterZ(const FCSRoofDesc& Desc)
{
	return Desc.EaveZ - Desc.TanPitch() * Desc.Overhang;
}

/**
 * 屋面外法线（单位，朝上外）。
 *
 * 坡面 k 的法线是 (外法线_k · sin p, cos p)；把**所有并列取到最小内距**的边的法线相加再归一化，
 * 角斜脊上自然给出两面的平均、脊上两面对冲成正上、金字塔尖上全部相消退化成正上方 ——
 * 不需要为脊 / 角脊 / 尖顶各写一个特例。
 */
inline FVector CSHouseRoof_EvalNormal(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	const int32 N = Desc.Footprint.NumEdges();
	if (N < 3) return FVector::UpVector;

	TArray<double, TInlineAllocator<16>> Dist;
	TArray<FVector2D, TInlineAllocator<16>> Fall;
	double MinDist = TNumericLimits<double>::Max();
	for (int32 Edge = 0; Edge < N; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Desc.Footprint, 0.0f);
		if (F.Len <= 0.0f) continue;
		Dist.Add(FVector2D::DotProduct(LocalXY - F.Start, F.In));
		Fall.Add(-F.In);                         // 坡面的下降方向（水平分量）= 外法线
		MinDist = FMath::Min(MinDist, Dist.Last());
	}

	const float SinP = Desc.SinPitch();
	FVector Sum(0, 0, 0);
	// 容差按 cm 取：并列与否是"这一点在不在角斜脊上"，那是厘米量级的事，不是 ulp 量级的。
	for (int32 I = 0; I < Dist.Num(); ++I)
	{
		if (Dist[I] > MinDist + 0.01) continue;
		Sum += FVector(Fall[I].X * SinP, Fall[I].Y * SinP, Desc.CosPitch());
	}
	return Sum.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
}

/** 该局部 XY 是否被屋面覆盖（外挑都算）。D8 那条"落屋顶 → 不生成窗"的谓词用它。 */
inline bool CSHouseRoof_IsUnderRoof(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	return Desc.Footprint.IsValidFootprint() && Desc.InsetDistance(LocalXY) >= -double(Desc.Overhang);
}
