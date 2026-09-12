#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"   // FCSOpeningSite::Openings —— CoreMinimal 不带它
#include "CSHouseProfile.generated.h"

/**
 * 洞的 2D 剖面求值器（TinyGladeHouse_Plan.md D4/D6/D8）。
 *
 * **开洞不走布尔**（用户裁决 2026-08-29「尽可能不用 MeshBoolean」）：洞的形状本来就是有限的
 * 原型集合（拱 / 矩形 / 圆…），每种配一条手写 2D 剖面，墙板生成时按剖面直接砌出带洞几何，
 * 洞缘精度只受分段数控制、洞口内壁一并产出。洞不携带、不生成、不回读任何 per-opening 几何。
 *
 * **这是正确性关键路径**：同一条剖面同时供三处使用 ——
 *   ① 墙板砌洞（上下过梁带 / 窗台带）
 *   ② 洞口内壁扫掠（reveal）
 *   ③ QueryFeaturePlacement 的重叠谓词
 * 三处不同式就会出现"谓词说能放、几何砌出来穿帮"。所以它必须是唯一的纯函数，且进单测。
 *
 * 墙空间口径：(S, Z) —— S 是沿边缘线段的弧长（从线段起点算），Z 是从房底起算的高度。
 * 与 FCSHouseEdgeFrame 的 (Start, U, In) 一一对应。
 */

UENUM(BlueprintType)
enum class ECSOpeningType : uint8
{
	/** 道路推导：地面顶点色 R 过阈的子段自动点亮（D6）。 */
	Door,
	/** 特征标记注册：ACSHouseFeatureMarker 提诉求、房子裁决（D8）。 */
	Window,
	/** 第三方注入（楼梯穿墙等）。**当前不生产，只保证格式容纳**，见计划开放问题。 */
	Stair,
};

UENUM(BlueprintType)
enum class ECSOpeningShape : uint8
{
	/** 矩形下身 + 半圆顶。门拱的原型。 */
	Arch,
	/** 纯矩形。 */
	Rect,
	/** 正圆（Width 同时决定直径，Z0/Z1 被居中的圆覆盖）。 */
	Circle,
};

/**
 * 一个洞 = 原型剖面 + 沿边的摆位。不存任何切出的几何。
 *
 * **为什么现在就要 Z0/Z1/AxisUS/Skew**（用户指令 2026-08-29：楼梯暂不做，但洞逻辑必须容纳它）：
 * 早先的"Height 从墙基起算"+"切轴恒为墙法线"这两条硬编码，把三类洞挡在门外 ——
 *   ① 楼梯穿墙：洞底在半空（踏面高度）、洞顶随坡倾斜、切轴是楼梯行进方向而非墙法线；
 *   ② 窗台高度：窗只能从墙基起算的话，窗台压根表达不出来；
 *   ③ 多边形 footprint 的转角洞：切轴需要偏离单一边的法线。
 * 三者共用这一个判决点，所以字段必须在 D8 冻结 openings 格式**之前**加，不能等楼梯排期。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSWallOpening
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	ECSOpeningType Type = ECSOpeningType::Door;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	ECSOpeningShape Shape = ECSOpeningShape::Arch;

	/** 边缘线段索引（矩形房 = 0..3；多边形 footprint 后为折线段索引）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	int32 EdgeIndex = 0;

	/** 沿边参数位置（从边起点算的弧长），洞的中心。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float CenterS = 0.0f;

	/** 洞宽（沿边）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Width = 0.0f;

	/** 洞底，墙空间高度。门恒 0；窗台 / 楼梯口 > 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Z0 = 0.0f;

	/** 洞顶，墙空间高度（取代早先那个"从墙基算的 Height"）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Z1 = 0.0f;

	/**
	 * 切轴在 (沿边 U, 墙内法线 In) 平面内的方向。(0,1) = 垂直墙面（绝大多数洞）；
	 * 楼梯斜穿时非 (0,1)，洞口内壁沿它扫掠而不是沿法线挤出。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	FVector2D AxisUS = FVector2D(0.0, 1.0);

	/** Z1 沿 S 的线性斜率。楼梯洞顶随坡倾斜；门窗恒 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Skew = 0.0f;

	/** (线段, 子段) 或 特征标记 GUID / 注入方 id。滞回与哈希用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	FGuid SourceId;

	/** 悬停高亮用的 8 位标签（进房体顶点色 G 通道，见 ACSHouseActor 的通道字典）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	uint8 Tag = 0;

	/**
	 * 与左右两侧残料跨度有关的样式位（`CSHouse_StylePier*`）。
	 *
	 * **为什么把它挂在洞上而不是另起一张表**：墩的判定带双阈迟滞 ⇒ 它是路径依赖的，
	 * 记忆只能活在 actor 里（同 `DoorRunMemory`）；而铺墙板的 `CSHouse_BuildBodySoup` 是纯函数，
	 * 只吃一份 desc。中间要么再传一张按跨度序号索引的表（两边各自枚举跨度、序号一旦对不上就
	 * 静默错位），要么把结论粘在洞本身上——洞是唯一同时流过两边、且顺序天然一致的东西。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	uint8 StyleFlags = 0;

	/**
	 * 拱顶相对**起拱线**的升高 cm（`Shape == Arch` 才有意义）。**0 = 用半宽**，即正半圆。
	 *
	 * 加它是为了把**拱高与洞宽解耦**（2026-09-04，用户"门上升过高"）。半圆下拱高恒等于半宽，
	 * 洞一宽拱就顶到很高；TG 不是这样 —— `img/tiny-glade-ref-twin-arch-pier.jpg` 里那两个拱
	 * 跨约 200 cm、起拱线以上只升约 80 cm，明显**扁于半圆**（TG 侧的载体是
	 * `create_stone_arch_profile` + `ArchFunction::remap_t` 的剖面曲线）。
	 *
	 * 归一化之后判据一个字都不用改：`q = ((S−Cs)/HW, (Z−拱脚)/Rise)`，`dot(q,q) < 1` 从正圆
	 * 变成**椭圆**。⇒ **材质里那段 HLSL 不需要跟着改** —— 它只看已经归一化的 q，
	 * 缩放全在 `FCSOpeningClipField` 里。这条是这次改动能这么小的原因。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float ArchRise = 0.0f;

	float HalfWidth() const { return Width * 0.5f; }
	/** 实际拱高：`ArchRise` 为 0 时退回半宽（正半圆），并夹在洞高之内。 */
	float Rise() const
	{
		const float Full = FMath::Max(Z1 - Z0, UE_KINDA_SMALL_NUMBER);
		const float Want = (ArchRise > UE_KINDA_SMALL_NUMBER) ? ArchRise : HalfWidth();
		return FMath::Clamp(Want, UE_KINDA_SMALL_NUMBER, Full);
	}
	float S0() const { return CenterS - Width * 0.5f; }
	float S1() const { return CenterS + Width * 0.5f; }
	bool IsValid() const { return Width > UE_KINDA_SMALL_NUMBER && Z1 > Z0 + UE_KINDA_SMALL_NUMBER; }
};

/** 剖面在某个横向位置上的上下边界。ZLow == ZHigh 表示洞在此闭合（圆的两端）。 */
struct FCSOpeningProfileSample
{
	float S = 0.0f;
	float ZLow = 0.0f;
	float ZHigh = 0.0f;
};

/**
 * 曲线段数按弦高容差自适应，而不是写死一个常数。
 *
 * 半圆按 N 段折线近似时弦高 = R·(1 − cos(π/2N))，反解得
 *   N = ceil( π / (2·acos(1 − Tol/R)) )
 * **注意分母那个 2** —— 漏掉它会在 Tol=0.2、R=55 时给出 37 段而不是正确的 19 段。
 *
 * 写死 12 段的问题是它把绝对误差绑在了半径上：R=55 时折角 0.47 cm，房子放大即线性放大。
 */
inline int32 CSHouse_ProfileSegments(float Radius, float ChordTolerance)
{
	if (Radius <= UE_KINDA_SMALL_NUMBER) return 6;
	const float Tol = FMath::Max(ChordTolerance, 1.0e-3f);
	const float CosHalf = FMath::Clamp(1.0f - Tol / Radius, -1.0f, 1.0f - 1.0e-6f);
	const float HalfAngle = FMath::Acos(CosHalf);
	if (HalfAngle <= 1.0e-6f) return 48;
	return FMath::Clamp(FMath::CeilToInt(PI / (2.0f * HalfAngle)), 6, 48);
}

/**
 * 采样一个洞的剖面，产出沿 S 单调递增的上下边界折线。
 *
 * **圆弧按外接多边形建，不是内接**（计划「洞口内壁的接缝精度」）：折线内接会比精确曲线
 * 缩进最多一个弦高，与按精确曲线摆放的洞缘装饰之间就会漏缝；外接则是微覆盖，宁可覆盖不可漏。
 * 顶点半径因此乘 1/cos(π/2N)，折线的边恰好切在真圆上。
 *
 * Skew 把洞顶沿 S 线性倾斜（楼梯口）；Z0/Z1 是未倾斜时的洞底与洞顶。
 */
inline void CSHouse_SampleOpeningProfile(const FCSWallOpening& Opening, float ChordTolerance, TArray<FCSOpeningProfileSample>& OutSamples)
{
	OutSamples.Reset();
	if (!Opening.IsValid()) return;

	const float R = Opening.HalfWidth();
	const float Skew = Opening.Skew;
	auto Emit = [&OutSamples, &Opening, Skew](float S, float ZLow, float ZHigh)
	{
		FCSOpeningProfileSample Sample;
		Sample.S = S;
		const float Tilt = Skew * (S - Opening.CenterS);
		Sample.ZLow = ZLow;
		Sample.ZHigh = ZHigh + Tilt;
		OutSamples.Add(Sample);
	};

	switch (Opening.Shape)
	{
	case ECSOpeningShape::Rect:
		// 上下边界都是常数：两个样本就够，法线不会因为多插几个点而变好。
		Emit(Opening.S0(), Opening.Z0, Opening.Z1);
		Emit(Opening.S1(), Opening.Z0, Opening.Z1);
		break;

	case ECSOpeningShape::Circle:
	{
		// 洞高服从洞宽（正圆），Z0/Z1 只用来定圆心。
		const float ZCentre = (Opening.Z0 + Opening.Z1) * 0.5f;
		const int32 N = CSHouse_ProfileSegments(R, ChordTolerance) * 2;   // 整圆，半圆的两倍
		const float Circum = R / FMath::Cos(PI / (2.0f * N));
		for (int32 K = 0; K <= N; ++K)
		{
			const float A = PI * K / N;   // 0..π 扫过 S，上下边界对称
			Emit(Opening.CenterS - Circum * FMath::Cos(A), ZCentre - Circum * FMath::Sin(A), ZCentre + Circum * FMath::Sin(A));
		}
		break;
	}

	case ECSOpeningShape::Arch:
	default:
	{
		// 矩形下身（Z0 → 拱脚）+ **椭圆**顶：半轴 = (半宽, Rise)。`ArchRise` 为 0 时 Rise = 半宽，
		// 退化成原来的正半圆，逐位不变。拱脚高 = 洞顶 − Rise。
		const float Rise = Opening.Rise();
		const float SpringZ = FMath::Max(Opening.Z1 - Rise, Opening.Z0);
		// 段数按**较长**那根半轴取：扁拱的 S 向仍然很长，按 Rise 取会在两肩上欠采样。
		const int32 N = CSHouse_ProfileSegments(FMath::Max(R, Rise), ChordTolerance);
		// 外接补偿逐轴各做一次（弦高补偿是等比缩放，两轴不同尺度时不能共用一个系数）。
		const float Inflate = 1.0f / FMath::Cos(PI / (2.0f * N));
		const float CircumS = R * Inflate;
		const float CircumZ = Rise * Inflate;
		for (int32 K = 0; K <= N; ++K)
		{
			const float A = PI * K / N;   // 0..π，从 S0 扫到 S1
			Emit(Opening.CenterS - CircumS * FMath::Cos(A), Opening.Z0, SpringZ + CircumZ * FMath::Sin(A));
		}
		break;
	}
	}
}

/**
 * 洞的**解析裁剪场**：把墙空间 (S, Z) 归一化成 q，材质按 q 逐像素判 discard。
 *
 * 这是 Tiny Glade 原版的开洞方式（逆向报告第一轮【确凿，GLSL 直接给出】）——CPU 只提供解析
 * 参数，洞形在像素阶段成立，零几何运算、洞缘是解析精确曲线而非折线。TG 的载体是逐砖实例的
 * `vec3 global_arch_height_vals`；本项目的墙是三角汤，对位物是**逐顶点 UV1**（UV0 留给墙面
 * 贴图，靠 FCSMeshStreamLayout::NumTexCoordSets 的逐 mesh 变体腾出来的）。
 *
 * **每种形状自带一套归一化，判据因此都是自封闭的**（不需要额外传洞底/洞高）：
 *   Arch   q = ((S−Cs)/HW, (Z−SpringZ)/Rise)    判据 q.y ≤ 0 ? |q.x| < 1 : dot(q,q) < 1
 *          （Rise 缺省 = HW ⇒ 正半圆；给了别的值就是**椭圆拱**，见 `FCSWallOpening::ArchRise`）
 *   Rect   q = ((S−Cs)/HW, (Z−Zmid)/HH)         判据 max(|q.x|, |q.y|) < 1
 *   Circle q = ((S−Cs)/HW, (Z−Zc)/HW)           判据 dot(q,q) < 1
 *
 * Arch 的判据在拱脚线以下是**无下界**的 —— 这不是疏漏：洞面板本身从 Z0 起建，面板底就是洞底，
 * 窗台那一截是下面另一块实心盒。这样两个 float 就够，不必再挤一个归一化洞底进 8-bit 通道
 * （直线的量化不可见，曲线的量化贴脸可见，所以宁可让几何承担直线那一半）。
 *
 * **判据必须与材质里那份逐字对应** —— 同一形状两处写不一样，就会出现"CPU 说能放、画面上切穿帮"。
 * CSHouse_ClipKeeps() 是 CPU 侧的那一份，单测拿它与几何交叉验证。
 */
USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSOpeningClipField
{
	GENERATED_BODY()

	/** 沿边弧长上的洞心。 */
	UPROPERTY() float CenterS = 0.0f;
	/** 1 / 半宽。 */
	UPROPERTY() float InvHalfWidth = 0.0f;
	/** 竖直参考高（Arch = 拱脚, Rect = 洞中, Circle = 圆心）。 */
	UPROPERTY() float RefZ = 0.0f;
	/** 1 / 竖直半尺度。 */
	UPROPERTY() float InvScaleZ = 0.0f;
	/** 形状 id，写进顶点色 B 通道（/255）。 */
	UPROPERTY() ECSOpeningShape Shape = ECSOpeningShape::Arch;
	/** false = 这块面板上没有洞，UV1 写哨兵、B 写 255。 */
	UPROPERTY() bool bValid = false;

	FVector2f Eval(float S, float Z) const
	{
		if (!bValid) return FVector2f(8.0f, 8.0f);   // 哨兵：任何判据下都在洞外，整块面板保留
		return FVector2f((S - CenterS) * InvHalfWidth, (Z - RefZ) * InvScaleZ);
	}
};

/** 从一个洞算出它的裁剪场。与 CSHouse_SampleOpeningProfile 同源，两者描述的是同一条曲线。 */
inline FCSOpeningClipField CSHouse_ComputeClipField(const FCSWallOpening& Opening)
{
	FCSOpeningClipField Field;
	if (!Opening.IsValid()) return Field;

	const float HW = Opening.HalfWidth();
	Field.CenterS = Opening.CenterS;
	Field.InvHalfWidth = 1.0f / HW;
	Field.Shape = Opening.Shape;
	Field.bValid = true;

	switch (Opening.Shape)
	{
	case ECSOpeningShape::Rect:
	{
		const float HH = FMath::Max((Opening.Z1 - Opening.Z0) * 0.5f, UE_KINDA_SMALL_NUMBER);
		Field.RefZ = (Opening.Z0 + Opening.Z1) * 0.5f;
		Field.InvScaleZ = 1.0f / HH;
		break;
	}
	case ECSOpeningShape::Circle:
		Field.RefZ = (Opening.Z0 + Opening.Z1) * 0.5f;
		Field.InvScaleZ = 1.0f / HW;   // 正圆：竖直尺度服从洞宽
		break;
	case ECSOpeningShape::Arch:
	default:
	{
		// 竖直尺度是 **Rise 而不是半宽**（2026-09-04）：两者相等时就是原来的正半圆。
		// 这一行是"拱高与洞宽解耦"的全部实现 —— 判据与材质 HLSL 都只看归一化后的 q，不必改。
		const float Rise = Opening.Rise();
		Field.RefZ = Opening.Z1 - Rise;  // 拱脚
		Field.InvScaleZ = 1.0f / Rise;
		break;
	}
	}
	return Field;
}

/**
 * CPU 侧的裁剪判据。**返回 true = 这一点保留（在洞外）**，与材质 OpacityMask 的语义一致。
 * 材质里那份是它的逐字翻译；两处必须一起改。
 */
inline bool CSHouse_ClipKeeps(const FCSOpeningClipField& Field, const FVector2f& Q)
{
	if (!Field.bValid) return true;
	switch (Field.Shape)
	{
	case ECSOpeningShape::Rect:   return !(FMath::Max(FMath::Abs(Q.X), FMath::Abs(Q.Y)) < 1.0f);
	case ECSOpeningShape::Circle: return !((Q.X * Q.X + Q.Y * Q.Y) < 1.0f);
	case ECSOpeningShape::Arch:
	default:
		return !(Q.Y <= 0.0f ? FMath::Abs(Q.X) < 1.0f : (Q.X * Q.X + Q.Y * Q.Y) < 1.0f);
	}
}

/**
 * 洞在高度 `Z` 处的**半宽**（0 = 这个高度上没有洞）。`CSHouse_ClipKeeps` 的逆解。
 *
 * 砖层（D4 两层之 A）的①删实例要它：按洞的**包围盒**裁行，拱洞会被切成一个矩形缺口 ——
 * 拱顶两侧本该有砖的地方全空了。按剪影裁才是对的，而剪影就是"每个高度上洞有多宽"。
 *
 * ⚠️ **两处边界不能只看 clip 场**：
 * - 拱的 clip 场在**拱脚以下无下界**（`FCSOpeningClipField` 注释里写明是故意的，窗台那一截由
 *   实心盒承担）。只问 clip 场的话，一扇 `Z0 = 90` 的拱窗会一路裁到地面。所以先用洞自己的
 *   `[Z0, Z1]` 卡一道。
 * - 判据用 **`<=` 而不是 `<`**：`|q| == 1` 恰好落在洞缘上（矩形洞的 `Z == Z0` 就是这种情况）。
 *   取闭区间会把洞算大一丝丝 ⇒ 多删一块砖而不是留一块砖在洞里，**朝安全的方向错**。
 */
inline float CSHouse_OpeningHalfWidthAtZ(const FCSWallOpening& Opening, float Z)
{
	if (!Opening.IsValid()) return 0.0f;
	if (Z < Opening.Z0 || Z > Opening.Z1) return 0.0f;

	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Opening);
	if (!Field.bValid || Field.InvHalfWidth <= UE_KINDA_SMALL_NUMBER) return 0.0f;

	const float HalfWidth = 1.0f / Field.InvHalfWidth;
	const float QY = (Z - Field.RefZ) * Field.InvScaleZ;

	switch (Field.Shape)
	{
	case ECSOpeningShape::Rect:
		return FMath::Abs(QY) <= 1.0f ? HalfWidth : 0.0f;
	case ECSOpeningShape::Circle:
		return FMath::Abs(QY) <= 1.0f ? HalfWidth * FMath::Sqrt(FMath::Max(1.0f - QY * QY, 0.0f)) : 0.0f;
	case ECSOpeningShape::Arch:
	default:
		// 拱脚以下是矩形下身（满宽），以上是椭圆。
		if (QY <= 0.0f) return HalfWidth;
		return QY <= 1.0f ? HalfWidth * FMath::Sqrt(FMath::Max(1.0f - QY * QY, 0.0f)) : 0.0f;
	}
}

/**
 * 洞在一条**横带** `[LowZ, HighZ]` 上挡掉的 S 区间（返回 false = 这一带它挡不住）。
 *
 * 取带内**最宽**的那个高度 —— 少取一点就会在洞里留下半块砖。三个采样点足够覆盖三种形状：
 * 两个端点，加上 `RefZ`（圆洞的最宽处在中间，拱与矩形则单调或恒定，端点已经包住）。
 */
inline bool CSHouse_OpeningSpanForBand(const FCSWallOpening& Opening, float LowZ, float HighZ,
	float& OutS0, float& OutS1)
{
	if (!Opening.IsValid()) return false;

	// 带与洞在 Z 上的交集。空 ⇒ 挡不住（高窗不切勒脚、落地门不切压顶就靠这一句）。
	const float Lo = FMath::Max(LowZ, Opening.Z0);
	const float Hi = FMath::Min(HighZ, Opening.Z1);
	if (Hi < Lo) return false;

	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Opening);
	const float Peak = FMath::Clamp(Field.RefZ, Lo, Hi);

	float HalfWidth = 0.0f;
	HalfWidth = FMath::Max(HalfWidth, CSHouse_OpeningHalfWidthAtZ(Opening, Lo));
	HalfWidth = FMath::Max(HalfWidth, CSHouse_OpeningHalfWidthAtZ(Opening, Hi));
	HalfWidth = FMath::Max(HalfWidth, CSHouse_OpeningHalfWidthAtZ(Opening, Peak));
	if (HalfWidth <= UE_KINDA_SMALL_NUMBER) return false;

	OutS0 = Opening.CenterS - HalfWidth;
	OutS1 = Opening.CenterS + HalfWidth;
	return true;
}

/**
 * 一条横带上，洞缘从**带底到带顶**收进去多少（≥ 0）。砖层洞缘四级的③要它。
 *
 * ①按剪影裁行时，切口取的是带内**最宽**的那个高度 —— 于是洞窄下去的那一头（拱圈往上）
 * 会在砖与洞之间留一条缝，最宽处 = 本函数的返回值。③把端头那块砖**剪切**成上宽下窄，
 * 缝就没了：TG 对位物是 `flags & 32` 那条"局部 z 按拱高曲线缩放"，做的是同一件事。
 *
 * ⚠️ 只处理**往上收窄**（拱；矩形恒 0）。往上变宽的洞（正圆的下半）返回 0 ⇒ 保持①的保守
 * 阶梯：剪切是**平行四边形**，一个自由度救不了上下两头都要动的梯形，硬凑会把砖推进洞里。
 */
inline float CSHouse_OpeningTopShear(const FCSWallOpening& Opening, float LowZ, float HighZ)
{
	float S0 = 0.0f, S1 = 0.0f;
	if (!CSHouse_OpeningSpanForBand(Opening, LowZ, HighZ, S0, S1)) return 0.0f;

	const float WidestHalf = (S1 - S0) * 0.5f;
	const float TopHalf = CSHouse_OpeningHalfWidthAtZ(Opening, FMath::Min(HighZ, Opening.Z1));
	return FMath::Max(WidestHalf - TopHalf, 0.0f);
}

/** 洞在墙上实际占掉的沿边区间（面板宽度）：半宽再向两侧各让出半个墩，端盖因此 |q.x| > 1 恒保留。 */
inline void CSHouse_OpeningCell(const FCSWallOpening& Opening, float PierWidth, float& OutMinS, float& OutMaxS)
{
	const float CellHalf = Opening.HalfWidth() + FMath::Max(PierWidth, 1.0f) * 0.5f;
	OutMinS = Opening.CenterS - CellHalf;
	OutMaxS = Opening.CenterS + CellHalf;
}

// -----------------------------------------------------------------------------
// 拱间墩（计划 D6「拱间墩与转角墩」，2026-08-30 的用户实拍裁决）
//
// 实拍（`Docs/TinyGlade/img/TG_continuous_arches.png`）读出的三件事：连续拱之间**没有"墙"这个表面**，
// 起拱线以下只有一根细墩、两侧直接透到草地；墩是**砖**不是残料墙，拱圈的楔石不间断砌到地面；
// 墙面本身是灰泥。
//
// ⚠️ **落点在 2026-08-30「裁决三」之后改了一次**：计划原话是「窄跨度上**不生成**灰泥面板」，
// 而裁决三把「避免所有真几何洞、一律渲染层挖」升格成了架构不变量 —— 不生成面板就是一个真几何洞。
// 所以现在的做法是：**面板照常砌成实心盒，起拱线以下那截由一块矩形裁剪场在像素阶段裁掉**
// （`CSHouse_PierClipField`），与门拱走同一套设施。观感等价，几何实心，烘成 StaticMesh 之后
// 洞仍由材质切出（裁决六）。
//
// ⚠️ **残料跨度按洞剖面之间量，不是按面板格之间量。** 默认参数下拱宽 = 段距 − 墩宽，
// 于是两个**格**恰好首尾相接、格之间那块"实心段"面板宽度为 0 —— 只跳过它等于什么都没做。
// 真正的那片灰泥是两个格各自伸进跨度里的**端盖**（各半个 PierWidth），所以墩侧必须把格收回到
// 洞缘（见 CSHouse_BuildBodySoup）。
// -----------------------------------------------------------------------------

/** `FCSWallOpening::StyleFlags` 的位：我**左**（S 小）/ **右**（S 大）那段残料按墩处理。 */
constexpr uint8 CSHouse_StylePierBefore = 1 << 0;
constexpr uint8 CSHouse_StylePierAfter = 1 << 1;
// `1 << 2` 曾是 `CSHouse_StyleCornerDoor`（跨转角段被切出的那片），**2026-09-06 用户裁决删除**：
// 转角就是一个墩 —— 两道拱只是不在同一条边上 —— 所以它走上面那两位，配对在
// `ResolvePierSpans` 里跨角做，中间的柱子由 `BuildCornerPierBricks` 出。这一位空着不复用，
// 免得旧序列化数据里残留的位被读成别的意思。
/**
 * 这个洞是**一条路被切成多拱**（拱廊）里的一个子拱（2026-09-04 傍晚）。
 *
 * 用户在 TG 里的实测："两道门并排时只有拱没有木门" —— 木门只装在**单拱**上，
 * 一排连续拱是敞开的过道（实拍 `img/tiny-glade-ref-twin-arch-pier.jpg` 里两道拱都空着）。
 * ⚠️ `ResolvePierSpans` 只清墩那两位，这一位能活到门扇那一步。
 */
constexpr uint8 CSHouse_StyleArcade = 1 << 3;
/**
 * 门扇看这一组：任一位为真 ⇒ 敞开过道，不装门扇。
 *
 * 墩位进这里是 2026-09-06 删掉转角位的直接后果：转角一对靠墩位相认，"转角不装门扇"就得从墩位读。
 * 附带的行为变化：**两扇独立的门挨得够近被判成墩时也不装门扇了** —— 与 TG 实测
 * "两道门并排时只有拱没有木门"一致，此前那种情况反而是例外。
 */
constexpr uint8 CSHouse_StyleNoLeafMask = CSHouse_StylePierBefore | CSHouse_StylePierAfter | CSHouse_StyleArcade;

/** 墩样式的两个阈值。双阈迟回与门拱点亮同一条纪律（计划 D6）。 */
struct FCSHousePierStyle
{
	bool bEnabled = true;
	/** 跨度 ≤ 此值 ⇒ 转墩（低阈）。 */
	float MaxWidth = 60.0f;
	/** 已是墩时跨度 ≥ 此值才转回墙（高阈）。 */
	float RestoreWidth = 75.0f;
};

/**
 * 双阈迟回：跨度是**连续量**，单阈会让跨度在阈值附近抖动时样式来回切换（整面墙的灰泥
 * 忽有忽无，比"选错一种样式"难看得多）。与门洞的区间滞回（`DoorKeepWidthRatio`）同型。
 */
inline bool CSHouse_SpanIsPier(const FCSHousePierStyle& Style, float SpanWidth, bool bWasPier)
{
	if (!Style.bEnabled) return false;
	// 高阈被填得比低阈还小时退化成单阈，不至于出现"转墩了就再也转不回来"。
	const float High = FMath::Max(Style.RestoreWidth, Style.MaxWidth);
	return bWasPier ? (SpanWidth < High) : (SpanWidth <= Style.MaxWidth);
}

/**
 * 相邻两个洞之间那段残料**能不能**按墩处理；能的话给出跨度与墩顶高。
 *
 * **只认"两侧都是落地的拱"**，理由是墩顶只有对拱才定义得出来：拱在起拱线以下是等宽矩形，
 * 起拱线正是"两拱的砖脚并排竖直往下走"的那条线，也就是墩该砌到多高。矩形洞没有起拱线
 * （洞是整块矩形），圆洞的对位物是圆心 —— 拿它们当墩顶会在窗台下方挖掉一段本该有的墙。
 * 窗（Z0 > 0）更直接：它下面那截墙是窗台，不是墩。
 *
 * 墩顶取两拱起拱线的**较低者**：取较高者会在两条起拱线之间留一条横向缝（低的那一拱在那里
 * 已经把洞开满了，而墩上的灰泥还没开始砌）。
 */
inline bool CSHouse_PierSpanBetween(const FCSWallOpening& Left, const FCSWallOpening& Right,
	float& OutSpan, float& OutTopZ)
{
	OutSpan = 0.0f;
	OutTopZ = 0.0f;
	if (!Left.IsValid() || !Right.IsValid()) return false;
	if (Left.EdgeIndex != Right.EdgeIndex) return false;
	if (Left.Shape != ECSOpeningShape::Arch || Right.Shape != ECSOpeningShape::Arch) return false;
	if (Left.Z0 > UE_KINDA_SMALL_NUMBER || Right.Z0 > UE_KINDA_SMALL_NUMBER) return false;

	const float Span = Right.S0() - Left.S1();
	if (Span < 0.0f) return false;   // 洞重叠（谓词本该挡住）：不是跨度，别当墩

	OutSpan = Span;
	OutTopZ = FMath::Max(FMath::Min(Left.Z1 - Left.HalfWidth(), Right.Z1 - Right.HalfWidth()), 0.0f);
	return true;
}

/**
 * 墩跨度的裁剪矩形要比跨度本身宽出来的余量 cm。
 *
 * 与普通洞**恰好相反**：普通面板的端盖必须落在洞**外**（`|q.x| > 1`）才不会把墩切掉；
 * 墩跨度这块面板的两片端盖必须落在洞**内**才会被裁掉 —— 否则跨度两端会各立起一片贯穿墙厚的
 * 灰泥薄片，正好卡在墩的位置上，从拱洞里一眼看见。
 */
constexpr float CSHouse_PierCutMargin = 1.0f;

/**
 * 一段矩形裁剪场的**唯一真源**（墩跨度与 D7 接缝共用）。
 *
 * 抽出来不是为了省行数，是因为"端盖要落在裁剪矩形**内**"这条反直觉的约束现在有两个消费者：
 * 各写一份的症状是其中一处忘了加余量，于是那一处的跨度两端各立起一片贯穿墙厚的薄片 ——
 * 而两处都自洽，谁都不报错。余量由调用方给，因为两处的取值理由不同（见各自的常量注释）。
 *
 * 竖向是**闭区间** [BottomZ, TopZ]：矩形判据 `|q.y| < 1` 上下都有界。想要"下边界在地面以下、
 * 只裁上界"就传一个负的 BottomZ（墩传的是 −TopZ，对称矩形）。
 */
inline FCSOpeningClipField CSHouse_RectCutField(float MinS, float MaxS, float BottomZ, float TopZ, float Margin)
{
	FCSWallOpening Cut;
	Cut.Shape = ECSOpeningShape::Rect;
	Cut.CenterS = (MinS + MaxS) * 0.5f;
	Cut.Width = FMath::Max(MaxS - MinS, 0.0f) + 2.0f * FMath::Max(Margin, 0.0f);
	Cut.Z0 = BottomZ;
	Cut.Z1 = FMath::Max(TopZ, BottomZ + 1.0f);   // 退化输入不许把 InvScaleZ 除成 inf
	return CSHouse_ComputeClipField(Cut);
}

/**
 * 墩跨度的裁剪场：把起拱线以下那一整片灰泥裁掉。
 *
 * **不是"不生成面板"**（2026-08-30 裁决三：避免所有真几何洞，一律用顶点色或门洞那套 clip 场在
 * 渲染层挖）—— 面板照常砌成实心盒，只是这块矩形在像素阶段被 discard。代价与门拱逐字相同：
 * 距离场 / Lumen 把 masked 洞当实心墙，墩两侧在 GI 里不透光（用户已知情接受）。
 *
 * 竖向取 [−PierTopZ, +PierTopZ] 的对称矩形：墙空间 Z ≥ 0，所以 `|q.y| < 1` 就等于"在起拱线以下"，
 * 不必给矩形另造一套"下边界在地面以下"的表达。
 */
inline FCSOpeningClipField CSHouse_PierClipField(float SpanMinS, float SpanMaxS, float PierTopZ)
{
	const float Top = FMath::Max(PierTopZ, 1.0f);
	return CSHouse_RectCutField(SpanMinS, SpanMaxS, -Top, Top, CSHouse_PierCutMargin);
}

/**
 * 接缝裁剪矩形的余量 cm。理由与 `CSHouse_PierCutMargin` 同型（端盖必须落在矩形**内**），
 * 但**取值不能共用一个常量**：墩的余量是跨度尺度（几十 cm）上的事，接缝这一刀横跨的是
 * 邻居整个 footprint（几百 cm），将来谁想调其中一个都不该顺手动到另一个。
 */
constexpr float CSHouse_SeamCutMargin = 1.0f;

/**
 * D7 接缝在墙上切的那一段：**纯裁剪，不是洞**。
 *
 * 不进 `CurrentOpenings`、不过 `CSHouse_QueryOpening`、不长门框砖 —— 洞是"这面墙上有个开口"
 * 这件设计意图，接缝裁剪是"这截墙插进邻居房间里了，别画它"这件事实。混进 openings 表的症状
 * 有三：谓词会拿它去挤掉真正的窗、`ResolvePierSpans` 会拿它当拱去配墩、`MakeOpeningPath`
 * 会绕着它铺一圈门框砖（而接缝砖只该立在轮廓交点上）。
 */
struct FCSWallCut
{
	int32 EdgeIndex = 0;
	/** 沿边弧长区间（与 `CSHouse_GetEdge` 的 S 同口径）。 */
	float MinS = 0.0f;
	float MaxS = 0.0f;
	/** 墙空间高度区间。BottomZ ≤ 0 表示"一直裁到墙底"。 */
	float BottomZ = 0.0f;
	float TopZ = 0.0f;

	bool IsValid() const { return MaxS - MinS > UE_KINDA_SMALL_NUMBER && TopZ > BottomZ + UE_KINDA_SMALL_NUMBER; }
};

/** 接缝裁剪场。与墩共用 `CSHouse_RectCutField`，只是余量与竖向区间的来源不同。 */
inline FCSOpeningClipField CSHouse_SeamClipField(float MinS, float MaxS, float BottomZ, float TopZ)
{
	// 下边界 ≤ 0 时改用对称矩形：墙空间 Z ≥ 0，对称矩形在墙内没有下边界，省得在墙底附近
	// 因为 `|q.y|` 的浮点 1.0 而留下一条一像素宽的墙脚（同 `CSHouse_PierClipField` 的理由）。
	const float Bottom = BottomZ > UE_KINDA_SMALL_NUMBER ? BottomZ : -FMath::Max(TopZ, 1.0f);
	return CSHouse_RectCutField(MinS, MaxS, Bottom, TopZ, CSHouse_SeamCutMargin);
}

/**
 * 两个洞是否冲突。
 *
 * 判据是**同边一维 S 区间**：两个洞的**面板格**（CSHouse_OpeningCell —— 半宽再向两侧各让半个墩）
 * 按 Clearance 膨胀后是否相交，Z 一律不参与。
 *
 * **用户裁决 2026-08-30（C1 选甲）：永久放弃"门上开窗"。** 此前判的是 (S, Z) 二维矩形，允许高窗
 * 压在低门正上方 —— 但 CSHouse_BuildBodySoup 铺墙板是沿 S 的单游标扫掠、每块面板只带一个 clip
 * 场，S 上重叠的第二个洞根本砌不出来（轻则被前一块无 clip 的面板咬掉半边，重则面板装不下而
 * 静默 continue）。谓词与几何必须同维，D8「谓词说能放就一定砌得出」才成立。
 *
 * 比的是**格**而不是洞本身：格才是扫掠真正消费的那个区间 —— 两格互不相交 ⇒ 游标永远顶不到洞。
 * 墩宽因此只有 CSHouse_OpeningCell 一处真源，将来按洞型分流墩宽时谓词自动跟随。
 */
inline bool CSHouse_OpeningsOverlap(const FCSWallOpening& A, const FCSWallOpening& B, float PierWidth, float Clearance)
{
	if (A.EdgeIndex != B.EdgeIndex) return false;

	float AMin = 0, AMax = 0, BMin = 0, BMax = 0;
	CSHouse_OpeningCell(A, PierWidth, AMin, AMax);
	CSHouse_OpeningCell(B, PierWidth, BMin, BMax);
	return !(AMin - Clearance > BMax || BMin > AMax + Clearance);
}

// -----------------------------------------------------------------------------
// 墙面框架与可放置性谓词（D8「谓词是唯一真源」的执行面）
// -----------------------------------------------------------------------------

/** 一面墙的局部框架：Start 起点（外皮线上）、U 沿墙、In 向内（厚度方向）、Len 墙长。U×In = +Z（已验）。 */
struct FCSHouseEdgeFrame
{
	FVector2D Start = FVector2D::ZeroVector;
	FVector2D U = FVector2D::ZeroVector;
	FVector2D In = FVector2D::ZeroVector;
	float Len = 0;
};

/**
 * 房屋 footprint：**闭合折线**（2026-09-12 用户裁决，取代原来的 `FVector2D` 矩形）。
 *
 * 顶点按**逆时针**排列；边 `i` 从 `Verts[i]` 走到 `Verts[(i + 1) % N]`，`0` 号边的起点就是
 * `Verts[0]`。矩形由 `MakeRect` 造出来时与老口径同序（0 南 1 东 2 北 3 西），所以边号的含义
 * 一个字都没变。
 *
 * ⚠️ **逆时针不是风格选择，是 `In` 的来源**：逆时针多边形的内部恒在行进方向的**左**手侧，
 * 于是 `In = Perp(U) = (-U.y, U.x)` 对任意边都成立 —— 这一条取代了原来四条边各自硬编码的
 * `In`，也是折线化之所以能不碰下游那一整类纯函数（clip 场、谓词、门段、包边、砖层、藤条、
 * 摆件锚点、门扇）的原因：它们只认 `(Start, U, In, Len)`，不认边数。
 *
 * 本类型目前**还不是序列化的权威** —— `ACSHouseActor::FootprintSize` 仍是被编辑的那个量，
 * 折线由它派生（3a-1 的边界）。让折线本身可编辑、可存盘是 3a-2 的事。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSHouseFootprint
{
	GENERATED_BODY()

	/** 逆时针闭合折线的顶点（局部空间，房心为原点）。末顶点与首顶点自动相连，不要重复首点。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	TArray<FVector2D> Verts;

	int32 NumEdges() const { return Verts.Num(); }
	bool IsValidFootprint() const { return Verts.Num() >= 3; }

	/** 边 `i` 的两个端点（外皮线上）。越界或退化时返回 false，出参不动。 */
	bool GetEdgeVerts(int32 EdgeIndex, FVector2D& OutA, FVector2D& OutB) const
	{
		const int32 N = Verts.Num();
		if (N < 3 || EdgeIndex < 0 || EdgeIndex >= N) return false;
		OutA = Verts[EdgeIndex];
		OutB = Verts[(EdgeIndex + 1) % N];
		return true;
	}

	/**
	 * 轴对齐矩形折线，与 `CSHouse_GetEdge` 的老口径逐位同序：
	 * 0 号边从 (−HX, −HY) 走到 (+HX, −HY)（南，+X 向），其余按逆时针。
	 */
	static FCSHouseFootprint MakeRect(const FVector2D& Size)
	{
		const double HX = Size.X * 0.5, HY = Size.Y * 0.5;
		FCSHouseFootprint FP;
		FP.Verts.Reserve(4);
		FP.Verts.Add({ -HX, -HY });
		FP.Verts.Add({ HX, -HY });
		FP.Verts.Add({ HX, HY });
		FP.Verts.Add({ -HX, HY });
		return FP;
	}
};

/**
 * 转角对接归谁：**过渡期的直角约定**，`true` = 这条边两端各让出一个墙厚 `T`。
 *
 * ⚠️ **这是 3a-2 要换掉的东西，不要在它上面盖新逻辑。** 今天的语义是「偶数边吃下两个转角
 * 方块，奇数边缩进去抵住它们」（矩形上就是南北墙通长、东西墙缩 2T）。它只在**偶数条边且
 * 全为直角**的多边形上成立 —— 奇数边会有一个转角两边都想让，非直角处让出的量也不该是 T。
 * 折线的终局是**斜接**：转角方块沿角平分线一分为二，两条边各拿一半，让出的量是
 * `T / (2·tan(θ/2))`，对直角恰好退化成 `T/2`（而不是今天的 0 或 T）。
 *
 * 斜接落地时这个函数删掉，`CSHouse_MakeEdgeFrame` 的 `bInsetBothEnds` 换成逐端的让出量。
 */
inline bool CSHouse_EdgeInsetsBothEnds(int32 EdgeIndex)
{
	return (EdgeIndex & 1) != 0;
}

/**
 * 一条边的框架：外皮线段 `A → B`，按转角约定让出两端，`In` 由逆时针性质导出。
 *
 * 所有 `CSHouse_GetEdge` 重载共用这一份 —— 矩形那条也走它，所以「矩形是折线的特例」不是
 * 说法而是代码事实。
 */
inline FCSHouseEdgeFrame CSHouse_MakeEdgeFrame(const FVector2D& A, const FVector2D& B, bool bInsetBothEnds, float T)
{
	FCSHouseEdgeFrame F;
	const FVector2D D = B - A;
	const double Len0 = D.Size();
	if (Len0 <= UE_DOUBLE_SMALL_NUMBER) return F;   // 退化边：零长框架，调用方按 Len <= 0 跳过

	F.U = D / Len0;
	// 逆时针 ⇒ 内部在左手侧。原来四条边各写一份 In 的地方就是这一行。
	F.In = FVector2D(-F.U.Y, F.U.X);
	// ⚠️ **先转 float 再减**，与老口径 `float(Footprint.Y) - 2 * T` 的舍入逐位一致；
	// 在 double 里减完再转会差 1 ULP，而墙长进哈希。
	F.Len = bInsetBothEnds ? (float(Len0) - 2 * T) : float(Len0);
	F.Start = bInsetBothEnds ? (A + F.U * T) : A;
	return F;
}

/**
 * 周界某一面墙。折线版：边号即顶点号，`In` 由逆时针导出，两端按转角约定让出。
 *
 * **住在头文件里而不是 CSHouseActor.cpp 的匿名命名空间里**：墙板、门框砖、藤蔓、摆件、
 * 以及下面那条谓词全都要问"这面墙在哪、有多长"，各抄一份的症状是"藤悬在离墙半个墙厚的
 * 空中"这类只在改过 WallThickness 之后才显形的错位。单测也要拿它算墙长。
 */
inline FCSHouseEdgeFrame CSHouse_GetEdge(int32 EdgeIndex, const FCSHouseFootprint& Footprint, float T)
{
	FVector2D A, B;
	if (!Footprint.GetEdgeVerts(EdgeIndex, A, B)) return FCSHouseEdgeFrame();
	return CSHouse_MakeEdgeFrame(A, B, CSHouse_EdgeInsetsBothEnds(EdgeIndex), T);
}

/**
 * 周界四墙的矩形口径：0 南(+X 向) 1 东(+Y 向) 2 北(-X 向) 3 西(-Y 向)，东西两面缩短 2T。
 *
 * 与折线版**逐位等价**（`House.FootprintPolylineMatchesRect` 钉住）：这里不重排顶点、不走
 * `TArray`，只是把 `MakeRect` 的四个顶点就地算出来喂同一个核，免掉热路径上的堆分配
 * （门框砖 / 藤条 / 摆件都在逐实例的循环里问边框架）。
 *
 * 3a-1 期间调用方可以继续传 `FVector2D`；能产出非矩形 footprint 之后，这个重载随
 * `ACSHouseActor::FootprintSize` 一起退役。
 */
inline FCSHouseEdgeFrame CSHouse_GetEdge(int32 EdgeIndex, const FVector2D& Footprint, float T)
{
	const double HX = Footprint.X * 0.5, HY = Footprint.Y * 0.5;
	FVector2D A, B;
	switch (EdgeIndex & 3)
	{
	case 0:  A = { -HX, -HY }; B = { HX, -HY };  break;
	case 1:  A = { HX, -HY };  B = { HX, HY };   break;
	case 2:  A = { HX, HY };   B = { -HX, HY };  break;
	default: A = { -HX, HY };  B = { -HX, -HY }; break;
	}
	return CSHouse_MakeEdgeFrame(A, B, CSHouse_EdgeInsetsBothEnds(EdgeIndex), T);
}

/**
 * 射线（或一个点）落在哪面外墙上。**房子局部空间** —— 世界变换由调用方先解掉。
 *
 * D8 特征标记的宿主解析用它：`S` / `Z` 恰好就是 `FCSHouseWindow` 要的 `CenterS` / 窗台高，
 * 所以标记从"我在世界的哪里"到"我是这面墙上的哪一扇窗"是一步换算，不需要第二套坐标。
 */
struct FCSWallHit
{
	bool  bHit = false;
	/** 0..3，与 `CSHouse_GetEdge` 同号。 */
	int32 EdgeIndex = -1;
	/** 沿边弧长，与 `CSHouse_GetEdge` 的 S 同口径。 */
	float S = 0.0f;
	/** 墙空间高度，房底为 0。 */
	float Z = 0.0f;
	/** 射线版是沿射线的距离；就近版是到墙面的垂距。 */
	float Distance = 0.0f;
};

/**
 * 射线 × 四面外墙，取最近的一次命中（**解析求交，不是引擎 trace**）。
 *
 * ⚠️ **必须解析求交**：房子的 gpumesh 全线 `NoCollision`（`CSGpuMeshComponent.cpp`），
 * `LineTraceSingle` 永远打不到 —— 计划 D8 明写这条，照 trace 写会得到"窗户一放就没"，
 * 而且不会有任何断言报红（标记自毁是"合法"行为）。
 *
 * 只判**外表面**（`dot(Dir, 外法线) < 0`）：从墙里往外打不算命中，否则站在房内乱挥鼠标
 * 会把窗贴到背面那堵墙上。
 */
inline FCSWallHit CSHouse_RayHitWall(
	const FVector& LocalOrigin, const FVector& LocalDir,
	const FVector2D& Footprint, float T, float WallHeight, float MaxDistance)
{
	FCSWallHit Best;
	Best.Distance = MaxDistance;

	const FVector2D Dir2(LocalDir.X, LocalDir.Y);
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, T);
		if (F.Len <= UE_KINDA_SMALL_NUMBER) continue;

		const FVector2D N(-F.In.X, -F.In.Y);          // 外法线
		const double Denom = FVector2D::DotProduct(Dir2, N);
		if (Denom > -UE_KINDA_SMALL_NUMBER) continue;  // 平行、或从背面打过来

		const double Dist = FVector2D::DotProduct(F.Start - FVector2D(LocalOrigin.X, LocalOrigin.Y), N) / Denom;
		if (Dist <= 0.0 || Dist >= double(Best.Distance)) continue;

		const FVector P = LocalOrigin + LocalDir * Dist;
		const double S = FVector2D::DotProduct(FVector2D(P.X, P.Y) - F.Start, F.U);
		if (S < 0.0 || S > double(F.Len)) continue;
		if (P.Z < 0.0 || P.Z > double(WallHeight)) continue;

		Best.bHit = true;
		Best.EdgeIndex = Edge;
		Best.S = float(S);
		Best.Z = float(P.Z);
		Best.Distance = float(Dist);
	}
	return Best;
}

/**
 * 就近找墙：把点投到四面外墙上取最近的一面（计划 D8 的 `csh.WindowSnapDist` 退路）。
 *
 * 射线版是"朝向摆正了"的通路；这条兜的是"贴着墙但朝向没摆正"——两条都空才判无宿主。
 * S / Z 都夹到墙面内，所以它返回的永远是墙上一个**合法**位置，谓词那关照旧另判。
 */
inline FCSWallHit CSHouse_NearestWall(
	const FVector& LocalPoint, const FVector2D& Footprint, float T, float WallHeight, float MaxDistance)
{
	FCSWallHit Best;
	Best.Distance = MaxDistance;

	const FVector2D P2(LocalPoint.X, LocalPoint.Y);
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, T);
		if (F.Len <= UE_KINDA_SMALL_NUMBER) continue;

		const FVector2D N(-F.In.X, -F.In.Y);
		const double S = FMath::Clamp(FVector2D::DotProduct(P2 - F.Start, F.U), 0.0, double(F.Len));
		const double Z = FMath::Clamp(double(LocalPoint.Z), 0.0, double(WallHeight));
		// 到墙面的距离取三维：面内偏移已经被夹住，剩下的就是法向 + 被夹掉的那两段。
		const FVector2D OnFace = F.Start + F.U * S;
		const FVector Delta(P2.X - OnFace.X, P2.Y - OnFace.Y, LocalPoint.Z - Z);
		// 背面（法向为负）不算 —— 与射线版同一条纪律。
		if (FVector2D::DotProduct(P2 - F.Start, N) < 0.0) continue;

		const double Dist = Delta.Size();
		if (Dist >= double(Best.Distance)) continue;

		Best.bHit = true;
		Best.EdgeIndex = Edge;
		Best.S = float(S);
		Best.Z = float(Z);
		Best.Distance = float(Dist);
	}
	return Best;
}

/**
 * 附属物挂在墙上的**锚点**（D8，用户裁决 2026-09-06「位置的存储方式与 TG 一致」）。
 *
 * ## 为什么是「离某个角多远」而不是绝对弧长
 *
 * `FCSHouseWindow::CenterS` 的原点是 `CSHouse_GetEdge` 的 `F.Start`，而 `ACSHouseActor::PushEdge(e)`
 * 会把 e 那一侧的**两个角**各挪 `Δ` —— 它们恰好是第 `e` 与第 `(e+1)%4` 条边的 S 原点。后果实测过
 * （2026-09-05 四条边逐一推算）：**推第 e 条边，第 e 与第 e+1 条边上的窗错位 Δ，另外两条完全正确**，
 * 其中 e+1 那面墙**根本没动**、窗却沿着它滑了 Δ —— 最难解释的一种。
 *
 * 锚到「最近的那个角」之后：被推那一侧的窗随墙走、另一侧的窗原地不动，行为可预期。
 * 这与 TG `wall_attachment::WallCornerAttachment::{from_rectangle_corner_id, to_rectangle_corner_id}`
 * 用**拓扑角编号**而不是坐标是同一个思路 `[PDB]`。
 *
 * ⚠️ **锚点是唯一权威，actor 变换是派生量**（TG `CachedDecoratorTransforms` + `move_decorators_following_anchors`）。
 * 序列化这份结构，不要序列化世界变换 —— 反过来做就是 2026-09-05 分析出的那条"重开关卡窗又挪一次位"。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSWallAnchor
{
	GENERATED_BODY()

	/** 挂在哪条边，0..3，与 `CSHouse_GetEdge` 同号。`-1` = 尚未锚定。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	int32 EdgeIndex = -1;

	/** `false` = 从边的**起点角**量起，`true` = 从**终点角**量起。取离命中点近的那个。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	bool bFromEndCorner = false;

	/** 到那个角的沿墙距离 cm（恒 ≥ 0）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	float DistFromCorner = 0.0f;

	/** 洞底离墙基的高度 cm（= `FCSHouseWindow::SillZ`）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	float SillZ = 0.0f;

	bool IsValidAnchor() const { return EdgeIndex >= 0 && EdgeIndex <= 3 && DistFromCorner >= 0.0f; }

	/** 逐字段相等。用来在重登记前挡掉"没动"的那些，口径同 `FCSHouseWindow::operator==`。 */
	bool operator==(const FCSWallAnchor& O) const
	{
		return EdgeIndex == O.EdgeIndex && bFromEndCorner == O.bFromEndCorner
			&& DistFromCorner == O.DistFromCorner && SillZ == O.SillZ;
	}
	bool operator!=(const FCSWallAnchor& O) const { return !(*this == O); }
};

/**
 * 一次墙面命中 → 锚点。**沿边弧长离哪个角近就锚哪个角**（见 `FCSWallAnchor` 的注释）。
 *
 * `SillZ` 由调用方给：命中点的 `Z` 是窗**心**高度，洞底要再减半个窗高（口径与
 * `ACSWindowMarker::MakeDemand` 一致，两处必须同源，否则窗会整体偏高半扇）。
 */
inline FCSWallAnchor CSHouse_MakeWallAnchor(const FCSWallHit& Hit, const FVector2D& Footprint, float T, float SillZ)
{
	FCSWallAnchor A;
	if (!Hit.bHit) return A;

	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Hit.EdgeIndex, Footprint, T);
	const float S = FMath::Clamp(Hit.S, 0.0f, FMath::Max(F.Len, 0.0f));

	A.EdgeIndex = Hit.EdgeIndex & 3;
	// 正中间（S == Len/2）归**起点角**：判据要给出确定的一侧，`>` 而不是 `>=` 就是这个用意。
	A.bFromEndCorner = (S > F.Len * 0.5f);
	A.DistFromCorner = A.bFromEndCorner ? (F.Len - S) : S;
	A.SillZ = SillZ;
	return A;
}

/**
 * 锚点 → 沿边弧长 `S`（`FCSHouseWindow::CenterS` 要的那个量）。
 *
 * 墙缩短到锚点越界时**夹到 `[0, Len]`** 而不是留在墙外：谓词那关照旧会判 `NearCorner` 把它拒掉，
 * 但夹住能保证派生变换永远落在墙面上 —— 标记不会飞到墙外的空中去。
 */
inline float CSHouse_AnchorS(const FCSWallAnchor& A, const FVector2D& Footprint, float T)
{
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(A.EdgeIndex, Footprint, T);
	const float Len = FMath::Max(F.Len, 0.0f);
	const float S = A.bFromEndCorner ? (Len - A.DistFromCorner) : A.DistFromCorner;
	return FMath::Clamp(S, 0.0f, Len);
}

/**
 * 锚点 → **房子构建空间**的变换（`ACSHouseActor::GetBuildTransform()` 那个只取 yaw 的空间）。
 * 这就是 TG `CachedDecoratorTransforms` 的对位物：变换是从锚点算出来的，不是存下来的。
 *
 * 约定（与 `ACSHouseFeatureMarker` 的探针同向，**别改**）：
 * - **+X = 墙的内法线 `In`**（标记站在墙外看着墙）。`ResolveHostAndRegister` 沿 `GetActorForwardVector()`
 *   打探针，反过来的话标记一放上去就找不到宿主 —— 而找不到宿主是"合法"的自毁，不会有断言报红。
 * - 原点在墙的外表面**再往外 `Standoff`** 处。窗框资产按 −X 朝外来摆。
 *
 * ⚠️ **`Standoff` 必须 ≥ 0，不能把原点推进墙里**（否则派生出来的位置会让探针失效，而且两条通路
 * 一起失效：射线版从外皮内侧往内打得到**负**距离被 `Dist <= 0` 挡掉，就近版的
 * `dot(P − Start, 外法线) < 0` 判成背面直接 `continue`）。症状是"窗吸附一次之后就再也解析不到宿主"，
 * 而无宿主会自毁 —— 又是一次不报红的静默失效。派生变换必须是解析的**不动点**：
 * `SnapToAnchor()` 之后再 `ResolveHostAndRegister()` 必须拿回同一个锚点，单测 `House.WallAnchor` 钉这条。
 *
 * `HalfHeight` = 窗高的一半：锚点存的是洞底，而标记本体锚在窗**心**（口径同 `MakeDemand`）。
 */
inline FTransform CSHouse_AnchorToLocal(
	const FCSWallAnchor& A, const FVector2D& Footprint, float T, float HalfHeight, float Standoff)
{
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(A.EdgeIndex, Footprint, T);
	const float S = CSHouse_AnchorS(A, Footprint, T);

	// 外表面上的点 + 沿**外**法线站开一点。`F.Start` 本来就在外皮上（东西两面缩 T 是为了避开转角
	// 重叠，缩的是长度不是法向），所以这里不需要再补半个墙厚。
	const FVector2D P2 = F.Start + F.U * double(S) - F.In * double(FMath::Max(Standoff, 0.0f));
	const FVector Pos(P2.X, P2.Y, double(A.SillZ + HalfHeight));

	const FVector Fwd(F.In.X, F.In.Y, 0.0);
	return FTransform(FRotationMatrix::MakeFromXZ(Fwd, FVector::UpVector).Rotator(), Pos);
}

/** 洞被拒的原因（计划 D8 的 `FCSFeaturePlacement::Reason`）。编辑器/脚本据此区分"没生成"与"生成了但看不见"。 */
UENUM(BlueprintType)
enum class ECSFeatureReject : uint8
{
	/** 放得下。 */
	None,
	/** 尺寸退化（宽 ≤ 0 或 Z1 ≤ Z0），根本不是一个洞。 */
	Degenerate,
	/** 边号不在 0..3。 */
	NotOnWall,
	/** 会吃掉墙两端的护角。 */
	NearCorner,
	/** 洞底在地面以下，或低于窗台下限（几何合法但观感荒唐）。 */
	SillTooLow,
	/** 洞顶会吃掉墙顶那条连续砖带。 */
	AboveEave,
	/** 与已有洞的**面板格**按 Clearance 膨胀后相交。 */
	OverlapsOpening,
	/** 落在判为墩的跨度上 —— 那截墙起拱线以下整片被裁掉，开在上面就是一扇悬空的洞。 */
	OnPierSpan,
};

/** 谓词的全部输入。房子只是把自己的属性填进来 —— 抽出来是为了让纯 CPU 单测能调**同一条**判据。 */
struct FCSOpeningSite
{
	FVector2D Footprint = FVector2D(600.0, 400.0);
	float WallThickness = 24.0f;
	float WallHeight = 300.0f;
	float LintelBand = 40.0f;
	float CornerMargin = 60.0f;
	float PierWidth = 20.0f;
	float OpeningClearance = 10.0f;

	/** 窗台下限 cm。只对**非门**生效：门恒 Z0 = 0，拿这条卡它等于把所有门都拒了。 */
	float MinSillZ = 0.0f;

	/** 墩样式开关与它的**高阈**（判据为什么取高阈见 CSHouse_QueryOpening 里那段）。 */
	bool bPierStyleEnabled = true;
	float PierRestoreWidth = 75.0f;

	/**
	 * 已生效的洞。**必须按 (EdgeIndex, CenterS) 升序** —— 墩跨度那一段靠"相邻两项即同边相邻
	 * 两洞"来找跨度，乱序会让它拿隔了一个洞的两端当跨度，把中间那一整块墙误判成墩。
	 */
	TArrayView<const FCSWallOpening> Openings;
};

/**
 * 这个洞放得下吗（D8 的可行性谓词，纯参数判定、零 GPU 回读）。
 *
 * **判据是同边一维 S 区间**：两洞的面板格（`CSHouse_OpeningCell`）按 Clearance 膨胀后是否
 * 相交，Z 一律不参与 —— 用户裁决 2026-08-30（C1 选甲），永久放弃"门上开窗"。谓词与
 * `CSHouse_BuildBodySoup` 的单游标扫掠同维，"谓词说能放就一定砌得出"才成立。**别把它改回二维。**
 *
 * ⚠️ **墩跨度那一条只能按高阈保守判**：墩的双阈迟回（`ResolvePierSpans`）跑在窗过完谓词
 * **之后**（墩是最终洞集合的函数），所以谓词跑的时候 `StyleFlags` 还没算、也不该读。这里改判
 * "这条跨度**有没有可能**被判成墩"——跨度 ≤ 高阈就一律不接受窗。宁可多拒一扇，也不能放出
 * 一扇悬在被裁空的墩上的窗。
 */
inline ECSFeatureReject CSHouse_QueryOpening(const FCSOpeningSite& Site, const FCSWallOpening& Candidate)
{
	if (!Candidate.IsValid()) return ECSFeatureReject::Degenerate;
	if (Candidate.EdgeIndex < 0 || Candidate.EdgeIndex > 3) return ECSFeatureReject::NotOnWall;

	// 必须整个落在这面墙的可用段内（护角照留），且顶部不能吃掉墙顶的连续砖带。
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Candidate.EdgeIndex, Site.Footprint, Site.WallThickness);
	if (Candidate.S0() < Site.CornerMargin || Candidate.S1() > F.Len - Site.CornerMargin) return ECSFeatureReject::NearCorner;
	if (Candidate.Z0 < 0.0f) return ECSFeatureReject::SillTooLow;
	if (Candidate.Type != ECSOpeningType::Door && Candidate.Z0 < Site.MinSillZ) return ECSFeatureReject::SillTooLow;
	if (Candidate.Z1 + FMath::Abs(Candidate.Skew) * Candidate.HalfWidth() > Site.WallHeight - Site.LintelBand) return ECSFeatureReject::AboveEave;

	for (const FCSWallOpening& Existing : Site.Openings)
	{
		if (Existing.SourceId.IsValid() && Existing.SourceId == Candidate.SourceId) continue;   // 自己不与自己冲突
		if (CSHouse_OpeningsOverlap(Existing, Candidate, Site.PierWidth, Site.OpeningClearance)) return ECSFeatureReject::OverlapsOpening;
	}

	// 墩跨度不接受窗（计划 D6「拱间墩」）。默认参数下这条不可达 —— 墩跨度 ≈ PierWidth，而格谓词
	// 本来就要求两洞的格（各含半个墩宽）不相交 ⇒ 窗压根挤不进两个拱之间。把 PierStyleMaxWidth
	// 调到远超 PierWidth 时才会露出来：那时墩上会挖出一扇悬在半空的窗。
	if (Site.bPierStyleEnabled && Site.Openings.Num() >= 2)
	{
		float CellMin = 0, CellMax = 0;
		CSHouse_OpeningCell(Candidate, Site.PierWidth, CellMin, CellMax);
		for (int32 K = 0; K + 1 < Site.Openings.Num(); ++K)
		{
			const FCSWallOpening& Left = Site.Openings[K];
			const FCSWallOpening& Right = Site.Openings[K + 1];
			if (Left.EdgeIndex != Candidate.EdgeIndex) continue;
			float Span = 0.0f, TopZ = 0.0f;
			// 跨度与墩顶只认这一份判据（"两侧都是落地的拱"），与房体那块墩裁剪场同源。
			if (!CSHouse_PierSpanBetween(Left, Right, Span, TopZ)) continue;
			if (Span > FMath::Max(Site.PierRestoreWidth, 0.0f)) continue;   // 宽到永远判不成墩
			if (CellMin < Right.S0() && CellMax > Left.S1()) return ECSFeatureReject::OnPierSpan;
		}
	}
	return ECSFeatureReject::None;
}

/** 拒绝原因 → 一句人话。编辑器线框颜色与脚本日志共用，别在调用点各写一份。 */
inline const TCHAR* CSHouse_FeatureRejectText(ECSFeatureReject Reason)
{
	switch (Reason)
	{
	case ECSFeatureReject::None:            return TEXT("");
	case ECSFeatureReject::Degenerate:      return TEXT("尺寸退化（宽或高 ≤ 0）");
	case ECSFeatureReject::NotOnWall:       return TEXT("边号不在 0..3");
	case ECSFeatureReject::NearCorner:      return TEXT("吃掉了墙端的护角");
	case ECSFeatureReject::SillTooLow:      return TEXT("窗台太低（低于 WindowMinSillZ 或在地面以下）");
	case ECSFeatureReject::AboveEave:       return TEXT("洞顶吃掉了墙顶的连续砖带");
	case ECSFeatureReject::OverlapsOpening: return TEXT("与已有洞的面板格相交（门拱优先）");
	case ECSFeatureReject::OnPierSpan:      return TEXT("落在拱间墩的跨度上");
	default:                                return TEXT("未知");
	}
}
