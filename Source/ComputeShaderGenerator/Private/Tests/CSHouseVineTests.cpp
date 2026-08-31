#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSHouseActor.h"
#include "CSHouseProfile.h"
#include "CSHouseVine.h"
#include "Math/NumericLimits.h"

// -----------------------------------------------------------------------------
// 墙面藤蔓（D13）的验收。
//
// ⚠️ **这里的三条都是纯 CPU 判据，刻意不碰 RHI**：藤蔓最容易错的三件事
// （避不避墙洞、随机稳不稳、藤有没有跑出墙）在 GPU 那一侧只剩下一个实例计数，
// 到那时候什么都断言不了。GPU 那一侧的判据是**另一条**：
// `ACSHouseActor::IsVineDrawable`（渲染环节逐环检查）+ 出图脚本的像素门。
// 两者缺一不可 —— 石阶那个坑（`StairMesh` 恒 NULL、画面黑块、readback 全绿）
// 正是"只有数值判据"的后果。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSVineTest_ 前缀
// （与 CSHouseVine.cpp 的 CSHouseVine_、CSHouseLogicTests 的 CSHouseTest_ 都不同）。

constexpr float CSVineTest_Length = 600.0f;
constexpr float CSVineTest_Height = 300.0f;

/** 一面沿 +X 的墙，墙脚在原点，外法线 −Y。四面墙里最简单的那一面，够钉所有几何判据。 */
CSHouseVine::FWallStrip CSVineTest_MakeStrip(int32 EdgeIndex = 0)
{
	CSHouseVine::FWallStrip Strip;
	Strip.EdgeIndex = EdgeIndex;
	Strip.Origin = FVector(0.0, 0.0, 0.0);
	Strip.U = FVector(1.0, 0.0, 0.0);
	Strip.Up = FVector(0.0, 0.0, 1.0);
	Strip.N = FVector(0.0, -1.0, 0.0);
	Strip.Length = CSVineTest_Length;
	Strip.Height = CSVineTest_Height;
	return Strip;
}

CSHouseVine::FParams CSVineTest_MakeParams()
{
	CSHouseVine::FParams P;   // 默认值 = ACSHouseActor 的出厂值
	return P;
}

/** 世界点 → 墙面参数 (s, z)。墙脚在原点、U = +X、Up = +Z ⇒ 直接取分量。 */
FVector2D CSVineTest_ToWall(const FVector3f& World)
{
	return FVector2D(World.X, World.Z);
}

/**
 * (s, z) 真的落在洞形里？**直接调材质那份判据**（`CSHouse_ClipKeeps`），不是洞的外接矩形。
 *
 * ⚠️ 这一条是有意与被测代码"同源"的：被测的是"藤让开的地方与墙被切掉的地方**是不是同一块**"，
 * 所以判据必须是墙那边的那一份。另写一份近似的反而会把两边的分歧掩盖掉。
 */
bool CSVineTest_InArchProfile(const FCSWallOpening& Opening, const FVector2D& SZ)
{
	if (SZ.Y < Opening.Z0) return false;
	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Opening);
	return !CSHouse_ClipKeeps(Field, Field.Eval(float(SZ.X), float(SZ.Y)));
}

/** 洞的**外接矩形**里（第一档的判据）。拱肩那两块在矩形里、却是实心墙。 */
bool CSVineTest_InOpeningRect(const FCSWallOpening& Opening, const FVector2D& SZ)
{
	return SZ.X >= Opening.S0() && SZ.X <= Opening.S1() && SZ.Y >= Opening.Z0 && SZ.Y <= Opening.Z1;
}

/** 墙正中一个 200 cm 宽、220 cm 高的落地拱门。 */
FCSWallOpening CSVineTest_MakeDoor(float CenterS = CSVineTest_Length * 0.5f, float Width = 200.0f)
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
}

// -----------------------------------------------------------------------------
// ① 藤长在墙上，而且长在墙的**外**面
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineOnWallTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineStaysOnWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineOnWallTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);

	TestTrue(TEXT("一面 6 m × 3 m 的墙上排得出藤"), Plan.Branch.Num() > 20);
	TestTrue(TEXT("排得出叶子"), Plan.Leaf.Num() > 5);

	// 藤脚**必须**贴着墙脚：整根藤是从 z=0 长上去的，第一段的起点就是根。
	// 这一条会在"起点被错误地写成段中点"时报红 —— 症状是整片藤悬空半段。
	float MinZ = TNumericLimits<float>::Max();
	float MaxZ = -TNumericLimits<float>::Max();
	float MinS = TNumericLimits<float>::Max();
	float MaxS = -TNumericLimits<float>::Max();
	float MaxOffWall = 0.0f;
	for (const CSHouseVine::FRecord& R : Plan.Branch)
	{
		const FVector2D SZ = CSVineTest_ToWall(R.WorldPos);
		MinS = FMath::Min(MinS, float(SZ.X));
		MaxS = FMath::Max(MaxS, float(SZ.X));
		MinZ = FMath::Min(MinZ, float(SZ.Y));
		MaxZ = FMath::Max(MaxZ, float(SZ.Y));
		// 离墙距离 = −Y（外法线是 −Y）。整片藤都应当正好在 StandOff 上。
		MaxOffWall = FMath::Max(MaxOffWall, FMath::Abs(-R.WorldPos.Y - Params.StandOff));
	}

	TestEqual(TEXT("藤脚正好落在墙脚（z = 0）"), MinZ, 0.0f, 0.001f);
	TestTrue(FString::Printf(TEXT("藤爬得上去（最高 %.1f cm，墙高 %.0f）"), MaxZ, CSVineTest_Height),
		MaxZ > CSVineTest_Height * 0.4f);
	TestTrue(FString::Printf(TEXT("藤不许爬出墙顶（最高 %.1f cm ≤ %.0f）"), MaxZ, CSVineTest_Height),
		MaxZ <= CSVineTest_Height + 0.001f);
	TestTrue(FString::Printf(TEXT("藤不许跑出墙的左端（最小 s = %.1f）"), MinS), MinS >= -0.001f);
	TestTrue(FString::Printf(TEXT("藤不许跑出墙的右端（最大 s = %.1f ≤ %.0f）"), MaxS, CSVineTest_Length),
		MaxS <= CSVineTest_Length + 0.001f);
	TestEqual(TEXT("整片藤都贴在墙外皮上（离墙距离恒 = StandOff）"), MaxOffWall, 0.0f, 0.01f);

	// 负缝：段的**渲染长度**必须比它跨过的几何距离长（Bloat > 1），否则每个折点都会露一条亮缝。
	// 这是门框砖那条 TG 实证（`FrameBrickOverlap`）在藤上的同构版本。
	int32 PositiveSeam = 0;
	for (const CSHouseVine::FRecord& R : Plan.Branch)
	{
		if (R.LengthScale <= Params.SegmentLength * 1.0001f) ++PositiveSeam;
	}
	TestEqual(TEXT("没有任何一段是正缝（渲染长度 > 几何段长）"), PositiveSeam, 0);

	return true;
}

// -----------------------------------------------------------------------------
// ② 逐实例随机只由身份决定 —— "不许拿 InterlockedAdd 的槽位当种子"那条纪律的执行面
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineIdentityTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineRandomIsIdentityBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineIdentityTest::RunTest(const FString& Parameters)
{
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	// ⓐ 同一份输入两次规划**逐位相同**。规划是纯函数，所以这一条是可判定的；
	//    S1 那个 bug（种子取槽位）在这条断言下依然会绿 —— 所以还有 ⓑ。
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	CSHouseVine::FPlan A, B;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, A);
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, B);
	TestEqual(TEXT("两次规划的段数相同"), B.Branch.Num(), A.Branch.Num());
	int32 Differ = 0;
	for (int32 i = 0; i < FMath::Min(A.Branch.Num(), B.Branch.Num()); ++i)
	{
		if (A.Branch[i].Random01 != B.Branch[i].Random01) ++Differ;
	}
	TestEqual(TEXT("两次规划的逐实例随机逐位相同"), Differ, 0);

	// ⓑ **这才是那条纪律**：把第 0 面墙换成"更长的墙"⇒ 段数变、槽位全体重排，
	//    但**同一根藤的同一段**必须拿到同一个随机数。取槽位的话这一条必红。
	TArray<CSHouseVine::FWallStrip> Longer;
	CSHouseVine::FWallStrip Wide = CSVineTest_MakeStrip();
	Wide.Length = CSVineTest_Length * 2.0f;
	Longer.Add(Wide);
	CSHouseVine::FPlan C;
	CSHouseVine::BuildPlan(Longer, TArray<FCSWallOpening>(), Params, C);
	TestTrue(TEXT("更长的墙确实排出了更多段（槽位一定重排过）"), C.Branch.Num() > A.Branch.Num());

	// 直接对身份哈希本身取样：同一身份恒等、不同身份必不同。
	const uint32 H0 = CSHouseVine::IdentityHash(0, 3, 7, 71u, Params.Seed);
	const uint32 H1 = CSHouseVine::IdentityHash(0, 3, 7, 71u, Params.Seed);
	TestEqual(TEXT("同一身份两次取样恒等"), H1, H0);
	TestNotEqual(TEXT("换一根藤就换一个随机"), CSHouseVine::IdentityHash(0, 4, 7, 71u, Params.Seed), H0);
	TestNotEqual(TEXT("换一段就换一个随机"), CSHouseVine::IdentityHash(0, 3, 8, 71u, Params.Seed), H0);
	TestNotEqual(TEXT("换一面墙就换一个随机"), CSHouseVine::IdentityHash(1, 3, 7, 71u, Params.Seed), H0);
	TestNotEqual(TEXT("换用户种子就换一个随机"), CSHouseVine::IdentityHash(0, 3, 7, 71u, Params.Seed + 1), H0);

	// 随机数必须真的铺满 [0,1)：一个恒定的"随机"同样能让上面几条全绿。
	float Lo = 1.0f, Hi = 0.0f;
	for (const CSHouseVine::FRecord& R : A.Branch) { Lo = FMath::Min(Lo, R.Random01); Hi = FMath::Max(Hi, R.Random01); }
	TestTrue(FString::Printf(TEXT("随机数铺得开（[%.3f, %.3f]）"), Lo, Hi), Hi - Lo > 0.8f);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 藤避让墙洞 —— TG 的 `ivy_grower` 读集里有 `PrevWallHoles`（对照文档 §6.1【确凿】）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineAvoidsHolesTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineAvoidsWallHoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineAvoidsHolesTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	const FCSWallOpening Door = CSVineTest_MakeDoor();
	TArray<FCSWallOpening> Openings;
	Openings.Add(Door);

	CSHouseVine::FPlan Clear, Holed;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Clear);
	CSHouseVine::BuildPlan(Strips, Openings, Params, Holed);

	// ⓐ 洞**形**里（拱 = 矩形下身 + 半圆顶，不含外扩带）一段都不许有。
	// ⚠️ 判据从"洞的外接矩形"换成了 `CSHouse_ClipKeeps` 那条曲线（2026-08-31，第二档）：
	// 拱肩那两块在外接矩形里、却是**实心墙**，材质也确实把它们画出来。拿矩形当判据的话
	// 下面 ⓔ 那条改进就无法成立（它恰恰要求藤长到拱肩上去）。
	int32 Inside = 0;
	for (const CSHouseVine::FRecord& R : Holed.Branch)
	{
		if (CSVineTest_InArchProfile(Door, CSVineTest_ToWall(R.WorldPos))) ++Inside;
	}
	TestEqual(TEXT("门洞里一段藤都没有"), Inside, 0);

	// ⓑ 对照组：没有洞的时候那块地方**本来是有藤的** —— 否则 ⓐ 是恒真的空话。
	int32 WouldHaveBeen = 0;
	for (const CSHouseVine::FRecord& R : Clear.Branch)
	{
		if (CSVineTest_InArchProfile(Door, CSVineTest_ToWall(R.WorldPos))) ++WouldHaveBeen;
	}
	TestTrue(FString::Printf(TEXT("没洞时那块地方本来有 %d 段藤（否则 ⓐ 是空话）"), WouldHaveBeen),
		WouldHaveBeen > 0);

	// ⓒ 洞只吃掉洞附近的藤，不是把整面墙的藤都干掉。
	TestTrue(FString::Printf(TEXT("洞外的藤还在（%d / %d）"), Holed.Branch.Num(), Clear.Branch.Num()),
		Holed.Branch.Num() > Clear.Branch.Num() / 3);

	// ⓓ 叶子也一起避让 —— 叶挂在段中点上，段没了叶自然没了；这一条钉的是"没有漏网的叶"。
	int32 LeavesInside = 0;
	for (const CSHouseVine::FRecord& R : Holed.Leaf)
	{
		if (CSVineTest_InArchProfile(Door, CSVineTest_ToWall(R.WorldPos))) ++LeavesInside;
	}
	TestEqual(TEXT("门洞里一片叶子都没有"), LeavesInside, 0);

	// ⓔ 拱肩（拱脚线以上、外接矩形以内、拱形以外）是**实心墙**，把判据从外接矩形换成解析
	// clip 场之后藤可以长上去。
	//
	// ⚠️ **这一条只出数、不设门**，理由与状态文件里裙边噪声那条 `pair3q` 同型：信号量到不了
	// 能与噪声分开的量级。实测过三种形状，肩上分别只有 0 / 1 / 2 段 —— 因为 12 cm 外扩
	// 把拱肩上"藤够得着"的那部分几乎吃光了（高拱：半宽 100、拱脚 120 ⇒ 外扩后半径 112，
	// 自由带在 Z=220 处宽 49.6 cm、Z=180 处只剩 5.4 cm，往下直接闭合，而藤是从墙脚爬上来的）。
	// ⇒ **换判据这件事本身几乎不改变画面**，别把它当成"拱附近变稀"的解药 ——
	//    那条的解药是藤脚侧移（见 VineRootEscapesHoles，实测保留率 0.663）。
	//    换判据的价值在**一致性**：藤让开的地方与墙真被切掉的地方从此是同一条曲线（裁决三），
	//    那一条由 VineHoleFieldMatchesClipField 的 7381 个取样点守着。
	// 一面 12 m 长的墙 + 一个纯半圆拱（宽 400 ⇒ 拱脚落到 Z1 − 半宽 = 0）：肩是两块
	// 从拱脚一路开到洞顶的大楔形，藤贴着拱缘爬上去正好落在里面。
	FCSWallOpening Squat = CSVineTest_MakeDoor(600.0f, 400.0f);
	Squat.Z1 = 200.0f;
	TArray<FCSWallOpening> SquatOnly;
	SquatOnly.Add(Squat);
	TArray<CSHouseVine::FWallStrip> LongWall;
	{
		CSHouseVine::FWallStrip Wide = CSVineTest_MakeStrip();
		Wide.Length = 1200.0f;
		LongWall.Add(Wide);
	}
	CSHouseVine::FPlan SquatPlan;
	CSHouseVine::BuildPlan(LongWall, SquatOnly, Params, SquatPlan);

	int32 OnShoulder = 0, InSquatHole = 0;
	for (const CSHouseVine::FRecord& R : SquatPlan.Branch)
	{
		const FVector2D SZ = CSVineTest_ToWall(R.WorldPos);
		if (CSVineTest_InArchProfile(Squat, SZ)) ++InSquatHole;
		else if (CSVineTest_InOpeningRect(Squat, SZ)) ++OnShoulder;
	}
	TestEqual(TEXT("半圆拱的洞形里也一段藤都没有"), InSquatHole, 0);
	AddInfo(FString::Printf(TEXT("拱肩（矩形内、拱形外）上 %d 段 —— 只记录，不设门（见上）"), OnShoulder));
	return true;
}

// -----------------------------------------------------------------------------
// ⑩ 洞外扩量不许小于门框砖伸出去的那一半 —— "要不要再放宽"这个问题的可判定形式
//
// `VineHoleClearance` 存在的**唯一**理由是别让藤穿进门框砖：那一列砖骑在墩心、横向占
// `FrameBrickDepth`（默认 20 cm）⇒ 它伸到洞缘外 10 cm。外扩小于 10 就等于让藤长进砖里，
// 而症状是"拱缘上一圈藤和砖互相穿插"——只在贴脸看时才看得出来，出图的差异率读不出来。
// 所以"能不能再放宽"这件事有一条硬下界，本条把它钉住，省得将来有人为了让拱周围密一点
// 顺手把它调小。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineClearanceCoversFrameTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineClearanceCoversFrameBricks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineClearanceCoversFrameTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* Defaults = GetDefault<ACSHouseActor>();
	if (!Defaults) { AddError(TEXT("拿不到 ACSHouseActor 的 CDO")); return false; }

	const float Reach = Defaults->FrameBrickDepth * 0.5f;
	TestTrue(FString::Printf(TEXT("洞外扩 %.1f cm ≥ 门框砖伸出的 %.1f cm"),
		Defaults->VineHoleClearance, Reach), Defaults->VineHoleClearance >= Reach);
	// 也别虚高：外扩每多 1 cm，拱周围就少一圈藤。三倍是"还算得上贴着砖"的上界。
	TestTrue(FString::Printf(TEXT("洞外扩 %.1f cm 没有虚高到 3 × %.1f cm 以上"),
		Defaults->VineHoleClearance, Reach), Defaults->VineHoleClearance <= Reach * 3.0f);
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ 洞判据与材质那份 clip 场**逐点一致** —— 裁决三（渲染层挖洞）的一条可判定形式
//
// 洞是逐像素 discard 切出来的，所以"墙在哪里没了"的唯一真源是 `CSHouse_ClipKeeps`。
// 藤要是自己另写一份近似（第一档就是外接矩形），两边的分歧不会有任何断言报红 ——
// 只会在画面上表现成"拱周围一圈莫名其妙的秃斑"，而所有计数照绿。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineHoleFieldParityTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineHoleFieldMatchesClipField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineHoleFieldParityTest::RunTest(const FString& Parameters)
{
	TArray<FCSWallOpening> Openings;
	Openings.Add(CSVineTest_MakeDoor());
	{
		FCSWallOpening Window = CSVineTest_MakeDoor(120.0f, 90.0f);
		Window.Type = ECSOpeningType::Window;
		Window.Shape = ECSOpeningShape::Rect;
		Window.Z0 = 110.0f;
		Window.Z1 = 200.0f;
		Openings.Add(Window);
	}

	// Clearance = 0 时两份判据必须**逐点相同**。取样格刻意跨过拱脚线与窗台，
	// 那两条正是两种形状各自的分段点。
	int32 Mismatch = 0;
	int32 InsideCount = 0;
	for (int32 IS = 0; IS <= 120; ++IS)
	{
		for (int32 IZ = 0; IZ <= 60; ++IZ)
		{
			const float S = float(IS) * (CSVineTest_Length / 120.0f);
			const float Z = float(IZ) * (CSVineTest_Height / 60.0f);
			bool bReference = false;
			for (const FCSWallOpening& O : Openings) bReference |= CSVineTest_InArchProfile(O, FVector2D(S, Z));
			const bool bVine = CSHouseVine::IsInsideOpening(Openings, 0, S, Z, 0.0f);
			if (bVine != bReference) ++Mismatch;
			if (bReference) ++InsideCount;
		}
	}
	TestTrue(FString::Printf(TEXT("取样格上真的有点落在洞里（%d 个，否则这条是空话）"), InsideCount),
		InsideCount > 100);
	TestEqual(TEXT("藤的洞判据与材质的 clip 场逐点一致"), Mismatch, 0);

	// 外扩把洞胀大**而不是把结论外扩**：拱胀大以后拱脚线逐位不变。
	// 破了它的症状是拱一开外扩就整体上移，藤在拱顶留一条月牙形秃带。
	FCSWallOpening Door = CSVineTest_MakeDoor();
	const float SpringZ = Door.Z1 - Door.HalfWidth();
	TestTrue(TEXT("拱脚线正上方一点点、洞外半宽 + 一半外扩处仍算洞内"),
		CSHouseVine::IsInsideOpening(Openings, 0, Door.CenterS + Door.HalfWidth() + 6.0f, SpringZ - 1.0f, 12.0f));
	TestFalse(TEXT("同一高度上、洞外半宽 + 两倍外扩处已经在洞外"),
		CSHouseVine::IsInsideOpening(Openings, 0, Door.CenterS + Door.HalfWidth() + 24.0f, SpringZ - 1.0f, 12.0f));
	// 洞底以下是实心（窗台那一截）：Arch 的 clip 场在拱脚线以下无下界，这条边界只能自己补。
	FCSWallOpening Sill = CSVineTest_MakeDoor(120.0f, 90.0f);
	Sill.Z0 = 110.0f;
	Sill.Z1 = 200.0f;
	TArray<FCSWallOpening> Only;
	Only.Add(Sill);
	TestFalse(TEXT("拱形窗的窗台以下是实心墙"), CSHouseVine::IsInsideOpening(Only, 0, Sill.CenterS, 40.0f, 12.0f));
	TestTrue(TEXT("拱形窗的洞口里是洞"), CSHouseVine::IsInsideOpening(Only, 0, Sill.CenterS, 140.0f, 12.0f));
	return true;
}

// -----------------------------------------------------------------------------
// ⑥ 藤脚落在洞里时**侧移**而不是整根丢掉
//
// 第一档："藤脚在洞里 ⇒ 这根不长"。落地拱把整个洞宽的墙脚都吃掉，所以拱越多秃得越狠。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineRootEscapesHolesTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineRootEscapesHoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineRootEscapesHolesTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	// 三个落地拱（演示房子那面长墙的形状）。
	TArray<FCSWallOpening> Openings;
	for (int32 K = 0; K < 3; ++K) Openings.Add(CSVineTest_MakeDoor(120.0f + 180.0f * float(K), 150.0f));

	CSHouseVine::FPlan Clear, Holed;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Clear);
	CSHouseVine::BuildPlan(Strips, Openings, Params, Holed);

	// 洞形里仍然一段都没有 —— 放宽不许换来穿帮。
	int32 Inside = 0;
	for (const CSHouseVine::FRecord& R : Holed.Branch)
	{
		for (const FCSWallOpening& O : Openings)
		{
			if (CSVineTest_InArchProfile(O, CSVineTest_ToWall(R.WorldPos))) { ++Inside; break; }
		}
	}
	TestEqual(TEXT("三个拱全开时洞形里一段藤都没有"), Inside, 0);

	// 藤脚必须**贴地**：侧移只沿墙脚挪，不许把根抬到半空（那会长出一片悬空的藤）。
	float MinZ = TNumericLimits<float>::Max();
	for (const CSHouseVine::FRecord& R : Holed.Branch) MinZ = FMath::Min(MinZ, R.WorldPos.Z);
	TestEqual(TEXT("侧移过的藤脚仍然落在墙脚（z = 0）"), MinZ, 0.0f, 0.001f);

	// 三个拱吃掉 450 / 600 cm 的墙脚（含外扩后自由墙脚只剩 78 / 600 cm ≈ 13%），
	// 藤仍应保住**大部分**段数。阈值 0.45 的依据是两头都够远：
	//   · 实测 0.663（本条自己 AddInfo 出来的数，重跑即可核对）；
	//   · 第一档的同一场景在解析上**不可能**超过 13% —— 它的规则是"藤脚在洞里就整根不长"，
	//     而自由墙脚只有 13%。所以 0.45 既在实测之下有余量，又远在第一档之上，不会两头都松。
	const float Kept = float(Holed.Branch.Num()) / FMath::Max(float(Clear.Branch.Num()), 1.0f);
	AddInfo(FString::Printf(TEXT("三拱全开保留率 %.3f（%d / %d 段）"),
		Kept, Holed.Branch.Num(), Clear.Branch.Num()));
	TestTrue(FString::Printf(TEXT("三个落地拱之下藤不会成片秃掉（保留 %.1f%%）"), Kept * 100.0f), Kept > 0.45f);
	return true;
}

// -----------------------------------------------------------------------------
// ⑦ 转角跨墙（TG 的 `check_for_wall_jump`）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineWallJumpTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineJumpsAtCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineWallJumpTest::RunTest(const FString& Parameters)
{
	// 一圈四面墙，摆成一个真的矩形（这一条必须有真几何：跨墙那一段的法线取两面墙的平均，
	// 四面墙叠在一起的话这条断言恒真）。
	const double HX = 300.0, HY = 200.0, T = 24.0;
	auto MakeRing = [&](TArray<CSHouseVine::FWallStrip>& Out)
	{
		Out.Reset();
		const FVector2D Starts[4] = { {-HX, -HY}, {HX, -HY + T}, {HX, HY}, {-HX, HY - T} };
		const FVector2D Us[4] = { {1, 0}, {0, 1}, {-1, 0}, {0, -1} };
		const FVector2D Ns[4] = { {0, -1}, {1, 0}, {0, 1}, {-1, 0} };
		const double Lens[4] = { 2 * HX, 2 * HY - 2 * T, 2 * HX, 2 * HY - 2 * T };
		for (int32 E = 0; E < 4; ++E)
		{
			CSHouseVine::FWallStrip Strip;
			Strip.EdgeIndex = E;
			Strip.Origin = FVector(Starts[E].X, Starts[E].Y, 0.0);
			Strip.U = FVector(Us[E].X, Us[E].Y, 0.0);
			Strip.Up = FVector::UpVector;
			Strip.N = FVector(Ns[E].X, Ns[E].Y, 0.0);
			Strip.Length = float(Lens[E]);
			Strip.Height = CSVineTest_Height;
			Out.Add(Strip);
		}
	};

	TArray<CSHouseVine::FWallStrip> Strips;
	MakeRing(Strips);

	CSHouseVine::FParams Jump = CSVineTest_MakeParams();
	CSHouseVine::FParams NoJump = CSVineTest_MakeParams();
	NoJump.JumpChance = 0.0f;

	CSHouseVine::FPlan WithJump, Without;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Jump, WithJump);
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), NoJump, Without);

	// 跨墙那一段的截面法线是**两面墙法线的平均**（45°斜角），同墙段则逐位等于墙法线。
	// 数"斜角段"就能判定真的跨过墙了，而且这个量在关掉跨墙时必须恰好是 0。
	auto CountDiagonal = [](const CSHouseVine::FPlan& Plan)
	{
		int32 N = 0;
		for (const CSHouseVine::FRecord& R : Plan.Branch)
		{
			// 轴对齐的墙法线只有一个非零分量；平均出来的斜角两个分量都 ≈ 0.707。
			if (FMath::Abs(R.Normal.X) > 0.2f && FMath::Abs(R.Normal.Y) > 0.2f) ++N;
		}
		return N;
	};
	const int32 Diagonal = CountDiagonal(WithJump);
	AddInfo(FString::Printf(TEXT("跨墙段 %d / %d"), Diagonal, WithJump.Branch.Num()));
	TestTrue(FString::Printf(TEXT("开着跨墙时真的有藤绕过转角（%d 段）"), Diagonal), Diagonal > 0);
	TestEqual(TEXT("关掉跨墙就一段都不许跨"), CountDiagonal(Without), 0);

	// 跨墙不许把藤送出这一圈墙之外：所有段仍在 footprint 的外皮上（含 StandOff）。
	const double Limit = FMath::Max(HX, HY) + Jump.StandOff + 1.0;
	int32 OutOfRing = 0;
	for (const CSHouseVine::FRecord& R : WithJump.Branch)
	{
		if (FMath::Abs(R.WorldPos.X) > Limit || FMath::Abs(R.WorldPos.Y) > Limit) ++OutOfRing;
	}
	TestEqual(TEXT("跨墙以后藤仍然贴在这一圈墙上"), OutOfRing, 0);

	// 身份稳定性：跨不跨墙由身份哈希决定 ⇒ 两次规划仍必须逐位相同。
	CSHouseVine::FPlan Again;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Jump, Again);
	TestEqual(TEXT("开着跨墙时两次规划的段数仍相同"), Again.Branch.Num(), WithJump.Branch.Num());
	int32 Differ = 0;
	for (int32 I = 0; I < FMath::Min(Again.Branch.Num(), WithJump.Branch.Num()); ++I)
	{
		if (Again.Branch[I].Random01 != WithJump.Branch[I].Random01) ++Differ;
	}
	TestEqual(TEXT("开着跨墙时逐实例随机仍逐位相同"), Differ, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ⑧ 山墙三角：墙顶是 S 的函数，藤能爬过檐口高度
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineClimbsGableTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineClimbsGable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineClimbsGableTest::RunTest(const FString& Parameters)
{
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	CSHouseVine::FWallStrip Flat = CSVineTest_MakeStrip();
	CSHouseVine::FWallStrip Gable = CSVineTest_MakeStrip();
	// 脊沿墙中，坡度 35°（演示房子的默认值）。
	Gable.GableTan = FMath::Tan(FMath::DegreesToRadians(35.0f));
	Gable.GablePeakS = CSVineTest_Length * 0.5f;
	Gable.GableHalfSpan = CSVineTest_Length * 0.5f;

	TestEqual(TEXT("檐墙的墙顶是平的"), Flat.TopAt(CSVineTest_Length * 0.5f), CSVineTest_Height, 0.001f);
	TestEqual(TEXT("山墙两端回到檐口高"), Gable.TopAt(0.0f), CSVineTest_Height, 0.001f);
	const float PeakTop = CSVineTest_Height + Gable.GableTan * Gable.GableHalfSpan;
	TestEqual(TEXT("山尖处的墙顶 = 脊高"), Gable.TopAt(Gable.GablePeakS), PeakTop, 0.001f);

	TArray<CSHouseVine::FWallStrip> FlatOnly, GableOnly;
	FlatOnly.Add(Flat);
	GableOnly.Add(Gable);
	CSHouseVine::FPlan FlatPlan, GablePlan;
	CSHouseVine::BuildPlan(FlatOnly, TArray<FCSWallOpening>(), Params, FlatPlan);
	CSHouseVine::BuildPlan(GableOnly, TArray<FCSWallOpening>(), Params, GablePlan);

	float FlatMaxZ = 0.0f, GableMaxZ = 0.0f;
	int32 AboveEave = 0, OverTop = 0;
	for (const CSHouseVine::FRecord& R : FlatPlan.Branch) FlatMaxZ = FMath::Max(FlatMaxZ, R.WorldPos.Z);
	for (const CSHouseVine::FRecord& R : GablePlan.Branch)
	{
		const FVector2D SZ = CSVineTest_ToWall(R.WorldPos);
		GableMaxZ = FMath::Max(GableMaxZ, float(SZ.Y));
		if (SZ.Y > CSVineTest_Height + 0.001f) ++AboveEave;
		// **每一段都不许越过它自己那个 S 处的墙顶** —— 只看最高点的话，山尖那一根合格就
		// 掩盖了两端翻出屋面的那些（"上一轮第一版判据拉错轴、把缺陷掩盖了"的同一类错）。
		if (SZ.Y > Gable.TopAt(float(SZ.X)) + 0.001f) ++OverTop;
	}
	TestTrue(FString::Printf(TEXT("檐墙的藤停在檐口（最高 %.1f）"), FlatMaxZ), FlatMaxZ <= CSVineTest_Height + 0.001f);
	TestTrue(FString::Printf(TEXT("山墙上真的有藤爬过了檐口（%d 段，最高 %.1f）"), AboveEave, GableMaxZ),
		AboveEave > 0);
	TestEqual(TEXT("山墙上没有一段翻出那条斜边"), OverTop, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ⑨ 花（`ivy_flower`）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineFlowersTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineFlowers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineFlowersTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	CSHouseVine::FParams Params = CSVineTest_MakeParams();
	Params.FlowerChance = 1.0f;   // 概率钉死，判据才不会因为掷不到而空转

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
	TestTrue(FString::Printf(TEXT("开得出花（%d 朵）"), Plan.Flower.Num()), Plan.Flower.Num() > 0);
	TestTrue(TEXT("花比枝少（只开在藤的上半截）"), Plan.Flower.Num() < Plan.Branch.Num());

	// 花簇朝外上方张开、基准向量不与朝向共线 —— 后者共线的话 kernel 的退化判据会把它整批丢掉，
	// 而 counter 照样是对的（"实例数对得上但屏幕上什么都没有"）。
	int32 Degenerate = 0, PointingIn = 0, TooLow = 0;
	const float FromZ = float(FMath::CeilToInt(float(Params.MaxSegments) * Params.FlowerFromFrac)) * Params.SegmentLength
		* FMath::Cos(Params.MaxLean);
	for (const CSHouseVine::FRecord& R : Plan.Flower)
	{
		const FVector3f Dir = R.Dir.GetSafeNormal();
		const FVector3f Ref = R.Normal.GetSafeNormal();
		if (Dir.IsNearlyZero() || Ref.IsNearlyZero() || FMath::Abs(FVector3f::DotProduct(Dir, Ref)) > 0.999f) ++Degenerate;
		// 墙的外法线是 −Y，花该朝外 ⇒ Dir.Y 必须为负。
		if (Dir.Y > -0.3f) ++PointingIn;
		if (R.WorldPos.Z < FromZ - 0.001f) ++TooLow;
	}
	TestEqual(TEXT("没有一朵花的基与朝向共线（共线会被 kernel 静默丢掉）"), Degenerate, 0);
	TestEqual(TEXT("每一朵花都朝屋外张开"), PointingIn, 0);
	TestEqual(TEXT("没有花开在藤的下半截"), TooLow, 0);

	// 概率是 0 时一朵都不许有 —— 也是"没配花网格 ⇒ 不排花记录"那条的执行面。
	Params.FlowerChance = 0.0f;
	CSHouseVine::FPlan NoFlower;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, NoFlower);
	TestEqual(TEXT("概率为 0 时一朵花都没有"), NoFlower.Flower.Num(), 0);
	TestEqual(TEXT("关掉花不影响枝"), NoFlower.Branch.Num(), Plan.Branch.Num());

	// 花也避让墙洞（它挂在段末端上，段没了花自然没了）。
	TArray<FCSWallOpening> Openings;
	Openings.Add(CSVineTest_MakeDoor());
	Params.FlowerChance = 1.0f;
	CSHouseVine::FPlan Holed;
	CSHouseVine::BuildPlan(Strips, Openings, Params, Holed);
	int32 InHole = 0;
	for (const CSHouseVine::FRecord& R : Holed.Flower)
	{
		if (CSVineTest_InArchProfile(Openings[0], CSVineTest_ToWall(R.WorldPos))) ++InHole;
	}
	TestEqual(TEXT("门洞里一朵花都没有"), InHole, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ④ 容量上限是**纯配置量** —— 零阻塞纪律的可判定形式
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineCapacityBoundTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineFitsReservedCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineCapacityBoundTest::RunTest(const FString& Parameters)
{
	// `ACSHouseActor::EnsureVineComponents` 按 (周长 / 间距 + 4) × MaxSegments 一次预留容量，
	// 之后**永不扩容**。这条断言钉的就是那个上限真的是上限 —— 破了它的后果不是崩，
	// 而是交互期的某一帧突然付一次设备同步（或者藤被静默截断），两种都很难归因。
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();
	const double FootprintX = 600.0, FootprintY = 400.0, WallThickness = 24.0;

	TArray<CSHouseVine::FWallStrip> Strips;
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		CSHouseVine::FWallStrip Strip = CSVineTest_MakeStrip(Edge);
		Strip.Length = float((Edge & 1) ? FootprintY - 2 * WallThickness : FootprintX);
		Strip.Height = CSVineTest_Height;
		// 四面墙摆在不同位置/朝向不影响计数，这里只关心长度。
		Strips.Add(Strip);
	}

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);

	const double Perimeter = 2.0 * (FootprintX + FootprintY);
	const int32 MaxStrands = FMath::CeilToInt(Perimeter / FMath::Max(Params.StrandSpacing, 20.0f)) + 4;
	// 解析上限（未量化）。`ACSHouseActor::EnsureVineComponents` 再把它过一遍
	// `CSShaperSteps::ReserveCount`（×1.5 对齐 4096）才交给 ReserveCapacity —— 那一步是为了
	// **拖尺寸时不重新分配**（上限是周长的连续函数），与这里要钉的"上限真的是上限"是两回事。
	const int32 Bound = MaxStrands * FMath::Clamp(Params.MaxSegments, 1, 128);

	TestTrue(FString::Printf(TEXT("枝装得下解析上限（%d ≤ %d）"), Plan.Branch.Num(), Bound),
		Plan.Branch.Num() <= Bound);
	TestTrue(FString::Printf(TEXT("叶装得下解析上限（%d ≤ %d）"), Plan.Leaf.Num(), Bound),
		Plan.Leaf.Num() <= Bound);
	// 上限也不该离谱地虚高：超过 4 倍就说明公式与规划走岔了，而症状不是崩 ——
	// 是显存白付、或者反过来（公式偏小）在某个尺寸上**静默截断**几段藤。
	TestTrue(FString::Printf(TEXT("上限没有离谱虚高（%d ≤ 4 × %d）"), Bound, Plan.Branch.Num()),
		Bound <= FMath::Max(Plan.Branch.Num(), 1) * 4);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
