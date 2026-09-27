#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSHouseActor.h"     // FCSHouseBodyDesc / CSHouse_BuildBodySoup / ECSHousePart
#include "CSHouseProfile.h"
#include "CSHouseQuoin.h"
#include "CSHouseVine.h"
#include "CSWall.h"
#include "CSWallBase.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UnrealType.h"

// -----------------------------------------------------------------------------
// 「统一房子和墙的逻辑」（2026-09-22 晚）的对照测试
//
// 房子的墙体 / 角石 / 藤的墙面、样条墙的墙体 / 角石，统一之后都走 `CSWall`。这里拿**冻结的旧实现**（统一之前各自那一份的
// 原文，只改了名字）与新路径逐个 float 对照 —— 房子的画面必须逐位不变（用户："注意要兼容房子墙顶端的表现"）。
// ⚠️ 别"顺手"跟着新代码改下面的冻结实现：它们是基准，改了就什么都证明不了。要有意改房子的画面，先改这里的期望。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSWallLegacy_ / CSWallUnifyTest_ 前缀。

// ---- 冻结：CSHouseActor.cpp 的 `FCSHouseMeshWriter` / `CSHouse_BuildBodySoup` / `BuildVineStrips`，CSHouseQuoin.h 的 `BuildQuoins` ----

constexpr float CSWallLegacy_UVScale = 200.0f;

FVector4f CSWallLegacy_Semantic(ECSHousePart Part, uint8 Tag = 0)
{
	return FVector4f(float(uint8(Part)) / 255.0f, float(Tag) / 255.0f, 1.0f, 0.0f);
}

/** 把三角形逐个写进快照的小写手。位置按 World 变换烘成世界空间（常驻流口径），
 *  法线/切线同旋转；面法线遵守常驻流绕序 cross(B-A, C-A)（CSMeshBuild.h）。 */
struct FCSWallLegacy_MeshWriter
{
	FCSGpuMeshCPUData& S;
	FTransform World;

	/** 当前正在写的构件的逐顶点语义色（通道字典见 ACSHouseActor 类注释）。写一组几何前设一次。
	 *  初值走 `CSWallLegacy_Semantic` 而不是零向量：B 的安全值是 255 不是 0，理由逐字见那个函数。 */
	FVector4f Semantic = CSWallLegacy_Semantic(ECSHousePart::Wall);

	/**
	 * 当前面板所属墙面的框架 + 洞的裁剪场。AddTri 据此为每个顶点算 UV1 = q —— 洞由材质
	 * 逐像素 discard 切出来，几何上不挖（Tiny Glade 原版做法，见 CSHouseProfile.h）。
	 * 无洞面板保持 Field.bValid = false，写哨兵 (8, 8)，判据下恒保留。
	 */
	FVector ClipOrigin = FVector::ZeroVector;
	FVector ClipAlong = FVector::ForwardVector;
	FCSOpeningClipField ClipField;

	/** 顶点局部位置 → UV1。S 是沿墙弧长、Z 是墙空间高度，与剖面/判据同一套坐标。 */
	FVector2f ClipUV(const FVector& LocalP) const
	{
		return ClipField.Eval(float(FVector::DotProduct(LocalP - ClipOrigin, ClipAlong)), float(LocalP.Z));
	}

	/** 换一块面板：设墙框架与裁剪场，并把形状 id 写进语义色 B 通道。 */
	void SetPanel(const FVector& Origin, const FVector& Along, const FCSOpeningClipField& Field, ECSHousePart Part, uint8 Tag)
	{
		ClipOrigin = Origin;
		ClipAlong = Along;
		ClipField = Field;
		Semantic = CSWallLegacy_Semantic(Part, Tag);
		// B = 形状 id / 255；255 = 这块面板没有洞（材质据此整块保留）。
		Semantic.Z = float(Field.bValid ? uint8(Field.Shape) : 255) / 255.0f;
	}

	void AddTri(const FVector& A, const FVector& B, const FVector& C, int32 Slot, const FVector2f& UVA, const FVector2f& UVB, const FVector2f& UVC)
	{
		const FVector N = FVector::CrossProduct(B - A, C - A).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		const FVector T = (B - A).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const int32 Base = S.Positions.Num();
		const FVector P[3] = { A, B, C };
		const FVector2f UV[3] = { UVA, UVB, UVC };
		for (int32 i = 0; i < 3; ++i)
		{
			S.Positions.Add(FVector3f(World.TransformPosition(P[i])));
			S.Normals.Add(FVector3f(World.TransformVectorNoScale(N)));
			S.Tangents.Add(FVector3f(World.TransformVectorNoScale(T)));
			S.TexCoords().Add(FVector2f(UV[i]));
			// UV1 = 解析裁剪场。逐顶点写，透视校正插值精确还原（q 是 (s, z) 的仿射函数）。
			S.TexCoordChannels[1].Add(ClipUV(P[i]));
			S.Colors.Add(Semantic);
		}
		// 角点 1/2 交换：法线按外法线算（上面的 cross(B-A,C-A)），但索引要按**引擎绕序**写，
		// 否则整栋房子朝里 —— 与地面同一条口径（见 CSGroundActor::BuildSnapshotFromMirror 的注释）。
		S.Indices.Append({ uint32(Base), uint32(Base + 2), uint32(Base + 1) });
		S.TriangleMaterialSlots.Add(Slot);
	}

	/** 平行四边形面：角点 A、A+U、A+U+V、A+V；常驻流法线 = U×V。UV 取 (沿U, 沿V) / 平铺周期。 */
	void AddQuad(const FVector& A, const FVector& U, const FVector& V, int32 Slot)
	{
		const float LU = float(U.Size()) / CSWallLegacy_UVScale;
		const float LV = float(V.Size()) / CSWallLegacy_UVScale;
		AddTri(A, A + U, A + U + V, Slot, { 0, 0 }, { LU, 0 }, { LU, LV });
		AddTri(A, A + U + V, A + V, Slot, { 0, 0 }, { LU, LV }, { 0, LV });
	}

	/**
	 * 实心平行六面体：O 为一角，X/Y/Z 为三条边向量。六面外法线遵守常驻流口径。
	 * 只要求**右手**，即 (X×Y)·Z > 0，**不要求正交**（剪切过的块也吃得下）。
	 */
	void AddBox(const FVector& O, const FVector& X, const FVector& Y, const FVector& Z, int32 Slot)
	{
		AddQuad(O, Y, X, Slot);              // bottom (-Z)
		AddQuad(O + Z, X, Y, Slot);          // top (+Z)
		AddQuad(O, X, Z, Slot);              // front (-Y)
		AddQuad(O + Y, Z, X, Slot);          // back (+Y)
		AddQuad(O, Z, Y, Slot);              // left (-X)
		AddQuad(O + X, Y, Z, Slot);          // right (+X)
	}

	/**
	 * 任意**平面**四边形：角点按环序 P0 → P1 → P2 → P3，法线 = (P1−P0)×(P3−P0) 的方向。
	 * 与 `AddQuad(A, U, V)` 同一套三角划分与 UV 约定（取 P0 处两条邻边的长度），
	 * 平行四边形时两者产出相同的角点。
	 */
	void AddQuad4(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, int32 Slot)
	{
		const float LU = float((P1 - P0).Size()) / CSWallLegacy_UVScale;
		const float LV = float((P3 - P0).Size()) / CSWallLegacy_UVScale;
		AddTri(P0, P1, P2, Slot, { 0, 0 }, { LU, 0 }, { LU, LV });
		AddTri(P0, P2, P3, Slot, { 0, 0 }, { LU, LV }, { 0, LV });
	}

	/**
	 * 一段墙板：外皮 S 区间 `[SA, SB]`、内皮 S 区间 `[SAin, SBin]`、高度 `[ZLo, ZHi]`、厚 `T`。
	 *
	 * 这就是斜接之后的面板形状：外皮与内皮都是矩形，只有两个端面可能是斜的（沿角平分线切）。
	 * 六个面的角点顺序与 `AddBox(Start + U·SA + Up·ZLo, U·(SB−SA), In·T, Up·(ZHi−ZLo))` 逐面对应，
	 * 所以 `SAin == SA && SBin == SB` 时它就是原来那个盒子。
	 */
	void AddWallPrism(const FVector& Start, const FVector& U, const FVector& In, const FVector& Up,
		float SA, float SB, float SAin, float SBin, float T, float ZLo, float ZHi, int32 Slot)
	{
		const FVector Lo = Up * ZLo, Hi = Up * ZHi, Deep = In * T;
		const FVector O0 = Start + U * SA + Lo,           O1 = Start + U * SB + Lo;
		const FVector I0 = Start + U * SAin + Deep + Lo,  I1 = Start + U * SBin + Deep + Lo;
		const FVector O0t = Start + U * SA + Hi,          O1t = Start + U * SB + Hi;
		const FVector I0t = Start + U * SAin + Deep + Hi, I1t = Start + U * SBin + Deep + Hi;

		AddQuad4(O0, I0, I1, O1, Slot);        // bottom (-Z)
		AddQuad4(O0t, O1t, I1t, I0t, Slot);    // top (+Z)
		AddQuad4(O0, O1, O1t, O0t, Slot);      // 外皮（背离房内）
		AddQuad4(I0, I0t, I1t, I1, Slot);      // 内皮（朝向房内）
		AddQuad4(O0, O0t, I0t, I0, Slot);      // 起点端面（斜接时是斜的）
		AddQuad4(O1, I1, I1t, O1t, Slot);      // 终点端面
	}

};

void CSWallLegacy_HouseBodySoup(const FCSHouseBodyDesc& Desc, FCSGpuMeshCPUData& S)
{
	FCSWallLegacy_MeshWriter Writer{ S, Desc.World };

	const float T = Desc.WallThickness, H = Desc.WallHeight;
	// 材质槽：0 墙面（Masked，按 UV1 逐像素切洞）。槽 1 留给屋顶，但四坡改瓦以后房体里
	// 一片屋面三角都没有 —— 瓦是独立的实例组件。洞缘同样不占槽位（门框是砖块实例）。
	constexpr int32 SlotWall = 0;

	// ---- 四面墙：一串闭合面板。洞不在几何里，由材质按 UV1 的解析判据逐像素 discard 切出 ----
	//
	// 这是 Tiny Glade 原版的开洞方式：CPU 只提供解析参数，洞形在像素阶段成立。洞缘因此是
	// 解析精确曲线（无限分辨率），而不是受弦高容差限制的折线。代价是 discard 只丢像素、
	// 不生成表面 —— 洞缘的厚度断口由门框砖块填满，见下面的说明。
	for (int32 Edge = 0; Edge < Desc.Footprint.NumEdges(); ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Desc.Footprint, T);
		const FVector U(F.U.X, F.U.Y, 0), In(F.In.X, F.In.Y, 0), Up(0, 0, 1);
		const FVector Start(F.Start.X, F.Start.Y, 0);

		TArray<FCSWallOpening> Openings;
		for (const FCSWallOpening& O : Desc.Openings) if (O.EdgeIndex == Edge) Openings.Add(O);
		Openings.Sort([](const FCSWallOpening& A, const FCSWallOpening& B) { return A.CenterS < B.CenterS; });

		// D7 接缝在这条边上要抹掉的段。**必须先排序再并掉重叠的**：一块面板只带一个裁剪场，
		// 两刀重叠时后一刀会静默丢失（症状是三栋房挤在一起时少裁一段墙，而砖照常立着）。
		// 并的是**并集包围**，宁可多裁一点也不留一片穿进邻居房间的墙。
		TArray<FCSWallCut> Cuts;
		for (const FCSWallCut& C : Desc.SeamCuts) if (C.EdgeIndex == Edge && C.IsValid()) Cuts.Add(C);
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

		// 一块面板：从 Z0 到墙顶（洞在其中被 clip 掉），Z0 以下那截是实心窗台盒。
		//
		// ⚠️ **任何情况下都不许靠"不生成面板"来开洞**（2026-08-30 裁决三，全项目架构不变量）：
		// 想让某一片墙消失，砌出实心盒再用裁剪场把它 discard 掉 —— 墩就是这么做的，见下面。
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

		auto AddPanel = [&](float SA, float SB, float Z0, const FCSOpeningClipField& Field, uint8 Tag)
		{
			// H − Z0 也要判：Z0 被参数推到墙顶以上时盒子会退化成反向挤出（面全朝里）。
			if (SB - SA < 0.5f || H - Z0 < 0.5f) return;
			float SAin = SA, SBin = SB;
			InnerSpan(SA, SB, SAin, SBin);
			Writer.SetPanel(Start, U, Field, ECSHousePart::Wall, Tag);
			Writer.AddWallPrism(Start, U, In, Up, SA, SB, SAin, SBin, T, Z0, H, SlotWall);
			if (Z0 > 0.5f)
			{
				// 窗台：真几何而不是 clip 的下界 —— 判据因此只需两个 float（见 CSHouseProfile.h）。
				Writer.SetPanel(Start, U, FCSOpeningClipField(), ECSHousePart::Wall, 0);
				Writer.AddWallPrism(Start, U, In, Up, SA, SB, SAin, SBin, T, 0.0f, Z0, SlotWall);
			}
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
			CSHouse_OpeningCell(O, Desc.PierWidth, CellMin, CellMax);
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
			CellMin = FMath::Clamp(CellMin, Cursor, F.Len);
			CellMax = FMath::Clamp(CellMax, CellMin, F.Len);
			// 1 cm 余量：墩侧的格是 S1() − S0() 再各让一点，浮点上不会逐位等于 Width，
			// 而真正的"装不下"是厘米量级的事。
			// 要求按**洞落在这面墙里的那一截**算，不是名义 `O.Width`：端点可能被量化推到墙外
			// （`ComputeDoors` 已经夹过一次，这里是第二道闸），墙外那一截任何面板都盖不到，
			// 拿名义宽去比就会把整个洞判成"装不下" ⇒ 砖拱砌好了、墙没挖洞，一声不吭。
			const float VisibleWidth = FMath::Min(O.S1(), F.Len) - FMath::Max(O.S0(), 0.0f);
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
		AddRun(Cursor, F.Len, false, 0.0f);

		// 洞口的厚度**不产扫掠面**（用户裁决）：断口由门框砖块填满 —— 与 TG 同构，
		// 它的拱/楣也是与墙砖并列的真实构件（flags&32：按拱高压扁贴合曲线 + 免拱裁剪），
		// 而不是一圈扫掠出来的内壁。门框沿 CSHouse_ComputeClipField 那条解析洞缘铺砖
		// （`BuildFrameArches` -> `CSHouseFrame::BuildEdgeElements`），与 clip 判据同源。
	}

	// ---- 底面（2026-09-17 用户要求「房子产生底面」）----
	//
	// 只铺**墙内皮**围出的那块：墙板自己的底已由 `AddWallPrism` 封住，外皮到内皮这一圈再铺一层会与之共面打架。
	// 内皮角点 = 外角沿角平分线内缩到墙厚 T（`PointAtDepth`，与斜接墙板同一个真源），footprint 取凸包 ⇒ 内皮也凸，扇形即可。
	// **朝下的单面**：墙材质不双面，从屋里往下看是背面、被剔除 —— 平地上房底正好压在地形上，双面就会透过门洞闪烁。
	// 不带裁剪场（UV1 哨兵，恒保留），语义色按墙面，UV0 按局部 XY 平铺（与墙面同一个周期）。
	if (Desc.bBottomFace)
	{
		const int32 NumCorners = Desc.Footprint.NumEdges();
		TArray<FVector> Inner;
		Inner.Reserve(NumCorners);
		for (int32 Corner = 0; Corner < NumCorners; ++Corner)
		{
			const FVector2D P = CSHouse_GetCorner(Corner, Desc.Footprint).PointAtDepth(T);
			Inner.Add(FVector(P.X, P.Y, 0.0));
		}
		// 墙厚大于房子一半时内皮会翻过来（有向面积 <= 0）：那时屋里本来就没有空腔，不铺。
		double TwiceArea = 0.0;
		for (int32 K = 0; K < Inner.Num(); ++K)
		{
			const FVector& A = Inner[K];
			const FVector& B = Inner[(K + 1) % Inner.Num()];
			TwiceArea += A.X * B.Y - B.X * A.Y;
		}
		if (Inner.Num() >= 3 && TwiceArea > 1.0)
		{
			Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(), ECSHousePart::Wall, 0);
			auto FloorUV = [](const FVector& P) { return FVector2f(float(P.X) / CSWallLegacy_UVScale, float(P.Y) / CSWallLegacy_UVScale); };
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

	// ---- 屋顶：四坡 + 全瓦片。房体三角汤里**一片屋面都不产**（2026-08-31）。 ----
	//
	// 这里原本是「两块实体坡板 + 两端山墙棱柱 + 两条檐口封口楔形」那一整套双坡结构，整段删除：
	// TG 的屋顶是**四个坡面**、整面**由瓦铺成**（实拍俯视 + `roof_shape::ridge_length_01_from_
	// rectangle_ratio`），既没有山墙这个构件，屋面也不是实体板。连带作废的还有"墙顶该砌到哪"
	// 那条纪律（咬入量 / `SoffitTopZ` / 封口楔形）—— 没有板底可咬，四面墙顶一律平在 WallHeight，
	// 墙顶与屋面之间那条缝在 TG 里本来就是露着的（室内实拍可见漏光）。
	//
	// 屋面几何改由**瓦片实例**承担（`Content/HouseTest/TinyGladeAsset/Meshes/roof_tile`），走 GPU 实例组件那条路
	// （与藤蔓 / 门框砖同构），不进这份三角汤。屋面方程仍然只有 `CSHouseRoof.h` 一份真源：
	// 铺瓦、铺梁、尖顶、雪、以及 D8「落屋顶 → 不生成」谓词全部调它。

	if (Desc.bFlatRoof)
	{
		Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(), ECSHousePart::Roof, 0);
		auto DeckPoint = [&](int32 Corner)
		{
			const FVector2D XY = CSHouse_GetCorner(Corner, Desc.Footprint).Point;
			return FVector(XY.X, XY.Y, H + 0.5f); // 避开墙板上沿的共面三角。
		};
		auto DeckUV = [](const FVector& P) { return FVector2f(float(P.X) / CSWallLegacy_UVScale, float(P.Y) / CSWallLegacy_UVScale); };
		for (int32 K = 1; K + 1 < Desc.Footprint.NumEdges(); ++K)
		{
			const FVector A = DeckPoint(0), B = DeckPoint(K), C = DeckPoint(K + 1);
			Writer.AddTri(A, B, C, 1, DeckUV(A), DeckUV(B), DeckUV(C));
		}
	}

	S.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	S.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	S.NumTexCoordChannels = 2;   // UV0 贴图 / UV1 解析裁剪场
}

int32 CSWallLegacy_HouseQuoins(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	float BaseZ, float WallHeight, float Inset, TArray<CSHouseQuoin::FQuoin>& Out)
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
		if (!CSHouseQuoin::IsQuoinCorner(C)) continue;
		const FVector2D LocalCorner = C.Point - C.Outward * double(Inset);

		const FVector WorldPoint = World.TransformPosition(FVector(LocalCorner.X, LocalCorner.Y, 0.0));
		const FVector WorldOut = World.TransformVectorNoScale(FVector(C.Outward.X, C.Outward.Y, 0.0));

		CSHouseQuoin::FQuoin Q;
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

void CSWallLegacy_HouseVineStrips(const FCSHouseFootprint& InFootprint, float WallThickness, float WallHeight, const FTransform& InWorld,
	double ActorZ, float VineGroundSampleSpacing, CSWall::FGroundSampler Ground, TArray<CSHouseVine::FWallStrip>& OutStrips)
{
	OutStrips.Reset();
	// **与房体面板同一份 `CSHouse_GetEdge`**：墙在哪儿只能有一个真源。各抄一份的症状是
	// "藤悬在离墙半个墙厚的空中"，而且只在改过 WallThickness 之后才显形。
	const FTransform World = InWorld;
	const FCSHouseFootprint Footprint = InFootprint;
	for (int32 EdgeIndex = 0; EdgeIndex < Footprint.NumEdges(); ++EdgeIndex)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, Footprint, WallThickness);
		if (F.Len <= UE_KINDA_SMALL_NUMBER) continue;

		CSHouseVine::FWallStrip Strip;
		Strip.EdgeIndex = EdgeIndex;
		Strip.Origin = World.TransformPosition(FVector(F.Start.X, F.Start.Y, 0.0));
		Strip.U = World.TransformVectorNoScale(FVector(F.U.X, F.U.Y, 0.0)).GetSafeNormal();
		Strip.Up = World.TransformVectorNoScale(FVector::UpVector).GetSafeNormal();
		// `In` 指向体内，藤长在**外**皮上。
		Strip.N = -World.TransformVectorNoScale(FVector(F.In.X, F.In.Y, 0.0)).GetSafeNormal();
		Strip.Length = F.Len;
		Strip.Height = WallHeight;

		// 地面空隙采样：与承重柱**同一个量**（`Gap = 房底 Z − 地面高度`，见 ComputePillars），
		// 只是采样点跟着墙走而不是跟着柱距走。**脚下没有地面**（场景里没有地面，或这一点落在
		// 地面范围外）写 `NoGroundGap` ⇒ 按悬空处理、这段墙不长藤（用户裁决 2026-09-21）：
		// 那样的藤收不到任何地面通知，留着只会是一份永远不刷新的陈旧结果。
		// ⚠️ 必须用 `TrySampleHeight`：`SampleHeight` 在范围外退回地面 actor 的 Z，等于在房子
		// 底下凭空垫一块无限大的平地。纯函数单测直接构造 `FWallStrip`、不经这里，不受影响。
		{
			const int32 SampleCount = FMath::Clamp(
				FMath::CeilToInt(F.Len / FMath::Max(VineGroundSampleSpacing, 10.0f)) + 1, 2, 64);
			Strip.GroundGaps.SetNumUninitialized(SampleCount);
			const double BaseZ = ActorZ;
			for (int32 K = 0; K < SampleCount; ++K)
			{
				const FVector P = Strip.Origin + Strip.U * (F.Len * double(K) / double(SampleCount - 1));
				float GroundZ = 0.0f;
				const bool bHasGround = Ground(FVector2D(P.X, P.Y), GroundZ);
				Strip.GroundGaps[K] = bHasGround ? float(BaseZ - GroundZ) : CSHouseVine::NoGroundGap;
			}
		}

		OutStrips.Add(Strip);
	}
}

// ---- 冻结：统一之前 CSWall.cpp 的样条墙墙体与角石 ----

/** 旧 `CSWall::SegmentDir`（2026-09-22 统一之前的 CSWall.cpp 原文）。 */
FVector2D CSWallLegacy_SegmentDir(const FCSWallPath& Path, int32 Segment)
{
	return (Path.Points[Path.NextIndex(Segment)] - Path.Points[Segment]).GetSafeNormal();
}

/** 旧 `CSWall::BuildBodySoup`（样条墙的连续墙体，2026-09-22 统一之前的 CSWall.cpp 原文）。 */
int32 CSWallLegacy_ContinuousBody(const FCSWallPath& Path, const FCSWallSkins& Skins, bool bBottomFace, float CornerTurnDegrees, FCSGpuMeshCPUData& Out)
{
	const int32 N = Path.NumPoints();
	if (!Path.IsValid() || !Skins.IsValid() || Skins.Left.Num() != N) return 0;

	const int32 TrianglesBefore = Out.Indices.Num() / 3;
	const FVector4f Semantic(0.0f, 0.0f, 1.0f, 0.0f);
	const FVector2f ClipSentinel = FCSOpeningClipField().Eval(0.0f, 0.0f);
	const float T = Skins.Thickness;

	auto AddTri = [&Out, &Semantic, &ClipSentinel](const FVector& A, const FVector& B, const FVector& C,
		const FVector& NA, const FVector& NB, const FVector& NC,
		const FVector2f& UVA, const FVector2f& UVB, const FVector2f& UVC, const FVector& Tangent)
	{
		const int32 Base = Out.Positions.Num();
		const FVector P[3] = { A, B, C };
		const FVector Nrm[3] = { NA, NB, NC };
		const FVector2f UV[3] = { UVA, UVB, UVC };
		for (int32 K = 0; K < 3; ++K)
		{
			Out.Positions.Add(FVector3f(P[K]));
			Out.Normals.Add(FVector3f(Nrm[K]));
			Out.Tangents.Add(FVector3f(Tangent));
			Out.TexCoords().Add(UV[K]);
			Out.TexCoordChannels[1].Add(ClipSentinel);
			Out.Colors.Add(Semantic);
		}
		Out.Indices.Append({ uint32(Base), uint32(Base + 2), uint32(Base + 1) });
		Out.TriangleMaterialSlots.Add(0);
	};
	auto FaceNormal = [](const FVector& A, const FVector& B, const FVector& C)
	{
		return FVector::CrossProduct(B - A, C - A).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	};
	auto AddFlatTri = [&AddTri, &FaceNormal](const FVector& A, const FVector& B, const FVector& C,
		const FVector2f& UVA, const FVector2f& UVB, const FVector2f& UVC, const FVector& Tangent)
	{
		const FVector Nrm = FaceNormal(A, B, C);
		AddTri(A, B, C, Nrm, Nrm, Nrm, UVA, UVB, UVC, Tangent);
	};
	auto UV = [](double U, double V) { return FVector2f(float(U / CSWall::UVScale), float(V / CSWall::UVScale)); };

	auto Bottom = [&](const FVector2D& XY, int32 Index) { return FVector(XY.X, XY.Y, Path.BaseZ[Index]); };
	auto Top = [&](const FVector2D& XY, int32 Index) { return FVector(XY.X, XY.Y, Path.TopZ[Index]); };

	for (int32 Segment = 0; Segment < Path.NumSegments(); ++Segment)
	{
		const int32 I = Segment;
		const int32 J = Path.NextIndex(Segment);
		const double S0 = Skins.S[I];
		const double S1 = (J == 0) ? Skins.Length : Skins.S[J];
		const FVector2D D = CSWallLegacy_SegmentDir(Path, Segment);
		const FVector Along(D.X, D.Y, 0.0);
		const FVector FaceRight(D.Y, -D.X, 0.0);
		const double HI = Path.TopZ[I] - Path.BaseZ[I];
		const double HJ = Path.TopZ[J] - Path.BaseZ[J];

		auto SideNormal = [&](int32 Vertex, double Sign) -> FVector
		{
			if (!CSWall::IsCorner(Skins, Vertex, CornerTurnDegrees))
			{
				const FVector2D Nrm = Skins.Normal[Vertex] * Sign;
				return FVector(Nrm.X, Nrm.Y, 0.0);
			}
			return FaceRight * Sign;
		};

		const FVector RbI = Bottom(Skins.Right[I], I), RbJ = Bottom(Skins.Right[J], J);
		const FVector RtI = Top(Skins.Right[I], I), RtJ = Top(Skins.Right[J], J);
		const FVector LbI = Bottom(Skins.Left[I], I), LbJ = Bottom(Skins.Left[J], J);
		const FVector LtI = Top(Skins.Left[I], I), LtJ = Top(Skins.Left[J], J);

		{
			const FVector NI = SideNormal(I, 1.0), NJ = SideNormal(J, 1.0);
			AddTri(RbI, RbJ, RtJ, NI, NJ, NJ, UV(S0, 0.0), UV(S1, 0.0), UV(S1, HJ), Along);
			AddTri(RbI, RtJ, RtI, NI, NJ, NI, UV(S0, 0.0), UV(S1, HJ), UV(S0, HI), Along);
		}
		{
			const FVector NI = SideNormal(I, -1.0), NJ = SideNormal(J, -1.0);
			AddTri(LbJ, LbI, LtI, NJ, NI, NI, UV(S1, 0.0), UV(S0, 0.0), UV(S0, HI), -Along);
			AddTri(LbJ, LtI, LtJ, NJ, NI, NJ, UV(S1, 0.0), UV(S0, HI), UV(S1, HJ), -Along);
		}
		AddFlatTri(RtI, RtJ, LtJ, UV(S0, 0.0), UV(S1, 0.0), UV(S1, T), Along);
		AddFlatTri(RtI, LtJ, LtI, UV(S0, 0.0), UV(S1, T), UV(S0, T), Along);
		if (bBottomFace)
		{
			AddFlatTri(RbI, LbJ, RbJ, UV(S0, 0.0), UV(S1, T), UV(S1, 0.0), Along);
			AddFlatTri(RbI, LbI, LbJ, UV(S0, 0.0), UV(S0, T), UV(S1, T), Along);
		}
	}

	if (!Path.bClosed)
	{
		const int32 Last = N - 1;
		const FVector2D D0 = CSWallLegacy_SegmentDir(Path, 0);
		const FVector2D DN = CSWallLegacy_SegmentDir(Path, Last - 1);
		{
			const FVector Lb = Bottom(Skins.Left[0], 0), Rb = Bottom(Skins.Right[0], 0);
			const FVector Lt = Top(Skins.Left[0], 0), Rt = Top(Skins.Right[0], 0);
			const double H = Path.TopZ[0] - Path.BaseZ[0];
			const FVector Across(-D0.Y, D0.X, 0.0);
			AddFlatTri(Lb, Rb, Rt, UV(0.0, 0.0), UV(T, 0.0), UV(T, H), -Across);
			AddFlatTri(Lb, Rt, Lt, UV(0.0, 0.0), UV(T, H), UV(0.0, H), -Across);
		}
		{
			const FVector Lb = Bottom(Skins.Left[Last], Last), Rb = Bottom(Skins.Right[Last], Last);
			const FVector Lt = Top(Skins.Left[Last], Last), Rt = Top(Skins.Right[Last], Last);
			const double H = Path.TopZ[Last] - Path.BaseZ[Last];
			const FVector Across(-DN.Y, DN.X, 0.0);
			AddFlatTri(Rb, Lb, Lt, UV(0.0, 0.0), UV(T, 0.0), UV(T, H), Across);
			AddFlatTri(Rb, Lt, Rt, UV(0.0, 0.0), UV(T, H), UV(0.0, H), Across);
		}
	}

	Out.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	Out.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	Out.NumTexCoordChannels = 2;
	return Out.Indices.Num() / 3 - TrianglesBefore;
}

/** 旧 `CSWall::BuildQuoins`（样条墙的角石，2026-09-22 统一之前的 CSWall.cpp 原文）。 */
int32 CSWallLegacy_FreestandingQuoins(const FCSWallPath& Path, const FCSWallSkins& Skins,
	bool bEnds, bool bCorners, float CornerTurnDegrees, TArray<CSHouseQuoin::FQuoin>& Out)
{
	const int32 N = Path.NumPoints();
	if (!Path.IsValid() || !Skins.IsValid() || Skins.Left.Num() != N) return 0;
	const int32 Before = Out.Num();

	auto Add = [&](const FVector2D& Point, const FVector2D& Outward, int32 Vertex, int32 Identity, double HalfTurnCos)
	{
		CSHouseQuoin::FQuoin Q;
		Q.Point = Point;
		Q.Outward = Outward.GetSafeNormal();
		Q.BottomZ = float(Path.BaseZ[Vertex]);
		Q.TopZ = float(Path.TopZ[Vertex]);
		Q.CornerIndex = Identity;
		Q.HalfTurnCos = float(HalfTurnCos);
		Out.Add(Q);
	};

	if (bEnds && !Path.bClosed)
	{
		const FVector2D D0 = CSWallLegacy_SegmentDir(Path, 0);
		const FVector2D DN = CSWallLegacy_SegmentDir(Path, N - 2);
		Add(Skins.Right[0], -D0 + Skins.Normal[0], 0, 100000, UE_INV_SQRT_2);
		Add(Skins.Left[0], -D0 - Skins.Normal[0], 0, 100001, UE_INV_SQRT_2);
		Add(Skins.Right[N - 1], DN + Skins.Normal[N - 1], N - 1, 100002, UE_INV_SQRT_2);
		Add(Skins.Left[N - 1], DN - Skins.Normal[N - 1], N - 1, 100003, UE_INV_SQRT_2);
	}

	if (bCorners)
	{
		for (int32 Index = 0; Index < N; ++Index)
		{
			const double HalfCos = Skins.HalfTurnCos[Index];
			if (!CSWall::IsCorner(Skins, Index, CornerTurnDegrees) || HalfCos < double(UE_INV_SQRT_2) - 1.0e-3) continue;
			if (Skins.TurnSin[Index] > 0.0) Add(Skins.Right[Index], Skins.Normal[Index], Index, Index, HalfCos);
			else Add(Skins.Left[Index], -Skins.Normal[Index], Index, Index, HalfCos);
		}
	}
	return Out.Num() - Before;
}

// ---- 夹具与对照工具 ----

/** 正 N 边形（外接于单位圆），经 `FromShape` 拉到 `Size` —— 与细节面板里改 `FootprintShape` 同一条路。 */
FCSHouseFootprint CSWallUnifyTest_Regular(int32 Sides, const FVector2D& Size, double PhaseDegrees = 0.0)
{
	TArray<FVector2D> Shape;
	for (int32 K = 0; K < Sides; ++K)
	{
		const double A = FMath::DegreesToRadians(PhaseDegrees) + UE_DOUBLE_TWO_PI * double(K) / double(Sides);
		Shape.Add(FVector2D(FMath::Cos(A), FMath::Sin(A)));
	}
	return FCSHouseFootprint::FromShape(Shape, Size);
}

/** 房子的几种 footprint：矩形（整数 / 非整数尺寸）、三角形、五 / 六 / 八边形。 */
TArray<TPair<FString, FCSHouseFootprint>> CSWallUnifyTest_Footprints()
{
	TArray<TPair<FString, FCSHouseFootprint>> Out;
	Out.Add({ TEXT("矩形600x400"), FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0)) });
	Out.Add({ TEXT("矩形350.5x812.25"), FCSHouseFootprint::MakeRect(FVector2D(350.5, 812.25)) });
	Out.Add({ TEXT("三角形"), FCSHouseFootprint::FromShape({ FVector2D(0.0, 0.0), FVector2D(100.0, 0.0), FVector2D(30.0, 90.0) }, FVector2D(700.0, 560.0)) });
	Out.Add({ TEXT("五边形"), CSWallUnifyTest_Regular(5, FVector2D(640.0, 600.0), 90.0) });
	Out.Add({ TEXT("六边形"), CSWallUnifyTest_Regular(6, FVector2D(720.0, 623.5)) });
	Out.Add({ TEXT("八边形"), CSWallUnifyTest_Regular(8, FVector2D(800.0, 800.0), 22.5) });
	return Out;
}

/** 一套会走遍面板规划器每个分支的洞：一对配墩的拱、窗台窗、圆窗、无效洞、装不下的洞。按各边外皮长摆。 */
TArray<FCSWallOpening> CSWallUnifyTest_Openings(const FCSHouseFootprint& Footprint, float T)
{
	TArray<FCSWallOpening> Out;
	auto Edge = [&](int32 Index) { return CSHouse_GetEdge(Index, Footprint, T); };
	auto Make = [](int32 EdgeIndex, ECSOpeningShape Shape, float CenterS, float Width, float Z0, float Z1, uint8 Tag, uint8 Style)
	{
		FCSWallOpening O;
		O.EdgeIndex = EdgeIndex;
		O.Shape = Shape;
		O.CenterS = CenterS;
		O.Width = Width;
		O.Z0 = Z0;
		O.Z1 = Z1;
		O.Tag = Tag;
		O.StyleFlags = Style;
		return O;
	};
	const int32 N = Footprint.NumEdges();
	{
		// 0 号边：两道落地拱，中间 40 cm 的跨度配墩（前一道 PierAfter、后一道 PierBefore）。
		const float L = Edge(0).Len;
		const float A = L * 0.3f, B = A + 120.0f + 40.0f;
		Out.Add(Make(0, ECSOpeningShape::Arch, A, 120.0f, 0.0f, 220.0f, 3, CSHouse_StylePierAfter));
		Out.Add(Make(0, ECSOpeningShape::Arch, B, 120.0f, 0.0f, 220.0f, 4, CSHouse_StylePierBefore));
	}
	if (N > 1) Out.Add(Make(1, ECSOpeningShape::Rect, Edge(1).Len * 0.5f, 70.0f, 90.0f, 190.0f, 7, 0));   // 窗台窗
	if (N > 2)
	{
		Out.Add(Make(2, ECSOpeningShape::Circle, Edge(2).Len * 0.6f, 60.0f, 140.0f, 200.0f, 9, 0));
		Out.Add(Make(2, ECSOpeningShape::Rect, Edge(2).Len * 0.2f, 0.0f, 0.0f, 100.0f, 11, 0));      // 无效洞（宽 0）
	}
	if (N > 3)
	{
		// 3 号边：第二个洞挤不下（格被前一个吃掉），走"这一洞放弃"那条分支。
		Out.Add(Make(3, ECSOpeningShape::Arch, 100.0f, 150.0f, 0.0f, 200.0f, 12, 0));
		Out.Add(Make(3, ECSOpeningShape::Arch, 180.0f, 150.0f, 0.0f, 200.0f, 13, 0));
	}
	return Out;
}

/** 接缝：0 号边两刀重叠（走合并）、2 号边一刀带底高。 */
TArray<FCSWallCut> CSWallUnifyTest_Cuts(const FCSHouseFootprint& Footprint, float T)
{
	TArray<FCSWallCut> Out;
	auto Cut = [](int32 EdgeIndex, float MinS, float MaxS, float BottomZ, float TopZ)
	{
		FCSWallCut C;
		C.EdgeIndex = EdgeIndex;
		C.MinS = MinS;
		C.MaxS = MaxS;
		C.BottomZ = BottomZ;
		C.TopZ = TopZ;
		return C;
	};
	const float L0 = CSHouse_GetEdge(0, Footprint, T).Len;
	Out.Add(Cut(0, L0 * 0.80f, L0 * 0.95f, 40.0f, 320.0f));
	Out.Add(Cut(0, L0 * 0.75f, L0 * 0.85f, 0.0f, 300.0f));
	if (Footprint.NumEdges() > 2)
	{
		const float L2 = CSHouse_GetEdge(2, Footprint, T).Len;
		Out.Add(Cut(2, L2 * 0.05f, L2 * 0.30f, 100.0f, 250.0f));
	}
	return Out;
}

template <typename TElement>
int32 CSWallUnifyTest_FirstDiff(const TArray<TElement>& A, const TArray<TElement>& B)
{
	if (A.Num() != B.Num()) return FMath::Min(A.Num(), B.Num());
	for (int32 I = 0; I < A.Num(); ++I) if (!(A[I] == B[I])) return I;
	return INDEX_NONE;
}

FString CSWallUnifyTest_Str(const FVector3f& V) { return FString::Printf(TEXT("(%.9g, %.9g, %.9g)"), V.X, V.Y, V.Z); }
FString CSWallUnifyTest_Str(const FVector2f& V) { return FString::Printf(TEXT("(%.9g, %.9g)"), V.X, V.Y); }
FString CSWallUnifyTest_Str(const FVector4f& V) { return FString::Printf(TEXT("(%.9g, %.9g, %.9g, %.9g)"), V.X, V.Y, V.Z, V.W); }
FString CSWallUnifyTest_Str(int64 V) { return FString::Printf(TEXT("%lld"), V); }

template <typename TElement>
bool CSWallUnifyTest_DiffArray(const TCHAR* Name, const TArray<TElement>& A, const TArray<TElement>& B, FString& OutDiff)
{
	const int32 At = CSWallUnifyTest_FirstDiff(A, B);
	if (At == INDEX_NONE) return false;
	OutDiff = FString::Printf(TEXT("%s[%d]（%d vs %d 项）：%s vs %s"), Name, At, A.Num(), B.Num(),
		A.IsValidIndex(At) ? *CSWallUnifyTest_Str(A[At]) : TEXT("-"), B.IsValidIndex(At) ? *CSWallUnifyTest_Str(B[At]) : TEXT("-"));
	return true;
}

/** 两份三角汤逐个 float 相等（`==`，不是容差）；不等时返回第一处差异。 */
FString CSWallUnifyTest_DiffSoups(const FCSGpuMeshCPUData& A, const FCSGpuMeshCPUData& B)
{
	FString Diff;
	if (CSWallUnifyTest_DiffArray(TEXT("Positions"), A.Positions, B.Positions, Diff)) return Diff;
	if (CSWallUnifyTest_DiffArray(TEXT("Normals"), A.Normals, B.Normals, Diff)) return Diff;
	if (CSWallUnifyTest_DiffArray(TEXT("Tangents"), A.Tangents, B.Tangents, Diff)) return Diff;
	if (CSWallUnifyTest_DiffArray(TEXT("UV0"), A.TexCoordChannels[0], B.TexCoordChannels[0], Diff)) return Diff;
	if (CSWallUnifyTest_DiffArray(TEXT("UV1"), A.TexCoordChannels[1], B.TexCoordChannels[1], Diff)) return Diff;
	if (CSWallUnifyTest_DiffArray(TEXT("Colors"), A.Colors, B.Colors, Diff)) return Diff;
	TArray<int64> IA, IB;
	for (uint32 I : A.Indices) IA.Add(I);
	for (uint32 I : B.Indices) IB.Add(I);
	if (CSWallUnifyTest_DiffArray(TEXT("Indices"), IA, IB, Diff)) return Diff;
	TArray<int64> SA, SB;
	for (int32 I : A.TriangleMaterialSlots) SA.Add(I);
	for (int32 I : B.TriangleMaterialSlots) SB.Add(I);
	if (CSWallUnifyTest_DiffArray(TEXT("Slots"), SA, SB, Diff)) return Diff;
	if (A.NumTexCoordChannels != B.NumTexCoordChannels) return FString::Printf(TEXT("NumTexCoordChannels %d vs %d"), A.NumTexCoordChannels, B.NumTexCoordChannels);
	if (A.SourceSpace != B.SourceSpace) return TEXT("SourceSpace");
	if (A.AttrLayout != B.AttrLayout) return TEXT("AttrLayout");
	return FString();
}

/**
 * 两份三角汤在容差内相等（位置 `PosTol` cm、法线 / 切线 / UV / 顶点色 `UnitTol`），索引与槽逐位相等。
 *
 * 连续墙体（样条墙）用它而不是 `DiffSoups`：模块按 fast-math 编译，同一个式子换一处调用，编译器可能把
 * `float(double(T) / 200)` 编成 float 的倒数乘（40 / 200 得 0.199999988 而不是 0.200000003）—— 末位之差，画面上不存在。
 * 房体那条仍用逐位对照（它的写手与旧代码是同一份源码，实测逐位相同）。
 */
FString CSWallUnifyTest_DiffSoupsNear(const FCSGpuMeshCPUData& A, const FCSGpuMeshCPUData& B, float PosTol, float UnitTol)
{
	auto Check3 = [](const TCHAR* Name, const TArray<FVector3f>& X, const TArray<FVector3f>& Y, float Tol, FString& Out)
	{
		if (X.Num() != Y.Num()) { Out = FString::Printf(TEXT("%s 数量 %d vs %d"), Name, X.Num(), Y.Num()); return true; }
		for (int32 I = 0; I < X.Num(); ++I)
		{
			if (!X[I].Equals(Y[I], Tol)) { Out = FString::Printf(TEXT("%s[%d]：%s vs %s"), Name, I, *CSWallUnifyTest_Str(X[I]), *CSWallUnifyTest_Str(Y[I])); return true; }
		}
		return false;
	};
	auto Check2 = [](const TCHAR* Name, const TArray<FVector2f>& X, const TArray<FVector2f>& Y, float Tol, FString& Out)
	{
		if (X.Num() != Y.Num()) { Out = FString::Printf(TEXT("%s 数量 %d vs %d"), Name, X.Num(), Y.Num()); return true; }
		for (int32 I = 0; I < X.Num(); ++I)
		{
			if (!X[I].Equals(Y[I], Tol)) { Out = FString::Printf(TEXT("%s[%d]：%s vs %s"), Name, I, *CSWallUnifyTest_Str(X[I]), *CSWallUnifyTest_Str(Y[I])); return true; }
		}
		return false;
	};
	FString Diff;
	if (Check3(TEXT("Positions"), A.Positions, B.Positions, PosTol, Diff)) return Diff;
	if (Check3(TEXT("Normals"), A.Normals, B.Normals, UnitTol, Diff)) return Diff;
	if (Check3(TEXT("Tangents"), A.Tangents, B.Tangents, UnitTol, Diff)) return Diff;
	if (Check2(TEXT("UV0"), A.TexCoordChannels[0], B.TexCoordChannels[0], UnitTol, Diff)) return Diff;
	if (Check2(TEXT("UV1"), A.TexCoordChannels[1], B.TexCoordChannels[1], UnitTol, Diff)) return Diff;
	if (A.Colors.Num() != B.Colors.Num()) return TEXT("Colors 数量");
	for (int32 I = 0; I < A.Colors.Num(); ++I)
	{
		const FVector4f& CA = A.Colors[I];
		const FVector4f& CB = B.Colors[I];
		if (FMath::Abs(CA.X - CB.X) > UnitTol || FMath::Abs(CA.Y - CB.Y) > UnitTol || FMath::Abs(CA.Z - CB.Z) > UnitTol || FMath::Abs(CA.W - CB.W) > UnitTol)
		{
			return FString::Printf(TEXT("Colors[%d]：%s vs %s"), I, *CSWallUnifyTest_Str(A.Colors[I]), *CSWallUnifyTest_Str(B.Colors[I]));
		}
	}
	if (A.Indices != B.Indices) return TEXT("Indices");
	if (A.TriangleMaterialSlots != B.TriangleMaterialSlots) return TEXT("Slots");
	if (A.NumTexCoordChannels != B.NumTexCoordChannels || A.SourceSpace != B.SourceSpace || A.AttrLayout != B.AttrLayout) return TEXT("流布局");
	return FString();
}

/** 三角汤按**引擎绕序**读出来的外法线面积向量之和与体积（散度定理；索引存的是 (A, C, B)）。 */
void CSWallUnifyTest_SurfaceIntegrals(const FCSGpuMeshCPUData& S, FVector& OutAreaSum, double& OutVolume)
{
	OutAreaSum = FVector::ZeroVector;
	OutVolume = 0.0;
	for (int32 T = 0; T + 2 < S.Indices.Num(); T += 3)
	{
		const FVector A(S.Positions[S.Indices[T]]);
		const FVector B(S.Positions[S.Indices[T + 1]]);
		const FVector C(S.Positions[S.Indices[T + 2]]);
		const FVector AreaVector = -FVector::CrossProduct(B - A, C - A) * 0.5;
		OutAreaSum += AreaVector;
		OutVolume += FVector::DotProduct((A + B + C) / 3.0, AreaVector) / 3.0;
	}
}

/** 解析地面：x + y > 300 处"没有地面"（走 NoGroundGap 分支；恒等变换下房子的东北角悬空），其余是缓坡起伏。 */
bool CSWallUnifyTest_Ground(const FVector2D& XY, float& OutZ)
{
	if (XY.X + XY.Y > 300.0) return false;
	OutZ = float(20.0 * FMath::Sin(XY.X * 0.01) + 5.0 * FMath::Cos(XY.Y * 0.02) - 3.0);
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallHouseBodyMatchesLegacyTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.HouseBodyMatchesLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallHouseBodyMatchesLegacyTest::RunTest(const FString& Parameters)
{
	// 房体三角汤：统一之后（`CSHouse_BuildBodySoup` → `CSWall::BuildBody(Faceted)`）与统一之前逐个 float 相等。
	// 夹具走遍面板规划器的每个分支：配墩的拱、窗台窗、圆窗、无效洞、装不下的洞、重叠接缝的合并、房底、平屋顶、非恒等变换。
	const FTransform Worlds[2] = { FTransform::Identity, FTransform(FRotator(0.0, 37.5, 0.0), FVector(1234.5, -987.25, 55.5)) };
	int32 Cases = 0, Mismatches = 0, MaxTris = 0;
	for (const TPair<FString, FCSHouseFootprint>& Foot : CSWallUnifyTest_Footprints())
	{
		for (const float T : { 24.0f, 37.5f })
		for (const bool bBottom : { true, false })
		for (const bool bFlat : { false, true })
		for (int32 W = 0; W < 2; ++W)
		for (const bool bHoles : { false, true })
		{
			FCSHouseBodyDesc Desc;
			Desc.Footprint = Foot.Value;
			Desc.WallThickness = T;
			Desc.WallHeight = 300.0f;
			Desc.PierWidth = 40.0f;
			Desc.bBottomFace = bBottom;
			Desc.bFlatRoof = bFlat;
			Desc.World = Worlds[W];
			if (bHoles)
			{
				Desc.Openings = CSWallUnifyTest_Openings(Foot.Value, T);
				Desc.SeamCuts = CSWallUnifyTest_Cuts(Foot.Value, T);
			}
			FCSGpuMeshCPUData Legacy, Unified;
			CSWallLegacy_HouseBodySoup(Desc, Legacy);
			CSHouse_BuildBodySoup(Desc, Unified);
			++Cases;
			MaxTris = FMath::Max(MaxTris, Unified.Indices.Num() / 3);
			const FString Diff = CSWallUnifyTest_DiffSoups(Legacy, Unified);
			if (Diff.IsEmpty()) continue;
			if (++Mismatches <= 6)
			{
				AddError(FString::Printf(TEXT("%s T=%.1f 房底=%d 平顶=%d 变换=%d 洞=%d：%s"),
					*Foot.Key, T, bBottom, bFlat, W, bHoles, *Diff));
			}
		}
	}
	// 墙厚大过房子一半：内皮翻过来，房底不铺（两边都得走到那条分支）。
	{
		FCSHouseBodyDesc Desc;
		Desc.Footprint = FCSHouseFootprint::MakeRect(FVector2D(400.0, 300.0));
		Desc.WallThickness = 150.0f;
		Desc.WallHeight = 260.0f;
		FCSGpuMeshCPUData Legacy, Unified;
		CSWallLegacy_HouseBodySoup(Desc, Legacy);
		CSHouse_BuildBodySoup(Desc, Unified);
		++Cases;
		const FString Diff = CSWallUnifyTest_DiffSoups(Legacy, Unified);
		if (!Diff.IsEmpty())
		{
			++Mismatches;
			AddError(FString::Printf(TEXT("厚墙：%s"), *Diff));
		}
	}
	AddInfo(FString::Printf(TEXT("房体对照 %d 组，最大 %d 个三角形"), Cases, MaxTris));
	TestEqual(TEXT("房体三角汤与统一之前逐位相同"), Mismatches, 0);

	// 房子的墙顶是**平顶**（用户："注意要兼容房子墙顶端的表现"）：墙顶砖一块不出，朝上的墙面全在墙高上。
	{
		FCSWallPath Ring;
		FCSWallSkins Skins;
		TestTrue(TEXT("footprint 建得出围合环"),
			CSWall::BuildEnclosureRing(FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0)), 24.0f, 0.0, 300.0, Ring, Skins));
		CSHouseFrame::FBrickParams Bricks;
		Bricks.Length = 30.0f;
		Bricks.MaxBricks = 4096;
		TArray<CSHouseFrame::FElement> Elements;
		const CSWall::FTopResult Top = CSWall::BuildTopElements(Ring, Skins, CSWall::FTopParams::Plain(), Bricks, Elements);
		TestTrue(TEXT("平顶：一块墙顶砖都不出"), Elements.IsEmpty() && Top.CopingBricks == 0 && Top.MerlonBricks == 0 && Top.Runs == 0);

		FCSHouseBodyDesc Desc;
		Desc.Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
		FCSGpuMeshCPUData Soup;
		CSHouse_BuildBodySoup(Desc, Soup);
		int32 Up = 0, OffTop = 0;
		for (int32 Tri = 0; Tri + 2 < Soup.Indices.Num(); Tri += 3)
		{
			if (Soup.Normals[Soup.Indices[Tri]].Z < 0.5f) continue;
			++Up;
			for (int32 K = 0; K < 3; ++K) OffTop += Soup.Positions[Soup.Indices[Tri + K]].Z == Desc.WallHeight ? 0 : 1;
		}
		TestTrue(TEXT("房体有朝上的墙顶"), Up > 0);
		TestEqual(TEXT("朝上的面全在墙高上（墙顶平，没有压顶 / 垛口）"), OffTop, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallHouseQuoinsAndVinesMatchLegacyTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.HouseQuoinsAndVinesMatchLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallHouseQuoinsAndVinesMatchLegacyTest::RunTest(const FString& Parameters)
{
	const FTransform Worlds[2] = { FTransform::Identity, FTransform(FRotator(0.0, -61.25, 0.0), FVector(-432.125, 2210.5, 88.0)) };
	TArray<TPair<FString, FCSHouseFootprint>> Footprints = CSWallUnifyTest_Footprints();
	// 退化：短边放不下两头的斜接让出量 ⇒ 角石整圈不出（两边都得走到那条分支）。
	Footprints.Add({ TEXT("退化矩形40x400"), FCSHouseFootprint::MakeRect(FVector2D(40.0, 400.0)) });

	// ---- 角石：`CSHouseQuoin::BuildQuoins`（现在是围合墙的 `CSWall::BuildQuoins`）vs 冻结的旧实现 ----
	int32 QuoinCases = 0, QuoinMismatches = 0, QuoinColumns = 0, DegenerateColumns = -1;
	for (const TPair<FString, FCSHouseFootprint>& Foot : Footprints)
	for (const float T : { 24.0f, 37.5f })
	for (const float Inset : { 0.0f, 5.0f, -3.0f })
	for (int32 W = 0; W < 2; ++W)
	for (const float BaseZ : { 0.0f, 123.456f })
	{
		TArray<CSHouseQuoin::FQuoin> Legacy, Unified;
		const int32 NL = CSWallLegacy_HouseQuoins(Worlds[W], Foot.Value, T, BaseZ, 300.0f, Inset, Legacy);
		const int32 NU = CSHouseQuoin::BuildQuoins(Worlds[W], Foot.Value, T, BaseZ, 300.0f, Inset, Unified);
		++QuoinCases;
		QuoinColumns += NU;
		if (Foot.Key.StartsWith(TEXT("退化"))) DegenerateColumns = FMath::Max(DegenerateColumns, NU);
		bool bSame = NL == NU && Legacy.Num() == Unified.Num();
		for (int32 K = 0; bSame && K < Legacy.Num(); ++K)
		{
			const CSHouseQuoin::FQuoin& A = Legacy[K];
			const CSHouseQuoin::FQuoin& B = Unified[K];
			bSame = A.Point == B.Point && A.Outward == B.Outward && A.BottomZ == B.BottomZ && A.TopZ == B.TopZ
				&& A.CornerIndex == B.CornerIndex && A.HalfTurnCos == B.HalfTurnCos && A.CullBelowZ == B.CullBelowZ;
		}
		if (!bSame && ++QuoinMismatches <= 6)
		{
			AddError(FString::Printf(TEXT("角石 %s T=%.1f Inset=%.1f 变换=%d 房底=%.3f：旧 %d 根 / 新 %d 根"),
				*Foot.Key, T, Inset, W, BaseZ, NL, NU));
		}
	}
	AddInfo(FString::Printf(TEXT("角石对照 %d 组，共 %d 根"), QuoinCases, QuoinColumns));
	TestEqual(TEXT("房子的角石与统一之前逐位相同"), QuoinMismatches, 0);
	TestEqual(TEXT("退化矩形不出角石"), DegenerateColumns, 0);

	// ---- 藤的墙面：围合墙（外皮逐边平面带）vs 冻结的旧 `ACSHouseActor::BuildVineStrips` ----
	int32 VineCases = 0, VineMismatches = 0, NoGround = 0;
	for (const TPair<FString, FCSHouseFootprint>& Foot : Footprints)
	for (const float T : { 24.0f, 37.5f })
	for (int32 W = 0; W < 2; ++W)
	for (const float Spacing : { 100.0f, 37.0f, 4.0f })
	for (const double ActorZ : { 0.0, 55.5 })
	{
		FTransform World = Worlds[W];
		World.SetLocation(World.GetLocation() + FVector(0.0, 0.0, ActorZ));
		TArray<CSHouseVine::FWallStrip> Legacy, Unified;
		CSWallLegacy_HouseVineStrips(Foot.Value, T, 300.0f, World, ActorZ, Spacing, CSWallUnifyTest_Ground, Legacy);

		FCSWallPath Ring;
		FCSWallSkins Skins;
		CSWall::BuildEnclosureRing(Foot.Value, T, 0.0, 300.0, Ring, Skins);
		CSWall::FVineStripParams Params;
		Params.Kind = ECSWallKind::Enclosure;
		Params.World = World;
		Params.GroundRefZ = ActorZ;
		Params.GroundSampleSpacing = Spacing;
		CSWall::BuildVineStrips(Ring, Skins, Params, CSWallUnifyTest_Ground, Unified);
		++VineCases;

		bool bSame = Legacy.Num() == Unified.Num();
		for (int32 K = 0; bSame && K < Legacy.Num(); ++K)
		{
			const CSHouseVine::FWallStrip& A = Legacy[K];
			const CSHouseVine::FWallStrip& B = Unified[K];
			bSame = A.EdgeIndex == B.EdgeIndex && A.Origin == B.Origin && A.U == B.U && A.Up == B.Up && A.N == B.N
				&& A.Length == B.Length && A.Height == B.Height && A.GroundGaps == B.GroundGaps && A.bLoop == B.bLoop
				&& !B.HasPath() && B.PathBase.IsEmpty();
			for (float Gap : B.GroundGaps) NoGround += Gap == CSHouseVine::NoGroundGap ? 1 : 0;
		}
		if (!bSame && ++VineMismatches <= 6)
		{
			AddError(FString::Printf(TEXT("藤 %s T=%.1f 变换=%d 采样=%.0f 房底=%.1f：旧 %d 条 / 新 %d 条"),
				*Foot.Key, T, W, Spacing, ActorZ, Legacy.Num(), Unified.Num()));
		}
	}
	AddInfo(FString::Printf(TEXT("藤的墙面对照 %d 组；无地面采样 %d 个"), VineCases, NoGround));
	TestEqual(TEXT("房子的藤墙面与统一之前逐位相同"), VineMismatches, 0);
	TestTrue(TEXT("夹具走到了\"脚下没有地面\"的分支"), NoGround > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallContinuousMatchesLegacyTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.ContinuousMatchesLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallContinuousMatchesLegacyTest::RunTest(const FString& Parameters)
{
	// 样条墙的墙体 / 角石：统一之后（`BuildBody(Continuous)` / `BuildQuoins(Freestanding)`）与统一之前的 `CSWall` 原文对照。
	TArray<TPair<FString, FCSWallPath>> Paths;
	{
		FCSWallPath L;
		L.Points = { FVector2D(0.0, 0.0), FVector2D(600.0, 0.0), FVector2D(600.0, 450.0) };
		L.BaseZ = { 0.0, 0.0, 0.0 };
		L.TopZ = { 150.0, 150.0, 150.0 };
		Paths.Add({ TEXT("L 形"), L });
	}
	{
		FCSWallPath Square;
		Square.Points = { FVector2D(0.0, 0.0), FVector2D(400.0, 0.0), FVector2D(400.0, 400.0), FVector2D(0.0, 400.0) };
		Square.BaseZ = { 0.0, 0.0, 0.0, 0.0 };
		Square.TopZ = { 150.0, 150.0, 150.0, 150.0 };
		Square.bClosed = true;
		Paths.Add({ TEXT("闭合正方形"), Square });
	}
	{
		FCSWallPath Arc;
		for (int32 K = 0; K <= 20; ++K)
		{
			const double A = UE_DOUBLE_HALF_PI * K / 20;
			Arc.Points.Add(FVector2D(500.0 * FMath::Cos(A), 500.0 * FMath::Sin(A)));
			Arc.BaseZ.Add(3.0 * FMath::Sin(A * 7.0));
			Arc.TopZ.Add(300.0 + 11.0 * FMath::Cos(A * 5.0));
		}
		Paths.Add({ TEXT("起伏的弧墙"), Arc });
	}
	{
		FCSWallPath Circle;
		for (int32 K = 0; K < 32; ++K)
		{
			const double A = UE_DOUBLE_TWO_PI * K / 32;
			Circle.Points.Add(FVector2D(500.0 * FMath::Cos(A) + 17.25, 500.0 * FMath::Sin(A) - 3.5));
			Circle.BaseZ.Add(-10.0 + 4.0 * FMath::Sin(A * 3.0));
			Circle.TopZ.Add(290.0 + 6.0 * FMath::Cos(A * 2.0));
		}
		Circle.bClosed = true;
		Paths.Add({ TEXT("闭合圆墙"), Circle });
	}
	{
		// 近 180° 的折返（斜接封顶）+ 一个右转。
		FCSWallPath Fold;
		Fold.Points = { FVector2D(0.0, 0.0), FVector2D(300.0, 0.0), FVector2D(0.0, 10.0), FVector2D(-50.0, 300.0) };
		Fold.BaseZ = { 0.0, 20.0, 5.0, -7.5 };
		Fold.TopZ = { 150.0, 170.0, 155.0, 142.5 };
		Paths.Add({ TEXT("折返"), Fold });
	}

	int32 Cases = 0, Mismatches = 0, ExactCases = 0;
	int32 QuoinCases = 0, QuoinMismatches = 0;
	for (const TPair<FString, FCSWallPath>& Item : Paths)
	for (const float T : { 34.0f, 40.0f })
	for (const float Threshold : { 30.0f, 15.0f })
	{
		FCSWallSkins Skins;
		if (!TestTrue(FString::Printf(TEXT("%s：皮线"), *Item.Key), CSWall::BuildSkins(Item.Value, T, Skins))) continue;
		for (const bool bBottom : { true, false })
		{
			FCSGpuMeshCPUData Legacy, Unified;
			const int32 NL = CSWallLegacy_ContinuousBody(Item.Value, Skins, bBottom, Threshold, Legacy);
			CSWall::FBodyParams Params;
			Params.Surface = ECSWallSurface::Continuous;
			Params.bBottomFace = bBottom;
			Params.CornerTurnDegrees = Threshold;
			const int32 NU = CSWall::BuildBody(Item.Value, Skins, Params, Unified);
			++Cases;
			const FString Exact = CSWallUnifyTest_DiffSoups(Legacy, Unified);
			ExactCases += Exact.IsEmpty() ? 1 : 0;
			const FString Diff = NL != NU ? FString::Printf(TEXT("三角形 %d vs %d"), NL, NU) : CSWallUnifyTest_DiffSoupsNear(Legacy, Unified, 1.0e-3f, 1.0e-6f);
			if (!Diff.IsEmpty() && ++Mismatches <= 6)
			{
				AddError(FString::Printf(TEXT("墙体 %s T=%.0f 阈值=%.0f 封底=%d：%s"), *Item.Key, T, Threshold, bBottom, *Diff));
			}
		}
		for (const bool bEnds : { true, false })
		{
			TArray<CSHouseQuoin::FQuoin> Legacy, Unified;
			CSWallLegacy_FreestandingQuoins(Item.Value, Skins, bEnds, true, Threshold, Legacy);
			CSWall::FQuoinParams Params;
			Params.Kind = ECSWallKind::Freestanding;
			Params.bEnds = bEnds;
			Params.CornerTurnDegrees = Threshold;
			CSWall::BuildQuoins(Item.Value, Skins, Params, Unified);
			++QuoinCases;
			// 墙端那几根的外法线在新代码里先单位化再过变换（内缩量要按单位方向算）：允许末位的舍入差。
			bool bSame = Legacy.Num() == Unified.Num();
			for (int32 K = 0; bSame && K < Legacy.Num(); ++K)
			{
				const CSHouseQuoin::FQuoin& A = Legacy[K];
				const CSHouseQuoin::FQuoin& B = Unified[K];
				bSame = A.Point == B.Point && A.Outward.Equals(B.Outward, 1.0e-12) && A.BottomZ == B.BottomZ && A.TopZ == B.TopZ
					&& A.CornerIndex == B.CornerIndex && A.HalfTurnCos == B.HalfTurnCos;
			}
			if (!bSame && ++QuoinMismatches <= 6)
			{
				AddError(FString::Printf(TEXT("角石 %s T=%.0f 阈值=%.0f 墙端=%d：旧 %d 根 / 新 %d 根"),
					*Item.Key, T, Threshold, bEnds, Legacy.Num(), Unified.Num()));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("连续墙体对照 %d 组（其中 %d 组逐位相同，其余只差 fast-math 的末位），角石对照 %d 组"), Cases, ExactCases, QuoinCases));
	TestEqual(TEXT("样条墙的墙体与统一之前相同（容差：位置 1e-3 cm、单位量 1e-6）"), Mismatches, 0);
	TestEqual(TEXT("样条墙的角石与统一之前相同"), QuoinMismatches, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallContinuousOpeningsTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.ContinuousOpenings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallContinuousOpeningsTest::RunTest(const FString& Parameters)
{
	// 统一之后样条墙与房子走同一个面板规划器：连续的墙也吃得下洞 / 窗台 / 接缝（计划第 2 步"路穿墙开拱"的地基）。
	// 洞仍然只在渲染层（UV1 裁剪场），几何实心：表面闭合、体积 = T·H·L。
	FCSWallPath Path;
	Path.Points = { FVector2D(0.0, 0.0), FVector2D(800.0, 0.0) };
	Path.BaseZ = { 0.0, 0.0 };
	Path.TopZ = { 300.0, 300.0 };
	FCSWallSkins Skins;
	if (!TestTrue(TEXT("直墙皮线"), CSWall::BuildSkins(Path, 34.0f, Skins))) return false;

	TArray<FCSWallOpening> Openings;
	{
		FCSWallOpening Door;
		Door.EdgeIndex = 0;
		Door.Shape = ECSOpeningShape::Arch;
		Door.CenterS = 250.0f;
		Door.Width = 120.0f;
		Door.Z1 = 220.0f;
		Door.Tag = 5;
		Openings.Add(Door);
		FCSWallOpening Window;
		Window.EdgeIndex = 0;
		Window.Type = ECSOpeningType::Window;
		Window.Shape = ECSOpeningShape::Rect;
		Window.CenterS = 560.0f;
		Window.Width = 80.0f;
		Window.Z0 = 100.0f;
		Window.Z1 = 200.0f;
		Window.Tag = 6;
		Openings.Add(Window);
	}
	TArray<FCSWallCut> Cuts;
	{
		FCSWallCut Cut;
		Cut.EdgeIndex = 0;
		Cut.MinS = 700.0f;
		Cut.MaxS = 760.0f;
		Cut.TopZ = 300.0f;
		Cuts.Add(Cut);
	}
	CSWall::FBodyParams Params;
	Params.Surface = ECSWallSurface::Continuous;
	Params.Openings = Openings;
	Params.SeamCuts = Cuts;
	FCSGpuMeshCPUData Solid, Holed;
	CSWall::BuildBody(Path, Skins, CSWall::FBodyParams(), Solid);
	CSWall::BuildBody(Path, Skins, Params, Holed);
	TestTrue(FString::Printf(TEXT("洞把墙切成更多面板（%d → %d 个三角形），一个三角形都不少"), Solid.Indices.Num() / 3, Holed.Indices.Num() / 3),
		Holed.Indices.Num() > Solid.Indices.Num());

	FVector AreaSum;
	double Volume = 0.0;
	CSWallUnifyTest_SurfaceIntegrals(Holed, AreaSum, Volume);
	const double Expected = 34.0 * 300.0 * 800.0;
	AddInfo(FString::Printf(TEXT("面积向量和 %s，体积 %.0f（期望 %.0f）"), *AreaSum.ToString(), Volume, Expected));
	TestTrue(TEXT("开洞的连续墙仍然闭合（洞在渲染层，几何实心）"), AreaSum.Size() < 1.0);
	TestTrue(TEXT("体积 = T·H·L"), FMath::Abs(Volume - Expected) < Expected * 1.0e-3);

	// 通道：门的面板 G = Tag 5、B = 拱；窗的面板 G = 6、B = 矩形；窗台那一截是实心墙（G = 0、B = 255）。
	const FVector2f Sentinel = FCSOpeningClipField().Eval(0.0f, 0.0f);
	int32 DoorVerts = 0, WindowVerts = 0, SillVerts = 0, ClipVerts = 0;
	for (int32 V = 0; V < Holed.Positions.Num(); ++V)
	{
		const FVector4f& C = Holed.Colors[V];
		const uint8 Tag = uint8(FMath::RoundToInt32(C.Y * 255.0f));
		const uint8 Shape = uint8(FMath::RoundToInt32(C.Z * 255.0f));
		ClipVerts += Holed.TexCoordChannels[1][V].Equals(Sentinel) ? 0 : 1;
		if (Tag == 5) DoorVerts += Shape == uint8(ECSOpeningShape::Arch) ? 1 : 0;
		if (Tag == 6) WindowVerts += Shape == uint8(ECSOpeningShape::Rect) ? 1 : 0;
		const float X = Holed.Positions[V].X;
		// 窗的格 = [560 − 40 − 20, 560 + 40 + 20] = [500, 620]：窗台层的顶点就在格的两头、窗台高上。
		if (Tag == 0 && Shape == 255 && FMath::IsNearlyEqual(Holed.Positions[V].Z, 100.0f, 1.0e-3f) && X > 499.0f && X < 621.0f) ++SillVerts;
	}
	TestTrue(TEXT("门的面板带拱的裁剪场"), DoorVerts > 0);
	TestTrue(TEXT("窗的面板带矩形裁剪场"), WindowVerts > 0);
	TestTrue(TEXT("窗台以下是实心墙（没有洞的那一截）"), SillVerts > 0);
	TestTrue(TEXT("有面板的 UV1 不是哨兵"), ClipVerts > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallHouseVineSettingsAdapterTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.HouseVineSettingsAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallHouseVineSettingsAdapterTest::RunTest(const FString& Parameters)
{
	// 藤的胶水统一之后（`ACSWallBase`），房子的 38 个 `Vine*` 平铺属性经 `GetVineSettings()` 现组成 `FCSWallVineSettings`。
	// 这条钉住"一个都不漏"：结构体的每个字段都要在房子上找到同名同型的属性（`X` ↔ `VineX`、`bX` ↔ `bVineX`），
	// 把房子的值逐个改掉之后，现组出来的结构体必须逐字段等于房子的值 —— 漏抄一个字段，那个参数在房子上就静默失效。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House actor"), House)) return false;

	const UScriptStruct* Struct = FCSWallVineSettings::StaticStruct();
	const UClass* HouseClass = ACSHouseActor::StaticClass();
	TArray<TPair<const FProperty*, const FProperty*>> Pairs;
	int32 Missing = 0;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FProperty* SP = *It;
		const FString Name = SP->GetName();
		const bool bBool = SP->IsA<FBoolProperty>() && Name.StartsWith(TEXT("b"));
		const FString HouseName = bBool ? FString(TEXT("bVine")) + Name.Mid(1) : FString(TEXT("Vine")) + Name;
		const FProperty* HP = HouseClass->FindPropertyByName(FName(*HouseName));
		if (!HP || !HP->SameType(SP))
		{
			++Missing;
			AddError(FString::Printf(TEXT("结构体字段 %s 在房子上没有同名同型的 %s"), *Name, *HouseName));
			continue;
		}
		Pairs.Add({ SP, HP });
	}
	TestEqual(TEXT("结构体的每个字段在房子上都有对应属性"), Missing, 0);
	TestEqual(TEXT("对上了 38 个字段"), Pairs.Num(), 38);

	// 逐个改掉房子的值（每一项都改成与默认不同的值），再现组。
	for (const TPair<const FProperty*, const FProperty*>& Pair : Pairs)
	{
		const FProperty* HP = Pair.Value;
		void* Value = HP->ContainerPtrToValuePtr<void>(House);
		if (const FFloatProperty* F = CastField<FFloatProperty>(HP)) F->SetPropertyValue(Value, F->GetPropertyValue(Value) + 1.25f);
		else if (const FIntProperty* I = CastField<FIntProperty>(HP)) I->SetPropertyValue(Value, I->GetPropertyValue(Value) + 3);
		else if (const FBoolProperty* B = CastField<FBoolProperty>(HP)) B->SetPropertyValue(Value, !B->GetPropertyValue(Value));
		else if (const FEnumProperty* E = CastField<FEnumProperty>(HP))
		{
			const FNumericProperty* Under = E->GetUnderlyingProperty();
			Under->SetIntPropertyValue(Value, (Under->GetSignedIntPropertyValue(Value) + 1) % 3);
		}
		else if (const FObjectPropertyBase* O = CastField<FObjectPropertyBase>(HP))
		{
			// 有值就清空、空的就填同类的默认对象 —— 只比身份，不渲染。
			O->SetObjectPropertyValue(Value, O->GetObjectPropertyValue(Value) ? nullptr : O->PropertyClass->GetDefaultObject());
		}
		else
		{
			AddError(FString::Printf(TEXT("没见过的属性类型：%s"), *HP->GetName()));
		}
	}
	const FCSWallVineSettings Settings = House->GetVineSettings();
	int32 Mismatch = 0;
	for (const TPair<const FProperty*, const FProperty*>& Pair : Pairs)
	{
		const void* A = Pair.Key->ContainerPtrToValuePtr<void>(&Settings);
		const void* B = Pair.Value->ContainerPtrToValuePtr<void>(House);
		if (!Pair.Key->Identical(A, B))
		{
			++Mismatch;
			AddError(FString::Printf(TEXT("GetVineSettings 没把 %s 抄过去"), *Pair.Value->GetName()));
		}
	}
	TestEqual(TEXT("现组的结构体逐字段等于房子的属性"), Mismatch, 0);
	TestTrue(TEXT("房子是围合墙"), House->GetWallKind() == ECSWallKind::Enclosure);

	// 路径来源：房子的环 = footprint 外皮（局部空间），变换 = GetBuildTransform。
	FCSWallPath Ring;
	FCSWallSkins Skins;
	FTransform RingWorld;
	TestTrue(TEXT("房子给得出墙的路径"), House->GetWallPath(Ring, Skins, RingWorld));
	TestTrue(TEXT("环是闭合的、点数 = footprint 顶点数"), Ring.bClosed && Ring.NumPoints() == House->GetFootprint().NumEdges());
	bool bOuter = Skins.Right.Num() == House->GetFootprint().NumEdges();
	for (int32 K = 0; bOuter && K < Skins.Right.Num(); ++K) bOuter = Skins.Right[K] == House->GetFootprint().Verts[K];
	TestTrue(TEXT("右皮逐位就是 footprint 顶点"), bOuter);
	World->DestroyActor(House);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
