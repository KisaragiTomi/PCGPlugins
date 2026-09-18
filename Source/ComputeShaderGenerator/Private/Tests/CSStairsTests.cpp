#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSStairs.h"
#include "CSStairsActor.h"
#include "CSGpuInstancedMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Math/RandomStream.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 玩家绘制楼梯（TG §4.1，2026-09-16）。常量与出处见附录 D。
//
// 纯函数三条钉 TG 的算法：等弧长重采样、坡度分档、横向切砖；再一条钉出砖的几何不变量
// （踏面单调、块高下限、宽度切满、高端加厚、同种子逐位相同）。actor 一条钉接线：默认带样条、
// 放进关卡就出砖、砖表没变不重传、改样条会重建。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo builds share a TU, so file-local names carry a CSStairsTest_ prefix.

/** 一条直的 run：从 A 到 B 均匀 `Count` 段，宽度恒定、方向取 AB 的水平方向。 */
CSStairs::FRun CSStairsTest_StraightRun(const FVector& A, const FVector& B, int32 Count, float Width, CSStairs::ESegmentType Type)
{
	CSStairs::FRun Run;
	Run.Type = Type;
	const FVector2D Dir = FVector2D(B - A).GetSafeNormal();
	for (int32 K = 0; K <= Count; ++K)
	{
		CSStairs::FSample Sample;
		Sample.Position = FMath::Lerp(A, B, double(K) / double(Count));
		Sample.Width = Width;
		Sample.Dir = Dir.IsNearlyZero() ? FVector2D(1.0, 0.0) : Dir;
		Run.Samples.Add(Sample);
	}
	return Run;
}

/** 一级的踏面高度 = 这一级所有砖的顶。 */
TMap<int32, double> CSStairsTest_TreadTops(const TArray<CSStairs::FBrick>& Bricks)
{
	TMap<int32, double> Tops;
	for (const CSStairs::FBrick& Brick : Bricks)
	{
		const double Top = Brick.Center.Z + Brick.Size.Z * 0.5;
		double& Slot = Tops.FindOrAdd(Brick.StepIndex, Top);
		Slot = FMath::Max(Slot, Top);
	}
	return Tops;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSStairsResampleTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.Resample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsResampleTest::RunTest(const FString& Parameters)
{
	// 直线 500 cm（3D）：round(500 / 50) = 10 段、11 个点、点距恰好 50。
	{
		const TArray<FVector> Line = { FVector(0.0, 0.0, 0.0), FVector(300.0, 0.0, 400.0) };
		TArray<FVector> Points;
		TArray<TPair<int32, float>> Source;
		CSStairs::ResamplePolyline(Line, 50.0f, Points, Source);
		TestEqual(TEXT("500 cm at 50 cm gives 11 points"), Points.Num(), 11);
		TestEqual(TEXT("one source entry per point"), Source.Num(), Points.Num());
		if (Points.Num() == 11)
		{
			TestTrue(TEXT("the first point is kept verbatim"), Points[0] == Line[0]);
			TestTrue(TEXT("the last point is kept verbatim"), Points.Last() == Line.Last());
			bool bEven = true;
			for (int32 Index = 1; Index < Points.Num(); ++Index) bEven &= FMath::IsNearlyEqual(FVector::Distance(Points[Index - 1], Points[Index]), 50.0, 0.01);
			TestTrue(TEXT("and every step is 50 cm of 3D arc"), bEven);
		}
	}

	// 余数不单独吸收：120 cm → round(2.4) = 2 段，每段 60。
	{
		const TArray<FVector> Line = { FVector::ZeroVector, FVector(120.0, 0.0, 0.0) };
		TArray<FVector> Points;
		TArray<TPair<int32, float>> Source;
		CSStairs::ResamplePolyline(Line, 50.0f, Points, Source);
		TestEqual(TEXT("120 cm rounds to 2 steps"), Points.Num(), 3);
		if (Points.Num() == 3) TestTrue(TEXT("each 60 cm"), FMath::IsNearlyEqual(Points[1].X, 60.0, 0.01));
	}

	// 折线：点落在正确的源段上，段内 t 在 [0, 1]。
	{
		const TArray<FVector> Poly = { FVector::ZeroVector, FVector(100.0, 0.0, 0.0), FVector(100.0, 250.0, 0.0) };
		TArray<FVector> Points;
		TArray<TPair<int32, float>> Source;
		CSStairs::ResamplePolyline(Poly, 50.0f, Points, Source);
		bool bSane = Points.Num() == 8;
		for (int32 Index = 0; Index < Source.Num(); ++Index)
		{
			bSane &= Source[Index].Key >= 0 && Source[Index].Key <= 1 && Source[Index].Value >= 0.0f && Source[Index].Value <= 1.0f;
			// 源段 + t 必须真的插回这个点。
			const FVector Back = FMath::Lerp(Poly[Source[Index].Key], Poly[Source[Index].Key + 1], double(Source[Index].Value));
			bSane &= Back.Equals(Points[Index], 0.01);
		}
		TestTrue(FString::Printf(TEXT("a bent polyline resamples onto its own segments (%d points)"), Points.Num()), bSane);
	}

	TArray<FVector> Empty;
	TArray<TPair<int32, float>> EmptySource;
	CSStairs::ResamplePolyline(TArray<FVector>{ FVector::ZeroVector }, 50.0f, Empty, EmptySource);
	TestEqual(TEXT("a single point resamples to nothing"), Empty.Num(), 0);

	// 坡度分档（TG `determine_segment_type`）：边界归缓的一侧。
	const CSStairs::FParams Params;
	TestEqual(TEXT("slope 0.2 is a walkway"), CSStairs::ClassifySegment(20.0f, 100.0f, Params), CSStairs::ESegmentType::Walkway);
	TestEqual(TEXT("slope 0.25 is still a walkway"), CSStairs::ClassifySegment(25.0f, 100.0f, Params), CSStairs::ESegmentType::Walkway);
	TestEqual(TEXT("slope 0.5 is steps"), CSStairs::ClassifySegment(-50.0f, 100.0f, Params), CSStairs::ESegmentType::Steps);
	TestEqual(TEXT("slope 2.5 is still steps"), CSStairs::ClassifySegment(250.0f, 100.0f, Params), CSStairs::ESegmentType::Steps);
	TestEqual(TEXT("slope 3 is a ladder"), CSStairs::ClassifySegment(300.0f, 100.0f, Params), CSStairs::ESegmentType::Ladder);
	TestEqual(TEXT("a vertical segment is a ladder"), CSStairs::ClassifySegment(100.0f, 0.0f, Params), CSStairs::ESegmentType::Ladder);

	// 切分（TG `random_splits` 语义，附录 D §6.4）：近似等分 + 分界抖动，抖动夹在 0.495 / 片数 ⇒ 永不交叉。
	{
		FRandomStream Rand(7);
		TArray<float> Bounds;
		for (const int32 Count : { 1, 2, 4, 10 })
		{
			// 故意给一个远超上限的抖动：夹子必须兜住，否则分界会交叉。
			CSStairs::RandomSplits(Count, 10.0f, Rand, Bounds);
			bool bOk = Bounds.Num() == Count + 1 && Bounds[0] == 0.0f && Bounds.Last() == 1.0f;
			const float Floor = (1.0f - 0.495f) / float(Count);   // 相邻两条各往里挪到头时剩下的宽度
			for (int32 Index = 1; Index < Bounds.Num(); ++Index) bOk &= Bounds[Index] - Bounds[Index - 1] >= Floor - 1.0e-4f;
			TestTrue(FString::Printf(TEXT("%d pieces stay ordered under any jitter"), Count), bOk);
		}
		CSStairs::RandomSplits(4, 0.0f, Rand, Bounds);
		TestTrue(TEXT("zero jitter is an exact equal split"), FMath::IsNearlyEqual(Bounds[1], 0.25f) && FMath::IsNearlyEqual(Bounds[3], 0.75f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSStairsDoorStepsTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.DoorSteps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsDoorStepsTest::RunTest(const FString& Parameters)
{
	// 门在原点朝 +X，门洞底 5 cm。地面：门下平的（Z = 0），往外 20 cm 起线性下沉，到 80 cm 处 −60 之后持平。
	const CSStairs::FGroundSampler Slope = [](const FVector2D& XY)
	{
		return -60.0f * FMath::Clamp(float(XY.X - 20.0) / 60.0f, 0.0f, 1.0f);
	};
	CSStairs::FDoorStepsInput Door;
	Door.DoorXY = FVector2D::ZeroVector;
	Door.Outward = FVector2D(1.0, 0.0);
	Door.DoorBottomZ = 5.0f;
	Door.Width = 114.8f;
	const CSStairs::FDoorStepsParams Params;

	TArray<CSStairs::FBrick> Bricks;
	const bool bBuilt = CSStairs::BuildDoorSteps(Door, Params, Slope, 42u, Bricks);
	TestTrue(TEXT("a grounded door with a 60 cm drop outside gets steps"), bBuilt);
	// H = 60 ⇒ N = clamp(ceil(3), 2, 5) = 3 排、L = max(3, 2) − 1 = 2 层、横向 max(ceil(2.55), 3) − 1 = 2 块；
	// 第 0 层 1 排、第 1 层 2 排 ⇒ (1 + 2) × 2 = 6 块。
	TestEqual(TEXT("two layers of a three-row pyramid, two bricks a row"), Bricks.Num(), 6);
	if (Bricks.Num() == 6)
	{
		const double RowDepth = 56.0 / 3.0;
		double TopOfAll = -1.0e9, BottomOfAll = 1.0e9;
		bool bDepth = true, bRows = true;
		TMap<int32, double> RowWidth;
		for (const CSStairs::FBrick& Brick : Bricks)
		{
			TopOfAll = FMath::Max(TopOfAll, Brick.Center.Z + Brick.Size.Z * 0.5);
			BottomOfAll = FMath::Min(BottomOfAll, Brick.Center.Z - Brick.Size.Z * 0.5);
			bDepth &= FMath::IsNearlyEqual(Brick.Size.X, RowDepth + 10.0, 0.01);
			bRows &= FMath::IsNearlyEqual(Brick.Center.X, 28.0 + Brick.StepIndex * RowDepth, 0.01);
			bool bLayer0 = Brick.Center.Z > -30.0;   // 第 0 层中心在 −15 附近
			RowWidth.FindOrAdd(Brick.StepIndex * 10 + (bLayer0 ? 0 : 1)) += Brick.Size.Y;
		}
		TestTrue(FString::Printf(TEXT("the top layer starts at the ground under the door (%.2f)"), TopOfAll), FMath::IsNearlyEqual(TopOfAll, 0.0, 0.01));
		TestTrue(FString::Printf(TEXT("the bottom layer ends at the lowest ground in front (%.2f)"), BottomOfAll), FMath::IsNearlyEqual(BottomOfAll, -60.0, 0.01));
		TestTrue(TEXT("every brick is one row deep plus the 10 cm overlap"), bDepth);
		TestTrue(TEXT("rows step out from 28 cm by 56 / N"), bRows);
		bool bCovered = true;
		for (const TPair<int32, double>& Pair : RowWidth) bCovered &= FMath::IsNearlyEqual(Pair.Value, double(Door.Width), 0.01);
		TestTrue(TEXT("each row spans exactly the door width"), bCovered);
	}

	TArray<CSStairs::FBrick> None;
	// 门悬空（门洞底比门下地面高 30 cm > 20）：TG 不出踏步，交给栏杆。
	{
		CSStairs::FDoorStepsInput Floating = Door;
		Floating.DoorBottomZ = 30.0f;
		TestFalse(TEXT("a floating door gets no steps"), CSStairs::BuildDoorSteps(Floating, Params, Slope, 42u, None));
	}
	// 门外平地：没有下坡。
	TestFalse(TEXT("flat ground outside gets no steps"),
		CSStairs::BuildDoorSteps(Door, Params, [](const FVector2D&) { return 0.0f; }, 42u, None));
	// 门外落差 ≥ 150：太高。
	TestFalse(TEXT("a 200 cm drop is too high for door steps"),
		CSStairs::BuildDoorSteps(Door, Params, [](const FVector2D& XY) { return XY.X > 50.0 ? -200.0f : 0.0f; }, 42u, None));
	TestEqual(TEXT("and none of the rejections wrote a brick"), None.Num(), 0);

	// 栏杆：没出踏步、墙脚高出地面 15 cm 以上才要。
	TestTrue(TEXT("a wall foot 20 cm above the ground without steps gets rails"), CSStairs::ShouldAddDoorRails(20.0f, 0.0f, false));
	TestFalse(TEXT("steps and rails are exclusive"), CSStairs::ShouldAddDoorRails(20.0f, 0.0f, true));
	TestFalse(TEXT("10 cm above the ground is not a balcony"), CSStairs::ShouldAddDoorRails(10.0f, 0.0f, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSStairsStepBricksTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.StepBricks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsStepBricksTest::RunTest(const FString& Parameters)
{
	const CSStairs::FParams Params;
	const float Width = 150.0f;

	// 一段坡度 0.5 的直楼梯：水平 400、升 200，3D 长 447.2 ⇒ round(8.94) = 9 级。
	const FVector A(0.0, 0.0, 0.0), B(400.0, 0.0, 200.0);
	const CSStairs::FRun Run = CSStairsTest_StraightRun(A, B, 8, Width, CSStairs::ESegmentType::Steps);

	TArray<CSStairs::FBrick> Bricks;
	const int32 Steps = CSStairs::BuildRunBricks(Run, Params, CSStairs::FGroundSampler(), 11u, Bricks);
	TestEqual(TEXT("447 cm of stairs at 50 cm a step is 9 steps"), Steps, 9);
	if (Steps != 9) return false;

	const double StepRun = 400.0 / 9.0;
	const double StepRise = 200.0 / 9.0;
	const TMap<int32, double> Tops = CSStairsTest_TreadTops(Bricks);
	bool bMonotonic = true, bTallEnough = true, bDepth = true, bWidthCovered = true, bInside = true;
	for (int32 K = 0; K < Steps; ++K)
	{
		// 踏面 = 这一级两端里高的那个 = 第 K+1 个重采样点的高度。
		const double* Top = Tops.Find(K);
		bMonotonic &= Top && FMath::IsNearlyEqual(*Top, StepRise * (K + 1), 0.05);

		double Covered = 0.0;
		for (const CSStairs::FBrick& Brick : Bricks)
		{
			if (Brick.StepIndex != K) continue;
			bTallEnough &= Brick.Size.Z >= Params.MinBlockHeight - 0.01;
			bDepth &= FMath::IsNearlyEqual(Brick.Size.X, StepRun * Params.DepthOverlap, 0.05);
			Covered += Brick.Size.Y;
			bInside &= FMath::Abs(Brick.Center.Y) + Brick.Size.Y * 0.5 <= Width * 0.5 + 0.01;
		}
		bWidthCovered &= FMath::IsNearlyEqual(Covered, double(Width), 0.05);
	}
	TestTrue(TEXT("each tread sits one rise above the last"), bMonotonic);
	TestTrue(TEXT("every block is at least 60 cm tall (steps overlap, no underside shows)"), bTallEnough);
	TestTrue(TEXT("every block's depth is the step run times 1.13"), bDepth);
	TestTrue(TEXT("the pieces of a step exactly cover the stair width"), bWidthCovered);
	TestTrue(TEXT("and stay inside it"), bInside);

	// 高端加厚：顶上那一端水平 56 cm 以内的级（44.4 cm 一级 ⇒ 最顶上 1 级）多一个踏高。
	{
		double TopHeight = 0.0, NextHeight = 0.0;
		for (const CSStairs::FBrick& Brick : Bricks)
		{
			if (Brick.StepIndex == Steps - 1) TopHeight = Brick.Size.Z;
			if (Brick.StepIndex == Steps - 2) NextHeight = Brick.Size.Z;
		}
		TestTrue(FString::Printf(TEXT("the topmost step is thickened by one rise (%.2f vs %.2f)"), TopHeight, NextHeight),
			FMath::IsNearlyEqual(TopHeight, NextHeight + StepRise, 0.05));
	}

	// 同种子逐位相同；150 宽会切砖，换种子切法就可能不同 —— 只要求同种子稳定。
	{
		TArray<CSStairs::FBrick> Again;
		CSStairs::BuildRunBricks(Run, Params, CSStairs::FGroundSampler(), 11u, Again);
		bool bSame = Again.Num() == Bricks.Num();
		for (int32 Index = 0; bSame && Index < Bricks.Num(); ++Index)
		{
			bSame &= Again[Index].Center == Bricks[Index].Center && Again[Index].Size == Bricks[Index].Size;
		}
		TestTrue(TEXT("the same seed builds the same bricks"), bSame);
	}

	// 窄楼梯（< 66 cm）一级只有一块砖。
	{
		const CSStairs::FRun Narrow = CSStairsTest_StraightRun(A, B, 8, 60.0f, CSStairs::ESegmentType::Steps);
		TArray<CSStairs::FBrick> NarrowBricks;
		const int32 NarrowSteps = CSStairs::BuildRunBricks(Narrow, Params, CSStairs::FGroundSampler(), 3u, NarrowBricks);
		TestEqual(TEXT("a 60 cm stair is one brick per step"), NarrowBricks.Num(), NarrowSteps);
	}

	// 贴地：平地 Z = 100 高过楼梯下半段 ⇒ 踏面至少地面 + 15。
	{
		const CSStairs::FGroundSampler Flat = [](const FVector2D&) { return 100.0f; };
		TArray<CSStairs::FBrick> Grounded;
		CSStairs::BuildRunBricks(Run, Params, Flat, 11u, Grounded);
		const TMap<int32, double> GroundTops = CSStairsTest_TreadTops(Grounded);
		bool bAbove = true;
		for (const TPair<int32, double>& Pair : GroundTops) bAbove &= Pair.Value >= 115.0 - 0.01;
		TestTrue(TEXT("no tread is below ground + 15 cm"), bAbove);
	}

	// MVP 支撑：地面远在下面时，踏步块底下砌成一层层的砖直到地面以下 10 cm。每层都不是被拉长的木桩
	// （< 2 层高，理由见 `FParams::bSolidToGround`），中间那些层的层缝落在层高的整数倍上。
	{
		const CSStairs::FGroundSampler Deep = [](const FVector2D&) { return -300.0f; };
		TArray<CSStairs::FBrick> Supported;
		CSStairs::BuildRunBricks(Run, Params, Deep, 11u, Supported);
		const TMap<int32, double> SupportedTops = CSStairsTest_TreadTops(Supported);
		double Lowest = 1.0e9;
		bool bCoursesSane = true, bSeamsAligned = true;
		int32 CourseBricks = 0;
		for (const CSStairs::FBrick& Brick : Supported)
		{
			const double Bottom = Brick.Center.Z - Brick.Size.Z * 0.5;
			const double Top = Brick.Center.Z + Brick.Size.Z * 0.5;
			Lowest = FMath::Min(Lowest, Bottom);
			if (FMath::IsNearlyEqual(Top, SupportedTops.FindRef(Brick.StepIndex), 0.01)) continue;   // 踏步块本身
			++CourseBricks;
			bCoursesSane &= Brick.Size.Z < Params.SupportCourseHeight * 2.0 + 0.01;
			// 层顶要么贴着踏步块底（第一层），要么落在整数倍上。
			const double OnGrid = FMath::Fmod(FMath::Abs(Top), double(Params.SupportCourseHeight));
			const bool bGrid = OnGrid < 0.01 || Params.SupportCourseHeight - OnGrid < 0.01;
			bool bUnderTread = false;
			for (const CSStairs::FBrick& Tread : Supported)
			{
				if (Tread.StepIndex != Brick.StepIndex || !FMath::IsNearlyEqual(Tread.Center.Z + Tread.Size.Z * 0.5, SupportedTops.FindRef(Brick.StepIndex), 0.01)) continue;
				bUnderTread |= FMath::IsNearlyEqual(Top, Tread.Center.Z - Tread.Size.Z * 0.5, 0.01);
			}
			bSeamsAligned &= bGrid || bUnderTread;
		}
		TestTrue(FString::Printf(TEXT("the support reaches 10 cm below the ground (%.2f)"), Lowest), FMath::IsNearlyEqual(Lowest, -310.0, 0.01));
		TestTrue(FString::Printf(TEXT("it is laid in courses (%d bricks)"), CourseBricks), CourseBricks > Steps);
		TestTrue(TEXT("no course brick is a stretched post (< 2 course heights)"), bCoursesSane);
		TestTrue(TEXT("course seams line up on the world grid"), bSeamsAligned);
	}

	// 平走道：整段抬到地面 + 15。
	{
		const CSStairs::FRun Walk = CSStairsTest_StraightRun(FVector::ZeroVector, FVector(300.0, 0.0, 0.0), 6, Width, CSStairs::ESegmentType::Walkway);
		const CSStairs::FGroundSampler Zero = [](const FVector2D&) { return 0.0f; };
		TArray<CSStairs::FBrick> Paving;
		const int32 Slabs = CSStairs::BuildRunBricks(Walk, Params, Zero, 5u, Paving);
		TestEqual(TEXT("300 cm of walkway is 6 slabs"), Slabs, 6);
		const TMap<int32, double> WalkTops = CSStairsTest_TreadTops(Paving);
		bool bLifted = WalkTops.Num() == Slabs;
		for (const TPair<int32, double>& Pair : WalkTops) bLifted &= FMath::IsNearlyEqual(Pair.Value, 15.0, 0.01);
		TestTrue(TEXT("a walkway is paved 15 cm above the ground"), bLifted);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSStairsActorTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsActorTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSStairsActor* Stairs = World->SpawnActor<ACSStairsActor>(FVector(0.0, 0.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Stairs actor"), Stairs)) return false;

	// 用户要的那一条：默认就带样条。
	USplineComponent* Spline = Stairs->GetSpline();
	if (!TestNotNull(TEXT("the stairs actor comes with a spline component by default"), Spline)) return false;
	TestEqual(TEXT("a three-point default path"), Spline->GetNumberOfSplinePoints(), 3);
	if (!TestNotNull(TEXT("the TG brick mesh is the default brick"), Stairs->BrickMesh.Get())) return false;

	Stairs->RebuildStairs();
	TestTrue(FString::Printf(TEXT("dropped into a level it builds steps (%d)"), Stairs->GetStepCount()), Stairs->GetStepCount() > 0);
	TestTrue(TEXT("at least one brick per step"), Stairs->GetBrickCount() >= Stairs->GetStepCount());
	TestEqual(TEXT("every brick is an instance"), Stairs->GetBrickComponent()->GetInstanceCount(), Stairs->GetBrickCount());

	// 默认样条：两段各自的首尾弦坡度都在踏步档 ⇒ 并成一个 run，端点落在首尾样条点上。
	{
		TArray<CSStairs::FRun> Runs;
		Stairs->BuildRuns(Runs);
		TestEqual(TEXT("the default path is one run of steps"), Runs.Num(), 1);
		if (Runs.Num() == 1)
		{
			TestEqual(TEXT("classified as steps"), Runs[0].Type, CSStairs::ESegmentType::Steps);
			TestTrue(TEXT("the run starts at the first spline point"),
				Runs[0].Samples[0].Position.Equals(Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World), 0.5));
			TestTrue(TEXT("and ends at the last one"),
				Runs[0].Samples.Last().Position.Equals(Spline->GetLocationAtSplinePoint(2, ESplineCoordinateSpace::World), 0.5));
		}
	}

	// 幂等：砖表没变就不重传（`SetInstances` 带阻塞刷新）。
	const int32 Uploads = Stairs->GetUploadCount();
	Stairs->RebuildStairs();
	TestEqual(TEXT("rebuilding an unchanged stair uploads nothing"), Stairs->GetUploadCount(), Uploads);

	// 改样条（再往上接一段）⇒ 级数变多、真的重传。
	const int32 StepsBefore = Stairs->GetStepCount();
	Spline->AddSplinePoint(FVector(750.0, 150.0, 375.0), ESplineCoordinateSpace::Local, true);
	Stairs->RebuildStairs();
	TestTrue(FString::Printf(TEXT("extending the spline adds steps (%d -> %d)"), StepsBefore, Stairs->GetStepCount()),
		Stairs->GetStepCount() > StepsBefore);
	TestEqual(TEXT("and re-uploads once"), Stairs->GetUploadCount(), Uploads + 1);

	// 节点宽 = Width × 点缩放 Y：把首点缩到 0.5，首级明显比末级窄。
	Spline->SetScaleAtSplinePoint(0, FVector(1.0, 0.5, 1.0), true);
	Stairs->RebuildStairs();
	{
		double FirstWidth = 0.0, LastWidth = 0.0;
		int32 LastStep = 0;
		for (const CSStairs::FBrick& Brick : Stairs->GetBricks()) LastStep = FMath::Max(LastStep, Brick.StepIndex);
		for (const CSStairs::FBrick& Brick : Stairs->GetBricks())
		{
			if (Brick.StepIndex == 0) FirstWidth += Brick.Size.Y;
			if (Brick.StepIndex == LastStep) LastWidth += Brick.Size.Y;
		}
		TestTrue(FString::Printf(TEXT("scaling a spline point narrows that end (%.1f vs %.1f)"), FirstWidth, LastWidth), FirstWidth < LastWidth - 20.0);
	}

	World->DestroyActor(Stairs);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
