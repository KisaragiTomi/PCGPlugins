#include "CSWall.h"

#include "CSGpuMeshTypes.h"
#include "CSWallMesh.h"       // FCSWallMeshWriter —— 房子与墙共用的三角形写手

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSWall_ 前缀。

/** 路径第 `Segment` 段的单位方向（水平）。零长段返回零向量 —— `BuildSkins` 先把它们拒掉了。 */
FVector2D CSWall_SegmentDir(const FCSWallPath& Path, int32 Segment)
{
	return (Path.Points[Path.NextIndex(Segment)] - Path.Points[Segment]).GetSafeNormal();
}

/** 中线弧长 S → (段号, 段内比例)。闭合路径先把 S 绕回 [0, Length)。 */
void CSWall_Locate(const FCSWallPath& Path, const FCSWallSkins& Skins, double S, int32& OutSegment, double& OutAlpha)
{
	const int32 NumSegments = Path.NumSegments();
	if (Path.bClosed && Skins.Length > UE_DOUBLE_SMALL_NUMBER)
	{
		S = FMath::Fmod(S, Skins.Length);
		if (S < 0.0) S += Skins.Length;
	}
	S = FMath::Clamp(S, 0.0, Skins.Length);
	// 二分：S[i] 是第 i 段的起点弧长；第 i 段的终点弧长是 S[i+1]，闭合的最后一段终点是 Length。
	int32 Lo = 0, Hi = NumSegments;
	while (Hi - Lo > 1)
	{
		const int32 Mid = (Lo + Hi) / 2;
		if (Skins.S[Mid] <= S) Lo = Mid; else Hi = Mid;
	}
	const double S0 = Skins.S[Lo];
	const double S1 = (Lo + 1 < Skins.S.Num()) ? Skins.S[Lo + 1] : Skins.Length;
	OutSegment = Lo;
	OutAlpha = S1 - S0 > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp((S - S0) / (S1 - S0), 0.0, 1.0) : 0.0;
}

/** 端点精确的线性插值：t = 0 / 1 时**逐位**取端点（`A + (B − A)·1` 不一定等于 B —— 相邻两段的接缝就差在这一位上）。 */
template <typename T>
T CSWall_LerpExact(const T& A, const T& B, double Alpha)
{
	if (Alpha <= 0.0) return A;
	if (Alpha >= 1.0) return B;
	return A + (B - A) * Alpha;
}

/** 面板规划器的出口：一块面板 = 段内 S 区间 [SA, SB]、下沿 Z0（离墙脚；> 0.5 时下面那截是实心窗台）、裁剪场、洞 Tag。 */
using FCSWall_PanelSink = TFunctionRef<void(float SA, float SB, float Z0, const FCSOpeningClipField& Field, uint8 Tag)>;

/**
 * 一条边上的面板序列 —— 房体原来那一整套（`CSHouse_BuildBodySoup` 的逐边前半）**逐字搬来**（2026-09-22「统一房子和墙」）：
 * 洞按格切面板、墩跨度整片裁、接缝段单独成板。按出面顺序逐块回调 `Emit`，顺序与搬之前一致（房子的三角汤逐位不变）。
 *
 * `MinPanelWidth` / `MinPanelHeight`：比这更窄 / 更矮的面板不出。`Faceted`（房子）是 0.5 / 0.5 —— 相邻棱柱各自封口，
 * 丢一块碎板不留缝；`Continuous` 没有段间端面，丢任何一块都是一道缝，所以只丢零宽的。
 */
void CSWall_PlanPanels(int32 Edge, float Len, float H, float PierWidth,
	TArrayView<const FCSWallOpening> AllOpenings, TArrayView<const FCSWallCut> AllCuts,
	float MinPanelWidth, float MinPanelHeight, FCSWall_PanelSink Emit)
{
	TArray<FCSWallOpening> Openings;
	for (const FCSWallOpening& O : AllOpenings) if (O.EdgeIndex == Edge) Openings.Add(O);
	Openings.Sort([](const FCSWallOpening& A, const FCSWallOpening& B) { return A.CenterS < B.CenterS; });

	// D7 接缝在这条边上要抹掉的段。**必须先排序再并掉重叠的**：一块面板只带一个裁剪场，
	// 两刀重叠时后一刀会静默丢失（症状是三栋房挤在一起时少裁一段墙，而砖照常立着）。
	// 并的是**并集包围**，宁可多裁一点也不留一片穿进邻居房间的墙。
	TArray<FCSWallCut> Cuts;
	for (const FCSWallCut& C : AllCuts) if (C.EdgeIndex == Edge && C.IsValid()) Cuts.Add(C);
	Cuts.Sort([](const FCSWallCut& A, const FCSWallCut& B) { return A.MinS < B.MinS; });
	for (int32 K = Cuts.Num() - 1; K > 0; --K)
	{
		FCSWallCut& Prev = Cuts[K - 1];
		if (Cuts[K].MinS > Prev.MaxS) continue;
		Prev.MaxS = FMath::Max(Prev.MaxS, Cuts[K].MaxS);
		Prev.TopZ = FMath::Max(Prev.TopZ, Cuts[K].TopZ);
		Prev.BottomZ = FMath::Min(Prev.BottomZ, Cuts[K].BottomZ);
		Cuts.RemoveAt(K);
	}

	// 一块面板：从 Z0 到墙顶（洞在其中被 clip 掉），Z0 以下那截是实心窗台（出面方式各自处理）。
	//
	// ⚠️ **任何情况下都不许靠"不生成面板"来开洞**（2026-08-30 裁决三，全项目架构不变量）：
	// 想让某一片墙消失，砌出实心盒再用裁剪场把它 discard 掉 —— 墩就是这么做的，见下面。
	auto AddPanel = [&](float SA, float SB, float Z0, const FCSOpeningClipField& Field, uint8 Tag)
	{
		// H − Z0 也要判：Z0 被参数推到墙顶以上时盒子会退化成反向挤出（面全朝里）。
		if (SB - SA < MinPanelWidth || SB <= SA || H - Z0 < MinPanelHeight) return;
		Emit(SA, SB, Z0, Field, Tag);
	};

	// 洞与洞之间那段实心墙。接缝把它再切成 [实心 | 接缝裁掉 | 实心 …]，因为**一块面板只带
	// 一个裁剪场** —— 想让一段墙消失就得让它单独成为一块面板（裁决三：绝不靠"不生成面板"开洞）。
	//
	// 墩跨度与接缝落在同一段时取**接缝**那个场：接缝整条 Z 都裁，是墩场（只裁起拱线以下）
	// 的超集，反过来取就会在接缝里留一截起拱线以上的墙。
	auto AddRun = [&](float SA, float SB, bool bPierSpan, float SpanTopZ)
	{
		auto RunField = [&](float A, float B) { return bPierSpan ? CSHouse_PierClipField(A, B, SpanTopZ) : FCSOpeningClipField(); };
		float At = SA;
		for (const FCSWallCut& C : Cuts)
		{
			const float C0 = FMath::Max(C.MinS, SA);
			const float C1 = FMath::Min(C.MaxS, SB);
			if (C1 - C0 < 0.5f) continue;
			AddPanel(At, C0, 0.0f, RunField(At, C0), 0);
			AddPanel(C0, C1, 0.0f, CSHouse_SeamClipField(C0, C1, C.BottomZ, C.TopZ), 0);
			At = C1;
		}
		AddPanel(At, SB, 0.0f, RunField(At, SB), 0);
	};

	float Cursor = 0;
	// 上一个**真砌出来了**的洞。跳过某个洞（装不下）就必须清空它：否则下一轮会拿隔了
	// 一个洞的两端去配墩，把中间那一整块墙当跨度抹掉。宁可多砌灰泥，不许少砌。
	const FCSWallOpening* Prev = nullptr;
	for (const FCSWallOpening& O : Openings)
	{
		if (!O.IsValid()) { Prev = nullptr; continue; }
		float CellMin = 0, CellMax = 0;
		CSHouse_OpeningCell(O, PierWidth, CellMin, CellMax);
		// 墩侧把格**收回到洞缘再多咬 CSHouse_PierCutMargin 的十分之一**：这一侧要抹掉的
		// 灰泥是格伸进跨度的那半个墩宽（端盖），不是格与格之间那块实心段 —— 默认参数下
		// 拱宽 = 段距 − 墩宽，两格首尾相接，那块段本来就是零宽，只跳过它等于什么都没做
		// （CSHouseProfile.h 记着这条）。
		// 多咬那 0.1 是为了把端盖推到拱判据的**洞内**一侧：正好收到洞缘时 |q.x| 是浮点的
		// 1.0，保不保由 `x * (1/x)` 的舍入决定 —— 一旦被保住，跨度两端就各立起一片贯穿
		// 墙厚的灰泥薄片。被它咬掉的那 1 mm 拱缘由墩跨度那块面板顶上（起拱线以上照常是灰泥）。
		const float PierBite = CSHouse_PierCutMargin * 0.1f;
		if (O.StyleFlags & CSHouse_StylePierBefore) CellMin = O.S0() + PierBite;
		if (O.StyleFlags & CSHouse_StylePierAfter) CellMax = O.S1() - PierBite;
		// 夹进这面墙、且不吃掉前一块面板 —— 洞挨得太近时宁可让墩变窄，也不让面板反向。
		CellMin = FMath::Clamp(CellMin, Cursor, Len);
		CellMax = FMath::Clamp(CellMax, CellMin, Len);
		// 1 cm 余量：墩侧的格是 S1() − S0() 再各让一点，浮点上不会逐位等于 Width，
		// 而真正的"装不下"是厘米量级的事。
		// 要求按**洞落在这面墙里的那一截**算，不是名义 `O.Width`：端点可能被量化推到墙外
		// （`ComputeDoors` 已经夹过一次，这里是第二道闸），墙外那一截任何面板都盖不到，
		// 拿名义宽去比就会把整个洞判成"装不下" ⇒ 砖拱砌好了、墙没挖洞，一声不吭。
		const float VisibleWidth = FMath::Min(O.S1(), Len) - FMath::Max(O.S0(), 0.0f);
		if (CellMax - CellMin < VisibleWidth - 1.0f) continue;   // 装不下这个洞的面板，这一洞放弃

		// 洞之间的实心段。判为墩的跨度这一块**照样砌成实心盒**，只是起拱线以下整片交给
		// 裁剪场在像素阶段裁掉（裁决三：避免所有真几何洞）—— 观感上起拱线以下就没有"墙"
		// 这个表面了，只剩两侧门樘砖自己站着，正是实拍 Docs/TinyGlade/img/TG_continuous_arches.png
		// 里"墙没了、只剩墩"的那一截。
		float SpanZ0 = 0.0f, SpanWidth = 0.0f;
		const bool bPierSpan = Prev != nullptr
			&& (Prev->StyleFlags & CSHouse_StylePierAfter) != 0
			&& (O.StyleFlags & CSHouse_StylePierBefore) != 0
			&& CSHouse_PierSpanBetween(*Prev, O, SpanWidth, SpanZ0);
		AddRun(Cursor, CellMin, bPierSpan, SpanZ0);
		// **洞面板不吃接缝裁剪**（裁决二明写的退出范围："接缝接受 openings" 不做）：
		// 它自带的裁剪场是洞形，一块面板容不下第二个场，而"两个场怎么合"正是被划出去的那件事。
		// 落进接缝里的门窗因此还是完整的门窗 —— 观感上不对，但它是**声明过的**不对。
		AddPanel(CellMin, CellMax, O.Z0, CSHouse_ComputeClipField(O), O.Tag);
		Cursor = CellMax;
		Prev = &O;
	}
	AddRun(Cursor, Len, false, 0.0f);

	// 洞口的厚度**不产扫掠面**（用户裁决）：断口由门框砖块填满 —— 与 TG 同构，
	// 它的拱/楣也是与墙砖并列的真实构件（flags&32：按拱高压扁贴合曲线 + 免拱裁剪），
	// 而不是一圈扫掠出来的内壁。门框沿 CSHouse_ComputeClipField 那条解析洞缘铺砖
	// （`BuildFrameArches` -> `CSHouseFrame::BuildEdgeElements`），与 clip 判据同源。
}
}

namespace CSWall
{
bool BuildSkins(const FCSWallPath& Path, float Thickness, FCSWallSkins& Out)
{
	Out = FCSWallSkins();
	if (!Path.IsValid() || Thickness <= 0.0f) return false;

	const int32 N = Path.NumPoints();
	const int32 NumSegments = Path.NumSegments();
	TArray<FVector2D, TInlineAllocator<64>> Dir;
	Dir.SetNumUninitialized(NumSegments);
	for (int32 Segment = 0; Segment < NumSegments; ++Segment)
	{
		// 零长段没有方向，斜接无从谈起。调用方（样条采样）负责先去重。
		if (Path.SegmentLength(Segment) < 1.0e-3) return false;
		Dir[Segment] = CSWall_SegmentDir(Path, Segment);
	}

	Out.Thickness = Thickness;
	Out.Left.SetNumUninitialized(N);
	Out.Right.SetNumUninitialized(N);
	Out.Normal.SetNumUninitialized(N);
	Out.TurnSin.SetNumUninitialized(N);
	Out.HalfTurnCos.SetNumUninitialized(N);
	Out.S.SetNumUninitialized(N);

	Out.S[0] = 0.0;
	for (int32 Index = 1; Index < N; ++Index) Out.S[Index] = Out.S[Index - 1] + Path.SegmentLength(Index - 1);
	Out.Length = Out.S[N - 1] + (Path.bClosed ? Path.SegmentLength(N - 1) : 0.0);

	const double Half = double(Thickness) * 0.5;
	for (int32 Index = 0; Index < N; ++Index)
	{
		// 开口路径的两个端点只有一段：进出方向取同一段 ⇒ 平分线就是那一段的法线、转角为零。
		const bool bHasIn = Path.bClosed || Index > 0;
		const bool bHasOut = Path.bClosed || Index < N - 1;
		const FVector2D DOut = bHasOut ? Dir[Index % NumSegments] : Dir[(Index - 1 + NumSegments) % NumSegments];
		const FVector2D DIn = bHasIn ? Dir[(Index - 1 + NumSegments) % NumSegments] : DOut;

		// 右法线 = (D.y, −D.x)（行进方向右手）。平分线 = 两段右法线之和的方向，
		// |和| = 2·cos(转角/2) —— 与 `CSHouse_GetCorner` 的 `HalfTurnCos` 同一个恒等式。
		const FVector2D RightIn(DIn.Y, -DIn.X);
		const FVector2D RightOut(DOut.Y, -DOut.X);
		const FVector2D Sum = RightIn + RightOut;
		const double SumLen = Sum.Size();
		FVector2D Bisector = RightOut;
		double HalfCos = 0.0;
		if (SumLen > 1.0e-6)
		{
			Bisector = Sum / SumLen;
			HalfCos = SumLen * 0.5;
		}
		// 皮线点 = 中线点沿平分线偏 `Half / cos(转角/2)`（与 `PointAtDepth` 同一个式子），尖角处封顶。
		const double Mitre = Half / FMath::Max(HalfCos, 1.0 / MaxMitre);

		Out.Right[Index] = Path.Points[Index] + Bisector * Mitre;
		Out.Left[Index] = Path.Points[Index] - Bisector * Mitre;
		Out.Normal[Index] = Bisector;
		Out.TurnSin[Index] = (bHasIn && bHasOut) ? DIn.X * DOut.Y - DIn.Y * DOut.X : 0.0;
		Out.HalfTurnCos[Index] = (bHasIn && bHasOut) ? HalfCos : 1.0;
	}
	return true;
}

bool BuildEnclosureRing(const FCSHouseFootprint& Outer, float Thickness, double BaseZ, double TopZ,
	FCSWallPath& OutPath, FCSWallSkins& OutSkins)
{
	OutPath = FCSWallPath();
	OutSkins = FCSWallSkins();
	const int32 N = Outer.NumEdges();
	if (N < 3) return false;

	OutPath.bClosed = true;
	OutPath.Points.Reserve(N);
	OutSkins.Thickness = Thickness;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		// 顶点 i 上的角是 (i − 1) 号角（房子的口径：k 号角夹在 k 号边远端与 k+1 号边近端之间，位置 = Verts[k+1]）。
		const FCSHouseCornerFrame C = CSHouse_GetCorner((Vertex + N - 1) % N, Outer);
		OutPath.Points.Add(C.PointAtDepth(Thickness * 0.5));
		OutPath.BaseZ.Add(BaseZ);
		OutPath.TopZ.Add(TopZ);
		OutPath.Kinks.Add(1);
		OutSkins.Right.Add(C.Point);                    // == Outer.Verts[Vertex]，逐位
		OutSkins.Left.Add(C.PointAtDepth(Thickness));   // 房底 / 内皮与房子同一个真源
		OutSkins.Normal.Add(C.Outward);
		OutSkins.TurnSin.Add(C.SinTurn);
		OutSkins.HalfTurnCos.Add(C.HalfTurnCos);
	}
	OutSkins.S.SetNumUninitialized(N);
	OutSkins.S[0] = 0.0;
	for (int32 Vertex = 1; Vertex < N; ++Vertex) OutSkins.S[Vertex] = OutSkins.S[Vertex - 1] + OutPath.SegmentLength(Vertex - 1);
	OutSkins.Length = OutSkins.S[N - 1] + OutPath.SegmentLength(N - 1);
	return true;
}

FCSHouseEdgeFrame OuterEdgeFrame(const FCSWallPath& Path, const FCSWallSkins& Skins, int32 Segment)
{
	const int32 N = Skins.Right.Num();
	if (N < 2 || Segment < 0 || Segment >= Path.NumSegments()) return FCSHouseEdgeFrame();
	const FVector2D& A = Skins.Right[Segment];
	const FVector2D& B = Skins.Right[(Segment + 1) % N];
	if (Path.bClosed)
	{
		// 与 `CSHouse_GetEdge` 同一组输入（房子的环：右皮就是 footprint 顶点）⇒ 逐位相同。
		return CSHouse_MakeEdgeFrame(Skins.Right[(Segment + N - 1) % N], A, B, Skins.Right[(Segment + 2) % N], Skins.Thickness);
	}
	// 开口路径：墙端是方头。首尾两段缺的那个邻点按直线外推，再把墙端那一头的让出量钉成 0（外推点的舍入别漏进来）。
	const bool bFirst = Segment == 0;
	const bool bLast = Segment == N - 2;
	const FVector2D Prev = bFirst ? A - (B - A) : Skins.Right[Segment - 1];
	const FVector2D Next = bLast ? B + (B - A) : Skins.Right[Segment + 2];
	FCSHouseEdgeFrame F = CSHouse_MakeEdgeFrame(Prev, A, B, Next, Skins.Thickness);
	if (bFirst) F.InsetStart = 0.0f;
	if (bLast) F.InsetEnd = 0.0f;
	return F;
}

void EvalCenter(const FCSWallPath& Path, const FCSWallSkins& Skins, double S,
	FVector2D& OutXY, double& OutTopZ, FVector2D& OutTangent)
{
	int32 Segment = 0;
	double Alpha = 0.0;
	CSWall_Locate(Path, Skins, S, Segment, Alpha);
	const int32 Next = Path.NextIndex(Segment);
	OutXY = FMath::Lerp(Path.Points[Segment], Path.Points[Next], Alpha);
	OutTopZ = FMath::Lerp(Path.TopZ[Segment], Path.TopZ[Next], Alpha);
	OutTangent = CSWall_SegmentDir(Path, Segment);
}

int32 BuildBody(const FCSWallPath& Path, const FCSWallSkins& Skins, const FBodyParams& Params, FCSGpuMeshCPUData& Out)
{
	const int32 N = Path.NumPoints();
	if (!Path.IsValid() || !Skins.IsValid() || Skins.Left.Num() != N || Skins.S.Num() != N) return 0;

	const int32 TrianglesBefore = Out.Indices.Num() / 3;
	FCSWallMeshWriter Writer{ Out, Params.World };
	const float T = Skins.Thickness;
	// 材质槽：0 墙面（Masked，按 UV1 逐像素切洞）。
	constexpr int32 SlotWall = 0;

	if (Params.Surface == ECSWallSurface::Faceted)
	{
		// ---- 逐面板实心斜接棱柱（房子一贯的画面，逐字搬自 `CSHouse_BuildBodySoup`）----
		//
		// 洞不在几何里，由材质按 UV1 的解析判据逐像素 discard 切出 —— Tiny Glade 原版的开洞方式：CPU 只提供解析参数，
		// 洞形在像素阶段成立。洞缘因此是解析精确曲线（无限分辨率），而不是受弦高容差限制的折线。代价是 discard 只丢像素、
		// 不生成表面 —— 洞缘的厚度断口由门框砖块填满。
		for (int32 Edge = 0; Edge < Path.NumSegments(); ++Edge)
		{
			const FCSHouseEdgeFrame F = OuterEdgeFrame(Path, Skins, Edge);
			const FVector U(F.U.X, F.U.Y, 0), In(F.In.X, F.In.Y, 0), Up(0, 0, 1);
			// 面板原点落在墙脚上：房子的局部墙脚是 0（与搬之前逐位相同），写手的 UV1 按离墙脚的高度取。
			const FVector Start(F.Start.X, F.Start.Y, Path.BaseZ[Edge]);
			const float H = float(Path.TopZ[Edge] - Path.BaseZ[Edge]);

			// 斜接：面板贴着转角的那一端，内皮要沿角平分线让出 `Inset`（凹角为负，内皮反而伸出去）。
			// 只有真正**顶到墙端**的面板才吃这个量；从墙中间起止的面板两端都是直的。被接缝切在斜接区
			// 里面的那种少见情况取 max / min，保证内皮区间不越过斜接面、不与邻边的面板重叠。
			auto InnerSpan = [&F](float SA, float SB, float& OutSAin, float& OutSBin)
			{
				OutSAin = (SA <= 0.5f) ? F.InsetStart : FMath::Max(SA, F.InsetStart);
				OutSBin = (SB >= F.Len - 0.5f) ? (F.Len - F.InsetEnd) : FMath::Min(SB, F.Len - F.InsetEnd);
				// 整块面板都落在斜接区里时内皮会反向：压成零长，棱台退化成三棱柱，面不翻。
				OutSBin = FMath::Max(OutSBin, OutSAin);
			};

			CSWall_PlanPanels(Edge, F.Len, H, Params.PierWidth, Params.Openings, Params.SeamCuts, 0.5f, 0.5f,
				[&](float SA, float SB, float Z0, const FCSOpeningClipField& Field, uint8 Tag)
				{
					float SAin = SA, SBin = SB;
					InnerSpan(SA, SB, SAin, SBin);
					Writer.SetPanel(Start, U, Field, CSWallMesh::PartWall, Tag);
					Writer.AddWallPrism(Start, U, In, Up, SA, SB, SAin, SBin, T, Z0, H, SlotWall);
					if (Z0 > 0.5f)
					{
						// 窗台：真几何而不是 clip 的下界 —— 判据因此只需两个 float（见 CSHouseProfile.h）。
						Writer.SetPanel(Start, U, FCSOpeningClipField(), CSWallMesh::PartWall, 0);
						Writer.AddWallPrism(Start, U, In, Up, SA, SB, SAin, SBin, T, 0.0f, Z0, SlotWall);
					}
				});
		}
	}
	else
	{
		// ---- 连续的墙（样条墙）：侧面沿中线弧长连续 UV、墙脚 / 墙顶逐点、平缓处平滑法线 ----
		//
		// 三角形 A、B、C 按**外法线**的右手序给（cross(B−A, C−A) 朝外），写手按引擎绕序交换 1/2 写。
		// 侧面逐顶点法线：不是拐角 ⇒ 平分线（两段共用，弯墙无棱）；拐角 ⇒ 这一段的面法线（棱是硬的）。
		auto UV = [](double U, double V) { return FVector2f(float(U / UVScale), float(V / UVScale)); };
		auto FaceNormal = [](const FVector& A, const FVector& B, const FVector& C)
		{
			return FVector::CrossProduct(B - A, C - A).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		};
		const FVector2f Sentinel = FCSOpeningClipField().Eval(0.0f, 0.0f);
		// 平面三角（墙顶 / 墙底 / 端面）：面法线；UV1 由调用方给（洞面板的顶 / 底也要按裁剪场判，门洞底下那条墙底才会一起被切掉）。
		auto AddFlatTri = [&](const FVector& A, const FVector& B, const FVector& C,
			const FVector2f& UVA, const FVector2f& UVB, const FVector2f& UVC,
			const FVector2f& ClipA, const FVector2f& ClipB, const FVector2f& ClipC, const FVector& Tangent)
		{
			const FVector Nrm = FaceNormal(A, B, C);
			Writer.AddTriEx(A, B, C, Nrm, Nrm, Nrm, SlotWall, UVA, UVB, UVC, ClipA, ClipB, ClipC, Tangent);
		};

		for (int32 Segment = 0; Segment < Path.NumSegments(); ++Segment)
		{
			const int32 I = Segment;
			const int32 J = Path.NextIndex(Segment);
			const double S0 = Skins.S[I];
			// 闭合路径的最后一段回到首点：终点弧长取全长，不取 S[0] = 0 —— 否则这一段 UV 倒着走一整圈。
			const double S1 = (J == 0) ? Skins.Length : Skins.S[J];
			const FVector2D D = CSWall_SegmentDir(Path, Segment);
			const FVector Along(D.X, D.Y, 0.0);
			const FVector FaceRight(D.Y, -D.X, 0.0);
			const float Len = float(Path.SegmentLength(Segment));
			const float MinHeight = float(FMath::Min(Path.TopZ[I] - Path.BaseZ[I], Path.TopZ[J] - Path.BaseZ[J]));

			auto SideNormal = [&](int32 Vertex, double Sign) -> FVector
			{
				if (!IsCorner(Skins, Vertex, Params.CornerTurnDegrees))
				{
					const FVector2D Nrm = Skins.Normal[Vertex] * Sign;
					return FVector(Nrm.X, Nrm.Y, 0.0);
				}
				return FaceRight * Sign;
			};
			const FVector RightNI = SideNormal(I, 1.0), RightNJ = SideNormal(J, 1.0);
			const FVector LeftNI = SideNormal(I, -1.0), LeftNJ = SideNormal(J, -1.0);

			// 段内一个截面：t ∈ [0, 1]（段内弧长 / 段长）。t = 0 / 1 逐位取端点 —— 相邻两段靠它严丝合缝（没有段间端面兜底）。
			struct FSection
			{
				FVector2D R, L;
				double Base = 0.0, Top = 0.0, S = 0.0;
				FVector NR, NL;
			};
			auto Section = [&](double Alpha)
			{
				FSection X;
				X.R = CSWall_LerpExact(Skins.Right[I], Skins.Right[J], Alpha);
				X.L = CSWall_LerpExact(Skins.Left[I], Skins.Left[J], Alpha);
				X.Base = CSWall_LerpExact(Path.BaseZ[I], Path.BaseZ[J], Alpha);
				X.Top = CSWall_LerpExact(Path.TopZ[I], Path.TopZ[J], Alpha);
				X.S = CSWall_LerpExact(S0, S1, Alpha);
				X.NR = Alpha <= 0.0 ? RightNI : (Alpha >= 1.0 ? RightNJ : FMath::Lerp(RightNI, RightNJ, Alpha).GetSafeNormal());
				X.NL = Alpha <= 0.0 ? LeftNI : (Alpha >= 1.0 ? LeftNJ : FMath::Lerp(LeftNI, LeftNJ, Alpha).GetSafeNormal());
				return X;
			};

			CSWall_PlanPanels(Segment, Len, MinHeight, Params.PierWidth, Params.Openings, Params.SeamCuts, 0.0f, 0.0f,
				[&](float SA, float SB, float Z0, const FCSOpeningClipField& Field, uint8 Tag)
				{
					const double TA = Len > 0.0f ? FMath::Clamp(double(SA) / double(Len), 0.0, 1.0) : 0.0;
					const double TB = Len > 0.0f ? FMath::Clamp(double(SB) / double(Len), 0.0, 1.0) : 1.0;
					const FSection A = Section(TA), B = Section(TB);

					// 一层：侧面 [墙脚 + ZLo, 墙脚 + ZHi]（ZHi < 0 = 到墙顶），顶面 / 底面按需。UV1 = 这块面板的裁剪场在 (段内 S, 离墙脚高度) 处的值。
					auto Layer = [&](double ZLo, double ZHi, const FCSOpeningClipField& LayerField, uint8 LayerTag, bool bTopFace, bool bBottom)
					{
						Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, LayerField, CSWallMesh::PartWall, LayerTag);
						const double LoA = A.Base + ZLo, LoB = B.Base + ZLo;
						const double HiA = ZHi < 0.0 ? A.Top : A.Base + ZHi;
						const double HiB = ZHi < 0.0 ? B.Top : B.Base + ZHi;
						const double VLoA = LoA - A.Base, VLoB = LoB - B.Base, VHiA = HiA - A.Base, VHiB = HiB - B.Base;
						const FVector2f ClipLoA = LayerField.Eval(SA, float(VLoA)), ClipLoB = LayerField.Eval(SB, float(VLoB));
						const FVector2f ClipHiA = LayerField.Eval(SA, float(VHiA)), ClipHiB = LayerField.Eval(SB, float(VHiB));

						const FVector RbA(A.R.X, A.R.Y, LoA), RbB(B.R.X, B.R.Y, LoB), RtA(A.R.X, A.R.Y, HiA), RtB(B.R.X, B.R.Y, HiB);
						const FVector LbA(A.L.X, A.L.Y, LoA), LbB(B.L.X, B.L.Y, LoB), LtA(A.L.X, A.L.Y, HiA), LtB(B.L.X, B.L.Y, HiB);

						// 右侧面（外法线朝右）。
						Writer.AddTriEx(RbA, RbB, RtB, A.NR, B.NR, B.NR, SlotWall, UV(A.S, VLoA), UV(B.S, VLoB), UV(B.S, VHiB), ClipLoA, ClipLoB, ClipHiB, Along);
						Writer.AddTriEx(RbA, RtB, RtA, A.NR, B.NR, A.NR, SlotWall, UV(A.S, VLoA), UV(B.S, VHiB), UV(A.S, VHiA), ClipLoA, ClipHiB, ClipHiA, Along);
						// 左侧面（外法线朝左）。UV 的 U 同样取中线弧长 —— 贴图在背面镜像，灰泥各向同性，看不出来。
						Writer.AddTriEx(LbB, LbA, LtA, B.NL, A.NL, A.NL, SlotWall, UV(B.S, VLoB), UV(A.S, VLoA), UV(A.S, VHiA), ClipLoB, ClipLoA, ClipHiA, -Along);
						Writer.AddTriEx(LbB, LtA, LtB, B.NL, A.NL, B.NL, SlotWall, UV(B.S, VLoB), UV(A.S, VHiA), UV(B.S, VHiB), ClipLoB, ClipHiA, ClipHiB, -Along);
						// 墙顶（朝上）。
						if (bTopFace)
						{
							AddFlatTri(RtA, RtB, LtB, UV(A.S, 0.0), UV(B.S, 0.0), UV(B.S, T), ClipHiA, ClipHiB, ClipHiB, Along);
							AddFlatTri(RtA, LtB, LtA, UV(A.S, 0.0), UV(B.S, T), UV(A.S, T), ClipHiA, ClipHiB, ClipHiA, Along);
						}
						// 墙底（朝下）。
						if (bBottom)
						{
							AddFlatTri(RbA, LbB, RbB, UV(A.S, 0.0), UV(B.S, T), UV(B.S, 0.0), ClipLoA, ClipLoB, ClipLoB, Along);
							AddFlatTri(RbA, LbA, LbB, UV(A.S, 0.0), UV(A.S, T), UV(B.S, T), ClipLoA, ClipLoA, ClipLoB, Along);
						}
					};

					if (Z0 > 0.5f)
					{
						// 窗台：下面那截是实心墙（没有洞），洞面板从窗台高起。两层之间不出面（连续的墙没有内部面）。
						Layer(double(Z0), -1.0, Field, Tag, true, false);
						Layer(0.0, double(Z0), FCSOpeningClipField(), 0, false, Params.bBottomFace);
					}
					else
					{
						Layer(0.0, -1.0, Field, Tag, true, Params.bBottomFace);
					}
				});
		}

		// 开口路径的两个端面（只有这两个 —— 段与段之间没有端面）。
		if (!Path.bClosed)
		{
			Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(), CSWallMesh::PartWall, 0);
			auto Bottom = [&](const FVector2D& XY, int32 Index) { return FVector(XY.X, XY.Y, Path.BaseZ[Index]); };
			auto Top = [&](const FVector2D& XY, int32 Index) { return FVector(XY.X, XY.Y, Path.TopZ[Index]); };
			const int32 Last = N - 1;
			const FVector2D D0 = CSWall_SegmentDir(Path, 0);
			const FVector2D DN = CSWall_SegmentDir(Path, Last - 1);
			{
				const FVector Lb = Bottom(Skins.Left[0], 0), Rb = Bottom(Skins.Right[0], 0);
				const FVector Lt = Top(Skins.Left[0], 0), Rt = Top(Skins.Right[0], 0);
				const double H = Path.TopZ[0] - Path.BaseZ[0];
				const FVector Across(-D0.Y, D0.X, 0.0);
				// 外法线 = −D0：(Lb, Rb, Rt)、(Lb, Rt, Lt)。
				AddFlatTri(Lb, Rb, Rt, UV(0.0, 0.0), UV(T, 0.0), UV(T, H), Sentinel, Sentinel, Sentinel, -Across);
				AddFlatTri(Lb, Rt, Lt, UV(0.0, 0.0), UV(T, H), UV(0.0, H), Sentinel, Sentinel, Sentinel, -Across);
			}
			{
				const FVector Lb = Bottom(Skins.Left[Last], Last), Rb = Bottom(Skins.Right[Last], Last);
				const FVector Lt = Top(Skins.Left[Last], Last), Rt = Top(Skins.Right[Last], Last);
				const double H = Path.TopZ[Last] - Path.BaseZ[Last];
				const FVector Across(-DN.Y, DN.X, 0.0);
				// 外法线 = +DN：(Rb, Lb, Lt)、(Rb, Lt, Rt)。
				AddFlatTri(Rb, Lb, Lt, UV(0.0, 0.0), UV(T, 0.0), UV(T, H), Sentinel, Sentinel, Sentinel, Across);
				AddFlatTri(Rb, Lt, Rt, UV(0.0, 0.0), UV(T, H), UV(0.0, H), Sentinel, Sentinel, Sentinel, Across);
			}
		}
	}

	// ---- 房底（2026-09-17 用户要求「房子产生底面」）----
	//
	// 只铺**墙内皮**围出的那块：墙板自己的底已由棱柱封住，外皮到内皮这一圈再铺一层会与之共面打架。
	// 内皮角点 = 外角沿角平分线内缩到墙厚 T（`PointAtDepth`，与斜接墙板同一个真源），footprint 取凸包 ⇒ 内皮也凸，扇形即可。
	// **朝下的单面**：墙材质不双面，从屋里往下看是背面、被剔除 —— 平地上房底正好压在地形上，双面就会透过门洞闪烁。
	// 不带裁剪场（UV1 哨兵，恒保留），语义色按墙面，UV0 按局部 XY 平铺（与墙面同一个周期）。
	// 扇形从 1 号顶点起（= 房子的 0 号角）：与搬之前的三角划分逐位相同。
	if (Params.bInteriorFloor && Path.bClosed)
	{
		TArray<FVector> Inner;
		Inner.Reserve(N);
		for (int32 Corner = 0; Corner < N; ++Corner)
		{
			const int32 Vertex = (Corner + 1) % N;
			Inner.Add(FVector(Skins.Left[Vertex].X, Skins.Left[Vertex].Y, Path.BaseZ[Vertex]));
		}
		// 墙厚大于房子一半时内皮会翻过来（有向面积 <= 0）：那时屋里本来就没有空腔，不铺。顺时针的环同理（内皮在右手）。
		double TwiceArea = 0.0;
		for (int32 K = 0; K < Inner.Num(); ++K)
		{
			const FVector& A = Inner[K];
			const FVector& B = Inner[(K + 1) % Inner.Num()];
			TwiceArea += A.X * B.Y - B.X * A.Y;
		}
		if (Inner.Num() >= 3 && TwiceArea > 1.0)
		{
			Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(), CSWallMesh::PartWall, 0);
			auto FloorUV = [](const FVector& P) { return FVector2f(float(P.X) / CSWallMesh::UVScale, float(P.Y) / CSWallMesh::UVScale); };
			for (int32 K = 1; K + 1 < Inner.Num(); ++K)
			{
				// 内皮环是逆时针（俯视）：按 0 → K+1 → K 反着连，面法线 cross(B−A, C−A) 才朝 −Z。
				const FVector& A = Inner[0];
				const FVector& B = Inner[K + 1];
				const FVector& C = Inner[K];
				Writer.AddTri(A, B, C, SlotWall, FloorUV(A), FloorUV(B), FloorUV(C));
			}
		}
	}

	Out.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	Out.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	Out.NumTexCoordChannels = 2;   // UV0 贴图 / UV1 解析裁剪场
	return Out.Indices.Num() / 3 - TrianglesBefore;
}

FTopResult BuildTopElements(const FCSWallPath& Path, const FCSWallSkins& Skins,
	const FTopParams& Params, const CSHouseFrame::FBrickParams& BrickParams, TArray<CSHouseFrame::FElement>& InOutElements)
{
	FTopResult Result;
	if (Params.IsPlain() || !Path.IsValid() || !Skins.IsValid() || Skins.Length <= 1.0) return Result;

	const double Nominal = FMath::Max(double(BrickParams.Length), 1.0);
	const int32 MerlonEvery = FMath::Max(Params.MerlonEvery, 1);
	// 断点：拐角（`IsCorner`）。压顶砖按弦铺，跨过拐角的那块会把角切掉一块 —— 断开就不会。
	TArray<double> Cuts;
	for (int32 Index = 0; Index < Path.NumPoints(); ++Index)
	{
		if (IsCorner(Skins, Index, Params.CornerTurnDegrees)) Cuts.Add(Skins.S[Index]);
	}

	// 段：开口 = [0, 断点..., 全长]；闭合无断点 = 整圈一段（首尾相接）；闭合有断点 = 断点之间绕一圈。
	struct FRun { double A = 0.0; double B = 0.0; bool bLoop = false; };
	TArray<FRun, TInlineAllocator<8>> Runs;
	if (!Path.bClosed)
	{
		double Prev = 0.0;
		for (double Cut : Cuts)
		{
			if (Cut - Prev > UE_KINDA_SMALL_NUMBER) Runs.Add({ Prev, Cut, false });
			Prev = Cut;
		}
		Runs.Add({ Prev, Skins.Length, false });
	}
	else if (Cuts.IsEmpty())
	{
		Runs.Add({ 0.0, Skins.Length, true });
	}
	else
	{
		for (int32 K = 0; K < Cuts.Num(); ++K)
		{
			const double A = Cuts[K];
			const double B = (K + 1 < Cuts.Num()) ? Cuts[K + 1] : Cuts[0] + Skins.Length;
			Runs.Add({ A, B, false });
		}
	}

	int32 Cursor = CSHouseFrame::NextBrickSlot(InOutElements);
	for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
	{
		const FRun& Run = Runs[RunIndex];
		const double L = Run.B - Run.A;
		if (L < Nominal * 0.5) continue;
		++Result.Runs;

		int32 Count = FMath::Max(1, FMath::RoundToInt32(L / Nominal));
		// 垛口隔一块抬一块时的奇偶：开口段取奇数 ⇒ 首尾两块都是垛（"收了头"）；整圈取偶数 ⇒ 接缝处
		// 不出两个相邻的垛。改块数时往"砖长更接近标称"的那一边改。
		if (Params.bMerlons && MerlonEvery == 2)
		{
			const bool bWantOdd = !Run.bLoop;
			if (((Count & 1) == 1) != bWantOdd)
			{
				const int32 Up = Count + 1;
				const int32 Down = Count - 1;
				const double ErrUp = FMath::Abs(L / Up / Nominal - 1.0);
				const double ErrDown = Down >= 1 ? FMath::Abs(L / Down / Nominal - 1.0) : TNumericLimits<double>::Max();
				Count = ErrDown <= ErrUp ? Down : Up;
			}
		}
		// 每个槽必须恰好一块砖：`SolveRun` 在弦长超过标称 √2 倍时会拆成两块（贪心 + 对数比较），
		// 那样垛口的"隔一块"就数错了。短段取 1 块时最容易撞上（40 cm 的段、30 cm 的砖）。
		const int32 Step = (Params.bMerlons && MerlonEvery == 2) ? 2 : 1;
		while (L / Count / Nominal >= 1.4) Count += Step;
		const double Len = L / Count;

		for (int32 K = 0; K < Count; ++K)
		{
			FVector2D XY0, XY1, Tangent;
			double Z0 = 0.0, Z1 = 0.0;
			EvalCenter(Path, Skins, Run.A + Len * K, XY0, Z0, Tangent);
			EvalCenter(Path, Skins, Run.A + Len * (K + 1), XY1, Z1, Tangent);
			const FVector P0(XY0.X, XY0.Y, Z0);
			FVector Axis = FVector(XY1.X, XY1.Y, Z1) - P0;
			const double Chord = Axis.Size();
			if (Chord < 1.0) continue;
			Axis /= Chord;

			// 框架：原点 = 这块砖起点处的墙顶中线点，U = 弦向（含坡度，砖贴着墙顶的起伏），V 竖直。
			CSHouseFrame::FWallFrame Frame;
			Frame.Origin = FVector3f(P0);
			Frame.AxisU = FVector3f(Axis);
			Frame.AxisV = FVector3f(0.0f, 0.0f, 1.0f);
			Frame.AxisN = FVector3f(FVector(Axis.Y, -Axis.X, 0.0).GetSafeNormal());

			// 身份 = 段号 × 块号：拐角增减只影响那一段，别的段的随机花样不动。
			const int32 Identity = RunIndex * 65536 + K;
			if (Params.bCoping)
			{
				Result.CopingBricks += CSHouseFrame::AppendFlatRun(Frame, 0.0f, float(Chord),
					Params.CourseHeight * 0.5f - Params.Sink,
					CSHouseFrame::PathRandomBase(Params.Seed, CSHouseFrame::EPathFamily::WallCoping, Identity),
					BrickParams, InOutElements, Cursor);
			}
			if (Params.bMerlons && (K % MerlonEvery) == 0)
			{
				// 没有压顶层时垛口直接坐在墙顶上。
				const float Level = Params.bCoping ? 1.5f : 0.5f;
				Result.MerlonBricks += CSHouseFrame::AppendFlatRun(Frame, 0.0f, float(Chord),
					Params.CourseHeight * Level - Params.Sink,
					CSHouseFrame::PathRandomBase(Params.Seed, CSHouseFrame::EPathFamily::WallMerlon, Identity),
					BrickParams, InOutElements, Cursor);
			}
		}
	}
	return Result;
}

int32 BuildQuoins(const FCSWallPath& Path, const FCSWallSkins& Skins, const FQuoinParams& Params, TArray<CSHouseQuoin::FQuoin>& Out)
{
	const int32 N = Path.NumPoints();
	if (!Path.IsValid() || !Skins.IsValid() || Skins.Left.Num() != N) return 0;
	const int32 Before = Out.Num();

	// `Outward` 必须是单位向量（调用处保证）：外棱沿它内缩 `Inset`。
	auto Add = [&](const FVector2D& Point, const FVector2D& Outward, int32 Vertex, int32 Identity, double HalfTurnCos)
	{
		const FVector2D Local = Point - Outward * double(Params.Inset);
		const FVector WorldPoint = Params.World.TransformPosition(FVector(Local.X, Local.Y, 0.0));
		const FVector WorldOut = Params.World.TransformVectorNoScale(FVector(Outward.X, Outward.Y, 0.0));
		CSHouseQuoin::FQuoin Q;
		Q.Point = FVector2D(WorldPoint.X, WorldPoint.Y);
		Q.Outward = FVector2D(WorldOut.X, WorldOut.Y).GetSafeNormal();
		Q.BottomZ = Params.ZOffset + float(Path.BaseZ[Vertex]);
		Q.TopZ = Params.ZOffset + float(Path.TopZ[Vertex]);
		Q.CornerIndex = Identity;
		Q.HalfTurnCos = float(HalfTurnCos);
		Out.Add(Q);
	};

	if (Params.Kind == ECSWallKind::Enclosure)
	{
		// 房子（`CSHouseQuoin::BuildQuoins` 原来那一份的规矩，逐位不变）。
		if (!Params.bCorners || !Path.bClosed) return 0;
		// ⚠️ **退化的环直接不出**：任一边的外皮放不下两头的斜接让出量时，相邻两角互相吃掉，
		// 柱心会跑到房子外面去（矩形上就是「任一边短于两个墙厚」）。
		const float T = FMath::Max(Skins.Thickness, 0.0f);
		for (int32 Edge = 0; Edge < N; ++Edge)
		{
			const FCSHouseEdgeFrame F = CSHouse_MakeEdgeFrame(Skins.Right[(Edge + N - 1) % N], Skins.Right[Edge],
				Skins.Right[(Edge + 1) % N], Skins.Right[(Edge + 2) % N], T);
			if (F.Len <= 0.0f || F.Len < FMath::Max(F.InsetStart, 0.0f) + FMath::Max(F.InsetEnd, 0.0f)) return 0;
		}
		// 按角号出（k 号角在 k+1 号顶点上）：单测按 `CSHouseQuoin::CornerSign(k)` 写矩形的期望，下标即角号。
		for (int32 Corner = 0; Corner < N; ++Corner)
		{
			const int32 Vertex = (Corner + 1) % N;
			if (Path.TopZ[Vertex] - Path.BaseZ[Vertex] <= 0.0) continue;
			// 外皮的凸角、不比直角尖（`CSHouseQuoin::IsQuoinCorner`，理由见那里）。
			const double HalfCos = Skins.HalfTurnCos[Vertex];
			if (Skins.TurnSin[Vertex] <= 1.0e-4 || HalfCos < double(UE_INV_SQRT_2) - 1.0e-3) continue;
			if (!IsCorner(Skins, Vertex, Params.CornerTurnDegrees)) continue;
			Add(Skins.Right[Vertex], Skins.Normal[Vertex], Vertex, Corner, HalfCos);
		}
		return Out.Num() - Before;
	}

	if (Params.bEnds && !Path.bClosed)
	{
		// 端面与侧面夹直角：平分线 = 端面外法线 + 侧面外法线，转角 90° ⇒ cos(45°)。
		const FVector2D D0 = CSWall_SegmentDir(Path, 0);
		const FVector2D DN = CSWall_SegmentDir(Path, N - 2);
		Add(Skins.Right[0], (-D0 + Skins.Normal[0]).GetSafeNormal(), 0, 100000, UE_INV_SQRT_2);
		Add(Skins.Left[0], (-D0 - Skins.Normal[0]).GetSafeNormal(), 0, 100001, UE_INV_SQRT_2);
		Add(Skins.Right[N - 1], (DN + Skins.Normal[N - 1]).GetSafeNormal(), N - 1, 100002, UE_INV_SQRT_2);
		Add(Skins.Left[N - 1], (DN - Skins.Normal[N - 1]).GetSafeNormal(), N - 1, 100003, UE_INV_SQRT_2);
	}

	if (Params.bCorners)
	{
		for (int32 Index = 0; Index < N; ++Index)
		{
			// 圆滑的弯不出；比直角还尖也不出（与 `CSHouseQuoin::IsQuoinCorner` 同一个上界）。
			const double HalfCos = Skins.HalfTurnCos[Index];
			if (!IsCorner(Skins, Index, Params.CornerTurnDegrees) || HalfCos < double(UE_INV_SQRT_2) - 1.0e-3) continue;
			// 凸侧：左转时外侧是右皮，右转时是左皮。
			if (Skins.TurnSin[Index] > 0.0) Add(Skins.Right[Index], Skins.Normal[Index], Index, Index, HalfCos);
			else Add(Skins.Left[Index], -Skins.Normal[Index], Index, Index, HalfCos);
		}
	}
	return Out.Num() - Before;
}

int32 BuildVineStrips(const FCSWallPath& Path, const FCSWallSkins& Skins,
	const FVineStripParams& Params, FGroundSampler Ground, TArray<CSHouseVine::FWallStrip>& Out)
{
	Out.Reset();
	const int32 N = Path.NumPoints();
	const int32 NumSegments = Path.NumSegments();
	if (!Path.IsValid() || !Skins.IsValid() || Skins.Left.Num() != N) return 0;

	if (Params.Kind == ECSWallKind::Enclosure)
	{
		// ---- 房子：外皮逐边一条平面带（`ACSHouseActor::BuildVineStrips` 原来那一份，逐位不变）----
		//
		// **与房体面板同一个边框架**（`OuterEdgeFrame` = `CSHouse_GetEdge`）：墙在哪儿只能有一个真源。各抄一份的症状是
		// "藤悬在离墙半个墙厚的空中"，而且只在改过 WallThickness 之后才显形。
		const FTransform& World = Params.World;
		for (int32 Edge = 0; Edge < NumSegments; ++Edge)
		{
			const FCSHouseEdgeFrame F = OuterEdgeFrame(Path, Skins, Edge);
			if (F.Len <= UE_KINDA_SMALL_NUMBER) continue;

			CSHouseVine::FWallStrip Strip;
			Strip.EdgeIndex = Edge;
			Strip.Origin = World.TransformPosition(FVector(F.Start.X, F.Start.Y, Path.BaseZ[Edge]));
			Strip.U = World.TransformVectorNoScale(FVector(F.U.X, F.U.Y, 0.0)).GetSafeNormal();
			Strip.Up = World.TransformVectorNoScale(FVector::UpVector).GetSafeNormal();
			// `In` 指向体内，藤长在**外**皮上。
			Strip.N = -World.TransformVectorNoScale(FVector(F.In.X, F.In.Y, 0.0)).GetSafeNormal();
			Strip.Length = F.Len;
			Strip.Height = float(Path.TopZ[Edge] - Path.BaseZ[Edge]);

			// 地面空隙采样：与承重柱**同一个量**（`Gap = 房底 Z − 地面高度`，见 ComputePillars），
			// 只是采样点跟着墙走而不是跟着柱距走。**脚下没有地面**（场景里没有地面，或这一点落在
			// 地面范围外）写 `NoGroundGap` ⇒ 按悬空处理、这段墙不长藤（用户裁决 2026-09-21）：
			// 那样的藤收不到任何地面通知，留着只会是一份永远不刷新的陈旧结果。
			// ⚠️ 采样器必须用 `TrySampleHeight`：`SampleHeight` 在范围外退回地面 actor 的 Z，等于在房子
			// 底下凭空垫一块无限大的平地。
			const int32 SampleCount = FMath::Clamp(
				FMath::CeilToInt(F.Len / FMath::Max(Params.GroundSampleSpacing, 10.0f)) + 1, 2, 64);
			Strip.GroundGaps.SetNumUninitialized(SampleCount);
			for (int32 K = 0; K < SampleCount; ++K)
			{
				const FVector P = Strip.Origin + Strip.U * (F.Len * double(K) / double(SampleCount - 1));
				float GroundZ = 0.0f;
				const bool bHasGround = Ground(FVector2D(P.X, P.Y), GroundZ);
				Strip.GroundGaps[K] = bHasGround ? float(Params.GroundRefZ - GroundZ) : CSHouseVine::NoGroundGap;
			}
			Out.Add(Strip);
		}
		return Out.Num();
	}

	// ---- 独立墙：两面折线带（开口墙在墙端绕过端面接成一圈）----
	if (Params.VineHeight <= 0.0f) return 0;
	const float BaseLift = Params.BaseLift;
	const float VineHeight = Params.VineHeight;
	const float GroundSampleSpacing = Params.GroundSampleSpacing;
	const float CornerTurnDegrees = Params.CornerTurnDegrees;

	auto SegmentRight = [&](int32 Segment)
	{
		const FVector2D D = CSWall_SegmentDir(Path, (Segment + NumSegments) % NumSegments);
		return FVector2D(D.Y, -D.X);
	};

	// 地面空隙：墙脚线 − 地面，沿 S 均匀采（`FWallStrip::GroundGaps` 的约定）。脚下没有地面 ⇒ 悬空不长藤。
	auto SampleGaps = [&](CSHouseVine::FWallStrip& W)
	{
		const int32 Count = FMath::Clamp(FMath::CeilToInt(W.Length / FMath::Max(GroundSampleSpacing, 10.0f)) + 1, 2, 512);
		W.GroundGaps.SetNumUninitialized(Count);
		for (int32 K = 0; K < Count; ++K)
		{
			const float S = W.Length * float(K) / float(Count - 1);
			const FVector P = W.HasPath() ? W.PathToWorld(S, 0.0f, 0.0f) : W.Origin + W.U * double(S);
			float GroundZ = 0.0f;
			W.GroundGaps[K] = Ground(FVector2D(P.X, P.Y), GroundZ) ? float(P.Z - GroundZ) : CSHouseVine::NoGroundGap;
		}
	};

	// 一面墙 = 一条折线带。右皮顺着走、左皮倒着走：两面都满足 N = U × Up（外法线朝外）。
	auto MakeFace = [&](bool bRight, int32 EdgeIndex)
	{
		CSHouseVine::FWallStrip W;
		W.EdgeIndex = EdgeIndex;
		W.bLoop = Path.bClosed;
		W.Up = FVector::UpVector;
		W.Height = VineHeight;

		// 闭合时补回首点，折线才绕满一圈。
		TArray<int32, TInlineAllocator<256>> Order;
		for (int32 K = 0; K < N; ++K) Order.Add(bRight ? K : N - 1 - K);
		if (Path.bClosed)
		{
			const int32 First = Order[0];   // 别写 Order.Add(Order[0])：引用自身元素的 Add 在扩容时是悬垂引用（UE 直接断言）
			Order.Add(First);
		}

		double Arc = 0.0;
		FVector2D PrevXY = FVector2D::ZeroVector;
		auto AddPoint = [&](const FVector2D& XY, int32 Index, const FVector2D& Normal)
		{
			if (!W.PathBase.IsEmpty()) Arc += FVector2D::Distance(PrevXY, XY);
			PrevXY = XY;
			W.PathBase.Add(FVector(XY.X, XY.Y, Path.BaseZ[Index] + double(BaseLift)));
			W.PathS.Add(float(Arc));
			W.PathN.Add(FVector(Normal.X, Normal.Y, 0.0));
		};
		for (int32 K = 0; K < Order.Num(); ++K)
		{
			const int32 Index = Order[K];
			const FVector2D XY = bRight ? Skins.Right[Index] : Skins.Left[Index];
			// 本面行进方向上进 / 出这个点的两段（路径段号）：右皮顺走 = (i−1, i)，左皮倒走 = (i, i−1)。
			const bool bHasIn = K > 0;
			const bool bHasOut = K < Order.Num() - 1;
			const int32 SegIn = bRight ? Index - 1 : Index;
			const int32 SegOut = bRight ? Index : Index - 1;
			auto FaceNormal = [&](int32 Segment) { const FVector2D R = SegmentRight(Segment); return bRight ? R : -R; };
			// 拐角：两侧各用各的面法线，同一个位置放两个点（弧长相同）。插值法线只在圆滑的弯上用 ——
			// 在拐角上插值的话，整段直墙的法线会从 0° 慢慢转到 45°，藤叶跟着歪。
			if (IsCorner(Skins, Index, CornerTurnDegrees) && (bHasIn || bHasOut))
			{
				if (bHasIn) AddPoint(XY, Index, FaceNormal(SegIn));
				if (bHasOut) AddPoint(XY, Index, FaceNormal(SegOut));
			}
			else
			{
				AddPoint(XY, Index, bRight ? Skins.Normal[Index] : -Skins.Normal[Index]);
			}
		}
		W.Length = float(Arc);
		// 平面口径的三件也填上（折线墙用不到，但诊断 / 旧读者读到的是一个说得通的值）。
		W.Origin = W.PathBase[0];
		W.U = FVector(Skins.Right.Num() >= 2 ? (bRight ? FVector2D(Path.Points[1] - Path.Points[0]) : FVector2D(Path.Points[N - 2] - Path.Points[N - 1])) : FVector2D(1.0, 0.0), 0.0).GetSafeNormal();
		W.N = W.PathN[0];
		SampleGaps(W);
		return W;
	};

	// 开口墙的端面：一小块平面墙（宽 = 墙厚）。它把两面墙在墙端接成一圈 —— 右面 → 远端面 → 左面 → 近端面，
	// `BuildPlan` 按边号取模找"隔壁那面墙"，藤走到墙端就能绕过端面接着长，而不是从墙体里穿过去。
	auto MakeCap = [&](int32 Index, bool bFarEnd, int32 EdgeIndex)
	{
		CSHouseVine::FWallStrip W;
		W.EdgeIndex = EdgeIndex;
		W.Up = FVector::UpVector;
		W.Height = VineHeight;
		const FVector2D A = bFarEnd ? Skins.Right[Index] : Skins.Left[Index];
		const FVector2D B = bFarEnd ? Skins.Left[Index] : Skins.Right[Index];
		W.Origin = FVector(A.X, A.Y, Path.BaseZ[Index] + double(BaseLift));
		W.U = FVector(B - A, 0.0).GetSafeNormal();
		W.N = FVector::CrossProduct(W.U, W.Up).GetSafeNormal();   // 远端 = +行进方向，近端 = −行进方向
		W.Length = float(FVector2D::Distance(A, B));
		SampleGaps(W);
		return W;
	};

	if (Path.bClosed)
	{
		Out.Add(MakeFace(true, 0));
		Out.Add(MakeFace(false, 1));
	}
	else
	{
		Out.Add(MakeFace(true, 0));
		Out.Add(MakeCap(N - 1, true, 1));
		Out.Add(MakeFace(false, 2));
		Out.Add(MakeCap(0, false, 3));
	}
	return Out.Num();
}
}

namespace CSHouseQuoin
{
int32 BuildQuoins(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	float BaseZ, float WallHeight, float Inset, TArray<FQuoin>& Out)
{
	// 房子的角石 = 围合墙（`ECSWallKind::Enclosure`）的角石：2026-09-22 起与样条墙同一份 `CSWall::BuildQuoins`。
	if (!Footprint.IsValidFootprint() || WallHeight <= 0.0f) return 0;
	FCSWallPath Path;
	FCSWallSkins Skins;
	if (!CSWall::BuildEnclosureRing(Footprint, WallThickness, 0.0, double(WallHeight), Path, Skins)) return 0;
	CSWall::FQuoinParams Params;
	Params.Kind = ECSWallKind::Enclosure;
	Params.bEnds = false;
	Params.CornerTurnDegrees = 0.0f;   // 房子的每个凸顶点都是角
	Params.Inset = Inset;
	Params.World = World;
	Params.ZOffset = BaseZ;
	return CSWall::BuildQuoins(Path, Skins, Params, Out);
}
}
