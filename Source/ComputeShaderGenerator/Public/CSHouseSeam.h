#pragma once

#include "CoreMinimal.h"
#include "CSHouseFrame.h"       // FPath / FWallFrame / FElement / SolveRun —— 接缝砖是门框砖的一条新路径类型
#include "CSHouseProfile.h"     // FCSWallCut / CSHouse_GetEdge —— 墙在哪儿只能有一个真源
#include "Misc/Guid.h"          // 规范序的键
#include "Templates/TypeHash.h" // HashCombine —— 接缝身份 → 逐实例随机数的基

/**
 * 两栋房交汇处的接缝（TinyGladeHouse D7；2026-08-30「裁决二」）。
 *
 * -----------------------------------------------------------------------------
 * 为什么是纯函数而不是 actor
 * -----------------------------------------------------------------------------
 * 用户原话：「两栋房交汇时只产生接缝砖，其它任何内容都是独立的。」⇒ `ACSHouseSeamActor`
 * （中立 actor + 两端弱引用 + 自 tick + 交点表生命周期 + 角柱邻近合并 + 跨接缝仲裁）**整套否决**。
 * TG 侧的实证同向且更硬（`Docs/TinyGlade/TinyGlade_模块对照与进度.md（卷一）` §2.3）：那边这一级只有两个参数
 * —— `Res<IntraShapeCorners> → ResMut<InterShapeBrickStitches>`，零地形、零历史、零撤销、零 UI。
 *
 * 所以这里一个可变量都没有：输入两房的 footprint / 朝向 / 高度，输出砖列。**两栋房各自算出
 * 同一条接缝是有意的**，不是浪费 —— 它换掉的是"谁拥有这条缝"这个问题，连带换掉归属、撤销、
 * 生命周期、以及"A 删了 B 身上的缝谁来收"这一整类 bug。
 *
 * ⚠️ **正因为两边都画，逐位相同就不再是洁癖而是正确性**：同一条缝会有两份重叠的砖实例，
 * 位置只要差一个 ulp 就可能在深度上互相闪烁。`Canonical()` 把无序对钉成有序对，之后的每一步
 * 都是同一个函数吃同一份参数 ⇒ 两边的输出**逐位**相同。单测 `House.SeamOrderInvariant`
 * 拿 memcmp 钉住这条。
 *
 * ⚠️ 逐实例随机数同理，且它**不在**几何里：砖的槽位是"这栋房自己的第几块砖"，两栋房必然不同。
 * 所以砖路带一个 `RandomBase`（`CSHouseFrame::FElement`），接缝那条从**接缝身份**派生而不是
 * 从槽位派生 —— 不这么做的话，将来谁给砖材质接一个 `PerInstanceRandom` 色差，这两份重叠的砖
 * 就会以不同的颜色互相闪，而几何断言全绿。
 *
 * -----------------------------------------------------------------------------
 * 相交处那个"洞"是 clip 出来的，不是挖出来的
 * -----------------------------------------------------------------------------
 * TG 在形状相交处先开洞再砌缝砖（`add_hole_at_shape_intersection` → `WallHoles`）。本项目**不能
 * 照抄那一步**：2026-08-30「裁决三」是全局不变量 —— 避免所有真几何洞，几何永远实心，要挖一律
 * 走顶点色或逐像素 clip 场。
 *
 * 所以接缝的洞复用门窗那套设施，一行新 shader 都没加：`CutOnEdge` 算出"这面墙被邻居 footprint
 * 盖住的那一段"，交给 `CSHouse_SeamClipField` 变成一块矩形裁剪场，`CSHouse_BuildBodySoup` 照常
 * 把那截墙砌成实心盒、由墙材质按 UV1 逐像素 discard。代价与门拱逐字相同（距离场 / Lumen 把
 * masked 洞当实心墙），用户已知情接受。
 *
 * -----------------------------------------------------------------------------
 * 本轮**不做**的（裁决二明写的退出范围，别顺手加）
 * -----------------------------------------------------------------------------
 * 角柱邻近合并、跨接缝仲裁、接缝接受 openings。第三条的落点在 `CSHouse_BuildBodySoup`：
 * 洞面板自带的裁剪场优先，落在接缝区间里的洞**不受接缝裁剪影响** —— 一块面板只带一个裁剪场，
 * 而"两个场怎么合"正是被划出去的那件事。
 */
namespace CSHouseSeam
{
/** 交点 / 裁剪区间短于这个就不值得出砖出洞 cm（与房体面板那条 0.5 cm 的下限同量级）。 */
constexpr float MinSpan = 1.0f;

/**
 * 一栋房参与接缝所需的**全部**输入。
 *
 * 刻意只有权威属性（身份 / 摆位 / 尺寸），**没有任何派生表或缓存** —— 裁决二那句"零共享状态"
 * 约束的是状态，不是只读的输入。地面高度也**故意不在**里面：接缝的底取两房房底的较高者，
 * 而房底本来就已经把地面吸收进去了（`ACSHouseActor::ComputeSeatZ`）。再引一次地面等于给
 * 这条纯函数加一个它不需要的依赖，且会让"交换顺序结果相同"多一个能坏掉的地方。
 */
struct FHouse
{
	/** 稳定身份。**只用来定规范序**，不参与任何几何。 */
	FGuid Id;
	/** 房子在世界里的 XY 与 yaw（度）—— 与 `ACSHouseActor::GetBuildTransform()` 同口径（只取 yaw）。 */
	FVector2D Center = FVector2D::ZeroVector;
	float Yaw = 0.0f;
	FVector2D Footprint = FVector2D(600.0, 400.0);
	/** 房底世界 Z（落座之后）。 */
	float BaseZ = 0.0f;
	float WallHeight = 300.0f;
	float WallThickness = 24.0f;

	float EaveZ() const { return BaseZ + WallHeight; }
	/** 外接圆半径：邻近粗筛用（半对角线）。 */
	float Reach() const { return 0.5f * float(FVector2D(Footprint.X, Footprint.Y).Size()); }
};

/**
 * 无序对 → 有序对。**这是"交换两房顺序结果逐位相同"的唯一机关**，其余每一步都只是同一个
 * 函数吃同一份参数。
 *
 * 按 GUID 而不是按几何定序：几何量是浮点的，两栋同尺寸的房子可以在某一帧比出相反的大小
 * （而且只在那一帧），于是砖列会无声地换一次序。GUID 是稳定的、离散的、且与摆位无关。
 */
inline bool IdLess(const FGuid& A, const FGuid& B)
{
	if (A.A != B.A) return A.A < B.A;
	if (A.B != B.B) return A.B < B.B;
	if (A.C != B.C) return A.C < B.C;
	return A.D < B.D;
}

/** 规范序。两个 GUID 相等（同一栋房 / 都没设）时按原样返回，调用方本来就该先排除自己。 */
inline void Canonical(const FHouse& A, const FHouse& B, const FHouse*& OutFirst, const FHouse*& OutSecond)
{
	const bool bAFirst = !IdLess(B.Id, A.Id);
	OutFirst = bAFirst ? &A : &B;
	OutSecond = bAFirst ? &B : &A;
}

/** 接缝身份 → 逐实例随机数的基。规范序之下两栋房算出同一个值（这正是它存在的理由）。 */
inline uint32 SeamSeed(const FHouse& A, const FHouse& B)
{
	const FHouse* First = nullptr;
	const FHouse* Second = nullptr;
	Canonical(A, B, First, Second);
	return HashCombine(GetTypeHash(First->Id), GetTypeHash(Second->Id));
}

/** 世界 XY → 房子局部（`GetBuildTransform()` 的逆，只有 yaw 与平移）。 */
inline FVector2D ToLocal(const FHouse& H, const FVector2D& World)
{
	const float Rad = FMath::DegreesToRadians(H.Yaw);
	const double C = FMath::Cos(Rad), S = FMath::Sin(Rad);
	const FVector2D D = World - H.Center;
	return FVector2D(D.X * C + D.Y * S, -D.X * S + D.Y * C);
}

/** 房子局部 XY → 世界。 */
inline FVector2D ToWorld(const FHouse& H, const FVector2D& Local)
{
	const float Rad = FMath::DegreesToRadians(H.Yaw);
	const double C = FMath::Cos(Rad), S = FMath::Sin(Rad);
	return H.Center + FVector2D(Local.X * C - Local.Y * S, Local.X * S + Local.Y * C);
}

/**
 * footprint 轮廓的四个角（世界 XY），**顺序与 `CSHouse_GetEdge` 的边号一一对应**：
 * 第 k 条轮廓边 = Out[k] → Out[(k+1)&3]。
 *
 * ⚠️ 用的是**整条**矩形边，不是 `CSHouse_GetEdge` 那条为了避免转角重叠而两端各内缩 T 的墙段
 * —— 轮廓是轮廓，墙段是墙段。拿内缩过的墙段求交点，会在两房恰好在角附近相交时漏掉交点
 * （漏掉的那一根接缝砖没有任何断言看得见）。
 */
inline void FootprintCorners(const FHouse& H, FVector2D Out[4])
{
	const double HX = H.Footprint.X * 0.5, HY = H.Footprint.Y * 0.5;
	Out[0] = ToWorld(H, FVector2D(-HX, -HY));
	Out[1] = ToWorld(H, FVector2D(HX, -HY));
	Out[2] = ToWorld(H, FVector2D(HX, HY));
	Out[3] = ToWorld(H, FVector2D(-HX, HY));
}

/** 第 k 条轮廓边的**世界外法线**（局部 −In，见 `CSHouse_GetEdge`）。 */
inline FVector2D EdgeOutward(const FHouse& H, int32 EdgeIndex)
{
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, H.Footprint, H.WallThickness);
	const FVector2D LocalOut(-F.In.X, -F.In.Y);
	const float Rad = FMath::DegreesToRadians(H.Yaw);
	const double C = FMath::Cos(Rad), S = FMath::Sin(Rad);
	return FVector2D(LocalOut.X * C - LocalOut.Y * S, LocalOut.X * S + LocalOut.Y * C);
}

/** 两房的 Z 区间交集（世界）。返回 false = 一栋整个在另一栋上面，够不着。 */
inline bool OverlapZ(const FHouse& A, const FHouse& B, float& OutBottom, float& OutTop)
{
	OutBottom = FMath::Max(A.BaseZ, B.BaseZ);
	OutTop = FMath::Min(A.EaveZ(), B.EaveZ());
	return OutTop - OutBottom > MinSpan;
}

/**
 * 两房是否真的交汇：**footprint OBB 真重叠 + Z 区间相交**（用户裁决：不是"靠得近"）。
 *
 * 分离轴只需要四根（两个矩形各自的两根轴）—— 2D 下矩形的边法线就是它的轴，没有第三类候选。
 */
inline bool Intersects(const FHouse& A, const FHouse& B)
{
	float Bottom = 0.0f, Top = 0.0f;
	if (!OverlapZ(A, B, Bottom, Top)) return false;

	FVector2D CornersA[4], CornersB[4];
	FootprintCorners(A, CornersA);
	FootprintCorners(B, CornersB);

	auto Separated = [](const FVector2D* P, const FVector2D* Q, const FVector2D& Axis)
	{
		// 从第 0 个角起算而不是从 ±FLT_MAX 起算：少一个"极值常量选错类型"的坑，也少一个 include。
		double PMin = FVector2D::DotProduct(P[0], Axis), PMax = PMin;
		double QMin = FVector2D::DotProduct(Q[0], Axis), QMax = QMin;
		for (int32 K = 1; K < 4; ++K)
		{
			const double DP = FVector2D::DotProduct(P[K], Axis);
			const double DQ = FVector2D::DotProduct(Q[K], Axis);
			PMin = FMath::Min(PMin, DP); PMax = FMath::Max(PMax, DP);
			QMin = FMath::Min(QMin, DQ); QMax = FMath::Max(QMax, DQ);
		}
		// 恰好相切不算相交：触发条件是"真正重叠"，相切的两栋房各自的墙面正好贴上，没有穿插要遮。
		return PMax <= QMin + MinSpan || QMax <= PMin + MinSpan;
	};

	for (int32 K = 0; K < 2; ++K)
	{
		if (Separated(CornersA, CornersB, EdgeOutward(A, K))) return false;
		if (Separated(CornersA, CornersB, EdgeOutward(B, K))) return false;
	}
	return true;
}

/** 粗筛：外接圆都够不着就连交点都不用算（也是"邻居进不进哈希"的判据，见 `GetTrackingHash`）。 */
inline bool WithinReach(const FHouse& A, const FHouse& B)
{
	return FVector2D::DistSquared(A.Center, B.Center) <= FMath::Square(A.Reach() + B.Reach());
}

/** 一个轮廓交点 = 一根接缝砖柱。 */
struct FCorner
{
	/** 世界 XY。 */
	FVector2D Point = FVector2D::ZeroVector;
	/** 两墙外法线的角平分（单位，世界 XY）：砖的进深轴朝它，也就是朝"两栋房外面"那个象限。 */
	FVector2D Outward = FVector2D(1.0, 0.0);
	/** 世界 Z 区间：底取两房房底的较高者，顶取两檐口的较低者。 */
	float BottomZ = 0.0f;
	float TopZ = 0.0f;
};

/**
 * 两房轮廓的交点表。**规范序内部化** ⇒ `BuildCorners(A,B)` 与 `BuildCorners(B,A)` 逐位相同。
 *
 * 16 次线段求交（4×4），顺序固定为"规范序第一栋的边号 × 第二栋的边号"，所以交点次序也是确定的
 * —— 不排序、也不能排序：按坐标排序又会把浮点比较请回来。
 *
 * 两矩形相交一般出 2 或 4 个交点（带 yaw 最多 8）。平行边共线的退化情形直接跳过：那种情形下
 * 两面墙是贴合的，没有穿插要遮，硬造一根柱子反而多出一个孤零零的砖堆。
 */
inline int32 BuildCorners(const FHouse& A, const FHouse& B, TArray<FCorner>& OutCorners)
{
	OutCorners.Reset();
	if (!Intersects(A, B)) return 0;

	const FHouse* First = nullptr;
	const FHouse* Second = nullptr;
	Canonical(A, B, First, Second);

	float Bottom = 0.0f, Top = 0.0f;
	if (!OverlapZ(*First, *Second, Bottom, Top)) return 0;

	FVector2D P[4], Q[4];
	FootprintCorners(*First, P);
	FootprintCorners(*Second, Q);

	for (int32 I = 0; I < 4; ++I)
	{
		const FVector2D P0 = P[I], D1 = P[(I + 1) & 3] - P0;
		const FVector2D NI = EdgeOutward(*First, I);
		for (int32 J = 0; J < 4; ++J)
		{
			const FVector2D Q0 = Q[J], D2 = Q[(J + 1) & 3] - Q0;
			const double Denom = D1.X * D2.Y - D1.Y * D2.X;
			if (FMath::Abs(Denom) <= UE_DOUBLE_KINDA_SMALL_NUMBER) continue;   // 平行/共线：见上
			const FVector2D W = Q0 - P0;
			const double T = (W.X * D2.Y - W.Y * D2.X) / Denom;
			const double U = (W.X * D1.Y - W.Y * D1.X) / Denom;
			if (T < 0.0 || T > 1.0 || U < 0.0 || U > 1.0) continue;

			const FVector2D NJ = EdgeOutward(*Second, J);
			FCorner Corner;
			Corner.Point = P0 + D1 * T;
			// 角平分指向"两房外面"那个象限：交点周围四个象限里，只有 (·NI > 0 且 ·NJ > 0)
			// 那一个是两栋房都在外面的，也正是两条裁剪断口露头的地方。两法线接近反向时
			// （两面墙近乎平行对撞）和退化成零向量，退回其中一条法线 —— 那种情形下砖朝哪
			// 都一样，重要的是别产出一个零向量把整根柱子变成镜像。
			const FVector2D Bisect = NI + NJ;
			Corner.Outward = Bisect.SizeSquared() > UE_DOUBLE_KINDA_SMALL_NUMBER ? Bisect.GetSafeNormal() : NI;
			Corner.BottomZ = Bottom;
			Corner.TopZ = Top;
			OutCorners.Add(Corner);
		}
	}
	return OutCorners.Num();
}

/**
 * `Self` 的第 `EdgeIndex` 面墙上被 `Other` 的 footprint 盖住的那一段（墙空间 S 区间 + Z 区间）。
 *
 * 参数化按 `CSHouse_GetEdge`（**内缩过的墙段**，因为要裁的是那块面板的 S 坐标），再把两端点
 * 拿到 Other 的局部系里做 Liang–Barsky 区间裁剪 —— 矩形是轴对齐的，四个半平面各夹一次即可，
 * 没有迭代、没有分支依赖顺序。
 */
inline bool CutOnEdge(const FHouse& Self, const FHouse& Other, int32 EdgeIndex, FCSWallCut& OutCut)
{
	OutCut = FCSWallCut();
	float Bottom = 0.0f, Top = 0.0f;
	if (!OverlapZ(Self, Other, Bottom, Top)) return false;

	const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, Self.Footprint, Self.WallThickness);
	if (F.Len <= MinSpan) return false;

	const FVector2D A = ToLocal(Other, ToWorld(Self, F.Start));
	const FVector2D Bv = ToLocal(Other, ToWorld(Self, F.Start + F.U * F.Len));
	const FVector2D Dir = Bv - A;
	const double HX = Other.Footprint.X * 0.5, HY = Other.Footprint.Y * 0.5;

	double T0 = 0.0, T1 = 1.0;
	auto Clip = [&T0, &T1](double P, double Q)
	{
		if (FMath::Abs(P) <= UE_DOUBLE_SMALL_NUMBER) return Q >= 0.0;   // 平行于这条边界：在界内才继续
		const double R = Q / P;
		if (P < 0.0) { if (R > T1) return false; if (R > T0) T0 = R; }
		else { if (R < T0) return false; if (R < T1) T1 = R; }
		return true;
	};
	if (!Clip(-Dir.X, A.X + HX)) return false;
	if (!Clip(Dir.X, HX - A.X)) return false;
	if (!Clip(-Dir.Y, A.Y + HY)) return false;
	if (!Clip(Dir.Y, HY - A.Y)) return false;

	OutCut.EdgeIndex = EdgeIndex;
	OutCut.MinS = float(T0 * F.Len);
	OutCut.MaxS = float(T1 * F.Len);
	// 墙空间：Z 从这栋房自己的房底起算。
	OutCut.BottomZ = Bottom - Self.BaseZ;
	OutCut.TopZ = FMath::Min(Top - Self.BaseZ, Self.WallHeight);
	return OutCut.IsValid() && OutCut.MaxS - OutCut.MinS > MinSpan;
}

/**
 * 交点 → 砖路元素（**追加**写，返回本次追加的砖数）。
 *
 * 砖路就是墩那一种：只有一条竖直段（`EMidKind::None` + 只出左樘），从 `BottomZ` 砌到 `TopZ`。
 * 复用 `CSHouseFrame` 的好处不只是省代码 —— 容量、交接、剔除球、`SaveToStaticMesh` 出口、
 * 以及那条"材质勾没勾 `bUsedWithInstancedStaticMeshes`"的执行面判据全都跟着白拿。
 *
 * 世界框架的取法：路径是竖直的 ⇒ 切向恒 (0,1) ⇒ kernel 里的面内朝外法线 = 切向逆时针转 90°
 * = (−1, 0)，也就是 **−AxisU**。所以 `AxisU` 要取角平分的**反向**，砖的进深轴才朝着两房外面
 * 那个象限。写成 `AxisU = Outward` 的症状是砖整根朝里、进深轴插进房间（而位置完全正确）。
 */
inline int32 BuildCornerElements(const TArray<FCorner>& Corners, uint32 Seed,
	const CSHouseFrame::FBrickParams& Params, TArray<CSHouseFrame::FElement>& InOutElements)
{
	const float Length = FMath::Max(Params.Length, 1.0f);
	const int32 MaxBricks = FMath::Max(Params.MaxBricks, 0);

	// 全局砖序号跨"门框砖 + 接缝砖"连续（整栋房子一个 dispatch），所以起点要从已有元素接着数。
	int32 Cursor = 0;
	for (const CSHouseFrame::FElement& Existing : InOutElements) Cursor = FMath::Max(Cursor, Existing.BrickBegin + Existing.BrickCount);
	const int32 Before = Cursor;

	for (int32 Index = 0; Index < Corners.Num(); ++Index)
	{
		const FCorner& Corner = Corners[Index];
		const float Height = Corner.TopZ - Corner.BottomZ;
		if (Height < Length * 0.5f) continue;   // 半块砖都摆不下，同门框那条下限

		CSHouseFrame::FPath Path;
		Path.BaseZ = 0.0f;                      // 世界高度全部吃进 Frame.Origin，路自己从 0 起算
		Path.TopZ = Height;
		Path.LeftS = Path.RightS = Path.CenterS = 0.0f;
		Path.MidKind = CSHouseFrame::EMidKind::None;
		Path.bLeftJamb = true;

		float Scale = 0.0f;
		int32 Count = CSHouseFrame::SolveRun(Path.TotalLen(), Length, Params.Gap, Scale);
		if (Count <= 0 || Scale <= 0.0f) continue;
		// **只截断，绝不扩容**：容量是注册期一次付清的常量（同门框砖）。
		Count = FMath::Min(Count, MaxBricks - Cursor);
		if (Count <= 0) break;

		CSHouseFrame::FElement Element;
		Element.Path = Path;
		Element.Frame.Origin = FVector3f(float(Corner.Point.X), float(Corner.Point.Y), Corner.BottomZ);
		Element.Frame.AxisU = FVector3f(float(-Corner.Outward.X), float(-Corner.Outward.Y), 0.0f).GetSafeNormal();
		Element.Frame.AxisV = FVector3f(0.0f, 0.0f, 1.0f);
		Element.Frame.AxisN = FVector3f(float(-Corner.Outward.Y), float(Corner.Outward.X), 0.0f).GetSafeNormal();
		Element.BrickBegin = Cursor;
		Element.BrickCount = Count;
		Element.Pitch = (Length + FMath::Max(Params.Gap, 0.0f)) * Scale;
		Element.HalfLen = Length * Scale * 0.5f;
		Element.LayoutScale = Scale;
		// 逐实例随机数从**接缝身份 + 交点序号**派生，不从槽位派生：槽位是"这栋房自己的第几块砖"，
		// 两栋房必然不同，而这两份砖是重叠的（见文件头）。
		Element.RandomBase = Seed ^ (uint32(Index) * 2654435761u);
		InOutElements.Add(Element);
		Cursor += Count;
	}
	return Cursor - Before;
}
}
