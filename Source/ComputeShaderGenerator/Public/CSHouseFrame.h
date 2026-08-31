#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"   // FPaletteBuffers —— 实例源的容器，与石阶/藤/摆件同一份
#include "CSHouseProfile.h"        // FCSWallOpening / FCSOpeningClipField

/**
 * 门框砖的 **100% GPU 解析推导**（TinyGladeHouse D6；2026-08-30「裁决一」选乙）。
 *
 * -----------------------------------------------------------------------------
 * 为什么不再走「样条 + 逐砖记录」那条路（那条路**已删**，本节是它的墓志铭）
 * -----------------------------------------------------------------------------
 * 旧路（`ACSHouseActor::BuildFramePlan`，连同 `csh.FrameLegacy` 开关一起删于裁决一第二步）
 * 把洞缘折线**等距重采样**成控制点、喂给
 * `RDG_SmoothSpline` 求值成均匀三次 B 样条，再按弧长占比给每块砖排一条 `(alpha, scale)` 记录。
 * 它带着三样代价，而这三样在**洞缘本来就是解析曲线**这个事实面前全都是白付的：
 *
 *   ① **B 样条会抹掉「门樘 → 拱 → 门樘」的两个 90° 折角**。均匀三次 B 样条在控制点处取
 *      `(P₋₁ + 4P₀ + P₁)/6`，等距 h 的直角上偏移恰好是 `h·√2/6` —— 26 cm 砖、h≈13 cm 时约 3 cm。
 *      砖因此在起拱线附近整体离开设计曲线（早期没做重采样时更狠，实测设计拱脚 Z=160、样条只到 135）。
 *   ② **容量随砖数走**。记录是逐砖的 ⇒ 那条路的 `CSShaperSteps::EnsureCapacity`（同样已删）
 *      必须能扩容 ⇒ 扩容那一趟是阻塞刷新，落在"用户恰好画到这一笔"上。解析推导的砖数只是
 *      一个标量，容量恒定，交互期永远走不到扩容。
 *   ③ **槽位来自 `InterlockedAdd`** ⇒ 逐实例随机数由线程组完成顺序决定 ⇒ 同一份世界状态
 *      两次散布可以给同一块砖不同的随机数（S1 已经栽过一次，症状是"画一笔路全场变色"且
 *      不会有任何断言报红）。这里线程 i 恒写第 i 槽，随机数因此是砖身份的纯函数。
 *
 * ⇒ **连带作废**：`CSShaperSteps::ResampleUniform` 那条「喂控制点前必须先重采样成等弧长」的
 *   契约**只对 B 样条那条路成立**。两条 B 样条路（门框旧路、地形石阶旧路）都已删除，
 *   所以这条契约现在**没有任何产线消费者** —— `ResampleUniform` 本身留着，因为等价性单测
 *   `House.FrameAnalyticMatchesLegacy` 的 CPU 镜像要逐字复现旧路的组路方式。
 *
 * -----------------------------------------------------------------------------
 * 定位与定向的依据：洞缘就是 `FCSOpeningClipField` 那条解析曲线
 * -----------------------------------------------------------------------------
 * 洞在画面上的边缘**不是**任何折线 —— 墙板是实心盒，洞由材质按 `FCSOpeningClipField` 逐像素
 * discard 切出来（裁决三：避免所有真几何洞）。所以「砖该骑在哪条线上」这个问题只有一个正确
 * 答案：**clip 场那条解析曲线本身**。四个参数一一对上：
 *
 *   `CenterS`      → 圆弧的横向圆心（也是竖直段的对称中心）
 *   `1/InvHalfWidth` → 拱/圆的半径；矩形洞的半宽
 *   `RefZ`         → 拱 = 起拱线；圆 = 圆心；矩形 = 洞中
 *   `1/InvScaleZ`  → 矩形洞的半高（拱与圆恒等于半宽，所以那两种不需要它）
 *
 * 定向同样是闭式的：路径切向 `T` 在墙空间 (S, Z) 里直接可导，**面内朝外法线 = T 逆时针转 90°**
 * （`(-T.z, T.s)`）。旧 kernel 要靠「减去曲线中心求径向 + 投影掉平面法线 + 判号翻转」三步才
 * 得到同一个方向，那三步在拱心退化或曲线不绕中心时会翻错号；解析式没有可翻错的号。
 *
 * -----------------------------------------------------------------------------
 * 砖路的形状：竖直段 + 中段 + 竖直段 + 窗台底边
 * -----------------------------------------------------------------------------
 * 一条砖路（`FPath`）最多四段，按弧长首尾相接，四段都可缺席：
 *
 *   ① 左竖直段：S = `LeftS`，Z 从 `BaseZ` 升到 `TopZ`，切向 (0, +1)
 *   ② 中段：圆弧（θ 从 0 扫到 `MidSweep`，S = Cs − R·cosθ、Z = TopZ + R·sinθ）
 *          或水平段（矩形洞的平顶，长 `FlatLen`，切向 (+1, 0)）
 *   ③ 右竖直段：S = `RightS`，Z 从 `TopZ` 降回 `BaseZ`，切向 (0, −1)
 *   ④ 窗台底边：Z = `BaseZ`，S 从 `RightS` 走回 `LeftS`，切向 (−1, 0)
 *
 * 洞型与段的对应：
 *   · 门（落地的拱）= 左樘 + 半圆（Sweep = π，R = 半宽，圆心在起拱线）+ 右樘
 *   · 窗（`Z0 > CSHouse_SillMinZ` 的拱或矩形）= 上面三段 **+ 窗台底边**，整条路闭合成一圈
 *   · 圆   = 只有中段，Sweep = 2π（一圈砖，起点在最左）
 *
 * ⚠️ **第四段是 2026-08-30 补回来的，别再把它删掉。** 已删的样条旧路对 `Z0 > 0` 的洞会额外
 * 出一条下边界曲线（当时的 `bAnySill`），迁到解析推导时它跟着旧路一起没了 —— 那一轮零回归，
 * 因为**当时没有任何东西产出非落地的洞**。窗一上线就露馅：洞的下边界同样是一条 clip 边
 * （矩形洞 `|q.y| < 1` 上下都有界；拱洞的下界是洞面板的底 = `Z0`），没有砖骑在上面，
 * 窗台正面就是一条裸露的裁剪断口 —— 而那正是门框砖存在的全部理由。
 *
 * 接缝处切向**正交**而不是连续（半圆 θ=0 的切向恰是 (0,1)、θ=π 恰是 (0,−1)，窗台段是 (−1,0)），
 * 与矩形洞平顶那两个折角同型：砖在折角处转 90°，靠 `FrameBrickBloat` 的负缝互相咬住。
 *
 * -----------------------------------------------------------------------------
 * 拱间墩：相邻两拱在墩上只砌**一列**砖（2026-08-30 观感缺陷）
 * -----------------------------------------------------------------------------
 * 旧路让每个拱各出一条门樘砖脚：跨度 = `PierWidth`、砖的面内进深 = `FrameBrickDepth`，
 * 两条砖脚各骑在跨度两端、各伸进跨度一半 ⇒ **恰好在墩正中对接**，于是墩上有一条从地面
 * 一直贯到起拱线的竖缝（两列砖共面相抵，法线与深度都断开，出图 `pier_after_pier.png` 可见）。
 * 而 TG 本体实拍（计划 D6「Tiny Glade 本体实拍」一节）里墩就是**一列**：拱圈的砖不间断地
 * 砌到地面，中间那根墩是相邻两拱的砖脚**合在一起**，看不出"这里是这一拱、那里是那一拱"。
 *
 * 解析推导正好有条件照实拍做：砖位置既然是从拱参数直接算的，墩就不必是"两条砖脚的产物"，
 * 可以**升格成一条自己的砖路** ——
 *
 *   · 判到墩（`CSHouse_StylePierAfter` / `Before`，双阈迟回已在 `ResolvePierSpans` 里算好）时，
 *     **两侧的拱都不出那一侧的门樘**；
 *   · 由左边那一拱**一次性**产出一条竖直砖路，S = 墩心、Z 从地面到墩顶（两拱起拱线的较低者，
 *     与 `CSHouse_PierSpanBetween` 同口径）。
 *
 * 覆盖条件**一字未变**：一列砖覆盖 `[墩心 − 进深/2, 墩心 + 进深/2]`，要盖住整条跨度就要求
 * `PierWidth ≤ FrameBrickDepth` —— 与旧路"两条砖脚各伸进一半"给出的条件完全相同
 * （演示回归里那条 `the shipped pier is narrow enough...` 因此继续成立、继续有意义）。
 * 换来的是墩上**没有共面对接**：拱圈第一块砖骑在起拱线上、横向占 `[S₁−d/2, S₁+d/2]`，
 * 墩顶那块砖横向占 `[S₁, S₁+PierWidth]`，两者横向搭接半块、竖向靠 `FrameBrickBloat` 互相穿插
 * ⇒ 接缝是**错缝咬合**而不是一条直缝。
 *
 * -----------------------------------------------------------------------------
 * 裁决六（终局要存成 StaticMesh）的对齐
 * -----------------------------------------------------------------------------
 * 输出仍然是 `UCSGpuInstancedMeshComponent` 那套 packed 实例行（5 个 float4 / 实例），
 * **没有引入任何新的逐实例语义** —— 逐实例随机数从"槽位哈希"改成"砖序号哈希"，值域与用法
 * 都不变，且改成了确定的。所以门框砖原有的 `SaveToStaticMesh` 出口一字不用改。
 */
namespace CSHouseFrame
{
/** 中段的形态。`None` = 只有竖直段（墩就是这一种）。 */
enum class EMidKind : uint8
{
	None,
	/** 圆弧：θ ∈ [0, MidSweep]，S = CenterS − R·cosθ，Z = TopZ + R·sinθ。 */
	Arc,
	/** 水平直段（矩形洞的平顶）：从 LeftS 走到 RightS，长 FlatLen。 */
	Flat,
};

/**
 * 一条砖路的**解析**描述。所有量都在墙空间 (S = 沿边弧长, Z = 从房底起算的高度)。
 *
 * 这个结构体就是 GPU 侧逐路常量的 CPU 原本 —— `CSHouseFrame.usf` 里的六行 float4 与它逐字对应，
 * 两处必须一起改。
 */
struct FPath
{
	/** 竖直段的底（洞底；门恒 0，墩恒 0）。 */
	float BaseZ = 0.0f;
	/** 竖直段的顶（拱 = 起拱线 / 矩形 = 洞顶 / 墩 = 墩顶）。 */
	float TopZ = 0.0f;
	/** 左竖直段所在的 S。 */
	float LeftS = 0.0f;
	/** 右竖直段所在的 S。 */
	float RightS = 0.0f;
	/** 圆弧的横向圆心（= clip 场的 CenterS）。 */
	float CenterS = 0.0f;
	/** 圆弧半径（= clip 场的 1/InvHalfWidth）。 */
	float Radius = 0.0f;
	/** 圆弧扫过的角度：拱 = π，圆 = 2π。 */
	float MidSweep = 0.0f;
	/** 水平中段的长度（`EMidKind::Flat` 时才有意义）。 */
	float FlatLen = 0.0f;

	EMidKind MidKind = EMidKind::None;
	bool bLeftJamb = false;
	bool bRightJamb = false;
	/** 窗台底边（第四段）。洞底离地才有意义 —— 门永远是 false。 */
	bool bSill = false;

	float JambLen() const { return FMath::Max(TopZ - BaseZ, 0.0f); }
	float LeftLen() const { return bLeftJamb ? JambLen() : 0.0f; }
	float RightLen() const { return bRightJamb ? JambLen() : 0.0f; }
	float MidLen() const
	{
		switch (MidKind)
		{
		case EMidKind::Arc:  return FMath::Max(Radius, 0.0f) * MidSweep;
		case EMidKind::Flat: return FMath::Max(FlatLen, 0.0f);
		default:             return 0.0f;
		}
	}
	/**
	 * 窗台底边的长度 = 两樘之间的净宽。
	 *
	 * **两侧门樘缺一不可**：这条横边的两个端点就是两条竖直段的底，少一个的话砖会从一个
	 * 断口凭空起头。产线上不可达（墩只在 `Z0 = 0` 的落地拱之间成立，那种洞根本没有窗台），
	 * 前置写在这里是为了让退化输入也自洽 —— 判据只该有一份，别在调用点各判一次。
	 */
	float SillLen() const { return (bSill && bLeftJamb && bRightJamb) ? FMath::Max(RightS - LeftS, 0.0f) : 0.0f; }
	float TotalLen() const { return LeftLen() + MidLen() + RightLen() + SillLen(); }
};

/**
 * 弧长 → 墙空间位置与**单位**切向。**`CSHouseFrame.usf` 的 `FrameEvalPath` 是它的逐字翻译**，
 * 两处写不一样就会出现"单测说砖在这儿、画面上砖在那儿"，而两边各自都自洽 —— 与
 * `CSHouse_ClipKeeps` / 材质那对判据同一条纪律。
 */
inline void EvalPath(const FPath& Path, float Arc, FVector2f& OutSZ, FVector2f& OutTangent)
{
	const float L0 = Path.LeftLen();
	const float L1 = Path.MidLen();
	const float L2 = Path.RightLen();
	const float L3 = Path.SillLen();
	const float Total = L0 + L1 + L2 + L3;
	const float A = FMath::Clamp(Arc, 0.0f, FMath::Max(Total, 0.0f));

	// 分支顺序即段序；每一段都带"我存在吗"与"我后面还有段吗"的前置判断，退化路径
	// （只有中段的圆洞 / 只有左段的墩）因此不会掉进后面那段的公式里去算出一个看着合理的错位置。
	if (L0 > 0.0f && (A <= L0 || (L1 <= 0.0f && L2 <= 0.0f && L3 <= 0.0f)))
	{
		OutSZ = FVector2f(Path.LeftS, Path.BaseZ + FMath::Min(A, L0));
		OutTangent = FVector2f(0.0f, 1.0f);
		return;
	}
	if (L1 > 0.0f && (A <= L0 + L1 || (L2 <= 0.0f && L3 <= 0.0f)))
	{
		const float T = FMath::Min(A - L0, L1);
		if (Path.MidKind == EMidKind::Arc)
		{
			const float Theta = Path.Radius > UE_KINDA_SMALL_NUMBER ? T / Path.Radius : 0.0f;
			const float C = FMath::Cos(Theta), S = FMath::Sin(Theta);
			OutSZ = FVector2f(Path.CenterS - Path.Radius * C, Path.TopZ + Path.Radius * S);
			OutTangent = FVector2f(S, C);   // d/dθ 的单位化：θ=0 → (0,1)，θ=π → (0,−1)
		}
		else
		{
			OutSZ = FVector2f(Path.LeftS + T, Path.TopZ);
			OutTangent = FVector2f(1.0f, 0.0f);
		}
		return;
	}
	if (L2 > 0.0f && (A <= L0 + L1 + L2 || L3 <= 0.0f))
	{
		OutSZ = FVector2f(Path.RightS, Path.TopZ - FMath::Min(A - L0 - L1, L2));
		OutTangent = FVector2f(0.0f, -1.0f);
		return;
	}
	if (L3 > 0.0f)
	{
		// 窗台底边：从右樘底走回左樘底。切向朝 −S ⇒ 面内朝外法线（切向逆时针转 90°）朝 −Z，
		// 也就是**朝下、背离洞**，与另外三段"法线一律指出洞外"的口径一致。走反了砖会整排翻身。
		OutSZ = FVector2f(Path.RightS - FMath::Min(A - L0 - L1 - L2, L3), Path.BaseZ);
		OutTangent = FVector2f(-1.0f, 0.0f);
		return;
	}
	// 四段全空（调用方本该早退）：给一个不会让 kernel 除零的确定值。
	OutSZ = FVector2f(Path.LeftS, Path.BaseZ);
	OutTangent = FVector2f(0.0f, 1.0f);
}

/**
 * 单条目 palette 下 `ACSSplineBlockActor::SolveBlockLayout` 的**同式重写**：给定弧长，
 * 求砖数与整体铺装缩放。
 *
 * ⚠️ **必须逐句照抄那边的贪心累加，不能改写成 `ceil((Total + Gap)/(Len + Gap))`**：那个闭式
 * 在实数上等价，在 float 上不等价 —— 贪心是"反复 `Sum += Gap; Sum += Len`"，累加误差与一次
 * 除法的舍入方向不同，弧长恰好落在跳变点附近时两者会差**一整块砖**。砖数是几何的一部分
 * （进 desc 哈希、进回归断言），差一块就是画面差一块。单测 `House.FrameRunMatchesLayout`
 * 拿密扫把这条钉住。
 *
 * 返回砖数；`OutScale` 是铺装缩放（砖长与砖缝都乘它）。
 */
inline int32 SolveRun(float TotalLength, float BrickLength, float Gap, float& OutScale)
{
	OutScale = 0.0f;
	if (TotalLength <= UE_KINDA_SMALL_NUMBER) return 0;
	const float Len = BrickLength;
	if (Len <= UE_KINDA_SMALL_NUMBER) return 0;
	const float SafeGap = FMath::Max(Gap, 0.0f);

	// ① 贪心填到首次越界：`Sum >= TotalLength` 才停，所以最后一块必然是越界块。
	constexpr int32 MaxBlocks = 65536;
	float Sum = 0.0f;       // 含最后一块（及其前置 gap）
	float PrevSum = 0.0f;   // 不含最后一块
	int32 Count = 0;
	while (Sum < TotalLength)
	{
		if (Count >= MaxBlocks) return 0;
		PrevSum = Sum;
		if (Count > 0) Sum += SafeGap;
		Sum += Len;
		++Count;
	}

	// ② 候选 A：保留最后一块整体压缩；候选 B：去掉最后一块整体拉伸。|log(scale)| 小者胜。
	const float ScaleA = TotalLength / Sum;
	if (Count <= 1 || PrevSum <= UE_KINDA_SMALL_NUMBER)
	{
		OutScale = ScaleA;
		return Count;
	}
	const float ScaleB = TotalLength / PrevSum;
	if (FMath::Abs(FMath::Loge(ScaleA)) <= FMath::Abs(FMath::Loge(ScaleB)))
	{
		OutScale = ScaleA;
		return Count;
	}
	OutScale = ScaleB;
	return Count - 1;
}

/**
 * 墙面在世界里的框架：墙空间 (S, Z) → 世界 = `Origin + AxisU·S + AxisV·Z`；`AxisN` 是墙外法线。
 *
 * ⚠️ `AxisN` **不进 GPU 记录**：kernel 用 `cross(面内朝外, −切向)` 现算，那样得到的基一定是
 * 右手系。传一个外部法线进去再拿它当基的 +Z，一旦它与另外两轴的定向不自洽，实例就成了镜像
 * —— 法线朝里、光照当场坏掉，而几何位置看着完全正常。这里留着它只是给 CPU 侧读者/单测点明
 * 这面墙朝哪边。
 */
struct FWallFrame
{
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f AxisU = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f AxisV = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f AxisN = FVector3f(0.0f, -1.0f, 0.0f);
};

/** 砖的排布参数（尺寸不在这里 —— 那是 `FPaletteBuffers::BlockSize` 的事）。 */
struct FBrickParams
{
	float Length = 26.0f;
	float Gap = 0.0f;
	/** 全部砖路加起来的硬上限 = 常驻容量。**超了就截断，绝不扩容**（零阻塞纪律）。 */
	int32 MaxBricks = 512;
};

/** 一条砖路 + 它的世界框架 + 它在全局砖序里占的区间。上传给 GPU 的就是它。 */
struct FElement
{
	FPath Path;
	FWallFrame Frame;
	/** 全局砖序号的起点（各元素前缀和）—— 线程 i 靠它找到自己属于哪条路。 */
	int32 BrickBegin = 0;
	int32 BrickCount = 0;
	/** 相邻砖中心距 = (Length + Gap)·Scale；第一块砖的弧长 = HalfLen = Length·Scale/2。 */
	float Pitch = 0.0f;
	float HalfLen = 0.0f;
	/** 铺装缩放：砖的长度轴额外乘它（kernel 里的 `BlockScale.y *= LengthScale`）。 */
	float LayoutScale = 1.0f;
	/**
	 * 本条路第一块砖的**逐实例随机数序号**（kernel 取 `FrameInstanceRandom(RandomBase + 路内序号)`）。
	 *
	 * 门框砖填 `BrickBegin` —— 与"随机数 = 砖的全局槽位"逐位等价，那条路一个字都没变。
	 * 存在的理由是 **D7 接缝**：同一条缝由两栋房各画一份重叠的砖（裁决二：零归属），而槽位是
	 * "这栋房自己的第几块砖"，两边必然不同。让随机数跟着槽位走，就会在将来谁给砖材质接上
	 * `PerInstanceRandom` 色差的那一天，让两份重叠的砖以不同颜色互相闪 —— 而所有几何断言全绿。
	 * 接缝那条因此从**接缝身份**派生（`CSHouseSeam::SeamSeed`）。
	 */
	uint32 RandomBase = 0;
};

/**
 * 一个洞 → 它的解析砖路。`PierBefore/After` 来自 `FCSWallOpening::StyleFlags`：
 * 那一侧是拱间墩 ⇒ 这一拱**不出**那一侧的门樘（墩由左邻那一拱单独出一条竖直砖路）。
 *
 * 返回 false = 这个洞不产砖（退化尺寸）。
 */
COMPUTESHADERGENERATOR_API bool MakeOpeningPath(const FCSWallOpening& Opening, FPath& OutPath);

/** 拱间墩那条独立砖路：S = 墩心，Z 从地面到墩顶。`CSHouse_PierSpanBetween` 给跨度与墩顶。 */
COMPUTESHADERGENERATOR_API bool MakePierPath(const FCSWallOpening& Left, const FCSWallOpening& Right, FPath& OutPath);

/**
 * 一面墙上的洞集合 → 砖路元素表（**纯函数**，单测直接吃它）。
 *
 * `EdgeOpenings` 必须是**同一条边、按 CenterS 升序**的连续片段 —— `ACSHouseActor::ComputeDoors`
 * 末尾那次排序已经保证了这一点，`ResolvePierSpans` 打的墩样式位也建立在同一个顺序上。
 *
 * 追加进 `InOutElements`，返回本次追加的砖数（已按 `Params.MaxBricks` 截断）。
 */
COMPUTESHADERGENERATOR_API int32 BuildEdgeElements(const FWallFrame& Frame, TArrayView<const FCSWallOpening> EdgeOpenings,
	const FBrickParams& Params, TArray<FElement>& InOutElements);

/**
 * 录一趟散布：**一线程一砖**，没有样条、没有逐砖记录、没有原子累加。
 *
 * 与已删的 `CSShaperSteps::Scatter` 的区别不只是"少了一步" —— 那边每条曲线都要先跑一次
 * `RDG_SmoothSpline`（一个 dispatch）再跑一次散布（又一个 dispatch），且实例槽位靠
 * `InterlockedAdd` 抢；这里**整栋房子只有一个 dispatch**，槽位恒等于砖序号。
 *
 * 录完 pass 直接返回，不阻塞：`Palettes` 按值捕获（`TRefCountPtr` 拷贝即加引用）。
 * 前置条件是容量已由 `ReserveCapacity` 备好 —— 这里**只用现有容量，绝不分配**。
 */
COMPUTESHADERGENERATOR_API bool Scatter(const TArray<FElement>& Elements, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes,
	const FMatrix44f& WorldToComponent);
}
