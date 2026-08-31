#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSHouseDecor.h"
#include "CSHouseProfile.h"
#include "CSHouseRoof.h"

// -----------------------------------------------------------------------------
// 装饰摆件（D12 的**锚点那一半**）的验收。
//
// ⚠️ **这里的判据全是纯 CPU 的，刻意不碰 RHI**：摆件最容易错的四件事
// （会不会堵在门口、密度是不是真由锚点数量决定、随机稳不稳、窗户那家有没有被误开）
// 在 GPU 那一侧只剩下一个实例计数，到那时候什么都断言不了。GPU 那一侧的判据是**另外两条**：
// `ACSHouseActor::IsDecorDrawable`（渲染环节逐环检查）+ `Scripts/TinyGladeShotDecor.py`
// 的像素门。三者缺一不可 —— 石阶那个坑（`StairMesh` 恒 NULL、画面黑块、readback 全绿）
// 正是"只有数值判据"的后果。
//
// ⚠️ 这里**不测复杂度场**，因为本轮不实现它：计划 D12 的 `RT_DecorField` + tile-argmax
// 在 TG 里没有对位物，取舍是挂起的决策 C2。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSDecorTest_ 前缀
//（与 CSHouseDecor.cpp 的 CSHouseDecor_、CSHouseVineTests 的 CSVineTest_ 都不同）。

constexpr float CSDecorTest_WallHeight = 300.0f;
constexpr float CSDecorTest_WallThickness = 24.0f;

/** 四面墙 + 屋面 + 摆位，口径与 `ACSHouseActor::BuildVineStrips` / `CSHouse_GetEdge` 一致。 */
CSHouseDecor::FSite CSDecorTest_MakeSite(const FVector2D& Footprint, const FVector& Origin = FVector::ZeroVector)
{
	CSHouseDecor::FSite Site;
	const double HX = Footprint.X * 0.5, HY = Footprint.Y * 0.5;
	const double T = CSDecorTest_WallThickness;

	// 0 南(+X 向) 1 东(+Y 向) 2 北(-X 向) 3 西(-Y 向)；`N` 是**外**法线（= -In）。
	const FVector2D Start[4] = { { -HX, -HY }, { HX, -HY + T }, { HX, HY }, { -HX, HY - T } };
	const FVector2D U[4] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
	const FVector2D N[4] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
	const double Len[4] = { Footprint.X, Footprint.Y - 2 * T, Footprint.X, Footprint.Y - 2 * T };

	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		CSHouseVine::FWallStrip Strip;
		Strip.EdgeIndex = Edge;
		Strip.Origin = Origin + FVector(Start[Edge].X, Start[Edge].Y, 0.0);
		Strip.U = FVector(U[Edge].X, U[Edge].Y, 0.0);
		Strip.Up = FVector::UpVector;
		Strip.N = FVector(N[Edge].X, N[Edge].Y, 0.0);
		Strip.Length = float(Len[Edge]);
		Strip.Height = CSDecorTest_WallHeight;
		Site.Strips.Add(Strip);
	}

	Site.Roof.RidgeAxis = ECSRidgeAxis::X;
	Site.Roof.Footprint = Footprint;
	Site.Roof.EaveZ = CSDecorTest_WallHeight;
	Site.Roof.Pitch = 35.0f;
	Site.Roof.Overhang = 25.0f;
	Site.Roof.Thickness = 12.0f;

	Site.World = FTransform(Origin);
	Site.BaseZ = Origin.Z;
	return Site;
}

/** 0 号边（南墙）上的一个门。门恒从墙基起算（Z0 = 0）。 */
FCSWallOpening CSDecorTest_MakeDoor(float CenterS, float Width = 160.0f)
{
	FCSWallOpening Door;
	Door.Type = ECSOpeningType::Door;
	Door.Shape = ECSOpeningShape::Arch;
	Door.EdgeIndex = 0;
	Door.CenterS = CenterS;
	Door.Width = Width;
	Door.Z0 = 0.0f;
	Door.Z1 = 220.0f;
	return Door;
}

/** 三家各一个 palette 条目；窗户那家恒空（见 `CSHouseDecor.h` 的文件头：等 C1）。 */
TArray<CSHouseDecor::FPaletteRange> CSDecorTest_MakeRanges()
{
	TArray<CSHouseDecor::FPaletteRange> Ranges;
	Ranges.SetNum(int32(CSHouseDecor::EFamily::Count));
	Ranges[int32(CSHouseDecor::EFamily::Gate)] = { 0, 1 };
	Ranges[int32(CSHouseDecor::EFamily::WallFoot)] = { 1, 1 };
	Ranges[int32(CSHouseDecor::EFamily::Eave)] = { 2, 1 };
	Ranges[int32(CSHouseDecor::EFamily::Ridge)] = { 2, 1 };
	return Ranges;
}

int32 CSDecorTest_CountFamily(const TArray<CSHouseDecor::FAnchor>& Anchors, CSHouseDecor::EFamily Family)
{
	int32 Count = 0;
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		if (Anchor.Family == Family) ++Count;
	}
	return Count;
}
}

// -----------------------------------------------------------------------------
// ① 锚点长在构件上：墙脚那一家在墙的**外**面、贴着地
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDecorAnchorsSitOnFeaturesTest,
	"PCGPlugins.ComputeShaderGenerator.House.DecorAnchorsSitOnFeatures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDecorAnchorsSitOnFeaturesTest::RunTest(const FString& Parameters)
{
	const CSHouseDecor::FSite Site = CSDecorTest_MakeSite(FVector2D(600.0, 400.0));
	const CSHouseDecor::FParams Params;

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSHouseDecor::BuildAnchors(Site, Params, Anchors);
	TestTrue(TEXT("光墙一栋房子也长得出锚点"), Anchors.Num() > 0);

	int32 Inside = 0, WrongHeight = 0;
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		if (Anchor.Family != CSHouseDecor::EFamily::WallFoot) continue;
		const CSHouseVine::FWallStrip& Strip = Site.Strips[Anchor.AnchorId / 65536];
		// 摆件必须在墙**外**：沿外法线的投影为正。摆进屋里的桶从外面根本看不见，
		// 而实例计数照样是对的 —— 这一条只能在这一层断言。
		if (FVector::DotProduct(Anchor.Location - Strip.Origin, Strip.N) <= 0.0) ++Inside;
		if (!FMath::IsNearlyEqual(Anchor.Location.Z, Site.BaseZ, 0.01)) ++WrongHeight;
	}
	TestEqual(TEXT("墙脚的锚点一个都没长进屋里"), Inside, 0);
	// 没有地面采样器时一律落在房底（`FSite` 的契约）。落高本身在演示回归里才测得到。
	TestEqual(TEXT("没有地面采样器时锚点落在房底"), WrongHeight, 0);

	// 屋顶那两家不落地，且必须真的在屋面高度上（高度只能来自 `CSHouseRoof.h` 的求值器）。
	const float RidgeZ = CSHouseRoof_RidgeZ(Site.Roof);
	const float EaveZ = CSHouseRoof_EaveOuterZ(Site.Roof);
	TestTrue(TEXT("屋脊比檐口外沿高（求值器口径自洽）"), RidgeZ > EaveZ);
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		if (Anchor.Family == CSHouseDecor::EFamily::Ridge)
		{
			TestTrue(TEXT("屋脊锚点落在屋脊高度上"),
				FMath::IsNearlyEqual(float(Anchor.Location.Z), RidgeZ + Params.RoofStandOff, 0.01f));
			break;
		}
	}
	return true;
}

// -----------------------------------------------------------------------------
// ② 门：两侧 + 引道各出一个锚，而门口的墙脚被让开
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDecorGateKeepsDoorwayClearTest,
	"PCGPlugins.ComputeShaderGenerator.House.DecorGateKeepsDoorwayClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDecorGateKeepsDoorwayClearTest::RunTest(const FString& Parameters)
{
	CSHouseDecor::FSite Site = CSDecorTest_MakeSite(FVector2D(600.0, 400.0));
	const CSHouseDecor::FParams Params;

	TArray<CSHouseDecor::FAnchor> Bare;
	CSHouseDecor::BuildAnchors(Site, Params, Bare);
	const int32 BareWallFoot = CSDecorTest_CountFamily(Bare, CSHouseDecor::EFamily::WallFoot);

	const FCSWallOpening Door = CSDecorTest_MakeDoor(300.0f);
	Site.Openings.Add(Door);

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSHouseDecor::BuildAnchors(Site, Params, Anchors);

	// 一个门恰好出 4 个锚：洞两侧各一 + 门前引道两侧各一。**这就是"密度由锚点数量决定"** ——
	// 多开一个门就多 4 个摆件位，不需要任何场来告诉它"这里该热闹一点"。
	TestEqual(TEXT("一个门出 4 个锚点（两侧 + 引道两侧）"),
		CSDecorTest_CountFamily(Anchors, CSHouseDecor::EFamily::Gate), 4);

	// 门口正前方的墙脚必须让开 —— 不让的话一开门就是一堵桶，而实例计数照样是对的。
	const CSHouseVine::FWallStrip& South = Site.Strips[0];
	int32 InDoorway = 0;
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		if (Anchor.Family != CSHouseDecor::EFamily::WallFoot || Anchor.AnchorId / 65536 != 0) continue;
		const double S = FVector::DotProduct(Anchor.Location - South.Origin, South.U);
		if (S > Door.S0() - Params.WallFootHoleClearance && S < Door.S1() + Params.WallFootHoleClearance) ++InDoorway;
	}
	TestEqual(TEXT("门口净空里一个墙脚摆件都没有"), InDoorway, 0);
	TestTrue(TEXT("开门确实挤掉了墙脚锚点（否则上一条是空判据）"),
		CSDecorTest_CountFamily(Anchors, CSHouseDecor::EFamily::WallFoot) < BareWallFoot);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 密度 = 锚点个数：房子拉长，墙脚那一家按周长线性增长
//
// 这一条钉的是本模块与计划 D12 那半（复杂度场）**在语义上的分界**：锚点法里没有"阈值"
// 这个东西，多少件完全由构件多少决定。它同时守住容量：上限公式与这条增长律必须同源。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDecorDensityFollowsAnchorCountTest,
	"PCGPlugins.ComputeShaderGenerator.House.DecorDensityFollowsAnchorCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDecorDensityFollowsAnchorCountTest::RunTest(const FString& Parameters)
{
	const CSHouseDecor::FParams Params;

	TArray<CSHouseDecor::FAnchor> Small, Big;
	CSHouseDecor::BuildAnchors(CSDecorTest_MakeSite(FVector2D(600.0, 400.0)), Params, Small);
	CSHouseDecor::BuildAnchors(CSDecorTest_MakeSite(FVector2D(1200.0, 800.0)), Params, Big);

	const int32 SmallFoot = CSDecorTest_CountFamily(Small, CSHouseDecor::EFamily::WallFoot);
	const int32 BigFoot = CSDecorTest_CountFamily(Big, CSHouseDecor::EFamily::WallFoot);
	TestTrue(FString::Printf(TEXT("周长翻倍，墙脚锚点大致翻倍（%d → %d）"), SmallFoot, BigFoot),
		BigFoot >= SmallFoot * 3 / 2 && BigFoot <= SmallFoot * 5 / 2);

	return true;
}

// -----------------------------------------------------------------------------
// ④ 随机只由身份决定：重跑逐位相同，**平移整栋房子不改变任何一件的取舍**
//
// ⚠️ 这条是"绝不取 `InterlockedAdd` 槽位"那条纪律的可判定形式。槽位派生的随机在
// 重扫时会重掷（S1 栽过：画一笔路全场变色，**没有任何断言报红**）；位置派生的随机则会在
// 拖房子的每一帧重掷。两种都只能在这一层抓出来。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDecorIdentityIsStableTest,
	"PCGPlugins.ComputeShaderGenerator.House.DecorIdentityIsStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDecorIdentityIsStableTest::RunTest(const FString& Parameters)
{
	const CSHouseDecor::FParams Params;
	const TArray<CSHouseDecor::FPaletteRange> Ranges = CSDecorTest_MakeRanges();

	CSHouseDecor::FSite Site = CSDecorTest_MakeSite(FVector2D(600.0, 400.0));
	Site.Openings.Add(CSDecorTest_MakeDoor(300.0f));

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSHouseDecor::BuildAnchors(Site, Params, Anchors);

	CSHouseDecor::FPlan A, B;
	CSHouseDecor::BuildPlan(Anchors, Params, Ranges, 3, A);
	CSHouseDecor::BuildPlan(Anchors, Params, Ranges, 3, B);
	TestTrue(TEXT("同一份输入两次规划有东西可比"), A.TotalRecords() > 0);

	bool bBitwiseSame = A.ByPalette.Num() == B.ByPalette.Num();
	for (int32 P = 0; bBitwiseSame && P < A.ByPalette.Num(); ++P)
	{
		bBitwiseSame = A.ByPalette[P].Num() == B.ByPalette[P].Num();
		for (int32 I = 0; bBitwiseSame && I < A.ByPalette[P].Num(); ++I)
		{
			const CSHouseDecor::FRecord& RA = A.ByPalette[P][I];
			const CSHouseDecor::FRecord& RB = B.ByPalette[P][I];
			bBitwiseSame = RA.WorldPos == RB.WorldPos && RA.Facing == RB.Facing
				&& RA.Scale == RB.Scale && RA.ScaleZ == RB.ScaleZ && RA.Random01 == RB.Random01;
		}
	}
	TestTrue(TEXT("同一份输入两次规划逐位相同"), bBitwiseSame);

	// 平移整栋房子：锚点的**身份**不含位置，所以活下来的应当是同一批（记录只是跟着平移）。
	CSHouseDecor::FSite Moved = CSDecorTest_MakeSite(FVector2D(600.0, 400.0), FVector(1234.0, -777.0, 55.0));
	Moved.Openings.Add(CSDecorTest_MakeDoor(300.0f));
	TArray<CSHouseDecor::FAnchor> MovedAnchors;
	CSHouseDecor::BuildAnchors(Moved, Params, MovedAnchors);

	CSHouseDecor::FPlan C;
	CSHouseDecor::BuildPlan(MovedAnchors, Params, Ranges, 3, C);

	bool bSameSelection = A.ByPalette.Num() == C.ByPalette.Num();
	for (int32 P = 0; bSameSelection && P < A.ByPalette.Num(); ++P)
	{
		bSameSelection = A.ByPalette[P].Num() == C.ByPalette[P].Num();
		for (int32 I = 0; bSameSelection && I < A.ByPalette[P].Num(); ++I)
		{
			// 随机数逐位相同才说明它真的只由身份决定；位置当然跟着平移了，不比。
			bSameSelection = A.ByPalette[P][I].Random01 == C.ByPalette[P][I].Random01;
		}
	}
	TestTrue(TEXT("平移整栋房子不改变任何一件的取舍与随机数"), bSameSelection);
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ 窗户那两家**确实没被打开**（预留接口必须是惰性的）
//
// 它们要的是"已放置的窗"，而 D8 窗户整个卡在 C1。这条断言的作用是：将来有人顺手把
// `Type == Window` 也放进门那一家时，会当场红 —— 那不是"多长几个花箱"，那是在 C1 未拍板
// 的前提下开了一条窗户通路。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDecorWindowFamilyStaysInertTest,
	"PCGPlugins.ComputeShaderGenerator.House.DecorWindowFamilyStaysInert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDecorWindowFamilyStaysInertTest::RunTest(const FString& Parameters)
{
	CSHouseDecor::FSite Site = CSDecorTest_MakeSite(FVector2D(600.0, 400.0));
	const CSHouseDecor::FParams Params;

	FCSWallOpening Window = CSDecorTest_MakeDoor(300.0f, 120.0f);
	Window.Type = ECSOpeningType::Window;
	Window.Z0 = 110.0f;
	Window.Z1 = 230.0f;
	Site.Openings.Add(Window);

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSHouseDecor::BuildAnchors(Site, Params, Anchors);

	TestEqual(TEXT("窗不生产门那一家的锚点"),
		CSDecorTest_CountFamily(Anchors, CSHouseDecor::EFamily::Gate), 0);
	TestEqual(TEXT("窗户那一家一个锚点都不生产（等 C1）"),
		CSDecorTest_CountFamily(Anchors, CSHouseDecor::EFamily::Window), 0);

	// 就算有人给窗户那一家配了 palette，结果也必须一模一样（`FillChance` 恒 0）。
	TArray<CSHouseDecor::FPaletteRange> WithWindow = CSDecorTest_MakeRanges();
	WithWindow[int32(CSHouseDecor::EFamily::Window)] = { 0, 1 };
	CSHouseDecor::FPlan Armed, Bare;
	CSHouseDecor::BuildPlan(Anchors, Params, WithWindow, 3, Armed);
	CSHouseDecor::BuildPlan(Anchors, Params, CSDecorTest_MakeRanges(), 3, Bare);
	TestEqual(TEXT("给窗户那一家配 palette 不改变任何结果"), Armed.TotalRecords(), Bare.TotalRecords());
	TestEqual(TEXT("窗户那一家的填充概率恒 0"), Params.FillChance(CSHouseDecor::EFamily::Window), 0.0f);
	return true;
}

// -----------------------------------------------------------------------------
// ⑥ 道路排除：路面上的锚点被丢掉，摆件因此自动分列路的两侧
//
// TG 的对位物是 `PathRaster` 那条 mask 订阅（对照文档 §5.2 第 4 条）。**这不是场** ——
// 它只对已经存在的锚点做逐点否决，不产生任何候选。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDecorRoadRejectsAnchorsTest,
	"PCGPlugins.ComputeShaderGenerator.House.DecorRoadRejectsAnchors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDecorRoadRejectsAnchorsTest::RunTest(const FString& Parameters)
{
	const CSHouseDecor::FParams Params;
	CSHouseDecor::FSite Site = CSDecorTest_MakeSite(FVector2D(600.0, 400.0));
	Site.Openings.Add(CSDecorTest_MakeDoor(300.0f));

	TArray<CSHouseDecor::FAnchor> Bare;
	CSHouseDecor::BuildAnchors(Site, Params, Bare);

	// 一条沿 Y 走、宽 240 cm 的路，正好压在南墙门的正前方（门在 CenterS=300 ⇒ 局部 X=0）。
	Site.SampleRoadWeight = [](const FVector2D& XY) { return FMath::Abs(XY.X) < 120.0 ? 1.0f : 0.0f; };

	TArray<CSHouseDecor::FAnchor> WithRoad;
	CSHouseDecor::BuildAnchors(Site, Params, WithRoad);

	TestTrue(FString::Printf(TEXT("路面挡掉了一部分锚点（%d → %d）"), Bare.Num(), WithRoad.Num()),
		WithRoad.Num() < Bare.Num());

	int32 OnRoad = 0;
	for (const CSHouseDecor::FAnchor& Anchor : WithRoad)
	{
		// 屋顶那两家不受道路排除（鸟窝挂在檐口上，与地面无关），所以只查落地的两家。
		if (Anchor.Family == CSHouseDecor::EFamily::Eave || Anchor.Family == CSHouseDecor::EFamily::Ridge) continue;
		if (FMath::Abs(Anchor.Location.X) < 120.0) ++OnRoad;
	}
	TestEqual(TEXT("路面上一个落地的锚点都没剩下"), OnRoad, 0);

	// 门前引道那两个锚**必须活下来**（它们岔在路两边）—— 否则"道路两侧"那一家等于没做。
	int32 Approach = 0;
	for (const CSHouseDecor::FAnchor& Anchor : WithRoad)
	{
		if (Anchor.Family == CSHouseDecor::EFamily::Gate && Anchor.Location.Y < -200.0) ++Approach;
	}
	TestTrue(FString::Printf(TEXT("门前引道两侧的锚点还在（%d 个）"), Approach), Approach > 0);
	return true;
}

// -----------------------------------------------------------------------------
// ⑦ 容量上限是**纯配置量** —— 零阻塞纪律的可判定形式
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDecorFitsReservedCapacityTest,
	"PCGPlugins.ComputeShaderGenerator.House.DecorFitsReservedCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDecorFitsReservedCapacityTest::RunTest(const FString& Parameters)
{
	// `ACSHouseActor::EnsureDecorComponents` 按 `MaxRecordsBound` 一次预留容量，之后**永不扩容**。
	// 这条断言钉的就是那个上限真的是上限 —— 破了它的后果不是崩，而是交互期的某一帧突然付一次
	// 设备同步（或者摆件被静默截断），两种都很难归因。
	const CSHouseDecor::FParams Params;
	const FVector2D Footprint(600.0, 400.0);
	const float PierWidth = 40.0f;

	CSHouseDecor::FSite Site = CSDecorTest_MakeSite(Footprint);
	// 把南墙塞满门（墩宽 40 + 洞宽 160 ⇒ 间距 200），逼近上限公式里那一项。
	for (float S = 120.0f; S < 560.0f; S += 200.0f) Site.Openings.Add(CSDecorTest_MakeDoor(S));

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSHouseDecor::BuildAnchors(Site, Params, Anchors);

	const int32 Bound = CSHouseDecor::MaxRecordsBound(Footprint, Site.Roof.Overhang, PierWidth, Params);
	TestTrue(FString::Printf(TEXT("锚点装得下解析上限（%d ≤ %d）"), Anchors.Num(), Bound),
		Anchors.Num() <= Bound);

	// 上限也不该离谱地虚高：超过 8 倍说明公式与生产走岔了，而症状不是崩 —— 是显存白付，
	// 或者反过来（公式偏小）在某个尺寸上**静默截断**几件摆件。
	TestTrue(FString::Printf(TEXT("上限没有离谱虚高（%d ≤ 8 × %d）"), Bound, Anchors.Num()),
		Bound <= FMath::Max(Anchors.Num(), 1) * 8);

	// 记录数不可能超过锚点数（一个锚点最多一件）—— 这是"密度 = 锚点个数"的形式化。
	CSHouseDecor::FPlan Plan;
	CSHouseDecor::BuildPlan(Anchors, Params, CSDecorTest_MakeRanges(), 3, Plan);
	TestTrue(FString::Printf(TEXT("记录数不超过锚点数（%d ≤ %d）"), Plan.TotalRecords(), Anchors.Num()),
		Plan.TotalRecords() <= Anchors.Num());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
