#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSGroundShaperSteps.h"
#include "CSHouseActor.h"
#include "CSHouseProfile.h"
#include "CSHouseResize.h"
#include "CSHouseRoof.h"
#include "CSHouseSeam.h"
#include "CSSplineBlockActor.h"
#include "Math/NumericLimits.h"
#include "Math/RandomStream.h"

// -----------------------------------------------------------------------------
// TinyGladeHouse 的判定纯函数用例：不碰 RHI / world，只钉数学 ——
// 屋面求值器（唯一真源，瓦/梁/落窗谓词将来都调它）、脊向滞回、边缘分割、离地收窄。
//
// 计划纪律（TinyGladeHouse_Plan.md 阶段计划）：门洞区间、接触段、柱布点、openings 排布
// 这类判定全部做成无 GPU 依赖的纯函数 + automation 测试。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo builds share a TU, so file-local names carry a CSHouseTest_ prefix
// （与 CSHouseActor.cpp 内的 CSHouse_ 前缀必须不同，否则 unity blob 里同名符号打架）。

FCSRoofDesc CSHouseTest_MakeRoof(ECSRidgeAxis Axis, double SizeX, double SizeY)
{
	FCSRoofDesc Desc;
	Desc.RidgeAxis = Axis;
	Desc.Footprint = FVector2D(SizeX, SizeY);
	Desc.EaveZ = 300.0f;
	Desc.Pitch = 35.0f;
	Desc.Overhang = 25.0f;
	Desc.Thickness = 12.0f;
	return Desc;
}
}

// -----------------------------------------------------------------------------
// 屋面求值器：三处关键高度自洽，且与脊向无关（换轴只是换了哪根轴当跨度）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofEvalTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofEval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofEvalTest::RunTest(const FString& Parameters)
{
	// 600 × 400，脊沿 X ⇒ 跨度是 Y（半跨 200）。
	const FCSRoofDesc Roof = CSHouseTest_MakeRoof(ECSRidgeAxis::X, 600.0, 400.0);
	const float TanP = Roof.TanPitch();

	TestTrue(TEXT("Ridge runs along the 600 side"), FMath::IsNearlyEqual(Roof.RidgeLength(), 600.0f));
	TestTrue(TEXT("Span is the 400 side"), FMath::IsNearlyEqual(Roof.SpanLength(), 400.0f));

	// 屋脊（跨度 0）= 墙顶 + tan(pitch) × 半跨。
	TestTrue(TEXT("Ridge height"), FMath::IsNearlyEqual(CSHouseRoof_RidgeZ(Roof), 300.0f + TanP * 200.0f, 1.0e-3f));

	// footprint 边界（|跨度| = 半跨）上屋面恰好落在墙顶 —— 这条自洽是"墙顶不漏缝"的保证。
	TestTrue(TEXT("Roof meets the wall top at the footprint edge"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(0.0, 200.0)), 300.0f, 1.0e-3f));
	TestTrue(TEXT("Symmetric across the ridge"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(0.0, -137.0)), CSHouseRoof_EvalZ(Roof, FVector2D(0.0, 137.0)), 1.0e-3f));

	// 檐口外沿继续往下走 overhang × tan。
	TestTrue(TEXT("Eave outer height"),
		FMath::IsNearlyEqual(CSHouseRoof_EaveOuterZ(Roof), 300.0f - TanP * 25.0f, 1.0e-3f));

	// 沿脊方向平移不改高度（双坡屋面在脊向是平的）。
	TestTrue(TEXT("Height is invariant along the ridge"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(-280.0, 90.0)), CSHouseRoof_EvalZ(Roof, FVector2D(275.0, 90.0)), 1.0e-3f));

	// 换脊向后同一座房子的屋脊高应当改变（跨度换成了 600 那条边），且仍与 EvalZ 自洽。
	const FCSRoofDesc RoofY = CSHouseTest_MakeRoof(ECSRidgeAxis::Y, 600.0, 400.0);
	TestTrue(TEXT("Ridge along Y spans the 600 side"), FMath::IsNearlyEqual(RoofY.SpanLength(), 600.0f));
	TestTrue(TEXT("Ridge along Y meets the wall top at x = half span"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(RoofY, FVector2D(300.0, 0.0)), 300.0f, 1.0e-3f));

	return true;
}

// -----------------------------------------------------------------------------
// 屋面法线与覆盖谓词
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofNormalTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofNormal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofNormalTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTest_MakeRoof(ECSRidgeAxis::X, 600.0, 400.0);

	const FVector NPos = CSHouseRoof_EvalNormal(Roof, FVector2D(0.0, 120.0));
	const FVector NNeg = CSHouseRoof_EvalNormal(Roof, FVector2D(0.0, -120.0));

	TestTrue(TEXT("Normals are unit length"), FMath::IsNearlyEqual(NPos.Size(), 1.0, 1.0e-4));
	TestTrue(TEXT("Normal points up"), NPos.Z > 0.0);
	TestTrue(TEXT("Normal leans outward on the +span side"), NPos.Y > 0.0);
	TestTrue(TEXT("The two slopes mirror each other"), FMath::IsNearlyEqual(NPos.Y, -NNeg.Y, 1.0e-4));
	TestTrue(TEXT("Normal is flat along the ridge axis"), FMath::IsNearlyZero(NPos.X, 1.0e-4));

	// 法线与屋面斜率一致：坡面切向 · 法线 = 0。切向沿 +跨度 是 (0, 1, -tan)。
	const FVector Tangent = FVector(0.0, 1.0, -Roof.TanPitch()).GetSafeNormal();
	TestTrue(TEXT("Normal is perpendicular to the slope"), FMath::IsNearlyZero(FVector::DotProduct(Tangent, NPos), 1.0e-4));

	// 脊线上退化为上方向（两坡在此不连续，取上是唯一无偏的选择）。
	TestTrue(TEXT("Ridge line normal is up"), CSHouseRoof_EvalNormal(Roof, FVector2D(10.0, 0.0)).Equals(FVector::UpVector, 1.0e-4));

	// 覆盖谓词含两个方向的外挑：半跨 200 + 25，沿脊半长 300 + 25。
	TestTrue(TEXT("Inside the footprint is under the roof"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 0.0)));
	TestTrue(TEXT("The overhang counts as under the roof"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 220.0)));
	TestFalse(TEXT("Past the overhang is not under the roof"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 240.0)));
	TestTrue(TEXT("Under the gable overhang"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(320.0, 0.0)));
	TestFalse(TEXT("Past the gable overhang"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(340.0, 0.0)));

	return true;
}

// -----------------------------------------------------------------------------
// 脊向滞回：长短轴穿越时屋顶不原地翻面
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRidgeHysteresisTest,
	"PCGPlugins.ComputeShaderGenerator.House.RidgeHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRidgeHysteresisTest::RunTest(const FString& Parameters)
{
	constexpr float Ratio = 1.15f;

	// 明确的长轴：无论从哪一侧进来都收敛到同一根轴。
	TestTrue(TEXT("Clearly long X stays X"),
		CSHouseRoof_ChooseRidgeAxis(FVector2D(600, 300), ECSRidgeAxis::X, Ratio) == ECSRidgeAxis::X);
	TestTrue(TEXT("Clearly long X flips from Y"),
		CSHouseRoof_ChooseRidgeAxis(FVector2D(600, 300), ECSRidgeAxis::Y, Ratio) == ECSRidgeAxis::X);
	TestTrue(TEXT("Clearly long Y flips from X"),
		CSHouseRoof_ChooseRidgeAxis(FVector2D(300, 600), ECSRidgeAxis::X, Ratio) == ECSRidgeAxis::Y);

	// 滞回带内（Y 只比 X 长一点，未达 1.15 倍）：保持现状，两个方向都不翻。
	// 这一条正是"单边推拉让 X 穿过 Y 时屋顶啪地翻过去"的防线。
	TestTrue(TEXT("Inside the band X keeps X"),
		CSHouseRoof_ChooseRidgeAxis(FVector2D(400, 420), ECSRidgeAxis::X, Ratio) == ECSRidgeAxis::X);
	TestTrue(TEXT("Inside the band Y keeps Y"),
		CSHouseRoof_ChooseRidgeAxis(FVector2D(420, 400), ECSRidgeAxis::Y, Ratio) == ECSRidgeAxis::Y);

	// 正方形是滞回带的正中心：谁进来谁留下，绝不抖动。
	TestTrue(TEXT("Square keeps X"), CSHouseRoof_ChooseRidgeAxis(FVector2D(400, 400), ECSRidgeAxis::X, Ratio) == ECSRidgeAxis::X);
	TestTrue(TEXT("Square keeps Y"), CSHouseRoof_ChooseRidgeAxis(FVector2D(400, 400), ECSRidgeAxis::Y, Ratio) == ECSRidgeAxis::Y);

	// ⚠️ **这条断言在 2026-08-30 裁决四之后收缩过，收缩是有意的**（原文："连续单边推拉扫过
	// 穿越点全程恰好翻一次" ⇒ 禁带口径下"一次都不翻"）。
	//
	// 收缩的理由与边界（写在这里，免得后人当成丢覆盖给"修"回去）：
	//  · 裸滞回下，**只**改尺寸的一次单边推拉必然在某个连续步里翻一次 —— 那一步的画面就是
	//    "屋顶原地 90° 跳过去"，正是裁决四要消掉的现象。
	//  · 禁带把整段滞回模糊区 [A/R, A·R] 吞掉之后，尺寸**根本停不到阈值上**，所以在
	//    "扫过翻轴阈"这件事上答案变成 0 次。这条 sweep 现在走的是禁带修正后的尺寸序列，
	//    因此它测的是**同一个现象的消失**，不是把覆盖删掉。
	//  · 全程完整推拉（从 X 长到 Y 长）仍然必须恰好翻一次 —— 那是拓扑必然（起点脊在 X、
	//    终点脊在 Y），禁带管不了也不该管。它被下面 ResizeBand 用例的
	//    "翻轴只发生在跳带那一步" 接手，那才是可断言的形态。
	{
		constexpr double Anchor = 400.0;             // X 恒定，只推 Y 那一边
		constexpr double StepCm = 5.0;               // 一帧拖 5 cm，真实拖拽的量级
		const double Threshold = Anchor * Ratio;     // 翻轴阈 = 460

		FCSHouseResizeBand Band;
		Band.Fraction = 0.20f;
		Band.RidgeSwitchRatio = Ratio;

		ECSRidgeAxis Axis = ECSRidgeAxis::X;
		int32 ContinuousFlips = 0, JumpFlips = 0, NearThreshold = 0;
		double Previous = CSHouseResize_ApplyBand(300.0, Anchor, Band);
		for (int32 Step = 1; Step <= 60; ++Step)
		{
			const double SizeY = CSHouseResize_ApplyBand(300.0 + Step * StepCm, Anchor, Band);
			// "跳带" = 这一步的尺寸位移明显超过一帧的拖动量。用户看到的是房子换档。
			const bool bJumped = FMath::Abs(SizeY - Previous) > StepCm + 1.0e-6;
			if (FMath::Abs(SizeY - Threshold) < 10.0) ++NearThreshold;

			const ECSRidgeAxis Next = CSHouseRoof_ChooseRidgeAxis(FVector2D(Anchor, SizeY), Axis, Ratio);
			if (Next != Axis) { if (bJumped) ++JumpFlips; else ++ContinuousFlips; }
			Axis = Next;
			Previous = SizeY;
		}
		// 这就是"一次都不翻"的可断言形态：**平滑拖动的每一步都不翻**。
		TestEqual(TEXT("A continuous drag never flips the ridge on a smooth step"), ContinuousFlips, 0);
		// 尺寸压根停不到翻轴阈附近 —— 阈值 460 整个落在禁带 (320, 480) 里。
		TestEqual(TEXT("No dragged size ever rests near the ridge-flip threshold"), NearThreshold, 0);
		// 拓扑必然：从 X 长拖到 Y 长，脊向总得改一次。禁带只保证它与尺寸跳变同步。
		TestEqual(TEXT("The one ridge flip happens on the band jump"), JumpFlips, 1);
		TestTrue(TEXT("The sweep still ends on Y"), Axis == ECSRidgeAxis::Y);
	}

	// 对照组（**故意关掉禁带**）：同一段推拉，翻轴就落回某个平滑步里 ——
	// 证明上面那 0 是禁带挣来的，不是这段 sweep 本来就翻不动。
	{
		ECSRidgeAxis Axis = ECSRidgeAxis::X;
		int32 ContinuousFlips = 0;
		for (int32 Step = 1; Step <= 60; ++Step)
		{
			const ECSRidgeAxis Next = CSHouseRoof_ChooseRidgeAxis(FVector2D(400.0, 300.0 + Step * 5.0), Axis, Ratio);
			if (Next != Axis) ++ContinuousFlips;
			Axis = Next;
		}
		TestEqual(TEXT("Without the band the same drag flips the ridge mid-motion"), ContinuousFlips, 1);
	}

	// SwitchRatio <= 1 退化为无滞回：等价于旧的 X >= Y 隐式规则。
	TestTrue(TEXT("Ratio 1 degenerates to the implicit long-axis rule"),
		CSHouseRoof_ChooseRidgeAxis(FVector2D(400, 401), ECSRidgeAxis::X, 1.0f) == ECSRidgeAxis::Y);

	return true;
}

// -----------------------------------------------------------------------------
// 拉尺寸（D5）：单边推拉的记账 + 尺寸禁带
// -----------------------------------------------------------------------------

namespace
{
/** 第 EdgeIndex 面墙外皮中心的世界位置。**推拉的两条不变量都只能靠它验**。 */
FVector CSHouseTest_WallCentre(const FVector2D& Size, const FVector& Centre, int32 EdgeIndex, float Yaw)
{
	const double Dim = CSHouseResize_EdgeDrivesX(EdgeIndex) ? Size.X : Size.Y;
	return Centre + CSHouseResize_EdgeOuterWorld(EdgeIndex, Yaw) * (Dim * 0.5);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseEdgePushTest,
	"PCGPlugins.ComputeShaderGenerator.House.EdgePush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseEdgePushTest::RunTest(const FString& Parameters)
{
	// 禁带默认关掉：这一组用例钉的是推拉的记账，掺进禁带就分不清位移是被谁改的。
	const FCSHouseResizeBand NoBand;

	// ① 幂等：连续 10 次 Offset=0 不许改动任何量。
	// 计划 D5 的第一条配套单测。它抓的是"每次事件都白走一遍状态机"这类漂移 ——
	// gizmo 在没动的那些帧照样发 PostEditMove，漂一点点就是拖动期的持续抖动。
	{
		FVector2D Size(600.0, 400.0);
		FVector Centre(1000.0, 2000.0, 50.0);
		for (int32 i = 0; i < 10; ++i)
		{
			const float Applied = CSHouse_ApplyEdgePush(Size, Centre, 1, 30.0f, 0.0f, 200.0f, NoBand);
			TestEqual(TEXT("A zero push applies zero"), Applied, 0.0f);
		}
		TestTrue(TEXT("Ten zero pushes leave the size untouched"), Size == FVector2D(600.0, 400.0));
		TestTrue(TEXT("Ten zero pushes leave the centre untouched"), Centre == FVector(1000.0, 2000.0, 50.0));
	}

	// ② 对侧墙不动、被推墙恰好走 Δ ——「拖 1 m 墙走 2 m」那个父子回路缺陷的钉子。
	// 四条边 × 带 yaw 各验一遍：中心随动是**世界**方向的量，只在 yaw=0 下对是最常见的漏法。
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		constexpr float Yaw = 37.0f;
		constexpr double Delta = 123.0;
		FVector2D Size(600.0, 400.0);
		FVector Centre(1000.0, 2000.0, 50.0);
		const FVector PushedBefore = CSHouseTest_WallCentre(Size, Centre, Edge, Yaw);
		const FVector OppositeBefore = CSHouseTest_WallCentre(Size, Centre, Edge + 2, Yaw);

		const float Applied = CSHouse_ApplyEdgePush(Size, Centre, Edge, Yaw, float(Delta), 200.0f, NoBand);
		TestEqual(FString::Printf(TEXT("Edge %d applies the whole offset"), Edge), double(Applied), Delta, 1.0e-3);

		const FVector PushedAfter = CSHouseTest_WallCentre(Size, Centre, Edge, Yaw);
		const FVector OppositeAfter = CSHouseTest_WallCentre(Size, Centre, Edge + 2, Yaw);
		TestTrue(FString::Printf(TEXT("Edge %d: the opposite wall does not move"), Edge),
			OppositeAfter.Equals(OppositeBefore, 1.0e-3));
		TestTrue(FString::Printf(TEXT("Edge %d: the pushed wall moves exactly the offset"), Edge),
			PushedAfter.Equals(PushedBefore + CSHouseResize_EdgeOuterWorld(Edge, Yaw) * Delta, 1.0e-3));
	}

	// ③ MinFootprint 是硬下界，且返回的是**实际**位移（不是请求值）。
	// 记账量法必须拿返回值累加：记成请求值的话，顶在下限上的那段时间里残差会一路攒着，
	// 松手瞬间房子跳一大截 —— 症状与"拖动漂移"一模一样，很难归因。
	{
		FVector2D Size(600.0, 400.0);
		FVector Centre = FVector::ZeroVector;
		const float Applied = CSHouse_ApplyEdgePush(Size, Centre, 0, 0.0f, -1000.0f, 200.0f, NoBand);
		TestEqual(TEXT("Shrinking past the floor applies only what was possible"), double(Applied), -200.0, 1.0e-3);
		TestEqual(TEXT("The floor holds"), Size.Y, 200.0, 1.0e-3);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseResizeBandTest,
	"PCGPlugins.ComputeShaderGenerator.House.ResizeBand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseResizeBandTest::RunTest(const FString& Parameters)
{
	FCSHouseResizeBand Band;
	Band.Fraction = 0.20f;
	Band.RidgeSwitchRatio = 1.15f;

	// ① 带宽下界由滞回比反解，不是独立调参量：外沿必须**严格**在翻轴阈之外。
	// 等号会让跳带那一步的落点恰好是 Y == R·X，而 ChooseRidgeAxis 用严格 > ⇒ 那一步不翻、
	// 下一个平滑步才翻，翻轴就这么漏回连续拖动里。
	{
		FCSHouseResizeBand Narrow;
		Narrow.Fraction = 0.01f;             // 用户把带调得比滞回还窄
		Narrow.RidgeSwitchRatio = 1.15f;
		const float F = CSHouseResize_EffectiveBandFraction(Narrow);
		TestTrue(TEXT("The band is widened to cover the hysteresis zone"), 1.0f + F > 1.15f);
		TestTrue(TEXT("The lower edge also clears the hysteresis zone"), 1.0f - F < 1.0f / 1.15f);

		FCSHouseResizeBand Off;
		Off.Fraction = 0.0f;
		TestEqual(TEXT("Fraction 0 really turns the band off"), CSHouseResize_EffectiveBandFraction(Off), 0.0f);
		TestEqual(TEXT("With the band off nothing is snapped"), CSHouseResize_ApplyBand(401.0, 400.0, Off), 401.0);
	}

	// ② 跳哪一侧只看落在带心（X == Y）的哪一边 ⇒ 幂等、来回拖不产生迟滞环。
	// 若改成"按拖动方向跳"，来回拖会在两条外沿之间来回蹦 2f·A，那才是真的抖。
	{
		// 容差 1e-3 cm（10 微米）而不是 1e-6：外沿是 `Anchor × float(0.20f)`，
		// 0.2 在 float 里存不下，边界天然差 1.2e-6 cm。拿它当"精确等于"是自找的假红。
		const double Low = CSHouseResize_ApplyBand(399.0, 400.0, Band);
		const double High = CSHouseResize_ApplyBand(401.0, 400.0, Band);
		TestEqual(TEXT("Just below the centre snaps down"), Low, 320.0, 1.0e-3);
		TestEqual(TEXT("Just above the centre snaps up"), High, 480.0, 1.0e-3);
		// 幂等按"再来一次给同一个答案"验，不按"等于理论边界值"验 —— 后者会被上面那 1.2e-6 咬到。
		TestEqual(TEXT("Snapping is idempotent (low edge)"), CSHouseResize_ApplyBand(Low, 400.0, Band), Low, 1.0e-9);
		TestEqual(TEXT("Snapping is idempotent (high edge)"), CSHouseResize_ApplyBand(High, 400.0, Band), High, 1.0e-9);
		TestEqual(TEXT("Outside the band nothing moves"), CSHouseResize_ApplyBand(700.0, 400.0, Band), 700.0, 1.0e-6);
	}

	// ③ 连续推拉全程：尺寸一次都没停在带内，跨带**只发生一次且是一次跳变**，
	//    而南墙（对侧）在整段拖动里**逐位不动** —— 包括跳的那一帧。
	// 最后半句才是这条用例的分量所在：跳带是尺寸的不连续跳变，中心随动的记账一旦漏了那半格，
	// 对侧墙就会在跳的那一帧被甩出去，画面上看着像"房子整个平移了一下"。
	{
		constexpr float Yaw = 21.0f;
		FVector2D Size(400.0, 320.0);        // 起手就在带外（低侧外沿）
		FVector2D Raw = Size;                // 原始诉求累加器，没有它墙会卡死在外沿上
		FVector Centre(500.0, -700.0, 0.0);
		const FVector SouthBefore = CSHouseTest_WallCentre(Size, Centre, 0, Yaw);

		int32 InsideBand = 0, Jumps = 0, SouthMoved = 0, Stuck = 0;
		for (int32 Step = 0; Step < 60; ++Step)
		{
			// 推北墙（edge 2，外法线 +Y），每帧 5 cm，把 Y 从 320 一路推过 400。
			const double Before = Size.Y;
			CSHouse_ApplyEdgePush(Size, Centre, 2, Yaw, 5.0f, 200.0f, Band, &Raw);
			if (FMath::Abs(Size.Y - Before) > 5.0 + 1.0e-6) ++Jumps;
			if (FMath::IsNearlyEqual(Size.Y, Before)) ++Stuck;
			if (CSHouseResize_IsInsideBand(Size.Y, Size.X, Band)) ++InsideBand;
			if (!CSHouseTest_WallCentre(Size, Centre, 0, Yaw).Equals(SouthBefore, 1.0e-3)) ++SouthMoved;
		}
		TestEqual(TEXT("No dragged size ever rests inside the band"), InsideBand, 0);
		TestEqual(TEXT("The drag crosses the band exactly once, as one jump"), Jumps, 1);
		TestEqual(TEXT("The opposite wall never moves, not even on the jump frame"), SouthMoved, 0);
		// 卡口是有代价的：跨带前后墙都会吸在外沿上不动若干帧。有 stuck 帧才说明卡口真的在工作
		// （把累加器删掉的话这里会变成 60 —— 墙一步都跨不过去）。
		TestTrue(TEXT("The detent really holds the wall for a while"), Stuck > 0 && Stuck < 60);
		TestTrue(TEXT("The drag really got out the far side"), Size.Y > 480.0);
		// **净位移守恒**：卡口只重排了位移的分布，没有凭空造出或吞掉长度 ——
		// 拖 3 m 墙就走 3 m，跳带那 160 cm 是从后面的卡顿里借的，不是白送的。
		// 「拖 1 m 走 2 m」那个父子回路缺陷在这条上会立刻现形。
		TestEqual(TEXT("Total wall travel equals total drag travel"), Size.Y - 320.0, 60 * 5.0, 1.0e-6);
		TestEqual(TEXT("The applied size has caught up with the raw request"), Size.Y, Raw.Y, 1.0e-6);
	}

	// ④ 跳带那一步：对侧墙逐位不动、被推墙恰好走返回值那么远。
	{
		constexpr float Yaw = 21.0f;
		FVector2D Size(400.0, 320.0);
		FVector Centre(500.0, -700.0, 0.0);
		const FVector SouthBefore = CSHouseTest_WallCentre(Size, Centre, 0, Yaw);
		const FVector NorthBefore = CSHouseTest_WallCentre(Size, Centre, 2, Yaw);

		// 请求只推 100 cm（320 → 420，落在带内），禁带把它顶到 480 ⇒ 实际走 160。
		const float Applied = CSHouse_ApplyEdgePush(Size, Centre, 2, Yaw, 100.0f, 200.0f, Band);
		TestEqual(TEXT("The band turns the request into a jump to the far edge"), double(Applied), 160.0, 1.0e-3);
		TestEqual(TEXT("The size lands on the far edge"), Size.Y, 480.0, 1.0e-3);
		TestTrue(TEXT("The opposite wall stays put across the jump"),
			CSHouseTest_WallCentre(Size, Centre, 0, Yaw).Equals(SouthBefore, 1.0e-3));
		TestTrue(TEXT("The pushed wall moves exactly the applied amount"),
			CSHouseTest_WallCentre(Size, Centre, 2, Yaw).Equals(
				NorthBefore + CSHouseResize_EdgeOuterWorld(2, Yaw) * double(Applied), 1.0e-3));
	}

	// ⑤ MinFootprint 赢过禁带：小房子的低侧外沿掉到下限以下时，硬顶回去只会让墙拖不动。
	{
		FVector2D Size(220.0, 220.0);
		FVector Centre = FVector::ZeroVector;
		const float Applied = CSHouse_ApplyEdgePush(Size, Centre, 0, 0.0f, -30.0f, 200.0f, Band);
		TestEqual(TEXT("The floor beats the band"), Size.Y, 200.0, 1.0e-3);
		TestEqual(TEXT("And the applied offset reports the truth"), double(Applied), -20.0, 1.0e-3);
	}

	// ⑥ 翻轴预判：禁带口径下，翻轴只可能与跳带同步。
	{
		TestFalse(TEXT("A smooth step outside the band never flips"),
			CSHouseResize_WouldFlipRidge(FVector2D(400.0, 320.0), FVector2D(400.0, 315.0), ECSRidgeAxis::X, 1.15f));
		TestTrue(TEXT("The band jump is where the flip lives"),
			CSHouseResize_WouldFlipRidge(FVector2D(400.0, 320.0), FVector2D(400.0, 480.0), ECSRidgeAxis::X, 1.15f));
	}

	return true;
}

// -----------------------------------------------------------------------------
// 边缘线段分割：等分、护角、最小宽度早退
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseEdgeSplitTest,
	"PCGPlugins.ComputeShaderGenerator.House.EdgeSplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseEdgeSplitTest::RunTest(const FString& Parameters)
{
	float FirstS = 0, Pitch = 0;

	// 600 长墙，护角 60 ⇒ 可用 480；目标段距 150 ⇒ round(3.2) = 3 段，段长 160。
	{
		const int32 N = ACSHouseActor::SplitEdgeIntoSlots(600.0f, 60.0f, 150.0f, 40.0f, FirstS, Pitch);
		TestEqual(TEXT("600 wall splits into three slots"), N, 3);
		TestTrue(TEXT("First slot starts at the corner margin"), FMath::IsNearlyEqual(FirstS, 60.0f));
		TestTrue(TEXT("Slots are an exact equal split"), FMath::IsNearlyEqual(Pitch, 160.0f, 1.0e-3f));
		// 等分的定义：最后一段的末端恰好落在另一侧护角上，没有余量。
		TestTrue(TEXT("The last slot ends exactly on the far corner margin"),
			FMath::IsNearlyEqual(FirstS + N * Pitch, 600.0f - 60.0f, 1.0e-3f));
	}

	// 短墙：可用长装不下一个最小拱 ⇒ 这条边不开门。
	{
		const int32 N = ACSHouseActor::SplitEdgeIntoSlots(150.0f, 60.0f, 150.0f, 40.0f, FirstS, Pitch);
		TestEqual(TEXT("A wall with no usable run yields no slots"), N, 0);
	}

	// 可用长比目标段距短但仍装得下一个拱 ⇒ 至少一段（clamp 下界）。
	{
		const int32 N = ACSHouseActor::SplitEdgeIntoSlots(220.0f, 60.0f, 150.0f, 40.0f, FirstS, Pitch);
		TestEqual(TEXT("A short usable run still yields one slot"), N, 1);
		TestTrue(TEXT("The single slot spans the whole usable run"), FMath::IsNearlyEqual(Pitch, 100.0f, 1.0e-3f));
	}

	// 段数随墙长单调不减 —— 拉尺寸时拱只会增删，不会莫名其妙重排。
	{
		int32 Previous = 0;
		for (int32 Step = 0; Step <= 40; ++Step)
		{
			const float Length = 150.0f + Step * 25.0f;
			const int32 N = ACSHouseActor::SplitEdgeIntoSlots(Length, 60.0f, 150.0f, 40.0f, FirstS, Pitch);
			TestTrue(TEXT("Slot count is monotonic in wall length"), N >= Previous);
			Previous = N;
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// 离地收窄：连续、单调、两端夹紧
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDoorWidthScaleTest,
	"PCGPlugins.ComputeShaderGenerator.House.DoorWidthScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDoorWidthScaleTest::RunTest(const FString& Parameters)
{
	constexpr float Full = 30.0f, Zero = 120.0f;

	TestTrue(TEXT("Flush with the ground is full width"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(0.0f, Full, Zero), 1.0f));
	TestTrue(TEXT("At the full-width threshold it is still full"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(Full, Full, Zero), 1.0f));
	TestTrue(TEXT("At the zero threshold it is gone"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(Zero, Full, Zero), 0.0f));
	TestTrue(TEXT("Past the zero threshold it stays gone"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(500.0f, Full, Zero), 0.0f));
	TestTrue(TEXT("Halfway is half width"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(75.0f, Full, Zero), 0.5f, 1.0e-4f));

	// 单调不增 —— 抬得越高门只会越窄，不会反弹（"没有二值跳变"的形式化）。
	float Previous = 1.0f;
	for (int32 Step = 0; Step <= 100; ++Step)
	{
		const float Gap = Step * 2.0f;
		const float Scale = ACSHouseActor::ComputeDoorWidthScale(Gap, Full, Zero);
		TestTrue(TEXT("Width scale never increases with the gap"), Scale <= Previous + 1.0e-6f);
		TestTrue(TEXT("Width scale stays in [0, 1]"), Scale >= 0.0f && Scale <= 1.0f);
		Previous = Scale;
	}

	// 参数被填反 / 相等时不除零，退化成硬阈。
	TestTrue(TEXT("Degenerate thresholds do not divide by zero"),
		FMath::IsFinite(ACSHouseActor::ComputeDoorWidthScale(50.0f, 100.0f, 100.0f)));

	return true;
}

// -----------------------------------------------------------------------------
// 洞的剖面：分段数自适应
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseProfileSegmentsTest,
	"PCGPlugins.ComputeShaderGenerator.House.ProfileSegments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseProfileSegmentsTest::RunTest(const FString& Parameters)
{
	// 计划里算过的那个例子：Tol = 0.2、R = 55 → 19 段。
	// 公式漏掉分母那个 2 会给出 37 —— 这条断言就是钉死它的。
	TestEqual(TEXT("Tol 0.2 at R 55 gives 19 segments"), CSHouse_ProfileSegments(55.0f, 0.2f), 19);

	// 半径越大越需要更多段（同一绝对弦高容差下），且始终夹在 [6, 48]。
	int32 Previous = 0;
	for (int32 Step = 1; Step <= 40; ++Step)
	{
		const float R = Step * 20.0f;
		const int32 N = CSHouse_ProfileSegments(R, 0.2f);
		TestTrue(TEXT("Segment count is monotonic in radius"), N >= Previous);
		TestTrue(TEXT("Segment count stays within the clamp"), N >= 6 && N <= 48);
		Previous = N;
	}

	// 实际弦高确实落在容差内（除非撞上 48 段的上限）—— 这才是段数存在的理由。
	for (const float R : { 30.0f, 55.0f, 120.0f })
	{
		const int32 N = CSHouse_ProfileSegments(R, 0.2f);
		if (N >= 48) continue;
		const float Sagitta = R * (1.0f - FMath::Cos(PI / (2.0f * N)));
		TestTrue(TEXT("Actual chord sagitta is within tolerance"), Sagitta <= 0.2f + 1.0e-4f);
	}

	// 退化输入不炸。
	TestTrue(TEXT("Zero radius is clamped"), CSHouse_ProfileSegments(0.0f, 0.2f) >= 6);
	TestTrue(TEXT("Zero tolerance is clamped"), CSHouse_ProfileSegments(55.0f, 0.0f) <= 48);

	return true;
}

// -----------------------------------------------------------------------------
// 洞的剖面：采样形状
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseProfileSampleTest,
	"PCGPlugins.ComputeShaderGenerator.House.ProfileSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseProfileSampleTest::RunTest(const FString& Parameters)
{
	TArray<FCSOpeningProfileSample> Samples;

	// ---- 拱：矩形下身 + 半圆顶 ----
	{
		FCSWallOpening Arch;
		Arch.Shape = ECSOpeningShape::Arch;
		Arch.CenterS = 300.0f;
		Arch.Width = 110.0f;
		Arch.Z0 = 0.0f;
		Arch.Z1 = 220.0f;
		CSHouse_SampleOpeningProfile(Arch, 0.2f, Samples);

		TestTrue(TEXT("Arch yields samples"), Samples.Num() >= 2);

		// S 严格单调递增 —— 墙板砌筑与内壁扫掠都依赖这条（否则条带会自交）。
		for (int32 K = 1; K < Samples.Num(); ++K)
		{
			TestTrue(TEXT("Profile S is strictly increasing"), Samples[K].S > Samples[K - 1].S);
		}

		// 洞底恒为 Z0（矩形下身），洞顶在中点最高。
		for (const FCSOpeningProfileSample& Sample : Samples)
		{
			TestTrue(TEXT("Arch floor stays at Z0"), FMath::IsNearlyEqual(Sample.ZLow, 0.0f));
			TestTrue(TEXT("Arch never exceeds its top"), Sample.ZHigh <= Arch.Z1 + 1.0f);
		}
		const int32 Mid = Samples.Num() / 2;
		TestTrue(TEXT("Arch peaks at the centre"), FMath::IsNearlyEqual(Samples[Mid].ZHigh, Arch.Z1, 1.0f));

		// **外接而非内接**：折线端点比标称半宽略宽（1/cos(π/2N)），微覆盖不漏缝。
		const float Span = Samples.Last().S - Samples[0].S;
		TestTrue(TEXT("Circumscribed profile is slightly wider than nominal"), Span > Arch.Width);
		TestTrue(TEXT("...but only slightly"), Span < Arch.Width * 1.02f);
	}

	// ---- 矩形：两个样本，上下边界都是常数 ----
	{
		FCSWallOpening Rect;
		Rect.Shape = ECSOpeningShape::Rect;
		Rect.CenterS = 200.0f;
		Rect.Width = 90.0f;
		Rect.Z0 = 110.0f;   // 窗台高
		Rect.Z1 = 210.0f;
		CSHouse_SampleOpeningProfile(Rect, 0.2f, Samples);

		TestEqual(TEXT("Rect needs only two samples"), Samples.Num(), 2);
		TestTrue(TEXT("Rect spans exactly its nominal width"), FMath::IsNearlyEqual(Samples.Last().S - Samples[0].S, 90.0f, 1.0e-3f));
		TestTrue(TEXT("Rect sill is flat"), FMath::IsNearlyEqual(Samples[0].ZLow, 110.0f) && FMath::IsNearlyEqual(Samples[1].ZLow, 110.0f));
		TestTrue(TEXT("Rect head is flat"), FMath::IsNearlyEqual(Samples[0].ZHigh, 210.0f) && FMath::IsNearlyEqual(Samples[1].ZHigh, 210.0f));
	}

	// ---- 圆：两端闭合（ZLow == ZHigh），中点最高最低 ----
	{
		FCSWallOpening Circle;
		Circle.Shape = ECSOpeningShape::Circle;
		Circle.CenterS = 250.0f;
		Circle.Width = 80.0f;
		Circle.Z0 = 160.0f;
		Circle.Z1 = 240.0f;
		CSHouse_SampleOpeningProfile(Circle, 0.2f, Samples);

		TestTrue(TEXT("Circle yields samples"), Samples.Num() >= 4);
		TestTrue(TEXT("Circle closes on the left"), FMath::IsNearlyEqual(Samples[0].ZLow, Samples[0].ZHigh, 1.0e-3f));
		TestTrue(TEXT("Circle closes on the right"), FMath::IsNearlyEqual(Samples.Last().ZLow, Samples.Last().ZHigh, 1.0e-3f));
		const int32 Mid = Samples.Num() / 2;
		TestTrue(TEXT("Circle is widest at the centre"), Samples[Mid].ZHigh - Samples[Mid].ZLow > 70.0f);
	}

	// ---- Skew：洞顶沿 S 线性倾斜（楼梯口），洞底不动 ----
	{
		FCSWallOpening Stair;
		Stair.Shape = ECSOpeningShape::Rect;
		Stair.Type = ECSOpeningType::Stair;
		Stair.CenterS = 300.0f;
		Stair.Width = 100.0f;
		Stair.Z0 = 50.0f;
		Stair.Z1 = 200.0f;
		Stair.Skew = 0.5f;
		CSHouse_SampleOpeningProfile(Stair, 0.2f, Samples);

		TestEqual(TEXT("Skewed rect still has two samples"), Samples.Num(), 2);
		TestTrue(TEXT("Skew tilts the head down on the near side"), FMath::IsNearlyEqual(Samples[0].ZHigh, 200.0f - 25.0f, 1.0e-3f));
		TestTrue(TEXT("Skew tilts the head up on the far side"), FMath::IsNearlyEqual(Samples[1].ZHigh, 200.0f + 25.0f, 1.0e-3f));
		TestTrue(TEXT("Skew leaves the sill alone"), FMath::IsNearlyEqual(Samples[0].ZLow, Samples[1].ZLow));
	}

	// 无效洞（零宽 / 上下颠倒）产出空表，调用方据此跳过。
	{
		FCSWallOpening Bad;
		Bad.Width = 0.0f;
		CSHouse_SampleOpeningProfile(Bad, 0.2f, Samples);
		TestEqual(TEXT("A zero-width opening yields nothing"), Samples.Num(), 0);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 洞的重叠谓词：同边**一维 S 区间**，比的是面板格，Z 不参与
//
// 用户裁决 2026-08-30（C1 选甲）：永久放弃"门上开窗"。谓词必须与 CSHouse_BuildBodySoup 的
// 单游标扫掠同维 —— 二维判据会放行几何砌不出来的堆叠，那正是 D8「谓词是唯一真源」要防的。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseOpeningOverlapTest,
	"PCGPlugins.ComputeShaderGenerator.House.OpeningOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseOpeningOverlapTest::RunTest(const FString& Parameters)
{
	// 门与窗各自的面板格 = 半宽 + 半个墩（CSHouse_OpeningCell），墩宽取项目默认。
	constexpr float Pier = 40.0f;
	constexpr float Clear = 10.0f;

	FCSWallOpening Door;
	Door.Type = ECSOpeningType::Door;
	Door.EdgeIndex = 1;
	Door.CenterS = 300.0f;
	Door.Width = 110.0f;
	Door.Z0 = 0.0f;
	Door.Z1 = 220.0f;

	// 同一 S 位置的高窗：**冲突**。这正是裁决放弃掉的那件事 —— 墙板是沿 S 的单游标扫掠，
	// 门上方那扇窗砌不出来，所以谓词必须当场拒绝，而不是放行到几何里静默消失。
	FCSWallOpening HighWindow;
	HighWindow.Type = ECSOpeningType::Window;
	HighWindow.EdgeIndex = 1;
	HighWindow.CenterS = 300.0f;
	HighWindow.Width = 60.0f;
	HighWindow.Z0 = 250.0f;
	HighWindow.Z1 = 300.0f;
	TestTrue(TEXT("A high window conflicts with a low door at the same S"),
		CSHouse_OpeningsOverlap(Door, HighWindow, Pier, Clear));

	// Z 一律不参与：把窗抬到天上仍然冲突。这条是回归守卫 —— 二维判据被悄悄加回来就会变红。
	FCSWallOpening WayUp = HighWindow;
	WayUp.Z0 = 1000.0f;
	WayUp.Z1 = 1060.0f;
	TestTrue(TEXT("Z plays no part in the predicate"),
		CSHouse_OpeningsOverlap(Door, WayUp, Pier, Clear));

	// 两洞本身让开了（净空 20 > Clearance），但两**格**仍相交 —— 扫掠会把窗的左半边留给
	// 前一块无 clip 的面板，所以照样判冲突。比格不比洞的理由就在这一条。
	FCSWallOpening CellClash = HighWindow;
	CellClash.CenterS = 300.0f + 110.0f * 0.5f + 60.0f * 0.5f + 20.0f;
	TestTrue(TEXT("Openings that clear each other but whose panel cells overlap still conflict"),
		CSHouse_OpeningsOverlap(Door, CellClash, Pier, Clear));

	// 让开一个整墩再加净距才放行。
	constexpr float CellTouch = 110.0f * 0.5f + 60.0f * 0.5f + Pier;   // 两格刚好相接的中心距
	FCSWallOpening Beside = HighWindow;
	Beside.CenterS = 300.0f + CellTouch + 15.0f;   // 格间净距 15 > Clearance
	TestFalse(TEXT("A full pier plus the clearance band apart is fine"),
		CSHouse_OpeningsOverlap(Door, Beside, Pier, Clear));
	Beside.CenterS = 300.0f + CellTouch + 5.0f;    // 格间净距 5 < Clearance
	TestTrue(TEXT("Inside the clearance band conflicts"),
		CSHouse_OpeningsOverlap(Door, Beside, Pier, Clear));

	// 不同边永远不冲突（洞只认自己那条边缘线段）。
	FCSWallOpening OtherEdge = HighWindow;
	OtherEdge.EdgeIndex = 2;
	TestFalse(TEXT("Different edges never conflict"),
		CSHouse_OpeningsOverlap(Door, OtherEdge, Pier, Clear));

	// 对称性：谓词不能因为参数顺序而改口。
	TestEqual(TEXT("Overlap is symmetric"),
		CSHouse_OpeningsOverlap(Door, Beside, Pier, Clear), CSHouse_OpeningsOverlap(Beside, Door, Pier, Clear));

	return true;
}

// -----------------------------------------------------------------------------
// 解析裁剪场：判据与剖面必须描述同一条曲线
//
// 这是 clip 路线的正确性支点。剖面（CSHouse_SampleOpeningProfile）供洞口内壁扫掠，
// 判据（CSHouse_ClipKeeps）供材质逐像素切洞 —— 两者若不同式，就会出现"内壁贴在这里、
// 洞却切在那里"的穿帮，且不报任何错。这里用剖面的采样点去打判据的边界来钉死它。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseClipFieldTest,
	"PCGPlugins.ComputeShaderGenerator.House.ClipField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseClipFieldTest::RunTest(const FString& Parameters)
{
	// ---- 拱：矩形下身 + 半圆顶 ----
	FCSWallOpening Arch;
	Arch.Shape = ECSOpeningShape::Arch;
	Arch.CenterS = 300.0f;
	Arch.Width = 110.0f;
	Arch.Z0 = 0.0f;
	Arch.Z1 = 220.0f;

	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Arch);
	TestTrue(TEXT("Arch yields a valid clip field"), Field.bValid);

	const float HW = Arch.HalfWidth();
	const float SpringZ = Arch.Z1 - HW;

	// 洞正中偏下：一定在洞内（被 discard）。
	TestFalse(TEXT("The middle of the doorway is clipped away"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS, SpringZ * 0.5f)));
	// 拱顶正下方一点：仍在洞内。
	TestFalse(TEXT("Just under the crown is clipped away"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS, Arch.Z1 - 2.0f)));
	// 拱顶之上：保留。
	TestTrue(TEXT("Above the crown is kept"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS, Arch.Z1 + 2.0f)));
	// 洞外侧向：保留 —— 这条就是"墩不被误切"。
	TestTrue(TEXT("Beyond the jamb is kept"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS + HW + 2.0f, SpringZ * 0.5f)));

	// 面板端盖必须落在洞外，否则墩会被切掉。|q.x| = (HW + PierWidth/2)/HW > 1。
	{
		float CellMin = 0, CellMax = 0;
		CSHouse_OpeningCell(Arch, 40.0f, CellMin, CellMax);
		TestTrue(TEXT("The panel is wider than the opening"), CellMax - CellMin > Arch.Width);
		for (const float Z : { 5.0f, SpringZ * 0.5f, SpringZ, Arch.Z1 - 1.0f })
		{
			TestTrue(TEXT("The panel's left end cap survives the clip"), CSHouse_ClipKeeps(Field, Field.Eval(CellMin, Z)));
			TestTrue(TEXT("The panel's right end cap survives the clip"), CSHouse_ClipKeeps(Field, Field.Eval(CellMax, Z)));
		}
	}

	// **判据边界 == 剖面曲线**：剖面上边界的采样点应当恰好落在判据的临界面上。
	// 剖面按外接建（比精确曲线略外扩 1/cos(π/2N)），所以它们全都刚好在"洞外"一侧 ——
	// 这正是想要的方向：内壁微覆盖，绝不漏缝。
	{
		TArray<FCSOpeningProfileSample> Samples;
		CSHouse_SampleOpeningProfile(Arch, 0.2f, Samples);
		TestTrue(TEXT("Arch profile yields samples"), Samples.Num() >= 2);

		float WorstOutside = 0.0f;
		for (const FCSOpeningProfileSample& Sample : Samples)
		{
			const FVector2f Q = Field.Eval(Sample.S, Sample.ZHigh);
			TestTrue(TEXT("Every profile crown point is on the kept side"), CSHouse_ClipKeeps(Field, Q));
			// 半圆段上 |q| 应当≈1（外接的那点余量之内）。
			if (Sample.ZHigh > SpringZ + 0.5f)
			{
				const float R = FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y);
				WorstOutside = FMath::Max(WorstOutside, R - 1.0f);
				TestTrue(TEXT("Crown points sit on the analytic circle"), FMath::Abs(R - 1.0f) < 0.02f);
			}
		}
		TestTrue(TEXT("The profile circumscribes rather than inscribes"), WorstOutside >= 0.0f);
	}

	// ---- 矩形 ----
	FCSWallOpening Rect;
	Rect.Shape = ECSOpeningShape::Rect;
	Rect.CenterS = 200.0f;
	Rect.Width = 90.0f;
	Rect.Z0 = 110.0f;
	Rect.Z1 = 210.0f;
	const FCSOpeningClipField RectField = CSHouse_ComputeClipField(Rect);
	TestFalse(TEXT("The middle of the window is clipped away"),
		CSHouse_ClipKeeps(RectField, RectField.Eval(200.0f, 160.0f)));
	TestTrue(TEXT("Below the sill is kept"), CSHouse_ClipKeeps(RectField, RectField.Eval(200.0f, 105.0f)));
	TestTrue(TEXT("Above the head is kept"), CSHouse_ClipKeeps(RectField, RectField.Eval(200.0f, 215.0f)));
	TestTrue(TEXT("Beside the reveal is kept"), CSHouse_ClipKeeps(RectField, RectField.Eval(250.0f, 160.0f)));

	// ---- 圆 ----
	FCSWallOpening Circle;
	Circle.Shape = ECSOpeningShape::Circle;
	Circle.CenterS = 250.0f;
	Circle.Width = 80.0f;
	Circle.Z0 = 160.0f;
	Circle.Z1 = 240.0f;
	const FCSOpeningClipField CircleField = CSHouse_ComputeClipField(Circle);
	TestFalse(TEXT("The middle of the oculus is clipped away"),
		CSHouse_ClipKeeps(CircleField, CircleField.Eval(250.0f, 200.0f)));
	// 圆的四个方向都在洞外一点点即保留。
	for (const FVector2D& Dir : { FVector2D(1, 0), FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1) })
	{
		const float S = 250.0f + float(Dir.X) * 42.0f;
		const float Z = 200.0f + float(Dir.Y) * 42.0f;
		TestTrue(TEXT("Just outside the oculus is kept"), CSHouse_ClipKeeps(CircleField, CircleField.Eval(S, Z)));
	}

	// ---- 无洞面板：哨兵 (8, 8) 在任何判据下都保留 ----
	{
		const FCSOpeningClipField Empty;
		TestFalse(TEXT("An empty field is not valid"), Empty.bValid);
		const FVector2f Q = Empty.Eval(123.0f, 45.0f);
		TestTrue(TEXT("The sentinel is the documented (8, 8)"), FMath::IsNearlyEqual(Q.X, 8.0f) && FMath::IsNearlyEqual(Q.Y, 8.0f));
		TestTrue(TEXT("A panel with no opening is kept everywhere"), CSHouse_ClipKeeps(Empty, Q));
	}

	return true;
}

// -----------------------------------------------------------------------------
// 门框曲线的控制点必须等弧长
//
// 散布 kernel 把记录里的 alpha 当**样条参数**用，而 BuildFramePlan 排砖时算的是**弧长
// 占比** —— 两者只有在控制点等距时才等价。原始的 U 形洞缘折线（门樘一整段 160cm、
// 拱缘每段 9.4cm）疏密比 16.9:1，砖会全挤到拱上互相穿模成薄鳍片。这条用例直接用真实
// 的拱剖面走一遍 BuildFramePlan 的组路逻辑，把"重采样后必须等距"钉死。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameCurveUniformTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameCurveUniform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameCurveUniformTest::RunTest(const FString& Parameters)
{
	// 演示关卡里那扇门：600 长墙、3 个拱位 ⇒ 宽 120、高 220 ⇒ 拱脚 160。
	FCSWallOpening Arch;
	Arch.Shape = ECSOpeningShape::Arch;
	Arch.CenterS = 140.0f;
	Arch.Width = 120.0f;
	Arch.Z0 = 0.0f;
	Arch.Z1 = 220.0f;

	TArray<FCSOpeningProfileSample> Samples;
	CSHouse_SampleOpeningProfile(Arch, 0.2f, Samples);
	TestTrue(TEXT("Arch profile yields samples"), Samples.Num() >= 2);

	// BuildFramePlan 的①号路径：左樘底 → 沿洞顶曲线 → 右樘底。
	TArray<FVector> Path;
	Path.Add(FVector(Samples[0].S, 0.0, Samples[0].ZLow));
	for (const FCSOpeningProfileSample& S : Samples) Path.Add(FVector(S.S, 0.0, S.ZHigh));
	Path.Add(FVector(Samples.Last().S, 0.0, Samples.Last().ZLow));

	auto SegmentLengths = [](const TArray<FVector>& Points)
	{
		TArray<double> Out;
		for (int32 K = 1; K < Points.Num(); ++K) Out.Add(FVector::Dist(Points[K - 1], Points[K]));
		return Out;
	};

	// 先证明"原样喂进去确实是坏的"，否则这条用例可能在两边都通过而钉不住任何东西。
	{
		const TArray<double> Raw = SegmentLengths(Path);
		double RawMin = TNumericLimits<double>::Max(), RawMax = 0.0;
		for (const double L : Raw) { RawMin = FMath::Min(RawMin, L); RawMax = FMath::Max(RawMax, L); }
		TestTrue(TEXT("The raw opening polyline is wildly non-uniform"), RawMax / RawMin > 8.0);
	}

	double RawLength = 0.0;
	for (const double L : SegmentLengths(Path)) RawLength += L;

	const float BrickLength = 26.0f;
	const int32 Segments = FMath::Clamp(
		FMath::CeilToInt(RawLength / FMath::Max(BrickLength * 0.5f, 1.0f)), 4, 128);

	TArray<FVector> Even;
	CSShaperSteps::ResampleUniform(Path, Segments, Even);

	TestEqual(TEXT("Resampling yields SegmentCount + 1 points"), Even.Num(), Segments + 1);
	// 端点必须逐位保留：两端外延一格 + 只取内区间那条纪律全靠它。
	TestTrue(TEXT("The first control point is untouched"), Even[0].Equals(Path[0], 1.0e-6));
	TestTrue(TEXT("The last control point is untouched"), Even.Last().Equals(Path.Last(), 1.0e-6));

	const TArray<double> Lengths = SegmentLengths(Even);
	double MinLen = TNumericLimits<double>::Max(), MaxLen = 0.0, Sum = 0.0;
	for (const double L : Lengths) { MinLen = FMath::Min(MinLen, L); MaxLen = FMath::Max(MaxLen, L); Sum += L; }
	// 弦长只在拐弯处比目标短一丝（折线内接圆弧），1% 已经宽松得能容下任何洞形。
	TestTrue(TEXT("Every control segment is the same length"), MaxLen / MinLen < 1.01);
	TestTrue(TEXT("Resampling preserves the arc length"), FMath::Abs(Sum - RawLength) < RawLength * 0.01);

	// 重采样点必须仍然贴在设计曲线上（拱段在解析圆上、门樘段在竖直线上）。
	const double SpringZ = Arch.Z1 - Arch.HalfWidth();
	const double Circum = Samples.Last().S - Arch.CenterS;   // 外接半径（剖面按外接建，略大于半宽）
	for (const FVector& P : Even)
	{
		if (P.Z > SpringZ + 0.5)
		{
			const double Radius = FMath::Sqrt(FMath::Square(P.X - Arch.CenterS) + FMath::Square(P.Z - SpringZ));
			TestTrue(TEXT("Crown samples stay on the analytic circle"), FMath::Abs(Radius - Circum) < 0.3);
		}
		else
		{
			const double Drift = FMath::Min(FMath::Abs(P.X - Path[0].X), FMath::Abs(P.X - Path.Last().X));
			TestTrue(TEXT("Jamb samples stay on the vertical"), Drift < 0.3);
		}
	}

	// 退化输入：点太少 / 零长度折线时原样返回，调用方的 Num() < 3 早退才有意义。
	{
		TArray<FVector> Degenerate = { FVector(1.0, 2.0, 3.0) };
		TArray<FVector> Result;
		CSShaperSteps::ResampleUniform(Degenerate, 8, Result);
		TestEqual(TEXT("A single point is returned untouched"), Result.Num(), 1);

		TArray<FVector> Coincident = { FVector::ZeroVector, FVector::ZeroVector };
		CSShaperSteps::ResampleUniform(Coincident, 8, Result);
		TestEqual(TEXT("A zero-length polyline is returned untouched"), Result.Num(), 2);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 解析推导（裁决一选乙）的三条断言：闭式砖数、与旧路等价、墩上只有一列砖
// -----------------------------------------------------------------------------

namespace
{
/**
 * 旧路（B 样条 + 逐砖记录）在墙空间摆出来的**砖心**。⚠️ **旧路的产线代码已经删掉**
 * （2026-08-30 裁决一第二步：`ACSHouseActor::BuildFramePlan` / `CSShaperSteps::EnsureCapacity`
 * / `CSShaperSteps::Scatter` / `CSGroundSteps.usf` / `csh.FrameLegacy` 全部不在了），
 * 所以这一份**测试内部的 CPU 镜像**从此就是旧路的唯一存本。它逐字镜像当时的三处：
 *   · 组路 / 重采样 / 排砖：`ACSHouseActor::BuildFramePlan` 里的 `EmitCurve`（已删）
 *   · 均匀三次 B 样条：`General.usf:332-361`（`BSplinePosition` / `BSplineMapGlobalAlpha`，仍在）
 *   · alpha → 采样插值：`CSGroundSteps.usf` 的 I0/I1 线性插值（已删）
 *
 * **它不调用任何产线代码**，这正是删掉旧路之后 `House.FrameAnalyticMatchesLegacy` 仍然成立
 * 的原因 —— 也是这条断言必须活下来的原因：解析推导有没有跑偏，只剩它一个守卫。
 * 唯一还与产线共用的是 `CSHouse_SampleOpeningProfile` / `CSShaperSteps::ResampleUniform` /
 * `ACSSplineBlockActor::SolveBlockLayout` 三个纯函数，它们因此**不许跟着旧路一起删**。
 */
void CSHouseTest_LegacyFrameBricks(const FCSWallOpening& Opening, float ChordTolerance,
	float BrickLength, float BrickGap, int32 SeedSalt, TArray<FVector2D>& OutSZ,
	double& OutControlSpacing)
{
	OutSZ.Reset();
	OutControlSpacing = 0.0;

	TArray<FCSOpeningProfileSample> Samples;
	CSHouse_SampleOpeningProfile(Opening, ChordTolerance, Samples);
	if (Samples.Num() < 2) return;

	// ①号路径：左樘底 → 沿洞顶曲线 → 右樘底。墙空间 (S, Z) 摊成 (X, _, Z)。
	TArray<FVector> Path;
	Path.Add(FVector(Samples[0].S, 0.0, Samples[0].ZLow));
	for (const FCSOpeningProfileSample& S : Samples) Path.Add(FVector(S.S, 0.0, S.ZHigh));
	Path.Add(FVector(Samples.Last().S, 0.0, Samples.Last().ZLow));

	double RawLength = 0.0;
	for (int32 K = 1; K < Path.Num(); ++K) RawLength += FVector::Dist(Path[K - 1], Path[K]);
	if (RawLength < BrickLength * 0.5) return;

	const int32 Segments = FMath::Clamp(
		FMath::CeilToInt(RawLength / FMath::Max(BrickLength * 0.5f, 1.0f)), 4, 128);
	TArray<FVector> Even;
	CSShaperSteps::ResampleUniform(Path, Segments, Even);
	if (Even.Num() < 3) return;

	double ArcLength = 0.0;
	for (int32 K = 1; K < Even.Num(); ++K) ArcLength += FVector::Dist(Even[K - 1], Even[K]);
	if (ArcLength < BrickLength * 0.5) return;
	// 控制点间距 h：均匀三次 B 样条把半径 R 的圆弧内缩 h²/(6R)，等价性判据要的就是这个 h。
	OutControlSpacing = ArcLength / Segments;

	FRandomStream Rand(1 * 7919 + SeedSalt);
	TArray<int32> Sequence;
	const TArray<float> Palette = { FMath::Max(BrickLength, 1.0f) };
	const float LayoutScale = ACSSplineBlockActor::SolveBlockLayout(float(ArcLength), BrickGap, Palette, Rand, Sequence);
	if (Sequence.IsEmpty() || LayoutScale <= 0.0f) return;

	// 两端各外延一格：均匀三次 B 样条在控制点处取 (P₋₁ + 4P₀ + P₁)/6，外延点选成
	// 2·E₀ − E₁ 时这一式恰好还原 E₀ —— 端点因此逐位钉在设计位置上。
	TArray<FVector> Ctrl;
	Ctrl.Reserve(Even.Num() + 2);
	Ctrl.Add(Even[0] + (Even[0] - Even[1]));
	Ctrl.Append(Even);
	Ctrl.Add(Even.Last() + (Even.Last() - Even[Even.Num() - 2]));

	const int32 NumCtrl = Ctrl.Num();
	const double Alpha0 = 1.0 / (NumCtrl - 1);
	const double Alpha1 = double(NumCtrl - 2) / (NumCtrl - 1);
	const int32 SampleCount = FMath::Clamp((NumCtrl - 1) * 8, 16, 512);

	auto BSplineAt = [&Ctrl, NumCtrl](double A)
	{
		const int32 NumSeg = FMath::Max(NumCtrl - 1, 1);
		const double G = FMath::Clamp(A, 0.0, 1.0) * NumSeg;
		const int32 Seg = FMath::Min(int32(FMath::FloorToDouble(G)), NumSeg - 1);
		const double T = G - Seg;
		auto Load = [&Ctrl, NumCtrl](int32 I) { return Ctrl[FMath::Clamp(I, 0, NumCtrl - 1)]; };
		const double T2 = T * T, T3 = T2 * T;
		return (-T3 + 3.0 * T2 - 3.0 * T + 1.0) / 6.0 * Load(Seg - 1)
			+ (3.0 * T3 - 6.0 * T2 + 4.0) / 6.0 * Load(Seg)
			+ (-3.0 * T3 + 3.0 * T2 + 3.0 * T + 1.0) / 6.0 * Load(Seg + 1)
			+ T3 / 6.0 * Load(Seg + 2);
	};

	TArray<FVector> Curve;
	Curve.Reserve(SampleCount);
	for (int32 K = 0; K < SampleCount; ++K) Curve.Add(BSplineAt(double(K) / double(SampleCount - 1)));

	double Cursor = 0.0;
	for (int32 Index = 0; Index < Sequence.Num(); ++Index)
	{
		const double BlockLength = Palette[Sequence[Index]] * LayoutScale;
		const double MidArc = Cursor + BlockLength * 0.5;
		Cursor += BlockLength + BrickGap * LayoutScale;
		const double Frac = FMath::Clamp(MidArc / ArcLength, 0.0, 1.0);
		const double Alpha = Alpha0 + Frac * (Alpha1 - Alpha0);

		// 旧 kernel 的采样插值（已删的 `CSGroundSteps.usf`）：alpha → 均匀采样序号 → 相邻两点线性插值。
		const int32 Last = SampleCount - 1;
		const double F = Alpha * Last;
		const int32 I0 = FMath::Min(int32(F), Last);
		const int32 I1 = FMath::Min(I0 + 1, Last);
		const FVector P = FMath::Lerp(Curve[I0], Curve[I1], FMath::Clamp(F - I0, 0.0, 1.0));
		OutSZ.Add(FVector2D(P.X, P.Z));
	}
}

/** 解析路（新路）在墙空间摆出来的砖心。墙框架取单位框架，比较才不掺进任何世界变换。 */
void CSHouseTest_AnalyticFrameBricks(TArrayView<const FCSWallOpening> Openings,
	float BrickLength, float BrickGap, TArray<CSHouseFrame::FElement>& OutElements, TArray<FVector2D>& OutSZ)
{
	OutElements.Reset();
	OutSZ.Reset();

	CSHouseFrame::FBrickParams Params;
	Params.Length = BrickLength;
	Params.Gap = BrickGap;
	Params.MaxBricks = 4096;

	CSHouseFrame::FWallFrame Frame;   // S 对 +X、Z 对 +Z：墙空间 = 世界空间
	CSHouseFrame::BuildEdgeElements(Frame, Openings, Params, OutElements);

	for (const CSHouseFrame::FElement& E : OutElements)
	{
		for (int32 K = 0; K < E.BrickCount; ++K)
		{
			FVector2f SZ, Tangent;
			CSHouseFrame::EvalPath(E.Path, E.HalfLen + K * E.Pitch, SZ, Tangent);
			OutSZ.Add(FVector2D(SZ.X, SZ.Y));
		}
	}
}

/** 演示关卡那扇门：600 长墙、3 个拱位、墩宽 20 ⇒ 段距 160、拱宽 140、拱高 220 ⇒ 起拱线 150。 */
FCSWallOpening CSHouseTest_DemoArch(float CenterS)
{
	FCSWallOpening Arch;
	Arch.Shape = ECSOpeningShape::Arch;
	Arch.CenterS = CenterS;
	Arch.Width = 140.0f;
	Arch.Z0 = 0.0f;
	Arch.Z1 = 220.0f;
	return Arch;
}
}

// -----------------------------------------------------------------------------
// 闭式砖数必须与 SolveBlockLayout **逐个弧长**一致
//
// 解析推导不再排一张逐砖记录表，砖数只能由闭式给出（`CSHouseFrame::SolveRun`）。它是
// 单条目 palette 下 `SolveBlockLayout` 的同式重写 —— 而"同式"这件事必须被钉住：砖数进
// desc 哈希、也进演示回归的断言，差一块就是画面差一块。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameRunMatchesLayoutTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameRunMatchesLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameRunMatchesLayoutTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;

	int32 Mismatch = 0, Jumps = 0, PrevCount = -1;
	double WorstScale = 0.0;
	float WorstArc = 0.0f;
	// 20..1300 cm 覆盖"墩那一小段（150 上下）"到"整条 U 形路（620 上下）"再到放大后的房子。
	// 步长 0.1 cm：砖数的跳变点是连续的实数，粗扫会整段跳过跳变那一帧。
	for (int32 Step = 0; Step <= 12800; ++Step)
	{
		const float Arc = 20.0f + float(Step) * 0.1f;
		for (float GapValue : { 0.0f, 1.5f })
		{
			FRandomStream Rand(1);
			TArray<int32> Sequence;
			const TArray<float> Palette = { BrickLength };
			const float RefScale = ACSSplineBlockActor::SolveBlockLayout(Arc, GapValue, Palette, Rand, Sequence);

			float Scale = 0.0f;
			const int32 Count = CSHouseFrame::SolveRun(Arc, BrickLength, GapValue, Scale);

			if (Count != Sequence.Num() || FMath::Abs(Scale - RefScale) > 1.0e-6f)
			{
				++Mismatch;
				WorstArc = Arc;
			}
			WorstScale = FMath::Max(WorstScale, FMath::Abs(double(Scale) - double(RefScale)));
			if (GapValue == 0.0f)
			{
				if (PrevCount >= 0 && Sequence.Num() != PrevCount) ++Jumps;
				PrevCount = Sequence.Num();
			}
		}
	}

	// 扫描必须真的穿过很多次砖数跳变，否则这条测试测的是空气。
	TestTrue(FString::Printf(TEXT("Sweep crosses brick-count jumps (%d)"), Jumps), Jumps >= 40);
	TestTrue(FString::Printf(TEXT("The closed form matches SolveBlockLayout at every arc length (%d misses, worst at %.1f cm, worst scale delta %.3g)"),
		Mismatch, WorstArc, WorstScale), Mismatch == 0);

	// 退化输入：零长度 / 零砖长一律给 0 块，调用方的早退才有意义。
	{
		float Scale = 1.0f;
		TestEqual(TEXT("A zero-length run yields no bricks"), CSHouseFrame::SolveRun(0.0f, BrickLength, 0.0f, Scale), 0);
		TestEqual(TEXT("A zero-length brick yields no bricks"), CSHouseFrame::SolveRun(500.0f, 0.0f, 0.0f, Scale), 0);
	}
	return true;
}

// -----------------------------------------------------------------------------
// **迁移的等价性判据**：新旧两条路把砖摆在同一个地方
//
// 「同一个地方」不可能是逐位相等，两条路本来就不是同一条曲线：
//   · 新路骑在 `FCSOpeningClipField` 那条**解析**曲线上 —— 那正是材质切洞用的那条边；
//   · 旧路骑在"外接多边形折线 → 等距重采样 → 均匀三次 B 样条"之后的近似曲线上。
// 所以判据不写成"差得不多"，写成**差异恰好等于那条近似的教科书误差、没有别的成分**：
//   ① 砖数逐块相同 —— 弧长参数化一致，这是"摆在同一批位置上"的骨架；
//   ② 门樘（直段）上的偏差 = **纯横向**的外接半径余量 `R·(sec(π/2N) − 1)`：旧路的洞缘折线
//      是按**外接**多边形建的（`CSHouse_SampleOpeningProfile` 的 `Circum`，"宁可覆盖不可漏"），
//      门樘线因此落在解析半宽之外那么一点；新路骑在 clip 场自己的边上，正好把这一点收回来。
//      竖直分量必须近乎为零 —— 那是"只有这一项、没有竖向漂移"的证据。
//   ③ 拱圈（圆弧）上的偏差 = 均匀三次 B 样条把半径 R 的圆弧按控制点间距 h 内缩的闭式量
//      **h²/(6R)**。⚠️ 早先按"90° 折角被抹圆"写这条判据是**错的**，实测把它证伪了：设计路径
//      在起拱线处切向连续（半圆 θ=0 的切向恰是竖直），根本没有折角，偏差在拱圈上均匀分布。
//      三个尺寸实测 Δ·6R/h² = 1.008 / 0.941 / 1.066，对理论值 1。
// 两条都带 ±25%~35% 的容差钉住：这不是"差不多"，是**把差异逐项归因到已知量** —— 朝向翻转、
// 参数化漂移、圆心取错，任何别的成分都会立刻把比值顶出带外。
// 顺带钉住新路的定位依据本身：砖心必须**恰好落在 clip 场的零等值线上**（|q| = 1）。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameAnalyticMatchesLegacyTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameAnalyticMatchesLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameAnalyticMatchesLegacyTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;
	const float BrickGap = CDO->FrameBrickGap;
	const float ChordTolerance = CDO->OpeningChordTolerance;

	// 三种尺寸都过一遍：出厂拱、窄拱、放大近一倍的拱。等价性不能只在一个尺寸上成立。
	const float Widths[3] = { 140.0f, 90.0f, 260.0f };
	const float Heights[3] = { 220.0f, 170.0f, 380.0f };

	for (int32 Case = 0; Case < 3; ++Case)
	{
		FCSWallOpening Arch = CSHouseTest_DemoArch(300.0f);
		Arch.Width = Widths[Case];
		Arch.Z1 = Heights[Case];

		TArray<FVector2D> Legacy;
		double ControlSpacing = 0.0;
		CSHouseTest_LegacyFrameBricks(Arch, ChordTolerance, BrickLength, BrickGap, /*SeedSalt*/ 0, Legacy, ControlSpacing);

		TArray<CSHouseFrame::FElement> Elements;
		TArray<FVector2D> Analytic;
		CSHouseTest_AnalyticFrameBricks(MakeArrayView(&Arch, 1), BrickLength, BrickGap, Elements, Analytic);

		TestTrue(FString::Printf(TEXT("Case %d lays bricks at all (%d)"), Case, Legacy.Num()), Legacy.Num() > 8);
		// ① 骨架：同一条洞缘、同一个排布器 ⇒ 砖数必须一块不差。
		TestEqual(FString::Printf(TEXT("Case %d: the two paths lay the same number of bricks"), Case),
			Analytic.Num(), Legacy.Num());
		if (Analytic.Num() != Legacy.Num()) continue;

		const double SpringZ = FMath::Max(Arch.Z1 - Arch.HalfWidth(), Arch.Z0);
		const double Radius = Arch.HalfWidth();
		double WorstJamb = 0.0, BestJamb = TNumericLimits<double>::Max(), WorstJambZ = 0.0, WorstRing = 0.0;
		int32 JambBricks = 0, RingBricks = 0, JambInward = 0;
		for (int32 K = 0; K < Analytic.Num(); ++K)
		{
			const double Delta = FVector2D::Distance(Analytic[K], Legacy[K]);
			// 拱圈 / 门樘的分界就是起拱线。留半块砖的余量，跨在分界上的那块砖算进拱圈
			// （它有一半确实在圆弧上，按直段判会误判成"直段也有误差"）。
			if (Analytic[K].Y > SpringZ + 0.5 * BrickLength) { WorstRing = FMath::Max(WorstRing, Delta); ++RingBricks; }
			else if (Analytic[K].Y < SpringZ - 0.5 * BrickLength)
			{
				WorstJamb = FMath::Max(WorstJamb, Delta);
				BestJamb = FMath::Min(BestJamb, Delta);
				WorstJambZ = FMath::Max(WorstJambZ, FMath::Abs(Analytic[K].Y - Legacy[K].Y));
				// 新路的门樘线应当**更靠近洞心**（把外接余量收回到 clip 场自己的边上）。
				if (FMath::Abs(Analytic[K].X - Arch.CenterS) < FMath::Abs(Legacy[K].X - Arch.CenterS)) ++JambInward;
				++JambBricks;
			}
		}
		// 外接半径余量：`CSHouse_SampleOpeningProfile` 把圆弧按外接多边形建，顶点半径因此是
		// `R / cos(π/2N)`。这是门樘那一项差异的**唯一**来源，所以用同一个 N 的公开求值器算它。
		const int32 ProfileSegments = CSHouse_ProfileSegments(float(Radius), ChordTolerance);
		const double JambOffset = Radius * (1.0 / FMath::Cos(PI / (2.0 * ProfileSegments)) - 1.0);
		// 外接多边形比真圆弧**长**这么多（周长 2N·R·tan(π/2N) 对 π·R）。旧路按它的弧长排砖，
		// 于是每块砖的弧长位置整体漂一点点 —— 门樘上那一点点就表现为竖向偏差，上限就是它。
		const double PolySurplus = 2.0 * ProfileSegments * Radius * FMath::Tan(PI / (2.0 * ProfileSegments)) - PI * Radius;
		// h²/(6R)：均匀三次 B 样条在控制点处取 (P₋₁ + 4P₀ + P₁)/6，圆弧上等距 h 的三点代进去
		// 恰好把半径缩掉这么多。这是这条判据里唯一允许存在的差异来源。
		const double Predicted = ControlSpacing * ControlSpacing / (6.0 * FMath::Max(Radius, 1.0));
		const double Ratio = Predicted > 0.0 ? WorstRing / Predicted : 0.0;
		const double JambRatio = JambOffset > 0.0 ? WorstJamb / JambOffset : 0.0;
		AddInfo(FString::Printf(TEXT("Case %d (W=%.0f H=%.0f): %d bricks (%d ring / %d jamb), h=%.2f cm; ring delta %.3f vs h²/(6R)=%.3f (ratio %.3f); jamb delta %.4f..%.4f vs R(sec(pi/2N)-1)=%.4f (ratio %.3f), |dZ| %.4f"),
			Case, Widths[Case], Heights[Case], Analytic.Num(), RingBricks, JambBricks,
			ControlSpacing, WorstRing, Predicted, Ratio, BestJamb, WorstJamb, JambOffset, JambRatio, WorstJambZ));

		TestTrue(FString::Printf(TEXT("Case %d samples both the ring and the jambs (%d / %d)"), Case, RingBricks, JambBricks),
			RingBricks >= 4 && JambBricks >= 2);
		// ② 门樘那一项差异基本是**横向**的外接半径余量；竖向那一丝由外接多边形的弧长盈余
		//    `2NR·tan(π/2N) − πR` 封顶（实测约 0.23 倍，三个尺寸一致），方向一律朝内。
		TestTrue(FString::Printf(TEXT("Case %d: the jamb's vertical drift stays under the polygon's arc-length surplus (%.4f of %.4f cm)"),
			Case, WorstJambZ, PolySurplus), WorstJambZ < PolySurplus && WorstJambZ < 0.15);
		TestEqual(FString::Printf(TEXT("Case %d: every jamb brick moves inward onto the clip edge"), Case), JambInward, JambBricks);
		TestTrue(FString::Printf(TEXT("Case %d: the jamb difference is exactly the circumscribed-radius margin (ratio %.3f)"), Case, JambRatio),
			JambRatio > 0.9 && JambRatio < 1.35 && BestJamb > 0.75 * JambOffset);
		// ③ 拱圈上的差异**恰好**是那条闭式的 B 样条圆弧内缩，没有别的成分。
		TestTrue(FString::Printf(TEXT("Case %d: the ring difference is exactly the B-spline arc shrink h²/(6R) (ratio %.3f)"), Case, Ratio),
			Ratio > 0.75 && Ratio < 1.25);
		// 兜底的绝对量：无论比值怎么算，偏差都必须远小于砖长（肉眼与出图都分辨不出）。
		TestTrue(FString::Printf(TEXT("Case %d: and it stays far below one brick (%.3f cm of %.1f)"), Case, WorstRing, BrickLength),
			WorstRing < 0.1 * BrickLength);

		// 新路的定位依据：砖心恰好落在 clip 场的零等值线上（拱圈 |q| = 1、门樘 |q.x| = 1）。
		const FCSOpeningClipField Field = CSHouse_ComputeClipField(Arch);
		double WorstRidge = 0.0;
		for (const FVector2D& P : Analytic)
		{
			const FVector2f Q = Field.Eval(float(P.X), float(P.Y));
			const double Norm = Q.Y > 0.0f ? FMath::Sqrt(double(Q.X) * Q.X + double(Q.Y) * Q.Y) : FMath::Abs(double(Q.X));
			WorstRidge = FMath::Max(WorstRidge, FMath::Abs(Norm - 1.0));
		}
		TestTrue(FString::Printf(TEXT("Case %d: every analytic brick sits on the clip field's own edge (|q|-1 = %.3g)"), Case, WorstRidge),
			WorstRidge < 1.0e-3);
	}
	return true;
}

// -----------------------------------------------------------------------------
// 拱间墩上只砌**一列**砖
//
// 旧路让相邻两拱各出一条门樘砖脚、各伸进跨度一半 ⇒ 两列砖在墩正中**共面对接**，从地面
// 一直贯着一条竖缝（出图 `pier_after_pier.png` 可见），而 TG 实拍里墩就是一列。
// 这条用例把"一列"钉成几何断言，而不是留给出图去看：
//   ① 跨度里的砖心 S 只有**一个**取值（一列），且恰在墩心；
//   ② 那一列覆盖满整条跨度（`PierWidth ≤ FrameBrickDepth` 那条配平条件的执行面）；
//   ③ 两侧的拱在墩那一侧**不再出门樘**（否则就是三列，比两列更糟）；
//   ④ 对照组：把墩样式位清掉（= 旧拓扑）时，跨度里恰好出现**两**列，且它们在墩正中对接
//      —— 没有这一条，①③ 可能只是在描述一个本来就不会发生的情况。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFramePierSingleColumnTest,
	"PCGPlugins.ComputeShaderGenerator.House.FramePierSingleColumn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFramePierSingleColumnTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;
	const float BrickGap = CDO->FrameBrickGap;
	const float BrickDepth = CDO->FrameBrickDepth;

	// 演示关卡的连拱：段距 160、拱宽 140 ⇒ 相邻两拱之间的跨度恰好 20 = PierWidth。
	FCSWallOpening Left = CSHouseTest_DemoArch(140.0f);
	FCSWallOpening Right = CSHouseTest_DemoArch(300.0f);
	const double SpanMin = Left.S1(), SpanMax = Right.S0();
	const double PierCentre = (SpanMin + SpanMax) * 0.5;
	const double Span = SpanMax - SpanMin;
	TestTrue(FString::Printf(TEXT("The two arches leave a pier span (%.1f cm)"), Span), Span > 1.0);

	const double SpringZ = Left.Z1 - Left.HalfWidth();
	auto ColumnsInSpan = [SpanMin, SpanMax, SpringZ](const TArray<FVector2D>& Bricks)
	{
		// 起拱线**以下**、落在跨度里的砖心（拱圈上的砖不算 —— 它们本来就骑在洞缘上）。
		TArray<double> Columns;
		for (const FVector2D& P : Bricks)
		{
			if (P.Y > SpringZ - 1.0) continue;
			if (P.X < SpanMin - 0.5 || P.X > SpanMax + 0.5) continue;
			bool bKnown = false;
			for (const double S : Columns) bKnown |= FMath::Abs(S - P.X) < 0.5;
			if (!bKnown) Columns.Add(P.X);
		}
		Columns.Sort();
		return Columns;
	};

	// ---- 新拓扑：墩样式位打上 ----
	{
		FCSWallOpening A = Left, B = Right;
		A.StyleFlags |= CSHouse_StylePierAfter;
		B.StyleFlags |= CSHouse_StylePierBefore;
		const FCSWallOpening Pair[2] = { A, B };

		TArray<CSHouseFrame::FElement> Elements;
		TArray<FVector2D> Bricks;
		CSHouseTest_AnalyticFrameBricks(MakeArrayView(Pair, 2), BrickLength, BrickGap, Elements, Bricks);

		// ③ 两拱各自砌一条路，墩单独一条 ⇒ 恰好三条。
		TestEqual(TEXT("Two arches sharing a pier lay three brick paths"), Elements.Num(), 3);
		if (Elements.Num() == 3)
		{
			TestTrue(TEXT("The left arch drops its pier-side jamb"), Elements[0].Path.bLeftJamb && !Elements[0].Path.bRightJamb);
			TestTrue(TEXT("The pier is a bare vertical column at the span centre"),
				Elements[1].Path.MidKind == CSHouseFrame::EMidKind::None
				&& FMath::IsNearlyEqual(double(Elements[1].Path.LeftS), PierCentre, 0.01)
				&& FMath::IsNearlyEqual(double(Elements[1].Path.TopZ), SpringZ, 0.01));
			TestTrue(TEXT("The right arch drops its pier-side jamb"), !Elements[2].Path.bLeftJamb && Elements[2].Path.bRightJamb);
		}

		// ① 一列，且恰在墩心。
		const TArray<double> Columns = ColumnsInSpan(Bricks);
		TestEqual(FString::Printf(TEXT("The pier carries exactly one brick column (%d)"), Columns.Num()), Columns.Num(), 1);
		if (Columns.Num() == 1)
		{
			TestTrue(FString::Printf(TEXT("That column stands on the pier centre (%.2f vs %.2f)"), Columns[0], PierCentre),
				FMath::Abs(Columns[0] - PierCentre) < 0.01);
			// ② 覆盖：一列砖横向占 FrameBrickDepth，要盖满跨度就要求 PierWidth ≤ FrameBrickDepth。
			TestTrue(FString::Printf(TEXT("One column of depth %.1f covers the whole %.1f cm span"), BrickDepth, Span),
				Columns[0] - BrickDepth * 0.5 <= SpanMin + 0.01 && Columns[0] + BrickDepth * 0.5 >= SpanMax - 0.01);
		}
		// 墩必须真的砌到起拱线：矮一截就会在墩顶露出一段被裁空的灰泥。
		double PierTop = 0.0;
		for (const FVector2D& P : Bricks) if (FMath::Abs(P.X - PierCentre) < 0.5) PierTop = FMath::Max(PierTop, P.Y);
		TestTrue(FString::Printf(TEXT("The pier column reaches the springing line (%.1f of %.1f)"), PierTop, SpringZ),
			PierTop > SpringZ - BrickLength);
	}

	// ---- 对照组：旧拓扑（不打墩样式位）⇒ 两列砖在墩正中对接，那就是那条竖缝 ----
	{
		const FCSWallOpening Pair[2] = { Left, Right };
		TArray<CSHouseFrame::FElement> Elements;
		TArray<FVector2D> Bricks;
		CSHouseTest_AnalyticFrameBricks(MakeArrayView(Pair, 2), BrickLength, BrickGap, Elements, Bricks);

		const TArray<double> Columns = ColumnsInSpan(Bricks);
		TestEqual(FString::Printf(TEXT("Without the pier merge the span carries two columns (%d)"), Columns.Num()),
			Columns.Num(), 2);
		if (Columns.Num() == 2)
		{
			// 两列各伸进跨度半个进深 ⇒ 右缘与左缘在墩正中重合，那条竖缝就是这么来的。
			const double SeamLeft = Columns[0] + BrickDepth * 0.5;
			const double SeamRight = Columns[1] - BrickDepth * 0.5;
			TestTrue(FString::Printf(TEXT("The two legacy columns butt exactly at the pier centre (%.2f / %.2f vs %.2f)"),
				SeamLeft, SeamRight, PierCentre),
				FMath::Abs(SeamLeft - PierCentre) < 0.51 && FMath::Abs(SeamRight - PierCentre) < 0.51);
		}
	}
	return true;
}

// -----------------------------------------------------------------------------
// 门框砖的**负缝**：任何弧长、任何块数下相邻砖都必须互相穿插
//
// 这条断言就是"胀大砖"那条改动的全部意义。门框砖的块数随洞口弧长跳变（SolveBlockLayout
// 变数量、近定距），跳变那一帧如果砖缝是正的，整条拱缘会同时露出一条缝 —— 而 TG 之所以
// 逐帧重排砖也看不出跳变，正是因为它的砖缝本来就是负的。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameBrickOverlapTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameBrickOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameBrickOverlapTest::RunTest(const FString& Parameters)
{
	// 参数取**出厂默认**（CDO），断言才真的钉住线上的观感：谁把 FrameBrickGap 调回正缝、
	// 或把 FrameBrickBloat 调回 1，这里立刻红，而不是等出图时用肉眼去找那条缝。
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;

	// 砖缝 = 相邻砖中心距 − 渲染长度。两行公式与产线**逐字对应**，改一处就要改这里：
	//   · 中心距：`CSHouseFrame::SolveRun` 定下的 `Pitch`（= 砖长 × 缩放 + 缝 × 缩放）；
	//   · 渲染长度：EnsureFrameComponent 的 BlockSize.Y = FrameBrickLength × Bloat，
	//     再被 CSHouseFrame.usf 的 `BlockScale.y *= LayoutScale` 乘上铺装缩放。
	auto Sweep = [BrickLength](float GapValue, float BloatValue, int32& OutCountJumps,
		float& OutMinSeam, float& OutMaxSeam)
	{
		OutCountJumps = 0;
		OutMinSeam = TNumericLimits<float>::Max();
		OutMaxSeam = -TNumericLimits<float>::Max();

		const TArray<float> Palette = { BrickLength };
		int32 PrevCount = -1;
		// 200..1200 cm 覆盖门樘 + 拱缘的真实弧长范围（演示房子实测一条曲线 ≈ 500 cm / 19 块）。
		// 步长 0.1 cm：块数跳变点是连续的实数，粗扫会整段跳过跳变那一帧。
		for (int32 Step = 0; Step <= 10000; ++Step)
		{
			const float Arc = 200.0f + float(Step) * 0.1f;
			FRandomStream Rand(1);
			TArray<int32> Sequence;
			const float Scale = ACSSplineBlockActor::SolveBlockLayout(Arc, GapValue, Palette, Rand, Sequence);
			if (Sequence.Num() < 2 || Scale <= 0.0f) continue;
			if (PrevCount >= 0 && Sequence.Num() != PrevCount) ++OutCountJumps;
			PrevCount = Sequence.Num();

			const float Pitch = (BrickLength + GapValue) * Scale;
			const float RenderLength = BrickLength * BloatValue * Scale;
			const float Seam = Pitch - RenderLength;     // > 0 露缝，< 0 穿插
			OutMinSeam = FMath::Min(OutMinSeam, Seam);
			OutMaxSeam = FMath::Max(OutMaxSeam, Seam);
		}
	};

	int32 CountJumps = 0;
	float MinSeam = 0.0f;
	float MaxSeam = 0.0f;
	Sweep(CDO->FrameBrickGap, CDO->FrameBrickBloat, CountJumps, MinSeam, MaxSeam);

	// 扫描必须真的穿过很多次块数跳变，否则这条测试测的是空气。
	TestTrue(FString::Printf(TEXT("Sweep crosses brick-count jumps (%d)"), CountJumps), CountJumps >= 8);
	// 全部意义所在：块数怎么跳，砖缝都是负的。
	TestTrue(FString::Printf(TEXT("Seam stays negative at every brick count (worst %.3f cm)"), MaxSeam),
		MaxSeam < 0.0f);
	// 而且穿插量始终有厚度 —— 差一丁点就穿插不上的话，浮点/铺装缩放一抖就还是会露缝。
	TestTrue(FString::Printf(TEXT("Interpenetration stays substantial (worst %.3f cm)"), MaxSeam),
		MaxSeam < -0.05f * BrickLength);
	// 反向护栏：穿插量不能大到吃掉整块砖（那就不是砌缝，是砖叠砖）。
	TestTrue(FString::Printf(TEXT("Interpenetration never swallows a whole brick (%.3f cm)"), MinSeam),
		MinSeam > -BrickLength);
	// TG 语义"砖数一变只是穿插量微调"：整个扫描里穿插量的摆幅要远小于砖长。
	TestTrue(FString::Printf(TEXT("Interpenetration only wobbles across jumps (%.3f cm)"), MaxSeam - MinSeam),
		(MaxSeam - MinSeam) < 0.25f * BrickLength);

	// 对照组 = 改动前的出厂参数（gap 1.5、不胀）：同一扫描下砖缝恒为正 —— 那就是拱缘上
	// 一格一格断开的那条缝，也是这条改动存在的理由。留着它，免得有人把默认值改回去。
	int32 LegacyJumps = 0;
	float LegacyMinSeam = 0.0f;
	float LegacyMaxSeam = 0.0f;
	Sweep(1.5f, 1.0f, LegacyJumps, LegacyMinSeam, LegacyMaxSeam);
	TestTrue(FString::Printf(TEXT("Legacy gap 1.5 without bloat always leaves a positive seam (best %.3f cm)"), LegacyMinSeam),
		LegacyMinSeam > 0.0f);
	return true;
}

// -----------------------------------------------------------------------------
// 墙-顶收口：檐口楔形缝里到底有没有实体 / 山墙与屋面底是不是共面 / 屋脊是收口还是互穿
//
// 为什么不是"从室内往天上打一条射线"：檐口那道楔形空腔在**数学上**是封住的 ——
// CSHouseRoof_EvalZAcross 在 footprint 边界处按构造等于墙顶（EaveZ），墙顶面与屋面底沿墙外棱
// 相切，任何直线射线都跨不过那条切线，所以射线判据在**修之前也全绿**，什么都测不出来。
// 真正的破绽是这条"封口"宽度为零：屋面底那张大四边形从墙顶外棱的**内部**横切过去（T 型接缝），
// 而顶点是 float32 世界坐标 —— 缝里没有任何实体，封口靠的是两张面在一条线上恰好相等。
// 所以断言落在**体积**上：楔形缝里每一点都必须被实体包住，且封口与屋面板是**互穿**（深度 ≥ 2）
// 而不是相切。这条判据与 FootprintSize / RoofPitch / RoofOverhang / 脊向 / 墙厚全都无关地成立。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseEaveSealedTest,
	"PCGPlugins.ComputeShaderGenerator.House.EaveSealed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
/**
 * 从 P 朝正上方数"套着几层实体"。
 *
 * 房体是**凸块的并集**（面板盒、窗台盒、屋面板、山墙棱柱、檐口封口楔形全是凸的，且每块闭合、
 * 法线朝外）。对一个凸块：起点在块内的竖直射线只穿出一次（N·up > 0，记 +1）；起点在块外则
 * 一进一出净 0。于是**所有交点上 sign(N·up) 的和 = 包住 P 的块数** —— 不要求 mesh 是流形，
 * 也不受"埋在实心里的背靠背重复面"影响（那种一对正好抵消）。
 * 竖直射线穿不过竖直面，所以 XY 投影退化的三角直接跳过。
 */
int32 CSHouseTest_SolidDepth(const FCSGpuMeshCPUData& S, const FVector& P)
{
	int32 Depth = 0;
	const int32 NumTris = S.Indices.Num() / 3;
	auto Cross2 = [](const FVector2D& U, const FVector2D& V) { return U.X * V.Y - U.Y * V.X; };
	for (int32 Tri = 0; Tri < NumTris; ++Tri)
	{
		const uint32 I0 = S.Indices[Tri * 3], I1 = S.Indices[Tri * 3 + 1], I2 = S.Indices[Tri * 3 + 2];
		const FVector A(S.Positions[I0]), B(S.Positions[I1]), C(S.Positions[I2]);
		const FVector2D A2(A.X, A.Y), B2(B.X, B.Y), C2(C.X, C.Y);

		const double Area2 = Cross2(B2 - A2, C2 - A2);
		if (FMath::Abs(Area2) < 1.0e-6) continue;   // 竖直面 / 退化三角：与竖直射线无关

		const FVector2D Q(P.X, P.Y);
		const double WA = Cross2(C2 - B2, Q - B2);
		const double WB = Cross2(A2 - C2, Q - C2);
		const double WC = Cross2(B2 - A2, Q - A2);
		if (!((WA >= 0 && WB >= 0 && WC >= 0) || (WA <= 0 && WB <= 0 && WC <= 0))) continue;

		const double HitZ = (WA * A.Z + WB * B.Z + WC * C.Z) / Area2;
		if (HitZ <= P.Z + 1.0e-4) continue;

		const double NormalZ = double(S.Normals[I0].Z);
		if (NormalZ > 0) ++Depth;
		else if (NormalZ < 0) --Depth;
	}
	return Depth;
}

/** 一组房体参数（脊向 / 底面 / 坡度 / 外挑 / 板厚 / 墙高 / 墙厚）。 */
struct FCSHouseTestBodyCase
{
	ECSRidgeAxis Axis;
	double SizeX, SizeY;
	float Pitch, Overhang, RoofThickness, WallHeight, WallThickness;
	const TCHAR* What;
};

FCSHouseBodyDesc CSHouseTest_MakeBody(const FCSHouseTestBodyCase& Case)
{
	FCSHouseBodyDesc Body;
	Body.Roof.RidgeAxis = Case.Axis;
	Body.Roof.Footprint = FVector2D(Case.SizeX, Case.SizeY);
	Body.Roof.EaveZ = Case.WallHeight;
	Body.Roof.Pitch = Case.Pitch;
	Body.Roof.Overhang = Case.Overhang;
	Body.Roof.Thickness = Case.RoofThickness;
	Body.Footprint = Body.Roof.Footprint;
	Body.WallHeight = Case.WallHeight;
	Body.WallThickness = Case.WallThickness;
	return Body;   // World = Identity ⇒ 三角汤就是局部坐标
}
}

bool FCSHouseEaveSealedTest::RunTest(const FString& Parameters)
{
	// 覆盖面就是这条断言的价值：只把演示那一个尺寸调对等于什么都没证。每一行都换掉至少两个
	// 会改变收口形状的量 —— 脊向决定哪两面是檐墙（两组边的长度口径还不一样）、外挑 0 是
	// 屋面端面与墙外表面共面的退化位、板厚下限是咬入量最小的最坏情况、坡度上下限决定缝有多深。
	const FCSHouseTestBodyCase Cases[] = {
		{ ECSRidgeAxis::X,  600, 400, 35.0f, 25.0f, 12.0f, 300.0f, 24.0f, TEXT("demo size") },
		{ ECSRidgeAxis::Y,  600, 400, 35.0f, 25.0f, 12.0f, 300.0f, 24.0f, TEXT("demo size, ridge along Y") },
		{ ECSRidgeAxis::X,  400, 600, 20.0f,  0.0f, 12.0f, 300.0f, 24.0f, TEXT("zero overhang") },
		{ ECSRidgeAxis::Y,  400, 600, 60.0f, 90.0f,  6.0f, 240.0f, 40.0f, TEXT("steep, deep overhang, thick wall") },
		{ ECSRidgeAxis::X, 1000, 320, 15.0f, 60.0f, 20.0f, 420.0f, 12.0f, TEXT("shallow, thick slab, thin wall") },
		{ ECSRidgeAxis::Y,  260, 250, 45.0f, 25.0f,  2.0f, 300.0f, 24.0f, TEXT("near square, thinnest slab") },
		{ ECSRidgeAxis::X,  250, 250, 70.0f, 25.0f, 12.0f, 300.0f, 24.0f, TEXT("pitch clamp") },
	};

	// 采样比例一律取"看着不像整数"的值：正好落在面板 / 棱柱边界上的竖直射线判定是模糊的。
	// 0.008 与 0.993 是**故意**贴到山墙棱柱盖着的那两小段上 —— 檐口封口件在那里是缺席的，
	// 转角靠山墙盖，漏了这两个比例就测不到"四条边处理不一样"的那两个角。
	const double AlongFrac[] = { 0.008, 0.037, 0.213, 0.409, 0.5, 0.661, 0.837, 0.971, 0.993 };
	const double DepthFrac[] = { 0.13, 0.47, 0.86 };
	const double HeightFrac[] = { 0.06, 0.31, 0.62, 0.93 };
	// 屋脊那组探针尤其不能取整比例：竖直射线落在**四边形的对角线**上时两个三角会同时判中，
	// 深度凭空 +1。踩过一次 —— 外挑 0 时 (沿脊 0, 跨度 半跨/2) 恰好压在屋面板上下表面的对角线上，
	// 报出"深度 2 = 两板互穿"，其实两板是好的。深度判据只会**多**数不会少数，所以只有
	// 「恰好等于 1」这一条会被它咬到。
	const double RidgeAbsCm[] = { 0.37, -0.37, 2.13, -2.13, 5.31, -5.31 };
	const double RidgeFrac[] = { 0.023, -0.023, 0.517, -0.517, 0.971, -0.971 };

	for (const FCSHouseTestBodyCase& Case : Cases)
	{
		const FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
		const FCSRoofDesc& Roof = Body.Roof;
		FCSGpuMeshCPUData S;
		CSHouse_BuildBodySoup(Body, S);

		const float T = Body.WallThickness;
		const float H = Body.WallHeight;
		const float LA = Roof.RidgeLength();
		const float HalfSpan = Roof.HalfSpan();
		const float RampW = FMath::Min(T, HalfSpan);
		const float SlabVert = CSHouseRoof_SlabVerticalThickness(Roof);
		const double HX = Body.Footprint.X * 0.5, HY = Body.Footprint.Y * 0.5;

		int32 OpenSamples = 0, TangentSamples = 0;
		double WorstOpenGap = 0.0;
		FVector WorstOpenAt = FVector::ZeroVector;

		// ---- ① 墙顶到屋面底之间那条楔形缝：**周界带**上每一点都必须被实体包住 ----
		// 周界带按 footprint 反推（离某条边不超过一个墙厚），不照抄生成器的四段分法 ——
		// 照抄的话生成器漏了哪条边，断言也会跟着漏。
		for (int32 Side = 0; Side < 4; ++Side)
		{
			for (double FA : AlongFrac)
			{
				for (double FD : DepthFrac)
				{
					// Side 0/2 = ±Y 那两面（带沿 X 走），1/3 = ±X 那两面。
					const double Inset = FD * T;
					double X = 0, Y = 0;
					if ((Side & 1) == 0)
					{
						X = -HX + FA * Body.Footprint.X;
						Y = (Side == 0) ? (-HY + Inset) : (HY - Inset);
					}
					else
					{
						Y = -HY + FA * Body.Footprint.Y;
						X = (Side == 1) ? (HX - Inset) : (-HX + Inset);
					}

					const FVector2D XY(X, Y);
					const double Across = Roof.LocalToAcross(XY);
					const double SoffitZ = CSHouseRoof_EvalZAcross(Roof, Across);
					if (SoffitZ - H < 0.05) continue;   // 墙外棱附近缝高本就是 0，没有"缝里"可言

					for (double FZ : HeightFrac)
					{
						const FVector P(X, Y, H + FZ * (SoffitZ - H));
						if (CSHouseTest_SolidDepth(S, P) < 1)
						{
							++OpenSamples;
							if (SoffitZ - H > WorstOpenGap) { WorstOpenGap = SoffitZ - H; WorstOpenAt = P; }
						}
					}

					// ---- ② 封口不是"刚好贴住"而是**咬进**屋面板：同一处 XY 在屋面底之上半个
					//      咬入量的地方，必须同时属于封口件和屋面板（深度 ≥ 2）。零余量共面正是
					//      山墙那条发丝亮线的成因，这一条把两处一起钉住。
					const float Bite = CSHouseRoof_SoffitBite(Roof, Across, RampW);
					if (Bite > 0.2f && CSHouseTest_SolidDepth(S, FVector(X, Y, SoffitZ + 0.5 * Bite)) < 2)
					{
						++TangentSamples;
					}
				}
			}
		}

		TestTrue(FString::Printf(TEXT("[%s] no sky through the eave wedge (%d open samples, worst gap %.2f cm at %s)"),
			Case.What, OpenSamples, WorstOpenGap, *WorstOpenAt.ToString()), OpenSamples == 0);
		TestTrue(FString::Printf(TEXT("[%s] wall tops bite into the slab instead of touching it (%d tangent samples)"),
			Case.What, TangentSamples), TangentSamples == 0);

		// ---- ② 补一刀：山墙**整条跨度**（不只周界带）都要咬进屋面板 ----
		int32 GableTangent = 0;
		for (int32 End = 0; End < 2; ++End)
		{
			const double Along = (End == 0 ? 1.0 : -1.0) * (LA * 0.5 - T * 0.5);
			for (double AF : { 0.041, 0.313, -0.313, 0.687, -0.687 })
			{
				const double Across = AF * HalfSpan;
				const float Bite = CSHouseRoof_SoffitBite(Roof, Across, RampW);
				if (Bite <= 0.2f) continue;
				const FVector L = Roof.RidgeToLocal(Along, Across, CSHouseRoof_EvalZAcross(Roof, Across) + 0.5 * Bite);
				if (CSHouseTest_SolidDepth(S, L) < 2) ++GableTangent;
			}
		}
		TestTrue(FString::Printf(TEXT("[%s] the gable slope is not coplanar with the roof underside (%d tangent samples)"),
			Case.What, GableTangent), GableTangent == 0);

		// ---- ③ 屋脊：两块坡板对切收口，不互穿 ----
		// 判据一：沿脊中点（山墙够不着的地方）实体深度恒为 1。互穿的话过冲那一段是 2。
		int32 RidgeOverlap = 0;
		FString RidgeWorst;
		const double ProbeAlong = 0.211 * LA * 0.5;   // 远离两端山墙，又不落在任何对称位上
		auto ProbeSlab = [&](double Across)
		{
			const FVector L = Roof.RidgeToLocal(ProbeAlong, Across, CSHouseRoof_EvalZAcross(Roof, Across) + 0.5 * SlabVert);
			const int32 D = CSHouseTest_SolidDepth(S, L);
			if (D != 1)
			{
				++RidgeOverlap;
				if (RidgeWorst.IsEmpty()) RidgeWorst = FString::Printf(TEXT("across=%.2f depth=%d"), Across, D);
			}
		};
		for (double AC : RidgeAbsCm) if (FMath::Abs(AC) < HalfSpan) ProbeSlab(AC);
		for (double AF : RidgeFrac) ProbeSlab(AF * HalfSpan);
		TestTrue(FString::Printf(TEXT("[%s] the two slabs miter at the ridge instead of crossing (%d doubled samples, first %s)"),
			Case.What, RidgeOverlap, *RidgeWorst), RidgeOverlap == 0);

		// 判据二：屋面覆盖范围内不许有任何顶点戳出屋面板的上表面 —— 互穿时露出来的那个交叉小尖
		// （旧做法 ≈ Thickness·sin(pitch)）就是这条抓的东西，山墙 / 封口咬得太深也一样抓。
		float WorstPoke = 0.0f;
		for (const FVector3f& Pf : S.Positions)
		{
			const FVector2D XY(Pf.X, Pf.Y);
			if (!CSHouseRoof_IsUnderRoof(Roof, XY)) continue;
			const float TopZ = CSHouseRoof_EvalZAcross(Roof, Roof.LocalToAcross(XY)) + SlabVert;
			WorstPoke = FMath::Max(WorstPoke, float(Pf.Z) - TopZ);
		}
		TestTrue(FString::Printf(TEXT("[%s] nothing pokes through the roof top surface (%.3f cm)"), Case.What, WorstPoke),
			WorstPoke < 0.05f);
	}
	return true;
}

// -----------------------------------------------------------------------------
// 拱间墩的判定：双阈迟回 + 「跨度只在两侧都是落地的拱时才成立」
//
// 迟回是**路径依赖**的：同一个跨度宽度，从窄边过来是墩、从宽边过来是墙。这不是可有可无的
// 打磨 —— 跨度是拉尺寸 / 离地收窄的**连续**函数，单阈会让它在阈值附近抖动时整面墙的灰泥
// 忽有忽无，比"选错一种样式"难看得多。所以这里不只测两条阈值，还要真扫一趟带内抖动。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHousePierHysteresisTest,
	"PCGPlugins.ComputeShaderGenerator.House.PierHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHousePierHysteresisTest::RunTest(const FString& Parameters)
{
	const FCSHousePierStyle Style;   // 默认 60 / 75，与 ACSHouseActor 的两个 UPROPERTY 同源

	// ---- ① 迟回带之外：结论与历史无关 ----
	TestTrue(TEXT("A narrow span is a pier whichever side it came from"),
		CSHouse_SpanIsPier(Style, 40.0f, false) && CSHouse_SpanIsPier(Style, 40.0f, true));
	TestFalse(TEXT("A wide span is a wall whichever side it came from"),
		CSHouse_SpanIsPier(Style, 90.0f, false) || CSHouse_SpanIsPier(Style, 90.0f, true));

	// ---- ② 迟回带内 (60, 75)：结论**只**由历史决定 ----
	for (const float Span : { 60.5f, 67.0f, 74.5f })
	{
		TestTrue(FString::Printf(TEXT("Span %.1f stays a pier when it already was one"), Span),
			CSHouse_SpanIsPier(Style, Span, true));
		TestFalse(FString::Printf(TEXT("Span %.1f stays a wall when it already was one"), Span),
			CSHouse_SpanIsPier(Style, Span, false));
	}

	// ---- ③ 带内来回抖 20 趟：一次都不许翻 ----
	// 这条就是本轮验收门那句"跨度在 60/75 之间来回时不许反复切换样式"的纯函数版。
	{
		bool bState = CSHouse_SpanIsPier(Style, 40.0f, false);   // 先从窄边进入墩态
		int32 Flips = 0;
		for (int32 Round = 0; Round < 20; ++Round)
		{
			for (const float Span : { 62.0f, 73.0f, 66.5f, 71.0f })
			{
				const bool bNext = CSHouse_SpanIsPier(Style, Span, bState);
				if (bNext != bState) ++Flips;
				bState = bNext;
			}
		}
		TestTrue(TEXT("Dithering inside the band never flips the style"), Flips == 0 && bState);
	}

	// ---- ④ 单调一往一返：样式恰好翻两次（进带不翻、出带才翻）----
	{
		// 起点必须**用起点跨度自己定**，不能拿一个凭空的 false 当初值 ——
		// 那会把"第一次求值"也数成一次翻转（实测：数出 3 次）。
		bool bState = CSHouse_SpanIsPier(Style, 30.0f, false);
		int32 Flips = 0;
		for (float Span = 31.0f; Span <= 100.0f; Span += 1.0f)
		{
			const bool bNext = CSHouse_SpanIsPier(Style, Span, bState);
			if (bNext != bState) ++Flips;
			bState = bNext;
		}
		for (float Span = 100.0f; Span >= 30.0f; Span -= 1.0f)
		{
			const bool bNext = CSHouse_SpanIsPier(Style, Span, bState);
			if (bNext != bState) ++Flips;
			bState = bNext;
		}
		TestTrue(FString::Printf(TEXT("A full narrow-wide-narrow sweep flips exactly twice (got %d)"), Flips),
			Flips == 2 && bState);
	}

	// ---- ⑤ 退化输入 ----
	{
		FCSHousePierStyle Off;
		Off.bEnabled = false;
		TestFalse(TEXT("Disabled means never a pier"), CSHouse_SpanIsPier(Off, 1.0f, true));

		// 高阈被填得比低阈还小 ⇒ 退化成单阈，绝不能出现"转墩了就再也转不回来"。
		FCSHousePierStyle Inverted;
		Inverted.MaxWidth = 60.0f;
		Inverted.RestoreWidth = 10.0f;
		TestFalse(TEXT("An inverted pair still lets a wide span go back to wall"),
			CSHouse_SpanIsPier(Inverted, 80.0f, true));
	}

	// ---- ⑥ 跨度本身：只认"两侧都是落地的拱"，墩顶取两条起拱线的较低者 ----
	{
		FCSWallOpening Left;
		Left.Shape = ECSOpeningShape::Arch;
		Left.EdgeIndex = 0;
		Left.CenterS = 200.0f;
		Left.Width = 120.0f;
		Left.Z0 = 0.0f;
		Left.Z1 = 220.0f;                       // 起拱线 220 − 60 = 160

		FCSWallOpening Right = Left;
		Right.CenterS = 360.0f;
		Right.Width = 100.0f;
		Right.Z1 = 260.0f;                      // 起拱线 260 − 50 = 210，比左边高

		float Span = 0.0f, TopZ = 0.0f;
		TestTrue(TEXT("Two grounded arches on one edge yield a span"), CSHouse_PierSpanBetween(Left, Right, Span, TopZ));
		TestEqual(TEXT("The span is measured between the openings, not between the panel cells"), Span, 50.0f);
		// 取较高者会在两条起拱线之间留一条横缝：低的那一拱在那里已经把洞开满了，
		// 而墩上的灰泥还没开始砌。
		TestEqual(TEXT("The pier top is the lower of the two springing lines"), TopZ, 160.0f);

		FCSWallOpening OtherEdge = Right;
		OtherEdge.EdgeIndex = 1;
		TestFalse(TEXT("Openings on different edges are not a span"),
			CSHouse_PierSpanBetween(Left, OtherEdge, Span, TopZ));

		FCSWallOpening Window = Right;
		Window.Z0 = 90.0f;                      // 窗：它下面那截墙是窗台，不是墩
		TestFalse(TEXT("A sill-height opening is not a pier side"), CSHouse_PierSpanBetween(Left, Window, Span, TopZ));

		FCSWallOpening RectHole = Right;
		RectHole.Shape = ECSOpeningShape::Rect; // 矩形洞没有起拱线 ⇒ 墩顶无定义
		TestFalse(TEXT("A rectangular opening is not a pier side"), CSHouse_PierSpanBetween(Left, RectHole, Span, TopZ));

		FCSWallOpening Overlapping = Right;
		Overlapping.CenterS = 250.0f;           // 与左洞重叠：谓词本该挡住，这里不许当成负跨度
		TestFalse(TEXT("Overlapping openings are not a span"), CSHouse_PierSpanBetween(Left, Overlapping, Span, TopZ));
	}

	return true;
}

// -----------------------------------------------------------------------------
// 连续拱之间到底还剩不剩灰泥（计划 D6 的实拍裁决，Docs/TinyGlade/img/TG_continuous_arches.png）
//
// 实拍读出来的是：起拱线以下**没有"墙"这个表面**，两侧直接透到背景草地，那一截只有门樘砖
// 自己站着。
//
// ⚠️ 判据必须**两条一起立**，只立一条都会写成空判据或者写反：
//  ① **几何仍然实心**（2026-08-30 裁决三：避免所有真几何洞）—— 面板照砌，一块都不许少。
//     只测"跨度里没实体"的话，等于把裁决三反过来钉死了。
//  ② **像素上被裁掉** —— 拿顶点色 B 通道还原形状 id、拿 UV1 当 q，逐字复刻材质那一侧的判据。
//     只测几何的话，把裁剪场漏掉也照样绿。
//
// ⚠️ 还有一条容易写成恒真：默认参数下拱宽 = 段距 − 墩宽，两个**面板格**首尾相接，格之间那块
// "实心段"宽度本来就是 0 —— 断言"实心段没了"永远成立。真正要抹掉的灰泥是两个格各自伸进跨度的
// **端盖**（各半个墩宽），所以探针必须打在跨度里、贴着洞缘那两侧。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHousePierPlasterTest,
	"PCGPlugins.ComputeShaderGenerator.House.PierPlaster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
/**
 * 逐字复刻材质那一侧：从顶点色 B 还原形状 id（255 = 这块面板没有洞）、拿 UV1 当 q，
 * 判这个顶点会不会活下来。**判据的真源只有一处**（`CSHouse_ClipKeeps`），这里只负责把
 * 常驻流里那两个通道解回它要的输入 —— 与 `FCSHouseMeshWriter::SetPanel` 写进去的口径对称。
 */
bool CSHouseTest_VertexKept(const FCSGpuMeshCPUData& S, int32 Index)
{
	const int32 ShapeId = FMath::RoundToInt(S.Colors[Index].Z * 255.0f);
	if (ShapeId == 255) return true;
	FCSOpeningClipField Field;
	Field.bValid = true;
	Field.Shape = ECSOpeningShape(uint8(ShapeId));
	return CSHouse_ClipKeeps(Field, S.TexCoordChannels[1][Index]);
}
}

bool FCSHousePierPlasterTest::RunTest(const FString& Parameters)
{
	const FCSHouseTestBodyCase Case = { ECSRidgeAxis::X, 600, 400, 35.0f, 25.0f, 12.0f, 300.0f, 24.0f, TEXT("piers") };
	const float PierWidth = 40.0f;

	// 边 0 = 南墙：Start = (−HX, −HY)、U = +X、In = +Y、Len = FootprintSize.X（见 CSHouse_GetEdge）。
	// 墙空间 (S, 厚度比 FD, Z) → 局部 (−HX + S, −HY + FD·T, Z)。
	auto Probe = [&Case](float S, double FD, float Z)
	{
		return FVector(-Case.SizeX * 0.5 + S, -Case.SizeY * 0.5 + FD * Case.WallThickness, Z);
	};
	auto LocalToS = [&Case](const FVector3f& P) { return float(P.X + Case.SizeX * 0.5); };

	FCSWallOpening Left;
	Left.Shape = ECSOpeningShape::Arch;
	Left.EdgeIndex = 0;
	Left.CenterS = 200.0f;
	Left.Width = 120.0f;
	Left.Z0 = 0.0f;
	Left.Z1 = 220.0f;
	FCSWallOpening Right = Left;
	Right.CenterS = 360.0f;                       // 跨度 = 300 − 260 = 40，起拱线 160

	float Span = 0.0f, PierTopZ = 0.0f;
	TestTrue(TEXT("The fixture really is a pier span"), CSHouse_PierSpanBetween(Left, Right, Span, PierTopZ));
	TestEqual(TEXT("The fixture span"), Span, PierWidth);
	TestEqual(TEXT("The fixture pier top"), PierTopZ, 160.0f);

	// 探针：跨度**内部**、贴着两侧洞缘，避开 260/280/300 那几个盒边界（竖直射线落在边界上判定是模糊的）。
	const float SpanS[] = { 264.0f, 271.5f, 288.5f, 296.0f };
	const double DepthFrac[] = { 0.19, 0.5, 0.81 };
	const float BelowZ[] = { 4.0f, 47.0f, 112.0f, 155.0f };
	const float AboveZ[] = { 168.0f, 213.0f, 271.0f };

	// 跨度里、起拱线以下的**顶点**（面板盒的下边那圈角点）。两种配置都要看它们，
	// 因为"灰泥没了"与"灰泥还在"的区别整个落在这些顶点的 UV1 上。
	auto CountSpanFloorVerts = [&](const FCSGpuMeshCPUData& S, int32& OutKept)
	{
		int32 Total = 0;
		OutKept = 0;
		for (int32 Index = 0; Index < S.Positions.Num(); ++Index)
		{
			const FVector3f& P = S.Positions[Index];
			if (FMath::Abs(P.Z) > 0.01f) continue;                                    // 只看落地那一圈
			if (P.Y > -Case.SizeY * 0.5 + Case.WallThickness + 0.01) continue;        // 只看南墙那一条带
			const float PS = LocalToS(P);
			if (PS < Left.S1() - 0.5f || PS > Right.S0() + 0.5f) continue;            // 只看跨度
			++Total;
			if (CSHouseTest_VertexKept(S, Index)) ++OutKept;
		}
		return Total;
	};

	// ---- ① 判为墙（不打样式位）：跨度里从地面到墙顶都是实心灰泥 —— 这是**改之前**的观感 ----
	{
		FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
		Body.PierWidth = PierWidth;
		Body.Openings = { Left, Right };
		FCSGpuMeshCPUData S;
		CSHouse_BuildBodySoup(Body, S);

		int32 Missing = 0;
		for (float PS : SpanS) for (double FD : DepthFrac) for (float PZ : BelowZ)
		{
			if (CSHouseTest_SolidDepth(S, Probe(PS, FD, PZ)) < 1) ++Missing;
		}
		// 这条同时是下面那条断言的**对照组**：它红了就说明探针根本没打在墙上，
		// 下面那条"墩跨度被裁掉了"也就成了空判据。
		TestTrue(FString::Printf(TEXT("Without the pier flags the span is solid plaster (%d hollow samples)"), Missing),
			Missing == 0);

		int32 Kept = 0;
		const int32 Total = CountSpanFloorVerts(S, Kept);
		TestTrue(FString::Printf(TEXT("Without the pier flags the span floor is drawn (%d of %d vertices kept)"), Kept, Total),
			Total > 0 && Kept > 0);
	}

	// ---- ② 判为墩 ----
	FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
	Body.PierWidth = PierWidth;
	FCSWallOpening PierLeft = Left, PierRight = Right;
	PierLeft.StyleFlags |= CSHouse_StylePierAfter;
	PierRight.StyleFlags |= CSHouse_StylePierBefore;
	Body.Openings = { PierLeft, PierRight };
	FCSGpuMeshCPUData S;
	CSHouse_BuildBodySoup(Body, S);

	// ①' **几何仍然实心**（裁决三）：墩不是靠"不生成面板"做出来的，跨度里每一点都得被实体包住 ——
	// 起拱线上下都要测，只测上半段等于没测。
	{
		int32 Hollow = 0;
		FVector WorstAt = FVector::ZeroVector;
		for (float PS : SpanS) for (double FD : DepthFrac)
		{
			for (float PZ : BelowZ) if (CSHouseTest_SolidDepth(S, Probe(PS, FD, PZ)) < 1) { ++Hollow; WorstAt = Probe(PS, FD, PZ); }
			for (float PZ : AboveZ) if (CSHouseTest_SolidDepth(S, Probe(PS, FD, PZ)) < 1) { ++Hollow; WorstAt = Probe(PS, FD, PZ); }
		}
		TestTrue(FString::Printf(TEXT("The pier span is still solid geometry (%d hollow samples, e.g. %s)"),
			Hollow, *WorstAt.ToString()), Hollow == 0);
	}

	// ②' **像素上整片裁掉**：跨度落地那一圈顶点一个都不许活下来。
	{
		int32 Kept = 0;
		const int32 Total = CountSpanFloorVerts(S, Kept);
		TestTrue(FString::Printf(TEXT("The pier span floor is clipped away (%d of %d vertices survive)"), Kept, Total),
			Total > 0 && Kept == 0);
	}

	// 裁剪场本身：起拱线以下裁掉、以上保留，且面板的两片端盖落在洞**内**（与普通面板相反）——
	// 端盖被保住的话，跨度两端会各立起一片贯穿墙厚的灰泥薄片，正好卡在墩的位置上。
	{
		const float SpanMin = Left.S1() - CSHouse_PierCutMargin * 0.1f;
		const float SpanMax = Right.S0() + CSHouse_PierCutMargin * 0.1f;
		const FCSOpeningClipField Cut = CSHouse_PierClipField(SpanMin, SpanMax, PierTopZ);
		TestTrue(TEXT("The pier cut is a valid field"), Cut.bValid);
		for (float PS : SpanS)
		{
			for (float PZ : BelowZ) TestFalse(TEXT("Under the springing line the pier cut discards"), CSHouse_ClipKeeps(Cut, Cut.Eval(PS, PZ)));
			for (float PZ : AboveZ) TestTrue(TEXT("Above the springing line the pier cut keeps"), CSHouse_ClipKeeps(Cut, Cut.Eval(PS, PZ)));
		}
		TestFalse(TEXT("The panel's own end caps fall inside the cut"),
			CSHouse_ClipKeeps(Cut, Cut.Eval(SpanMin, 1.0f)) || CSHouse_ClipKeeps(Cut, Cut.Eval(SpanMax, 1.0f)));
	}

	// **只**抹掉墩那一侧：两拱各自的外侧（没打样式位的那半个墩）一块灰泥都不许少，
	// 否则整面墙会跟着塌成一排孤零零的拱。几何与像素两条一起看。
	{
		int32 MissingOuter = 0, ClippedOuter = 0;
		const FCSOpeningClipField LeftField = CSHouse_ComputeClipField(PierLeft);
		const FCSOpeningClipField RightField = CSHouse_ComputeClipField(PierRight);
		// 落在两拱**外侧**那半个墩里（左 [120,140]、右 [420,440]），不是拱肚子底下 ——
		// 拱肚子那块面板盒无论如何都在（洞是逐像素 clip 的），拿它当探针证明不了任何事。
		for (float PS : { 123.7f, 137.2f }) for (float PZ : BelowZ)
		{
			if (CSHouseTest_SolidDepth(S, Probe(PS, 0.5, PZ)) < 1) ++MissingOuter;
			if (!CSHouse_ClipKeeps(LeftField, LeftField.Eval(PS, PZ))) ++ClippedOuter;
		}
		for (float PS : { 423.5f, 437.9f }) for (float PZ : BelowZ)
		{
			if (CSHouseTest_SolidDepth(S, Probe(PS, 0.5, PZ)) < 1) ++MissingOuter;
			if (!CSHouse_ClipKeeps(RightField, RightField.Eval(PS, PZ))) ++ClippedOuter;
		}
		TestTrue(FString::Printf(TEXT("The far side of each arch keeps its plaster (%d hollow, %d clipped)"),
			MissingOuter, ClippedOuter), MissingOuter == 0 && ClippedOuter == 0);
	}

	// 墙顶那一整条（灰泥 + 墙顶砖带）必须**没有断口** —— 墩是把墙"裁空"，不是把墙"切断"。
	{
		int32 MissingTop = 0;
		for (int32 K = 0; K < 59; ++K)
		{
			const float PS = 5.3f + K * 10.0f;   // 故意不取整数比例，避开每一处盒边界
			if (CSHouseTest_SolidDepth(S, Probe(PS, 0.5, 285.0f)) < 1) ++MissingTop;
		}
		TestTrue(FString::Printf(TEXT("The wall head runs unbroken across the whole edge (%d hollow samples)"), MissingTop),
			MissingTop == 0);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 窗（D8）：**谓词说能放的窗，几何一定砌得出**
//
// 这是 C1 选甲（谓词降维成同边一维 S 区间）的全部意义，也是 D8「谓词是唯一真源」那条纪律
// 唯一说得清的执行面。它必须**跨过两侧**：一侧调真正的谓词 `CSHouse_QueryOpening`，另一侧跑
// 真正的产线 `CSHouse_BuildBodySoup` + `CSHouseFrame::BuildEdgeElements`，中间不许有任何
// "测试自己写的镜像"——镜像只会证明两份代码长得像，证明不了谓词与几何同维。
//
// 观察通道用常驻流本来就带着的两条语义（`ACSHouseActor` 的通道字典），不另开后门：
//   · B = 洞形状 id / 255，255 = 这块面板没有洞 ⇒ **带裁剪场的顶点数 / 36 就是洞板数**
//     （一块洞板 = 一次 `AddBox` = 6 面 × 2 三角 × 3 顶点）。谓词放行的洞若被
//     `CellMax - CellMin < O.Width` 那条静默 `continue` 丢掉，这个数就少 36 —— 那正是
//     二维谓词时代"谓词说能放、几何砌不出"的**唯一**症状，画面上与"用户根本没放窗"逐像素相同。
//   · G = 洞的 Tag / 255。窗的 Tag 从 0x80 起编（`ACSHouseActor::BuildWindowOpenings`），
//     与门的子段号不撞，所以能把"这一扇窗的那块洞板"从整栋房子里精确挑出来。
//
// ⚠️ **谓词与几何必须吃同一个 `PierWidth`**：面板格（`CSHouse_OpeningCell`）两边都用它，
// 各填各的会让这条断言在参数不一致时静默变假。产线上两者同出于 `ACSHouseActor::PierWidth`，
// 这里也只写一个常量。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseWindowPredicateMatchesGeometryTest,
	"PCGPlugins.ComputeShaderGenerator.House.WindowPredicateMatchesGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
/**
 * 带裁剪场的**墙**面板顶点数。/36 = 洞板数（一块洞板恰好一次 `AddBox`）。
 *
 * ⚠️ **必须先按 R 通道筛出墙**：两块屋面板是直接写 `Writer.Semantic = CSHouse_Semantic(Roof)`
 * 的（没走 `SetPanel`），那一句不碰 `.Z` ⇒ 屋面顶点的 B 恒为 0，按通道字典读出来正好是
 * "形状 id = Arch"。屋面材质是 Opaque、不消费这两条通道，所以线上无害；但只按 B 数的话，
 * 一栋空房子也会数出两块"洞板"来（实测 72 个顶点），断言当场变成描述屋面。
 */
int32 CSHouseTest_ClipVertCount(const FCSGpuMeshCPUData& S)
{
	int32 Count = 0;
	for (const FVector4f& C : S.Colors)
	{
		if (FMath::RoundToInt(C.X * 255.0f) != int32(ECSHousePart::Wall)) continue;
		if (FMath::RoundToInt(C.Z * 255.0f) == 255) continue;   // 255 = 这块面板没有洞
		++Count;
	}
	return Count;
}

/** 挑出带某个 Tag 的洞板顶点（窗的 Tag ≥ 0x80，与门的子段号不撞）。 */
int32 CSHouseTest_TaggedClipVerts(const FCSGpuMeshCPUData& S, uint8 Tag, TArray<int32>& OutIndices)
{
	OutIndices.Reset();
	for (int32 Index = 0; Index < S.Colors.Num(); ++Index)
	{
		if (FMath::RoundToInt(S.Colors[Index].X * 255.0f) != int32(ECSHousePart::Wall)) continue;
		if (FMath::RoundToInt(S.Colors[Index].Z * 255.0f) == 255) continue;
		if (FMath::RoundToInt(S.Colors[Index].Y * 255.0f) != int32(Tag)) continue;
		OutIndices.Add(Index);
	}
	return OutIndices.Num();
}
}

bool FCSHouseWindowPredicateMatchesGeometryTest::RunTest(const FString& Parameters)
{
	// 出厂参数（CDO），断言才真的钉住线上那一份口径。
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float Pier = CDO->PierWidth;
	const FCSHouseTestBodyCase Case = { ECSRidgeAxis::X, 600, 400, 35.0f, 25.0f, 12.0f, 300.0f, 24.0f, TEXT("windows") };
	const FVector2D Foot(Case.SizeX, Case.SizeY);

	// 两扇门：一扇占住南墙（边 0）的右端 —— 窗挤到它跟前就该被判 `OverlapsOpening`（门拱优先）；
	// 另一扇在北墙（边 2），用来验"不同边的洞永远不冲突"这条在扫描里也成立。
	TArray<FCSWallOpening> Doors;
	{
		FCSWallOpening D0 = CSHouseTest_DemoArch(460.0f);
		D0.Type = ECSOpeningType::Door;
		D0.EdgeIndex = 0;
		D0.Tag = 2;
		Doors.Add(D0);
		FCSWallOpening D2 = CSHouseTest_DemoArch(300.0f);
		D2.Type = ECSOpeningType::Door;
		D2.EdgeIndex = 2;
		D2.Tag = 1;
		Doors.Add(D2);
	}

	FCSOpeningSite Site;
	Site.Footprint = Foot;
	Site.WallThickness = Case.WallThickness;
	Site.WallHeight = Case.WallHeight;
	Site.LintelBand = CDO->LintelBand;
	Site.CornerMargin = CDO->CornerMargin;
	Site.PierWidth = Pier;
	Site.OpeningClearance = CDO->OpeningClearance;
	Site.MinSillZ = CDO->WindowMinSillZ;
	Site.bPierStyleEnabled = CDO->bPierStyleEnabled;
	Site.PierRestoreWidth = FMath::Max(CDO->PierStyleRestoreWidth, CDO->PierStyleMaxWidth);
	Site.Openings = Doors;

	// 只有门时的洞板数：后面每放行一扇窗，这个数必须**恰好 +36**（多一块洞板，一块不多一块不少）。
	FCSHouseBodyDesc BaseBody = CSHouseTest_MakeBody(Case);
	BaseBody.PierWidth = Pier;
	BaseBody.Openings = Doors;
	FCSGpuMeshCPUData BaseSoup;
	CSHouse_BuildBodySoup(BaseBody, BaseSoup);
	const int32 BaseClipVerts = CSHouseTest_ClipVertCount(BaseSoup);
	TestEqual(TEXT("Two doors lay two clip panels (36 verts each)"), BaseClipVerts, 2 * 36);

	// 五组尺寸：前三组会被放行，后两组**必然**分别撞上窗台下限与过梁带 —— 拒绝理由的直方图
	// 因此不是空的（不然"扫描也拒了很多"那条会退化成只测到护角一种）。
	struct FSize { float Width, Z0, Height; };
	const FSize Sizes[5] = {
		{ 60.0f,  90.0f, 110.0f },   // 小窗
		{ 78.0f,  90.0f, 110.0f },   // TG window_cottage_1x1 的实测宽
		{ 120.0f, 95.0f, 140.0f },   // 宽窗
		{ 70.0f,  20.0f,  90.0f },   // 窗台压在地上 ⇒ SillTooLow
		{ 70.0f, 150.0f, 130.0f },   // 洞顶吃掉过梁带 ⇒ AboveEave
	};
	const ECSOpeningShape Shapes[2] = { ECSOpeningShape::Rect, ECSOpeningShape::Arch };
	// 边 0（南墙，长 600、有门）与边 1（东墙，长 400 − 2T = 352、无门）：后者顺带把
	// `CSHouse_GetEdge` 对短边的缩短口径也带进这条断言。
	const int32 Edges[2] = { 0, 1 };

	int32 Accepted = 0, Rejected = 0, DroppedByGeometry = 0, BadPanel = 0, MissingSill = 0, NoBricks = 0;
	int32 RejectHistogram[8] = { 0 };
	FString FirstFailure;

	for (const int32 Edge : Edges)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Foot, Case.WallThickness);
		for (const ECSOpeningShape Shape : Shapes)
		{
			for (const FSize& Size : Sizes)
			{
				for (int32 Step = 0; Step <= 75; ++Step)
				{
					FCSWallOpening Window;
					Window.Type = ECSOpeningType::Window;
					Window.Shape = Shape;
					Window.EdgeIndex = Edge;
					Window.CenterS = float(Step) * 8.0f;
					Window.Width = Size.Width;
					Window.Z0 = Size.Z0;
					Window.Z1 = Size.Z0 + Size.Height;
					Window.SourceId = FGuid(0x57494E44u, 0u, 0u, 0u);
					Window.Tag = 0x80;

					const ECSFeatureReject Reason = CSHouse_QueryOpening(Site, Window);
					if (Reason != ECSFeatureReject::None)
					{
						++Rejected;
						++RejectHistogram[FMath::Clamp(int32(Reason), 0, 7)];
						continue;
					}
					++Accepted;

					// ---- 几何那一侧：跑真正的产线，不写镜像 ----
					TArray<FCSWallOpening> All = Doors;
					All.Add(Window);
					All.Sort([](const FCSWallOpening& A, const FCSWallOpening& B)
					{
						return A.EdgeIndex != B.EdgeIndex ? A.EdgeIndex < B.EdgeIndex : A.CenterS < B.CenterS;
					});

					FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
					Body.PierWidth = Pier;
					Body.Openings = All;
					FCSGpuMeshCPUData Soup;
					CSHouse_BuildBodySoup(Body, Soup);

					// ① 洞板数恰好 +36 —— 那条静默 `continue` 对过了谓词的洞不可达，
					//    而且新来的窗也没有把哪扇门的面板挤掉（挤掉的话这个数会不增反平）。
					if (CSHouseTest_ClipVertCount(Soup) != BaseClipVerts + 36)
					{
						++DroppedByGeometry;
						if (FirstFailure.IsEmpty())
						{
							FirstFailure = FString::Printf(TEXT("edge=%d shape=%d S=%.0f W=%.0f Z=[%.0f,%.0f]"),
								Edge, int32(Shape), Window.CenterS, Window.Width, Window.Z0, Window.Z1);
						}
						continue;
					}

					// ② 这一扇窗那块洞板：恰好 36 个顶点、横向盖住整个洞、竖向从洞底一直到墙顶，
					//    而且**它自己的每个角点都活得下来**（端盖落进洞内的话，两侧的墙会跟着被切）。
					TArray<int32> Verts;
					const int32 TaggedCount = CSHouseTest_TaggedClipVerts(Soup, Window.Tag, Verts);
					float MinS = TNumericLimits<float>::Max(), MaxS = -TNumericLimits<float>::Max();
					float MinZ = TNumericLimits<float>::Max(), MaxZ = -TNumericLimits<float>::Max();
					int32 ClippedCorners = 0;
					for (const int32 Index : Verts)
					{
						const FVector3f& P = Soup.Positions[Index];
						const float S = float((FVector2D(P.X, P.Y) - F.Start) | F.U);
						MinS = FMath::Min(MinS, S);
						MaxS = FMath::Max(MaxS, S);
						MinZ = FMath::Min(MinZ, P.Z);
						MaxZ = FMath::Max(MaxZ, P.Z);
						if (!CSHouseTest_VertexKept(Soup, Index)) ++ClippedCorners;
					}
					const bool bPanelOk = TaggedCount == 36
						&& MinS <= Window.S0() + 0.02f && MaxS >= Window.S1() - 0.02f
						&& FMath::IsNearlyEqual(MinZ, Window.Z0, 0.02f)
						&& FMath::IsNearlyEqual(MaxZ, Case.WallHeight, 0.02f)
						&& ClippedCorners == 0;
					if (!bPanelOk)
					{
						++BadPanel;
						if (FirstFailure.IsEmpty())
						{
							FirstFailure = FString::Printf(
								TEXT("edge=%d shape=%d S=%.0f W=%.0f：verts=%d S=[%.1f,%.1f] 洞=[%.1f,%.1f] Z=[%.1f,%.1f] 被裁角点=%d"),
								Edge, int32(Shape), Window.CenterS, Window.Width, TaggedCount,
								MinS, MaxS, Window.S0(), Window.S1(), MinZ, MaxZ, ClippedCorners);
						}
						continue;
					}

					// ③ 窗台：洞面板底下那一截必须是**实心**（2026-08-30 裁决三：几何永远实心）。
					const FVector2D Flat = F.Start + F.U * Window.CenterS + F.In * (0.5 * Case.WallThickness);
					if (CSHouseTest_SolidDepth(Soup, FVector(Flat.X, Flat.Y, Window.Z0 * 0.5f)) < 1)
					{
						++MissingSill;
						if (FirstFailure.IsEmpty())
						{
							FirstFailure = FString::Printf(TEXT("edge=%d shape=%d S=%.0f 的窗台底下是空的"),
								Edge, int32(Shape), Window.CenterS);
						}
						continue;
					}

					// ④ 砖那一侧：窗必须拿到一条自己的砖路，且**带第四段**（窗台底边）。
					CSHouseFrame::FBrickParams Params;
					Params.Length = CDO->FrameBrickLength;
					Params.Gap = CDO->FrameBrickGap;
					Params.MaxBricks = 4096;
					CSHouseFrame::FWallFrame Frame;
					TArray<CSHouseFrame::FElement> Elements;
					TArray<FCSWallOpening> EdgeOnly;
					for (const FCSWallOpening& O : All) if (O.EdgeIndex == Edge) EdgeOnly.Add(O);
					CSHouseFrame::BuildEdgeElements(Frame, EdgeOnly, Params, Elements);
					bool bWindowPath = false;
					for (const CSHouseFrame::FElement& E : Elements)
					{
						bWindowPath |= FMath::IsNearlyEqual(E.Path.CenterS, Window.CenterS, 0.01f)
							&& E.BrickCount > 0 && E.Path.bSill && E.Path.SillLen() > 0.0f;
					}
					if (!bWindowPath)
					{
						++NoBricks;
						if (FirstFailure.IsEmpty())
						{
							FirstFailure = FString::Printf(TEXT("edge=%d shape=%d S=%.0f 没拿到带窗台段的砖路"),
								Edge, int32(Shape), Window.CenterS);
						}
					}
				}
			}
		}
	}

	AddInfo(FString::Printf(TEXT("sweep: %d accepted / %d rejected (corner=%d eave=%d sill=%d overlap=%d)"),
		Accepted, Rejected,
		RejectHistogram[int32(ECSFeatureReject::NearCorner)],
		RejectHistogram[int32(ECSFeatureReject::AboveEave)],
		RejectHistogram[int32(ECSFeatureReject::SillTooLow)],
		RejectHistogram[int32(ECSFeatureReject::OverlapsOpening)]));

	// 非空判据：扫描必须两边都踩到，而且每一条拒绝理由都真的发生过 —— 否则这条测试测的是空气。
	TestTrue(FString::Printf(TEXT("The sweep accepts a lot of windows (%d)"), Accepted), Accepted > 100);
	TestTrue(FString::Printf(TEXT("The sweep also rejects a lot (%d)"), Rejected), Rejected > 100);
	TestTrue(TEXT("Windows near the wall ends are rejected"), RejectHistogram[int32(ECSFeatureReject::NearCorner)] > 0);
	TestTrue(TEXT("Windows overlapping a door's panel cell are rejected (arches win, D6)"),
		RejectHistogram[int32(ECSFeatureReject::OverlapsOpening)] > 0);
	TestTrue(TEXT("Windows sitting on the floor are rejected"), RejectHistogram[int32(ECSFeatureReject::SillTooLow)] > 0);
	TestTrue(TEXT("Windows eating into the wall head are rejected"), RejectHistogram[int32(ECSFeatureReject::AboveEave)] > 0);

	// **本条测试的全部意义。**
	TestEqual(FString::Printf(TEXT("Every accepted window really gets its own clip panel (%s)"),
		FirstFailure.IsEmpty() ? TEXT("nothing dropped") : *FirstFailure), DroppedByGeometry, 0);
	TestEqual(TEXT("...and that panel spans the whole opening with its end caps outside the clip"), BadPanel, 0);
	TestEqual(TEXT("...and the wall under the sill stays solid (裁决三：几何永远实心)"), MissingSill, 0);
	TestEqual(TEXT("...and the frame lays it a brick path carrying the sill course"), NoBricks, 0);

	// ---- 墩跨度不接受窗（计划 D6）：默认参数下不可达，必须把跨度撑开才测得到 ----
	{
		FCSWallOpening Left = CSHouseTest_DemoArch(160.0f);
		Left.EdgeIndex = 0;
		FCSWallOpening Right = CSHouseTest_DemoArch(440.0f);
		Right.EdgeIndex = 0;
		const FCSWallOpening Pair[2] = { Left, Right };
		const float SpanMin = Left.S1(), SpanMax = Right.S0();
		TestTrue(FString::Printf(TEXT("The fixture leaves a wide span (%.0f cm)"), SpanMax - SpanMin),
			SpanMax - SpanMin > 120.0f);

		FCSOpeningSite Wide = Site;
		Wide.Openings = MakeArrayView(Pair, 2);
		Wide.PierRestoreWidth = 200.0f;     // 反常参数：这么宽也按墩处理，正是它让这条路可达

		FCSWallOpening Window;
		Window.Type = ECSOpeningType::Window;
		Window.Shape = ECSOpeningShape::Rect;
		Window.EdgeIndex = 0;
		Window.CenterS = (SpanMin + SpanMax) * 0.5f;
		Window.Width = 60.0f;
		Window.Z0 = 90.0f;
		Window.Z1 = 200.0f;
		Window.SourceId = FGuid(0x57494E44u, 9u, 0u, 0u);
		TestTrue(TEXT("A window in the middle of a pier span is rejected"),
			CSHouse_QueryOpening(Wide, Window) == ECSFeatureReject::OnPierSpan);

		// 对照组一：墩样式关掉 ⇒ 那截墙是普通灰泥，同一扇窗放得下。没有它，上一条可能只是在
		// 描述"那个位置本来就放不下"。
		FCSOpeningSite NoPier = Wide;
		NoPier.bPierStyleEnabled = false;
		TestTrue(TEXT("With the pier style off the very same window is fine"),
			CSHouse_QueryOpening(NoPier, Window) == ECSFeatureReject::None);

		// 对照组二：阈值调回正常 ⇒ 这条跨度宽到永远判不成墩，窗照样放得下
		// （判据认的是"会不会被判成墩"，不是"是不是一段跨度"）。
		FCSOpeningSite NormalThreshold = Wide;
		NormalThreshold.PierRestoreWidth = 75.0f;
		TestTrue(TEXT("A span too wide to ever become a pier accepts the window"),
			CSHouse_QueryOpening(NormalThreshold, Window) == ECSFeatureReject::None);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 门框砖的**第四段**：窗台底边
//
// 背景（别再把它删掉）：已删的样条旧路对 `Z0 > 0` 的洞会额外出一条下边界曲线（当时的
// `bAnySill`），门框迁到 100% GPU 解析推导时它跟着旧路一起没了 —— 那一轮零回归，因为
// **当时没有任何东西产出非落地的洞**。窗一上线就露馅：洞的下边界同样是一条 clip 边，
// 没有砖骑在上面，窗台正面就是一条裸露的裁剪断口。
//
// 三条判据：
//   ① **门一步不动**（回归护栏）：`Z0 = 0` 的拱不出第四段，弧长仍是 2×樘 + πR。
//   ② **砖骑在 clip 场的零等值线上、朝外的那一面真的朝外**：沿整条路密扫，往面内朝外法线挪
//      ε 必须落在洞外（保留）、往反方向挪 ε 必须落在洞内（被 discard）。
//      ⚠️ 拱洞的窗台段是**例外，而且是故意的**：`FCSOpeningClipField` 的拱判据在起拱线以下
//      无下界（注释写明了理由 —— 洞底那条直线的量化不可见，交给几何承担），所以"往下 ε 保留"
//      对拱窗不成立。那一段只判"往上 ε 被裁掉"；下界由洞面板的底承担，由上一条测试钉住。
//   ③ **底边真的被砖盖满**：窗台段上的砖首尾各自够到两樘，相邻砖心距不超过一个铺装间距。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameWindowSillTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameWindowSill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameWindowSillTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	CSHouseFrame::FBrickParams Params;
	Params.Length = CDO->FrameBrickLength;
	Params.Gap = CDO->FrameBrickGap;
	Params.MaxBricks = 4096;
	CSHouseFrame::FWallFrame Frame;   // S 对 +X、Z 对 +Z：墙空间 = 世界空间

	// ---- ① 门：不出第四段，弧长逐位等于改动之前 ----
	{
		const FCSWallOpening Door = CSHouseTest_DemoArch(300.0f);
		CSHouseFrame::FPath Path;
		TestTrue(TEXT("A door still yields a path"), CSHouseFrame::MakeOpeningPath(Door, Path));
		TestFalse(TEXT("A door has no sill course (its bottom edge is the ground)"), Path.bSill);
		TestEqual(TEXT("A door's sill length is zero"), Path.SillLen(), 0.0f);
		const float Jamb = FMath::Max(Door.Z1 - Door.HalfWidth(), Door.Z0) - Door.Z0;
		TestTrue(TEXT("A door's arc length is unchanged: two jambs plus a half circle"),
			FMath::IsNearlyEqual(Path.TotalLen(), 2.0f * Jamb + PI * Door.HalfWidth(), 0.001f));
	}

	// ---- 三种窗：矩形两档 + 尖顶（拱），窗台都离地 ----
	struct FCase { ECSOpeningShape Shape; float Width, Z0, Z1; const TCHAR* What; };
	const FCase Cases[3] = {
		{ ECSOpeningShape::Rect, 78.0f,  90.0f, 200.0f, TEXT("cottage-sized rect window") },
		{ ECSOpeningShape::Rect, 143.5f, 90.0f, 250.0f, TEXT("2x1 rect window") },
		{ ECSOpeningShape::Arch, 69.4f,  95.0f, 240.0f, TEXT("arched window") },
	};

	for (const FCase& C : Cases)
	{
		FCSWallOpening Window;
		Window.Type = ECSOpeningType::Window;
		Window.Shape = C.Shape;
		Window.EdgeIndex = 0;
		Window.CenterS = 300.0f;
		Window.Width = C.Width;
		Window.Z0 = C.Z0;
		Window.Z1 = C.Z1;

		CSHouseFrame::FPath Path;
		TestTrue(FString::Printf(TEXT("%s yields a path"), C.What), CSHouseFrame::MakeOpeningPath(Window, Path));
		TestTrue(FString::Printf(TEXT("%s carries the sill course"), C.What), Path.bSill);
		TestTrue(FString::Printf(TEXT("%s's sill spans the full opening width (%.2f vs %.2f)"),
			C.What, Path.SillLen(), C.Width), FMath::IsNearlyEqual(Path.SillLen(), C.Width, 0.01f));

		// 闭合：整条路走完必须回到起点（左樘底）。没有第四段时终点停在**右**樘底，差一整个洞宽。
		FVector2f Start, StartT, End, EndT;
		CSHouseFrame::EvalPath(Path, 0.0f, Start, StartT);
		CSHouseFrame::EvalPath(Path, Path.TotalLen(), End, EndT);
		TestTrue(FString::Printf(TEXT("%s's path closes back onto its own start ((%.2f, %.2f) vs (%.2f, %.2f))"),
			C.What, End.X, End.Y, Start.X, Start.Y),
			FMath::IsNearlyEqual(End.X, Start.X, 0.01f) && FMath::IsNearlyEqual(End.Y, Start.Y, 0.01f));

		// ---- ② 砖骑在零等值线上、法线朝外 ----
		const FCSOpeningClipField Field = CSHouse_ComputeClipField(Window);
		constexpr float Eps = 0.75f;      // 远大于 float 噪声，远小于一块砖
		constexpr float Corner = 2.0f;    // 折角两侧各让开这么多弧长，理由见下
		int32 OutsideBad = 0, InsideBad = 0, Samples = 0, SillSamples = 0;
		const float Total = Path.TotalLen();
		// 段与段的接缝处切向转 90°，法线在那一点没有定义 —— 在折角上取样等于问"这个角朝哪边"，
		// 两个答案都对。所以把四个折角（0 / L0 / L0+L1 / L0+L1+L2 / Total）各让开 2 cm。
		const float Junctions[5] = { 0.0f, Path.LeftLen(), Path.LeftLen() + Path.MidLen(),
			Path.LeftLen() + Path.MidLen() + Path.RightLen(), Total };
		for (int32 K = 0; K <= 400; ++K)
		{
			const float Arc = Total * float(K) / 400.0f;
			bool bAtCorner = false;
			for (const float J : Junctions) bAtCorner |= FMath::Abs(Arc - J) < Corner;
			if (bAtCorner) continue;
			FVector2f SZ, Tangent;
			CSHouseFrame::EvalPath(Path, Arc, SZ, Tangent);
			// 面内朝外法线 = 切向逆时针转 90°（`CSHouseFrame.usf` 里那一行的 CPU 对照）。
			const FVector2f Outward(-Tangent.Y, Tangent.X);
			const bool bOnSill = FMath::IsNearlyEqual(SZ.Y, Path.BaseZ, 0.01f) && Tangent.X < -0.5f;
			++Samples;
			if (bOnSill) ++SillSamples;

			// 往洞里挪：必须被裁掉 —— 否则砖是骑在实墙上，洞缘那条断口根本没被盖住。
			if (CSHouse_ClipKeeps(Field, Field.Eval(SZ.X - Outward.X * Eps, SZ.Y - Outward.Y * Eps))) ++InsideBad;
			// 往洞外挪：必须保留。⚠️ 拱洞的窗台段是**故意的例外**（拱判据在起拱线以下无下界）。
			const bool bArchBelowSpring = Window.Shape == ECSOpeningShape::Arch && bOnSill;
			if (!bArchBelowSpring
				&& !CSHouse_ClipKeeps(Field, Field.Eval(SZ.X + Outward.X * Eps, SZ.Y + Outward.Y * Eps))) ++OutsideBad;
		}
		TestTrue(FString::Printf(TEXT("%s: the sweep really walks the sill course (%d of %d samples)"),
			C.What, SillSamples, Samples), SillSamples > 20);
		TestEqual(FString::Printf(TEXT("%s: every path point has the opening on its inward side"), C.What), InsideBad, 0);
		TestEqual(FString::Printf(TEXT("%s: and solid wall on its outward side"), C.What), OutsideBad, 0);

		// ---- ③ 底边被砖盖满 ----
		TArray<CSHouseFrame::FElement> Elements;
		const int32 Bricks = CSHouseFrame::BuildEdgeElements(Frame, MakeArrayView(&Window, 1), Params, Elements);
		TestTrue(FString::Printf(TEXT("%s lays bricks (%d in %d paths)"), C.What, Bricks, Elements.Num()),
			Bricks > 8 && Elements.Num() == 1);
		if (Elements.Num() != 1) continue;

		const CSHouseFrame::FElement& E = Elements[0];
		TArray<float> SillS;
		for (int32 K = 0; K < E.BrickCount; ++K)
		{
			FVector2f SZ, Tangent;
			CSHouseFrame::EvalPath(E.Path, E.HalfLen + K * E.Pitch, SZ, Tangent);
			if (FMath::IsNearlyEqual(SZ.Y, Path.BaseZ, 0.01f) && Tangent.X < -0.5f) SillS.Add(SZ.X);
		}
		SillS.Sort();
		TestTrue(FString::Printf(TEXT("%s: the sill course carries bricks (%d)"), C.What, SillS.Num()), SillS.Num() >= 2);
		if (SillS.Num() >= 2)
		{
			float WorstGap = 0.0f;
			for (int32 K = 1; K < SillS.Num(); ++K) WorstGap = FMath::Max(WorstGap, SillS[K] - SillS[K - 1]);
			TestTrue(FString::Printf(TEXT("%s: the sill bricks run without a gap (worst %.2f vs pitch %.2f)"),
				C.What, WorstGap, E.Pitch), WorstGap <= E.Pitch + 0.01f);
			TestTrue(FString::Printf(TEXT("%s: the sill reaches the right jamb (%.2f vs %.2f)"),
				C.What, SillS.Last(), Path.RightS), Path.RightS - SillS.Last() <= E.Pitch + 0.01f);
			TestTrue(FString::Printf(TEXT("%s: the sill reaches the left jamb (%.2f vs %.2f)"),
				C.What, SillS[0], Path.LeftS), SillS[0] - Path.LeftS <= E.Pitch + 0.01f);
		}

		// ---- 对照组：把第四段单独摘掉，看差异是不是**恰好**它 ----
		//
		// ⚠️ 别拿"把窗台压到地面（`Z0 = 0`）"当对照：那样两条门樘会从 110 长到 200，砖数
		// **不减反增**（实测 14 → 18）—— 差异里混进了樘长，测不出第四段。摘的必须只有第四段。
		CSHouseFrame::FPath NoSill = Path;
		NoSill.bSill = false;
		TestTrue(FString::Printf(TEXT("%s: dropping the fourth course shortens the path by exactly one opening width (%.2f vs %.2f)"),
			C.What, Path.TotalLen() - NoSill.TotalLen(), C.Width),
			FMath::IsNearlyEqual(Path.TotalLen() - NoSill.TotalLen(), C.Width, 0.01f));
		FVector2f NoSillEnd, NoSillT;
		CSHouseFrame::EvalPath(NoSill, NoSill.TotalLen(), NoSillEnd, NoSillT);
		TestTrue(FString::Printf(TEXT("%s: without it the path stops dead at the right jamb foot (%.2f vs %.2f)"),
			C.What, NoSillEnd.X, Path.RightS), FMath::IsNearlyEqual(NoSillEnd.X, Path.RightS, 0.01f));

		// 落地的同一个洞（门那一档）不出第四段 —— 与上面 ① 的门是同一条不变量，换个形状再验一次。
		FCSWallOpening Grounded = Window;
		Grounded.Z0 = 0.0f;
		CSHouseFrame::FPath GroundedPath;
		TestTrue(FString::Printf(TEXT("%s grounded still yields a path"), C.What),
			CSHouseFrame::MakeOpeningPath(Grounded, GroundedPath));
		TestFalse(FString::Printf(TEXT("%s: a ground-level opening has no sill course"), C.What), GroundedPath.bSill);
	}

	return true;
}

// -----------------------------------------------------------------------------
// D7 接缝（裁决二）—— 纯函数性是**唯一**的硬证据
//
// "零共享状态、零归属、零撤销"这三句在代码里没有可以直接指的东西（没有 actor、没有表、
// 没有生命周期）。能证明它们的只有一条：**同一对房子，交换两者的顺序、或只重建其中一栋，
// 算出来的砖列逐位相同**。逐位不是洁癖 —— 两栋房各画一份重叠的砖，差一个 ulp 就可能闪。
// -----------------------------------------------------------------------------

namespace
{
CSHouseSeam::FHouse CSHouseTest_MakeSeamHouse(uint32 Tag, double X, double Y, float Yaw, double SizeX, double SizeY)
{
	CSHouseSeam::FHouse H;
	// 固定 GUID：规范序的键必须是确定的，随机 GUID 会让"交换顺序"这条断言时灵时不灵。
	H.Id = FGuid(Tag, 0x1111u, 0x2222u, 0x3333u);
	H.Center = FVector2D(X, Y);
	H.Yaw = Yaw;
	H.Footprint = FVector2D(SizeX, SizeY);
	H.BaseZ = 0.0f;
	H.WallHeight = 300.0f;
	H.WallThickness = 24.0f;
	return H;
}

/** 砖路元素的**逐位**相等（浮点用 `==`，不是 IsNearlyEqual —— 这条断言的全部意义就在"逐位"）。 */
bool CSHouseTest_ElementBitEqual(const CSHouseFrame::FElement& A, const CSHouseFrame::FElement& B)
{
	auto SameVec = [](const FVector3f& U, const FVector3f& V) { return U.X == V.X && U.Y == V.Y && U.Z == V.Z; };
	return A.Path.BaseZ == B.Path.BaseZ && A.Path.TopZ == B.Path.TopZ
		&& A.Path.LeftS == B.Path.LeftS && A.Path.RightS == B.Path.RightS
		&& A.Path.CenterS == B.Path.CenterS && A.Path.Radius == B.Path.Radius
		&& A.Path.MidSweep == B.Path.MidSweep && A.Path.FlatLen == B.Path.FlatLen
		&& A.Path.MidKind == B.Path.MidKind && A.Path.bLeftJamb == B.Path.bLeftJamb
		&& A.Path.bRightJamb == B.Path.bRightJamb && A.Path.bSill == B.Path.bSill
		&& SameVec(A.Frame.Origin, B.Frame.Origin) && SameVec(A.Frame.AxisU, B.Frame.AxisU)
		&& SameVec(A.Frame.AxisV, B.Frame.AxisV)
		&& A.BrickBegin == B.BrickBegin && A.BrickCount == B.BrickCount
		&& A.Pitch == B.Pitch && A.HalfLen == B.HalfLen && A.LayoutScale == B.LayoutScale
		&& A.RandomBase == B.RandomBase;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseSeamPureTest,
	"PCGPlugins.ComputeShaderGenerator.House.SeamIsAPureFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseSeamPureTest::RunTest(const FString& Parameters)
{
	CSHouseFrame::FBrickParams Params;
	Params.Length = 26.0f;
	Params.Gap = 0.0f;
	Params.MaxBricks = 512;

	// 三组：轴对齐十字搭、带 yaw 的斜搭、角对角只咬一个角。
	struct FCase { const TCHAR* What; CSHouseSeam::FHouse A; CSHouseSeam::FHouse B; };
	const FCase Cases[] = {
		{ TEXT("axis-aligned cross"),
			CSHouseTest_MakeSeamHouse(1, 0.0, 0.0, 0.0f, 600.0, 400.0),
			CSHouseTest_MakeSeamHouse(2, 200.0, 150.0, 0.0f, 500.0, 500.0) },
		{ TEXT("yawed overlap"),
			CSHouseTest_MakeSeamHouse(3, 0.0, 0.0, 0.0f, 600.0, 400.0),
			CSHouseTest_MakeSeamHouse(4, 250.0, 120.0, 37.0f, 520.0, 380.0) },
		{ TEXT("corner bite"),
			CSHouseTest_MakeSeamHouse(5, 0.0, 0.0, 0.0f, 600.0, 400.0),
			CSHouseTest_MakeSeamHouse(6, 340.0, 240.0, 0.0f, 300.0, 300.0) },
	};

	for (const FCase& C : Cases)
	{
		TestTrue(FString::Printf(TEXT("%s: the two houses really do overlap (otherwise everything below is vacuous)"), C.What),
			CSHouseSeam::Intersects(C.A, C.B));
		TestTrue(FString::Printf(TEXT("%s: overlap is symmetric"), C.What),
			CSHouseSeam::Intersects(C.A, C.B) == CSHouseSeam::Intersects(C.B, C.A));

		// ---- ① 交换顺序 ⇒ 砖列逐位相同 ----
		TArray<CSHouseSeam::FCorner> CornersAB, CornersBA;
		const int32 NAB = CSHouseSeam::BuildCorners(C.A, C.B, CornersAB);
		const int32 NBA = CSHouseSeam::BuildCorners(C.B, C.A, CornersBA);
		// 交点数必是**偶数**且 ≥ 2：两个凸多边形的边界沿轮廓交替进出，进几次就出几次。
		// 轴对齐一般是 2、带 yaw 时 4/6/8 都可能，所以钉的是奇偶性而不是具体的数。
		TestTrue(FString::Printf(TEXT("%s: contour crossings come in pairs (got %d)"), C.What, NAB),
			NAB >= 2 && (NAB % 2) == 0);
		TestEqual(FString::Printf(TEXT("%s: swapping the pair keeps the corner count"), C.What), NBA, NAB);

		TArray<CSHouseFrame::FElement> FromA, FromB;
		const int32 BricksA = CSHouseSeam::BuildCornerElements(CornersAB, CSHouseSeam::SeamSeed(C.A, C.B), Params, FromA);
		const int32 BricksB = CSHouseSeam::BuildCornerElements(CornersBA, CSHouseSeam::SeamSeed(C.B, C.A), Params, FromB);
		TestTrue(FString::Printf(TEXT("%s: the seam actually lays bricks (%d)"), C.What, BricksA), BricksA > 0);
		TestEqual(FString::Printf(TEXT("%s: swapping the pair keeps the brick count"), C.What), BricksB, BricksA);
		TestEqual(FString::Printf(TEXT("%s: swapping the pair keeps the path count"), C.What), FromB.Num(), FromA.Num());

		int32 Differing = 0;
		for (int32 K = 0; K < FMath::Min(FromA.Num(), FromB.Num()); ++K)
		{
			if (!CSHouseTest_ElementBitEqual(FromA[K], FromB[K])) ++Differing;
		}
		// **这就是"零共享状态、零归属"的硬证据**：两栋房各自从自己的视角算，输出逐位重合。
		TestEqual(FString::Printf(TEXT("%s: the two houses compute the SAME seam bit for bit"), C.What), Differing, 0);

		// ---- ② 只重建其中一栋 ⇒ 它算出来的还是同一条缝（无记忆、无增量）----
		TArray<CSHouseSeam::FCorner> Again;
		TArray<CSHouseFrame::FElement> AgainElements;
		CSHouseSeam::BuildCorners(C.A, C.B, Again);
		CSHouseSeam::BuildCornerElements(Again, CSHouseSeam::SeamSeed(C.A, C.B), Params, AgainElements);
		int32 DriftedOnRebuild = 0;
		for (int32 K = 0; K < FMath::Min(FromA.Num(), AgainElements.Num()); ++K)
		{
			if (!CSHouseTest_ElementBitEqual(FromA[K], AgainElements[K])) ++DriftedOnRebuild;
		}
		TestEqual(FString::Printf(TEXT("%s: rebuilding only one house reproduces the seam bit for bit"), C.What),
			DriftedOnRebuild, 0);

		// ---- ③ 逐实例随机数从接缝身份来，不从槽位来 ----
		//
		// 槽位是"这栋房自己的第几块砖"，两栋房必然不同。把同一条缝接在一堆已有的门框砖后面
		// （模拟"这栋房还有门"），砖序号整体挪位 —— 随机数基必须**一个都不变**。
		TArray<CSHouseFrame::FElement> Shifted;
		CSHouseFrame::FElement Filler;   // 假装前面已经有 37 块门框砖
		Filler.BrickBegin = 0;
		Filler.BrickCount = 37;
		Shifted.Add(Filler);
		CSHouseSeam::BuildCornerElements(CornersAB, CSHouseSeam::SeamSeed(C.A, C.B), Params, Shifted);
		int32 SeedChanged = 0, SlotUnshifted = 0;
		for (int32 K = 0; K + 1 < Shifted.Num(); ++K)
		{
			if (Shifted[K + 1].RandomBase != FromA[K].RandomBase) ++SeedChanged;
			if (Shifted[K + 1].BrickBegin != FromA[K].BrickBegin + 37) ++SlotUnshifted;
		}
		TestEqual(FString::Printf(TEXT("%s: the slots really did shift (otherwise the next check is vacuous)"), C.What),
			SlotUnshifted, 0);
		TestEqual(FString::Printf(TEXT("%s: but the per-instance random seed is derived from the seam, not the slot"), C.What),
			SeedChanged, 0);
	}

	// ---- ④ 没碰上就什么都不出（触发条件是 footprint **真重叠**，不是"靠得近"）----
	const CSHouseSeam::FHouse Far = CSHouseTest_MakeSeamHouse(7, 0.0, 0.0, 0.0f, 600.0, 400.0);
	const CSHouseSeam::FHouse Apart = CSHouseTest_MakeSeamHouse(8, 620.0, 0.0, 0.0f, 600.0, 400.0);
	TArray<CSHouseSeam::FCorner> None;
	TestFalse(TEXT("two houses that merely stand close do not seam"), CSHouseSeam::Intersects(Far, Apart));
	TestEqual(TEXT("no overlap, no corners"), CSHouseSeam::BuildCorners(Far, Apart, None), 0);
	FCSWallCut NoCut;
	int32 CutsWhenApart = 0;
	for (int32 Edge = 0; Edge < 4; ++Edge) if (CSHouseSeam::CutOnEdge(Far, Apart, Edge, NoCut)) ++CutsWhenApart;
	TestEqual(TEXT("no overlap, no wall cut"), CutsWhenApart, 0);

	// 抬高到够不着：footprint 完全重合，但 Z 区间不相交 ⇒ 仍然不是接缝。
	CSHouseSeam::FHouse Above = CSHouseTest_MakeSeamHouse(9, 0.0, 0.0, 0.0f, 600.0, 400.0);
	Above.BaseZ = 400.0f;
	TestFalse(TEXT("a house floating above another one does not seam"), CSHouseSeam::Intersects(Far, Above));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseSeamGeometryTest,
	"PCGPlugins.ComputeShaderGenerator.House.SeamCornersAndCuts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseSeamGeometryTest::RunTest(const FString& Parameters)
{
	const CSHouseSeam::FHouse A = CSHouseTest_MakeSeamHouse(11, 0.0, 0.0, 0.0f, 600.0, 400.0);
	const CSHouseSeam::FHouse B = CSHouseTest_MakeSeamHouse(12, 200.0, 150.0, 0.0f, 500.0, 500.0);

	// ---- ① 交点确实同时落在两条轮廓上 ----
	TArray<CSHouseSeam::FCorner> Corners;
	const int32 N = CSHouseSeam::BuildCorners(A, B, Corners);
	TestTrue(TEXT("the pair crosses"), N > 0);
	int32 OffContour = 0, BadBisect = 0;
	for (const CSHouseSeam::FCorner& C : Corners)
	{
		const FVector2D LA = CSHouseSeam::ToLocal(A, C.Point);
		const FVector2D LB = CSHouseSeam::ToLocal(B, C.Point);
		const double AX = FMath::Abs(LA.X) - A.Footprint.X * 0.5, AY = FMath::Abs(LA.Y) - A.Footprint.Y * 0.5;
		const double BX = FMath::Abs(LB.X) - B.Footprint.X * 0.5, BY = FMath::Abs(LB.Y) - B.Footprint.Y * 0.5;
		// 落在轮廓上 = 至少一个方向恰好贴边，且两个方向都不在框外。
		if (AX > 0.01 || AY > 0.01 || BX > 0.01 || BY > 0.01) ++OffContour;
		if (FMath::Abs(AX) > 0.01 && FMath::Abs(AY) > 0.01) ++OffContour;
		if (FMath::Abs(BX) > 0.01 && FMath::Abs(BY) > 0.01) ++OffContour;
		if (!FMath::IsNearlyEqual(float(C.Outward.Size()), 1.0f, 1.0e-3f)) ++BadBisect;
	}
	TestEqual(TEXT("every seam corner lies on BOTH contours"), OffContour, 0);
	TestEqual(TEXT("every bisector is a unit vector (a zero one would mirror the whole column)"), BadBisect, 0);

	// 角平分朝"两栋房外面"那个象限：沿它走一小步必须同时离开两个 footprint。
	int32 PointingInwards = 0;
	for (const CSHouseSeam::FCorner& C : Corners)
	{
		const FVector2D Probe = C.Point + C.Outward * 20.0;
		const FVector2D PA = CSHouseSeam::ToLocal(A, Probe);
		const FVector2D PB = CSHouseSeam::ToLocal(B, Probe);
		const bool bOutA = FMath::Abs(PA.X) > A.Footprint.X * 0.5 || FMath::Abs(PA.Y) > A.Footprint.Y * 0.5;
		const bool bOutB = FMath::Abs(PB.X) > B.Footprint.X * 0.5 || FMath::Abs(PB.Y) > B.Footprint.Y * 0.5;
		if (!bOutA || !bOutB) ++PointingInwards;
	}
	TestEqual(TEXT("the brick depth axis faces the quadrant that is outside both houses"), PointingInwards, 0);

	// ---- ② 裁剪段：落在段内的墙点确实在邻居 footprint 里，段外的确实在外面 ----
	int32 Cuts = 0, MisclassifiedIn = 0, MisclassifiedOut = 0;
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		FCSWallCut Cut;
		if (!CSHouseSeam::CutOnEdge(A, B, Edge, Cut)) continue;
		++Cuts;
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, A.Footprint, A.WallThickness);
		auto InsideB = [&](float S)
		{
			const FVector2D L = CSHouseSeam::ToLocal(B, CSHouseSeam::ToWorld(A, F.Start + F.U * S));
			return FMath::Abs(L.X) <= B.Footprint.X * 0.5 + 0.01 && FMath::Abs(L.Y) <= B.Footprint.Y * 0.5 + 0.01;
		};
		for (int32 K = 1; K < 20; ++K)
		{
			if (!InsideB(FMath::Lerp(Cut.MinS, Cut.MaxS, float(K) / 20.0f))) ++MisclassifiedIn;
		}
		// 段外两侧各探一步（探到墙外就不算）。
		if (Cut.MinS > 2.0f && InsideB(Cut.MinS - 2.0f)) ++MisclassifiedOut;
		if (Cut.MaxS < F.Len - 2.0f && InsideB(Cut.MaxS + 2.0f)) ++MisclassifiedOut;
	}
	TestTrue(FString::Printf(TEXT("the overlap cuts at least one wall (%d)"), Cuts), Cuts > 0);
	TestEqual(TEXT("every point inside the cut really is inside the neighbour"), MisclassifiedIn, 0);
	TestEqual(TEXT("and the cut stops where the neighbour stops"), MisclassifiedOut, 0);

	// ---- ③ 洞是 **clip** 出来的，几何仍然实心（裁决三，全局不变量）----
	//
	// 拿房体三角汤直接验：接缝那一段的三角形数**不许减少**（不生成面板就是一个真几何洞），
	// 而裁剪判据必须在那一段上说"丢掉"。两条一起才说得清"洞在渲染层、不在几何里"。
	FCSHouseBodyDesc Desc;
	Desc.Roof = CSHouseTest_MakeRoof(ECSRidgeAxis::X, A.Footprint.X, A.Footprint.Y);
	Desc.Footprint = A.Footprint;
	Desc.WallThickness = A.WallThickness;
	Desc.WallHeight = A.WallHeight;
	Desc.PierWidth = 40.0f;

	FCSGpuMeshCPUData Solid;
	CSHouse_BuildBodySoup(Desc, Solid);

	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		FCSWallCut Cut;
		if (CSHouseSeam::CutOnEdge(A, B, Edge, Cut)) Desc.SeamCuts.Add(Cut);
	}
	TestTrue(TEXT("the seam produced cuts to feed the body"), Desc.SeamCuts.Num() > 0);

	FCSGpuMeshCPUData Cutaway;
	CSHouse_BuildBodySoup(Desc, Cutaway);
	// 面板被切成三块（实心 | 裁掉 | 实心）⇒ 三角形只会变多，绝不会变少。
	TestTrue(FString::Printf(TEXT("the seam never removes geometry: %d tris with cuts vs %d without"),
		Cutaway.Indices.Num() / 3, Solid.Indices.Num() / 3),
		Cutaway.Indices.Num() >= Solid.Indices.Num());

	// 裁剪场本身：段中点被 discard、段外被保留。判据用 `CSHouse_ClipKeeps`（材质那份的 CPU 孪生）。
	int32 KeptInsideCut = 0, DroppedOutsideCut = 0;
	for (const FCSWallCut& Cut : Desc.SeamCuts)
	{
		const FCSOpeningClipField Field = CSHouse_SeamClipField(Cut.MinS, Cut.MaxS, Cut.BottomZ, Cut.TopZ);
		const float MidS = (Cut.MinS + Cut.MaxS) * 0.5f;
		const float MidZ = FMath::Max((FMath::Max(Cut.BottomZ, 0.0f) + Cut.TopZ) * 0.5f, 1.0f);
		if (CSHouse_ClipKeeps(Field, Field.Eval(MidS, MidZ))) ++KeptInsideCut;
		// 段外一步（横向）必须留着 —— 否则接缝会把整面墙吃掉。
		if (!CSHouse_ClipKeeps(Field, Field.Eval(Cut.MinS - 20.0f, MidZ))) ++DroppedOutsideCut;
		if (!CSHouse_ClipKeeps(Field, Field.Eval(Cut.MaxS + 20.0f, MidZ))) ++DroppedOutsideCut;
	}
	TestEqual(TEXT("the wall inside the neighbour is clipped away"), KeptInsideCut, 0);
	TestEqual(TEXT("the wall outside the neighbour survives"), DroppedOutsideCut, 0);

	return true;
}

// -----------------------------------------------------------------------------
// 屋面必须**符合**那本冻结的顶点色通道字典（P2 冻结，仲裁点在 `ACSHouseActor` 类注释）
//
// 两块坡板过去是 `Writer.Semantic = CSHouse_Semantic(Roof)` 直接砌的、**没走 `SetPanel`**，
// 而那一句既不碰 `.Z`、也不换裁剪场，于是屋面的两条通道同时在撒谎：
//   · B 恒为 0 —— 字典说 B 是洞形状 id，而 0 是**合法**的 id（`Arch`）⇒ "这块屋面上有个拱洞"；
//   · UV1 原样留着**上一块墙面板**的 clip 场 q。
// 屋面材质是 Opaque 常数色、不消费这两条通道，所以线上无害、也没有任何东西会报错；但裁决六
// 要求通道**随网格烘进 StaticMesh**，将来任何消费 B/UV1 的东西（铺瓦、雪线、屋顶天窗）
// 都会把整片屋面读成"有洞"。修法是让屋面去符合字典，**不动字典**。
//
// ⚠️ 断言必须**两条一起立**，只立一条都会退化：
//   · 只判 B：把那一句换成"只改语义色但把 B 写对"照样绿，UV1 还是残值；
//   · 只判 UV1：在"最后一块墙面板本来就没有裁剪场"的房子上残值与哨兵逐位相同 ⇒ 恒真。
// ⚠️ 因此夹具**必须自带对照组**：得有一栋房子的最后一块墙面板真的带着裁剪场。默认参数下
//    第 3 面墙末尾总会补一块无 clip 的实心段，残值恰好就是哨兵 —— 那种夹具证明不了 UV1
//    这一条。下面 `Stale` 那一组把洞的面板格顶到墙末端（`AddPanel` 的零宽守卫吃掉尾段），
//    最后一次 `SetPanel` 因此带着一个真的 Arch 场；`bStaleFixtureIsReal` 那条就是在证这件事。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofChannelDictionaryTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofChannelDictionary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofChannelDictionaryTest::RunTest(const FString& Parameters)
{
	// 无洞面板的哨兵：`FCSOpeningClipField::Eval` 在 bValid = false 时返回的那一对。
	// 从**真源**取而不是抄一个 8.0f 进来，改哨兵时这条断言才会跟着走。
	const FVector2f Sentinel = FCSOpeningClipField().Eval(0.0f, 0.0f);

	const FCSHouseTestBodyCase Cases[] = {
		{ ECSRidgeAxis::X,  600, 400, 35.0f, 25.0f, 12.0f, 300.0f, 24.0f, TEXT("demo size") },
		{ ECSRidgeAxis::Y,  600, 400, 35.0f, 25.0f, 12.0f, 300.0f, 24.0f, TEXT("ridge along Y") },
		{ ECSRidgeAxis::X,  400, 600, 20.0f,  0.0f, 12.0f, 300.0f, 24.0f, TEXT("zero overhang") },
	};

	// 第 3 面墙（西墙）长 = Footprint.Y − 2T = 352。面板格 = 半宽 + 半个墩 ⇒ 300 + 50 + 10 = 360
	// 被夹到 352，游标因此正好停在墙末端，尾段那次 `AddPanel` 被 `SB − SA < 0.5f` 吃掉 ——
	// 于是**最后一次 `SetPanel` 带的是这个洞的 Arch 场**，屋面若不换场就会原样继承它。
	FCSWallOpening Stale;
	Stale.Shape = ECSOpeningShape::Arch;
	Stale.EdgeIndex = 3;
	Stale.CenterS = 300.0f;
	Stale.Width = 100.0f;
	Stale.Z0 = 0.0f;
	Stale.Z1 = 200.0f;
	Stale.Tag = 7;

	bool bStaleFixtureIsReal = false;

	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		for (const FCSHouseTestBodyCase& Case : Cases)
		{
			FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
			Body.PierWidth = 20.0f;
			if (Variant == 1) Body.Openings = { Stale };

			FCSGpuMeshCPUData S;
			CSHouse_BuildBodySoup(Body, S);

			const FString What = FString::Printf(TEXT("%s%s"), Case.What,
				Variant == 1 ? TEXT(" + opening running to the end of the last wall") : TEXT(""));

			// ---- 对照组：探针真的打在屋面上（两块坡板 = 两次 AddBox = 2 × 36 顶点）----
			int32 FirstRoof = INDEX_NONE, RoofVerts = 0, BadShapeId = 0, BadUV1 = 0, Discarded = 0;
			FVector2f WorstUV1 = Sentinel;
			for (int32 Index = 0; Index < S.Colors.Num(); ++Index)
			{
				if (FMath::RoundToInt(S.Colors[Index].X * 255.0f) != int32(ECSHousePart::Roof)) continue;
				if (FirstRoof == INDEX_NONE) FirstRoof = Index;
				++RoofVerts;
				// ① B = 255：字典说"这块面板没有洞"。0 会被读成 Arch。
				if (FMath::RoundToInt(S.Colors[Index].Z * 255.0f) != 255) ++BadShapeId;
				// ② UV1 = 无洞哨兵，一位不差 —— 残值是上一块面板的 q，与哨兵没有任何关系。
				const FVector2f Q = S.TexCoordChannels[1][Index];
				if (Q != Sentinel) { ++BadUV1; WorstUV1 = Q; }
				// ③ 顺着材质那一侧再判一次：屋面一个像素都不许被 clip 掉。
				//    ⚠️ **这一条不是门**（故意破坏实验实测：把屋面改回旧写法时它照绿）——
				//    残值 q =(2.02, 2.94) 落在 Arch 判据的洞外，恰好活得下来。留着只因为它是唯一
				//    "按消费者口径"说话的一条；真正报红的是 ①②，别把它当成 ①② 的替代品。
				if (!CSHouseTest_VertexKept(S, Index)) ++Discarded;
			}

			TestEqual(FString::Printf(TEXT("[%s] the two roof slabs are 72 vertices"), *What), RoofVerts, 72);
			TestEqual(FString::Printf(TEXT("[%s] every roof vertex says 'this panel has no opening' (B = 255)"), *What),
				BadShapeId, 0);
			TestEqual(FString::Printf(TEXT("[%s] no roof vertex carries the previous wall panel's clip field (worst UV1 = %s, sentinel = %s)"),
				*What, *WorstUV1.ToString(), *Sentinel.ToString()), BadUV1, 0);
			TestEqual(FString::Printf(TEXT("[%s] the material criterion discards no roof vertex"), *What), Discarded, 0);

			// ---- 夹具自证：`Stale` 那一组里，紧挨屋面之前写的确实是一块**带裁剪场**的墙面板 ----
			// 不证这一条的话，UV1 那条断言可能只是在"残值本来就等于哨兵"的房子上恒真。
			if (Variant == 1 && FirstRoof > 0)
			{
				const int32 Prev = FirstRoof - 1;
				if (FMath::RoundToInt(S.Colors[Prev].X * 255.0f) == int32(ECSHousePart::Wall)
					&& FMath::RoundToInt(S.Colors[Prev].Z * 255.0f) != 255
					&& S.TexCoordChannels[1][Prev] != Sentinel)
				{
					bStaleFixtureIsReal = true;
				}
			}
		}
	}

	TestTrue(TEXT("the fixture really does leave a live clip field behind (otherwise the UV1 assertion is vacuous)"),
		bStaleFixtureIsReal);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
