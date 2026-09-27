#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CSHouseActor.h"
#include "CSStairsActor.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsTerraceGeometryTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.TerraceGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsTerraceGeometryTest::RunTest(const FString& Parameters)
{
	FCSStairsTerraceSite Site;
	Site.Roof.bFlat = true;
	Site.Roof.Pitch = 0;
	const FVector End(0, -220, 285), Neighbor(0, -620, 50);
	const auto Connected = CSStairs_ConnectTerrace(Site, End, Neighbor, 150, 80, 100);
	TestTrue(TEXT("An outside stair reaches a nearby terrace"), Connected.bConnected);
	TestTrue(TEXT("Landing is exactly level with the deck"), FMath::IsNearlyEqual(Connected.Position.Z, 300.5));
	TestTrue(TEXT("Landing overlaps inside the parapet"), Connected.Position.Y > -170);
	TestTrue(TEXT("Opening leaves room for the full stair width"), Connected.Opening.End - Connected.Opening.Start >= 170);
	TestFalse(TEXT("Low stairs do not jump up to the roof"), CSStairs_ConnectTerrace(Site, FVector(0,-220,30), Neighbor,150,80,100).bConnected);
	TestFalse(TEXT("Distant stairs remain unattached"), CSStairs_ConnectTerrace(Site, FVector(0,-350,285), Neighbor,150,80,100).bConnected);
	TestFalse(TEXT("A path parallel to the facade is not an entrance"), CSStairs_ConnectTerrace(Site, End,FVector(200,-220,200),150,80,100).bConnected);
	TestFalse(TEXT("An inside path does not cut an outside entrance"), CSStairs_ConnectTerrace(Site, End,FVector(0,0,200),150,80,100).bConnected);
	TestFalse(TEXT("A stair wider than the available wall cannot attach"), CSStairs_ConnectTerrace(Site, End,Neighbor,600,80,100).bConnected);
	Site.Roof.bFlat = false;
	TestFalse(TEXT("Pitched roofs never accept terrace stairs"), CSStairs_ConnectTerrace(Site,End,Neighbor,150,80,100).bConnected);
	Site.Roof.bFlat = true;
	Site.World = FTransform(FRotator(0, 47, 0), FVector(1000, -700, 90));
	const auto Rotated = CSStairs_ConnectTerrace(Site, Site.World.TransformPosition(End), Site.World.TransformPosition(Neighbor),150,80,100);
	TestTrue(TEXT("Yaw and translation preserve the same landing"), Rotated.bConnected && Rotated.Position.Equals(Site.World.TransformPosition(Connected.Position), 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsTerraceOpeningTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.TerraceOpenings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsTerraceOpeningTest::RunTest(const FString& Parameters)
{
	FCSRoofDesc Roof;
	Roof.bFlat = true;
	Roof.Pitch = 0;
	TArray<FCSRoofParapetOpening> Cuts = { {0, 200, 330}, {0, 280, 410} };
	TArray<FCSRoofParapetBlock> Blocks;
	CSHouseRoof_BuildParapet(Roof, 30, 55, Blocks, Cuts);
	auto IsCovered = [&](const FVector& P)
	{
		for (const FCSRoofParapetBlock& B : Blocks)
		{
			const FVector Q = B.Rotation.UnrotateVector(P - B.Center).GetAbs();
			if (Q.X < B.Size.X * 0.5 && Q.Y < B.Size.Y * 0.5 && Q.Z < B.Size.Z * 0.5) return true;
		}
		return false;
	};
	for (double X : { -95.0, 0.0, 105.0 }) TestFalse(TEXT("Overlapping entrance requests cut a clear continuous passage"), IsCovered(FVector(X,-185,310)));
	TestTrue(TEXT("Masonry outside the passage remains"), IsCovered(FVector(-150,-185,310)));
	TestTrue(TEXT("The opposite parapet remains closed"), IsCovered(FVector(0,185,310)));
	CSHouseRoof_BuildParapet(Roof, 30, 55, Blocks);
	TestTrue(TEXT("Removing requests restores the masonry"), IsCovered(FVector(0,-185,310)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsTerraceLifecycleTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.TerraceLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsTerraceLifecycleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>();
	House->RoofPitch = 0;
	House->WallHeight = 300;
	House->FootprintSize = FVector2D(600,400);
	House->ReevaluateSite();
	ACSStairsActor* Stairs = World->SpawnActor<ACSStairsActor>();
	Stairs->bFollowGround = false;
	USplineComponent* Spline = Stairs->GetSpline();
	Spline->SetSplinePoints({ FVector(0,-700,15), FVector(0,-400,150), FVector(0,-220,290) }, ESplineCoordinateSpace::World);
	for (int32 I = 0; I < 3; ++I) Spline->SetSplinePointType(I, ESplinePointType::Linear, false);
	Spline->UpdateSpline();
	Stairs->RebuildStairs();
	const FVector AuthoredEnd = Spline->GetLocationAtSplinePoint(2, ESplineCoordinateSpace::World);
	TestEqual(TEXT("The upper endpoint connects"), Stairs->GetTerraceConnectionCount(), 1);
	TestEqual(TEXT("The house opens one entrance"), House->GetTerraceEntranceCount(), 1);
	TArray<CSStairs::FRun> Runs;
	Stairs->BuildRuns(Runs);
	if (TestTrue(TEXT("The connected path has a run"), !Runs.IsEmpty())) TestTrue(TEXT("Resolved endpoint is flush with the deck"), FMath::IsNearlyEqual(Runs.Last().Samples.Last().Position.Z,300.5));
	TestTrue(TEXT("Connection leaves the authored spline untouched"), Spline->GetLocationAtSplinePoint(2, ESplineCoordinateSpace::World).Equals(AuthoredEnd));
	double HighestTread = -TNumericLimits<double>::Max();
	for (const CSStairs::FBrick& B : Stairs->GetBricks()) HighestTread = FMath::Max(HighestTread, B.Center.Z + B.Size.Z * 0.5);
	TestTrue(TEXT("The terminal brick sits below the deck to avoid coplanar flicker"), FMath::IsNearlyEqual(HighestTread, 299.5, 0.01));
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("Stairs terrace test")));
		Stairs->Modify();
		Stairs->bConnectTerraces = false;
		Stairs->RebuildStairs();
	}
	TestEqual(TEXT("An edit closes the entrance"), House->GetTerraceEntranceCount(), 0);
	GEditor->UndoTransaction();
	TestEqual(TEXT("Undo restores the stair connection"), Stairs->GetTerraceConnectionCount(), 1);
	TestEqual(TEXT("Undo restores the entrance"), House->GetTerraceEntranceCount(), 1);
	Spline = Stairs->GetSpline();
	House->WallHeight = 340;
	House->ReevaluateSite();
	Stairs->FlushPendingReevaluate();
	Stairs->BuildRuns(Runs);
	TestTrue(TEXT("House changes update the connected endpoint"), !Runs.IsEmpty() && FMath::IsNearlyEqual(Runs.Last().Samples.Last().Position.Z,340.5));
	House->RoofPitch = 35;
	House->ReevaluateSite();
	TestEqual(TEXT("Pitched roof detaches the stairs"), Stairs->GetTerraceConnectionCount(), 0);
	TestEqual(TEXT("Pitched roof has no entrance request"), House->GetTerraceEntranceCount(), 0);
	House->RoofPitch = 0;
	House->WallHeight = 300;
	House->ReevaluateSite();
	TestEqual(TEXT("Lowering the roof reconnects"), Stairs->GetTerraceConnectionCount(), 1);
	Stairs->AddActorWorldOffset(FVector(1200,0,0));
	TestEqual(TEXT("Moving away detaches without an explicit rebuild"), Stairs->GetTerraceConnectionCount(), 0);
	TestEqual(TEXT("Moving away closes the entrance"), House->GetTerraceEntranceCount(), 0);
	Stairs->AddActorWorldOffset(FVector(-1200,0,0));
	TestEqual(TEXT("Moving back reconnects"), Stairs->GetTerraceConnectionCount(), 1);
	Stairs->bConnectTerraces = false;
	Stairs->RebuildStairs();
	TestEqual(TEXT("Disabling automatic connection closes the entrance"), House->GetTerraceEntranceCount(), 0);
	Stairs->bConnectTerraces = true;
	Stairs->RebuildStairs();
	House->GetTerraceEntranceCount();
	Stairs->FlushPendingReevaluate();
	const int32 Uploads = Stairs->GetUploadCount();
	for (int32 I = 0; I < 3; ++I)
	{
		House->ReevaluateSite();
		Stairs->Tick(0.016f);
	}
	TestEqual(TEXT("Stable connections do not repeatedly upload"), Stairs->GetUploadCount(), Uploads);
	World->DestroyActor(Stairs);
	TestEqual(TEXT("Deleting the stairs restores the parapet"), House->GetTerraceEntranceCount(), 0);
	ACSStairsActor* Reverse = World->SpawnActor<ACSStairsActor>();
	Reverse->bFollowGround = false;
	Reverse->GetSpline()->SetSplinePoints({ FVector(0,-220,290), FVector(0,-400,150), FVector(0,-700,15) }, ESplineCoordinateSpace::World);
	Reverse->RebuildStairs();
	TestEqual(TEXT("The first endpoint can also connect"), Reverse->GetTerraceConnectionCount(), 1);
	Reverse->GetSpline()->SetClosedLoop(true);
	Reverse->RebuildStairs();
	TestEqual(TEXT("Closed loops create no endpoint entrances"), Reverse->GetTerraceConnectionCount(), 0);
	Reverse->GetSpline()->SetClosedLoop(false);
	Reverse->RebuildStairs();
	TestEqual(TEXT("Reopening the path reconnects"), Reverse->GetTerraceConnectionCount(), 1);
	World->DestroyActor(House);
	TestEqual(TEXT("Deleting the house detaches the stairs"), Reverse->GetTerraceConnectionCount(), 0);
	World->DestroyActor(Reverse);
	return true;
}
#endif
