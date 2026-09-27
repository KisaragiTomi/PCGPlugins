#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CSStairsSupport.h"
#include "CSHouseFrame.h"
#include "Algo/Reverse.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsSupportArchTest, "PCGPlugins.ComputeShaderGenerator.Stairs.ArchedSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsSupportArchTest::RunTest(const FString& Parameters)
{
	CSStairs::FRun Run;
	for (int32 I = 0; I <= 28; ++I) Run.Samples.Add({ FVector(I * 50.0, 0.0, 15.0 + I * 30.0), 180.0f, FVector2D(1, 0) });
	CSStairs::FParams Params;
	const CSStairs::FGroundSampler Ground = [](const FVector2D&) { return 0.0f; };
	TArray<FCSWallOpening> Openings;
	CSStairs::PlanSupportOpenings(Run, Params, Ground, Openings);
	TestTrue(TEXT("a long elevated flight fits several arches"), Openings.Num() >= 2);
	TArray<CSStairs::FBrick> Bricks;
	CSStairs::BuildRunSupports(Run, Params, Ground, 5, Bricks);
	TestTrue(TEXT("support wall and rings use bricks"), Bricks.Num() > 100);
	for (const auto& B : Bricks) TestTrue(TEXT("unrailed supports retain the full stair width"), FMath::IsNearlyEqual(B.Size.Y, 180.0, 0.001));
	for (const FCSWallOpening& O : Openings)
	{
		// Probe the same aperture from both sides and through its centre.
		for (double Y : { -65.0, 0.0, 65.0 })
		{
			const FVector P(O.CenterS, Y, O.Z1 * 0.5);
			bool bBlocked = false;
			for (const auto& B : Bricks)
			{
				const FVector L = B.Rotation.UnrotateVector(P - B.Center).GetAbs();
				bBlocked |= L.X < B.Size.X * 0.5 && L.Y < B.Size.Y * 0.5 && L.Z < B.Size.Z * 0.5;
			}
			TestFalse(TEXT("an arch is an actual through opening, with no solid tread columns inside"), bBlocked);
		}
	}
	CSStairs::FParams Railed = Params;
	Railed.bNarrowByRailing = true;
	TArray<CSStairs::FBrick> Narrowed;
	CSStairs::BuildRunSupports(Run, Railed, Ground, 5, Narrowed);
	for (const auto& B : Narrowed) TestTrue(TEXT("railed supports match the narrowed treads"), FMath::IsNearlyEqual(B.Size.Y, 155.0, 0.001));
	CSStairs::FRun Reversed = Run;
	Algo::Reverse(Reversed.Samples);
	for (auto& S : Reversed.Samples) S.Dir = -S.Dir;
	TArray<FCSWallOpening> ReverseOpenings;
	CSStairs::PlanSupportOpenings(Reversed, Params, Ground, ReverseOpenings);
	TestEqual(TEXT("drawing direction does not change the arch count"), ReverseOpenings.Num(), Openings.Num());
	for (int32 I = 0; I < Openings.Num() && I < ReverseOpenings.Num(); ++I)
	{
		const auto& A = Openings[I];
		const auto& B = ReverseOpenings[ReverseOpenings.Num() - 1 - I];
		TestTrue(TEXT("reverse drawing preserves arch height and span"), FMath::IsNearlyEqual(A.Z1, B.Z1, 0.01f) && FMath::IsNearlyEqual(A.Width, B.Width, 0.01f));
	}
	Params.bArchedSupport = false;
	CSStairs::PlanSupportOpenings(Run, Params, Ground, Openings);
	TestTrue(TEXT("solid mode has no apertures"), Openings.IsEmpty());
	Params.bArchedSupport = true;
	Run.Type = CSStairs::ESegmentType::Ladder;
	Bricks.Reset();
	CSStairs::BuildRunSupports(Run, Params, Ground, 5, Bricks);
	TestTrue(TEXT("wooden ladders do not grow stone walls"), Bricks.IsEmpty());
	Run.Type = CSStairs::ESegmentType::Steps;
	for (auto& S : Run.Samples) S.Position.Z = 15.0;
	CSStairs::PlanSupportOpenings(Run, Params, Ground, Openings);
	TestTrue(TEXT("ground level paving has no room for an arch"), Openings.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSSharedArchProfileTest, "PCGPlugins.ComputeShaderGenerator.House.SharedArchProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSSharedArchProfileTest::RunTest(const FString& Parameters)
{
	for (float Rise : { 40.0f, 80.0f, 140.0f })
	{
		FCSWallOpening O;
		O.Width = 280.0f; O.CenterS = 200.0f; O.Z0 = 0.0f; O.Z1 = 320.0f; O.ArchRise = Rise;
		CSHouseFrame::FPath Path;
		TestTrue(TEXT("shared opening produces the frame path"), CSHouseFrame::MakeOpeningPath(O, Path));
		const FCSOpeningClipField Field = CSHouse_ComputeClipField(O);
		for (int32 I = 0; I <= 60; ++I)
		{
			FVector2f SZ, T;
			CSHouseFrame::EvalPath(Path, Path.LeftLen() + Path.MidLen() * I / 60.0f, SZ, T);
			const FVector2f Q = Field.Eval(SZ.X, SZ.Y);
			TestTrue(TEXT("frame stones and the wall clip use the same ellipse"), FMath::IsNearlyEqual(Q.SizeSquared(), 1.0f, 0.001f));
			TestTrue(TEXT("frame tangent is unit length"), FMath::IsNearlyEqual(T.Size(), 1.0f, 0.001f));
		}
		FCSWallOpening Other = O;
		Other.CenterS += O.Width + 25.0f;
		float Span = 0, Top = 0;
		TestTrue(TEXT("the two arches share a pier"), CSHouse_PierSpanBetween(O, Other, Span, Top));
		TestTrue(TEXT("pier top uses the same spring line"), FMath::IsNearlyEqual(Top, O.SpringZ(), 0.001f));
	}
	return true;
}
#endif
