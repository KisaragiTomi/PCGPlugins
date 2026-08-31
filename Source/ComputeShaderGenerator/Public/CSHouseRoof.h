#pragma once

#include "CoreMinimal.h"
#include "CSHouseRoof.generated.h"

/**
 * 屋面的共享求值器（TinyGladeHouse_Plan.md D4「屋面：抽一个共享求值器，脊向要显式」）。
 *
 * 为什么必须共享：Tiny Glade 的屋面是被瓦、梁、尖顶、雪、老虎窗**共同引用的单一求值器**
 * （逆向报告 §3.3【确凿】：支撑梁沿 circle_normal 按 roof_profile 平移并施加与瓦片**完全
 * 相同**的屋面凹陷噪声；雪 mesh 加与瓦片一致的抖动噪声保证贴合）。不共享就会脱开 —— 屋面
 * 方程一旦散在各自的生成函数里，铺瓦/铺梁/落窗谓词就会各写一份，彼此差一点点就穿帮。
 *
 * 本文件是那个唯一真源：屋顶坡板、山墙三角、以及将来 D8「落屋顶 → 不生成」的谓词全部调这里。
 * 全部是无 GPU 依赖的纯函数，可直接进 automation 测试。
 *
 * 坐标口径：脊向坐标系 (AlongRidge, AcrossRidge, Z)，原点在 footprint 中心、Z=0 是房底；
 * RidgeToLocal() 是它到 actor 局部 XY 的唯一映射，屋面上一切摆位都过这一个函数。
 */

UENUM()
enum class ECSRidgeAxis : uint8
{
	/** 屋脊沿局部 +X 走，跨度在 Y 方向。 */
	X UMETA(DisplayName = "Ridge along X"),
	/** 屋脊沿局部 +Y 走，跨度在 X 方向。 */
	Y UMETA(DisplayName = "Ridge along Y"),
};

/** 一座双坡屋面的完整描述。房屋 actor 每次生成时现组，不序列化（RidgeAxis 除外，见房屋类）。 */
USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSRoofDesc
{
	GENERATED_BODY()

	/** 脊向。**显式而非从长轴隐式导出** —— 隐式的话 D5 单边推拉一旦让 X 穿过 Y，脊与山墙就
	 *  原地 90° 跳变，且 FootprintSize 在形状哈希里 ⇒ 拖动中用户看到屋顶"啪"地翻过去。 */
	UPROPERTY() ECSRidgeAxis RidgeAxis = ECSRidgeAxis::X;

	/** 底面尺寸 cm（局部 X/Y）。 */
	UPROPERTY() FVector2D Footprint = FVector2D(600.0, 400.0);

	/** 檐口高 = 墙高（局部 Z）。屋面在 footprint 边界处恰好等于它。 */
	UPROPERTY() float EaveZ = 300.0f;

	/** 坡度（度）。 */
	UPROPERTY() float Pitch = 35.0f;

	/** 屋檐外挑 cm（脊向两端也挑同样多）。 */
	UPROPERTY() float Overhang = 25.0f;

	/** 屋顶板厚 cm。 */
	UPROPERTY() float Thickness = 12.0f;

	float TanPitch() const { return FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	/** cos(pitch)。屋面板的竖直厚度、封口咬入量都按它换算 —— 别在生成器里再写一遍三角函数。 */
	float CosPitch() const { return FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	/** 沿脊方向的底面长。 */
	float RidgeLength() const { return float(RidgeAxis == ECSRidgeAxis::X ? Footprint.X : Footprint.Y); }

	/** 跨度方向的底面长（两坡各占一半）。 */
	float SpanLength() const { return float(RidgeAxis == ECSRidgeAxis::X ? Footprint.Y : Footprint.X); }

	float HalfSpan() const { return SpanLength() * 0.5f; }

	/** 檐口外沿的跨度坐标（含外挑）。 */
	float EaveOuter() const { return HalfSpan() + Overhang; }

	/** (沿脊, 跨度, Z) → actor 局部 (x, y, z)。线性映射，方向向量同样可以过它。 */
	FVector RidgeToLocal(double AlongRidge, double AcrossRidge, double Z) const
	{
		return RidgeAxis == ECSRidgeAxis::X ? FVector(AlongRidge, AcrossRidge, Z) : FVector(AcrossRidge, AlongRidge, Z);
	}

	/** actor 局部 XY → 跨度坐标（带符号，脊线上为 0）。 */
	double LocalToAcross(const FVector2D& LocalXY) const
	{
		return RidgeAxis == ECSRidgeAxis::X ? LocalXY.Y : LocalXY.X;
	}

	/** actor 局部 XY → 沿脊坐标。 */
	double LocalToAlong(const FVector2D& LocalXY) const
	{
		return RidgeAxis == ECSRidgeAxis::X ? LocalXY.X : LocalXY.Y;
	}
};

/**
 * 屋面外表面在给定跨度坐标处的高度（局部 Z）——整套屋面几何的一维内核。
 *
 * Z(b) = EaveZ + tan(pitch) · (HalfSpan − |b|)：脊线（b=0）最高，footprint 边界（|b|=HalfSpan）
 * 恰好落在墙顶 EaveZ 上，外挑段继续往下走。三处关键高度（屋脊 / 墙顶 / 檐口外沿）因此
 * 天然自洽，不需要各自再写一遍。
 */
inline float CSHouseRoof_EvalZAcross(const FCSRoofDesc& Desc, double AcrossRidge)
{
	return Desc.EaveZ + Desc.TanPitch() * float(Desc.HalfSpan() - FMath::Abs(AcrossRidge));
}

/** 屋面外表面在局部 XY 处的高度。不判是否落在轮廓内（那是 IsUnderRoof 的事）。 */
inline float CSHouseRoof_EvalZ(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	return CSHouseRoof_EvalZAcross(Desc, Desc.LocalToAcross(LocalXY));
}

/** 屋脊高（局部 Z）。 */
inline float CSHouseRoof_RidgeZ(const FCSRoofDesc& Desc)
{
	return CSHouseRoof_EvalZAcross(Desc, 0.0);
}

/** 檐口外沿高（局部 Z）——外挑最外一条边。 */
inline float CSHouseRoof_EaveOuterZ(const FCSRoofDesc& Desc)
{
	return CSHouseRoof_EvalZAcross(Desc, Desc.EaveOuter());
}

/**
 * 屋面板的**竖直**厚度。板厚 Thickness 是垂直于坡面量的，竖直方向要除 cos(pitch)。
 *
 * 为什么需要这个量（踩过的坑）：两块坡板原本沿**法线**挤出、并沿坡向过冲半个板厚来"相接"，
 * 结果是两板互穿、各自尖端戳出对方顶面约 Thickness·sin(pitch)（35° 下 ≈ 7 cm 的交叉小尖）。
 * 改成沿**竖直**挤出以后，坡板在 (跨度, Z) 平面上的截面是平行四边形、脊线那条边正好竖直 ——
 * 两块板可以在脊平面 across = 0 上直接对切收口，既不互穿也不留 V 形豁口，
 * 而垂直坡面量到的板厚仍然是 Thickness（这是 RoofThickness 属性的语义，不能变）。
 *
 * 顺带修掉一处几何与谓词打架：法线挤出会让屋面**顶面**比 CSHouseRoof_IsUnderRoof 声称的
 * 覆盖范围多探出 Thickness·sin(pitch)，竖直挤出后两者的外沿都恰好是 EaveOuter()。
 */
inline float CSHouseRoof_SlabVerticalThickness(const FCSRoofDesc& Desc)
{
	return Desc.Thickness / FMath::Max(Desc.CosPitch(), UE_KINDA_SMALL_NUMBER);
}

/**
 * 墙 / 山墙顶**咬进**屋面板的竖直量（footprint 边界处为 0，向内一个墙厚爬满）。
 *
 * 为什么要咬进去而不是刚好贴住：CSHouseRoof_EvalZAcross 在 footprint 边界处**按构造**等于
 * EaveZ（= 墙顶），于是墙顶面与屋面底沿墙外棱**相切** —— 零余量。零余量的两个后果本项目
 * 两处都吃到了：山墙斜边与屋面底共面 ⇒ 发丝亮线（z-fight 型）；檐墙那两条则连封口面都没有，
 * 墙顶到屋面底之间留着一条外侧 0、内侧 T·tan(pitch) 的楔形空腔。咬入量把"相切"改成"互穿"，
 * 与门框砖那条已落地的**负缝**同一条纪律（Tiny Glade 的砖本来就是故意胀大互穿的）。
 *
 * 量取多少（不硬编 cm 的理由）：
 *  · 唯一的硬上界是"别从屋面板上表面钻出去"，那个上界就是**竖直板厚**。所以按竖直板厚取比例，
 *    RoofThickness / RoofPitch 随便调都不会越界；RoofThickness 下限 2 cm ⇒ 最坏咬入 0.5 cm，
 *    仍比 float32 世界坐标在 1 km 处的 ulp（≈0.008 cm）大两个数量级。
 *  · 只咬 1/4，把剩下 3/4 板厚留给将来铺瓦 / 铺梁时的再次穿插。
 *  · **在 footprint 边界处必须归零**：Overhang = 0 时屋面板的檐口端面恰好落在墙外表面所在的
 *    平面上，那里给正咬入就会造出一对共面重叠的**可见**面（又一处 z-fight）。归零同时让檐口
 *    封口件退化成"外侧 0、内侧 T·tan(pitch)"那块正好补满楔形缝的板，形状上也更对。
 */
inline float CSHouseRoof_SoffitBite(const FCSRoofDesc& Desc, double AcrossRidge, float RampWidth)
{
	constexpr float BiteFraction = 0.25f;
	const double Ramp = FMath::Clamp(
		(Desc.HalfSpan() - FMath::Abs(AcrossRidge)) / FMath::Max(double(RampWidth), UE_DOUBLE_KINDA_SMALL_NUMBER), 0.0, 1.0);
	return BiteFraction * CSHouseRoof_SlabVerticalThickness(Desc) * float(Ramp);
}

/**
 * 墙 / 山墙顶轮廓（局部 Z）= 屋面板底面 + 咬入量。**整套「墙-顶收口」只有这一条方程**：
 * 檐口封口楔形、山墙多边形的斜边都取它，脱开就又会退回相切或留缝。RampWidth 传墙厚。
 */
inline float CSHouseRoof_SoffitTopZ(const FCSRoofDesc& Desc, double AcrossRidge, float RampWidth)
{
	return CSHouseRoof_EvalZAcross(Desc, AcrossRidge) + CSHouseRoof_SoffitBite(Desc, AcrossRidge, RampWidth);
}

/** 屋面外法线（单位，朝上外）。脊线上退化为世界上方向。 */
inline FVector CSHouseRoof_EvalNormal(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	const double Across = Desc.LocalToAcross(LocalXY);
	if (FMath::IsNearlyZero(Across)) return FVector::UpVector;

	// 坡面沿 +b 下降 ⇒ 切向 (1, −tan)，法线 (tan, 1) 归一化即 (sin p, cos p)。
	const float PitchRad = FMath::DegreesToRadians(FMath::Clamp(Desc.Pitch, 0.0f, 89.0f));
	const double Sign = Across > 0 ? 1.0 : -1.0;
	return Desc.RidgeToLocal(0.0, Sign * FMath::Sin(PitchRad), FMath::Cos(PitchRad)).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
}

/** 该局部 XY 是否被屋面覆盖（含两个方向的外挑）。D8 那条"落屋顶 → 不生成窗"的谓词用它。 */
inline bool CSHouseRoof_IsUnderRoof(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	return FMath::Abs(Desc.LocalToAcross(LocalXY)) <= Desc.EaveOuter()
		&& FMath::Abs(Desc.LocalToAlong(LocalXY)) <= Desc.RidgeLength() * 0.5f + Desc.Overhang;
}

/**
 * 脊向的滞回选择（计划 D4）。只有当另一根轴长出当前脊轴 SwitchRatio 倍以上时才换向 ——
 * 单边推拉让 X 恰好穿过 Y 时，没有滞回就会在阈值附近反复翻面，而 FootprintSize 在形状哈希里，
 * 每翻一次都是一次全量重建 + 肉眼可见的 90° 跳变。
 *
 * SwitchRatio ≤ 1 退化为无滞回（等价于旧的 X >= Y 隐式规则）。
 */
inline ECSRidgeAxis CSHouseRoof_ChooseRidgeAxis(const FVector2D& Footprint, ECSRidgeAxis Current, float SwitchRatio)
{
	const double X = FMath::Max(FMath::Abs(Footprint.X), UE_DOUBLE_SMALL_NUMBER);
	const double Y = FMath::Max(FMath::Abs(Footprint.Y), UE_DOUBLE_SMALL_NUMBER);
	const double Ratio = FMath::Max(double(SwitchRatio), 1.0);
	if (Current == ECSRidgeAxis::X) return Y > X * Ratio ? ECSRidgeAxis::Y : ECSRidgeAxis::X;
	return X > Y * Ratio ? ECSRidgeAxis::X : ECSRidgeAxis::Y;
}
