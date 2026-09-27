#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CSHouseActor.h"
#include "CSHouseHeightHandleActor.h"
#include "CSHouseTile.h"
#include "CSGpuMeshTypes.h"
#include "CSStairs.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoofTerraceGeometryTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofTerraceGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoofTerraceGeometryTest::RunTest(const FString& Parameters)
{
	FCSRoofDesc Roof;
	Roof.bFlat = true;
	Roof.Pitch = 0.0f;
	Roof.Overhang = 0.0f;
	TArray<CSHouseTile::FRecord> Tiles;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, CSHouseTile::FParams(), Tiles);
	TestEqual(TEXT("Terrace has no slope or ridge tiles"), Tiles.Num(), 0);
	TestEqual(TEXT("Terrace allocates no tile capacity"), CSHouseTile::MaxTilesBound(Roof, CSHouseTile::FParams()), 0);
	TArray<FCSRoofParapetBlock> Blocks;
	CSHouseRoof_BuildParapet(Roof, 24.0f, 55.0f, Blocks);
	TestTrue(TEXT("Terrace has masonry around its perimeter"), Blocks.Num() > 20);
	for (const FCSRoofParapetBlock& B : Blocks)
	{
		TestTrue(TEXT("Parapet remains above the deck"), B.Center.Z - B.Size.Z * 0.5 >= Roof.EaveZ - 0.01);
		TestTrue(TEXT("Parapet has positive finite dimensions"), !B.Size.ContainsNaN() && B.Size.GetMin() > 0.0);
	}
	for (int32 I = 0; I < Roof.Footprint.NumEdges(); ++I)
	{
		const FCSHouseEdgeFrame Edge = CSHouse_GetEdge(I, Roof.Footprint, 0.0f);
		const FVector2D XY = Edge.Start + Edge.In + Edge.U;
		const FVector Probe(XY.X, XY.Y, Roof.EaveZ + 5.0);
		bool bCovered = false;
		for (const FCSRoofParapetBlock& B : Blocks)
		{
			const FVector Local = B.Rotation.UnrotateVector(Probe - B.Center).GetAbs();
			bCovered |= Local.X <= B.Size.X * 0.5 && Local.Y <= B.Size.Y * 0.5 && Local.Z <= B.Size.Z * 0.5;
		}
		TestTrue(TEXT("Parapet masonry closes the outer corner"), bCovered);
	}
	FCSHouseBodyDesc Body;
	FCSGpuMeshCPUData Pitched, Flat;
	CSHouse_BuildBodySoup(Body, Pitched);
	Body.bFlatRoof = true;
	CSHouse_BuildBodySoup(Body, Flat);
	TestEqual(TEXT("Rectangular terrace adds two deck triangles"), Flat.Indices.Num() - Pitched.Indices.Num(), 6);
	Roof.bFlat = false;
	CSHouseRoof_BuildParapet(Roof, 24.0f, 55.0f, Blocks);
	TestEqual(TEXT("Raising the roof removes the parapet"), Blocks.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoofHeightHandleTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofHeightHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoofHeightHandleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>();
	House->EnterResizeMode();
	ACSHouseHeightHandleActor* Handle = House->GetRoofHandle();
	if (!TestNotNull(TEXT("Resize mode creates a roof handle"), Handle)) return false;
	const float WallHeight = House->WallHeight, Base = House->HeightOffset;
	const float Rise = House->GetRoofRise();
	Handle->AddActorWorldOffset(FVector(40, -30, -Rise));
	Handle->ConsumeDragToHost(false);
	TestTrue(TEXT("Lowering to zero switches to terrace"), House->IsFlatRoof());
	TestTrue(TEXT("Roof handle clamps at zero without debt"), FMath::IsNearlyZero(House->GetRoofRise(), 0.01f));
	Handle->AddActorWorldOffset(FVector(0, 0, -100));
	TestTrue(TEXT("Pushing below zero applies no movement"), FMath::IsNearlyZero(Handle->ConsumeDragToHost(false)));
	Handle->AddActorWorldOffset(FVector(0, 0, 60));
	Handle->ConsumeDragToHost(true);
	TestFalse(TEXT("Raising restores pitched roof"), House->IsFlatRoof());
	TestTrue(TEXT("One cm drag means one cm rise after clamp"), FMath::IsNearlyEqual(House->GetRoofRise(), 60.0f, 0.01f));
	TestEqual(TEXT("Roof drag leaves wall height intact"), House->WallHeight, WallHeight);
	TestEqual(TEXT("Roof drag leaves base height intact"), House->HeightOffset, Base);
	House->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsRailLadderTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.RailsAndLadder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsRailLadderTest::RunTest(const FString& Parameters)
{
	CSStairs::FRun Run;
	Run.Samples.Add({ FVector(0, 0, 100), 100.0f, FVector2D(1, 0) });
	Run.Samples.Add({ FVector(300, 0, 250), 180.0f, FVector2D(1, 0) });
	CSStairs::FParams Params;
	Params.bNarrowByRailing = true;
	TArray<CSStairs::FBrick> Rails;
	CSStairs::BuildRunRails(Run, Params, {}, Rails);
	TestEqual(TEXT("None emits no railing"), Rails.Num(), 0);
	for (ECSStairsRailing Type : { ECSStairsRailing::Low, ECSStairsRailing::High, ECSStairsRailing::Wooden })
	{
		Rails.Reset();
		Params.Railing = Type;
		CSStairs::BuildRunRails(Run, Params, {}, Rails);
		TestTrue(TEXT("Each railing style emits geometry"), Rails.Num() > 4);
		bool bLeft = false, bRight = false;
		for (const CSStairs::FBrick& B : Rails)
		{
			bLeft |= B.Center.Y < -30;
			bRight |= B.Center.Y > 30;
			TestTrue(TEXT("Railing transforms remain finite"), !B.Center.ContainsNaN() && !B.Rotation.ContainsNaN() && B.Size.GetMin() > 0);
			const double TreadHalfWidth = (FMath::Lerp(100.0, 180.0, B.Center.X / 300.0) - Params.RailingNarrow) * 0.5;
			TestTrue(TEXT("Railing centres stay supported by the tread"), FMath::Abs(B.Center.Y) < TreadHalfWidth);
		}
		TestTrue(TEXT("Both sides follow the path"), bLeft && bRight);
	}
	Run.Type = CSStairs::ESegmentType::Ladder;
	Run.Samples.Last().Position = FVector(0, 0, 500);
	TArray<CSStairs::FBrick> Wood;
	const int32 Rungs = CSStairs::BuildLadder(Run, Params, Wood);
	TestTrue(TEXT("Vertical segments have climbable rungs"), Rungs > 5);
	TestEqual(TEXT("Two side rails per interval plus rungs"), Wood.Num(), (Rungs - 1) * 2 + Rungs);
	for (const CSStairs::FBrick& B : Wood) TestTrue(TEXT("Vertical ladder has no degenerate transform"), !B.Rotation.ContainsNaN() && B.Size.GetMin() > 0);
	Run.Type = CSStairs::ESegmentType::Walkway;
	Run.Samples = { { FVector(0, 0, 0), 150.0f, FVector2D(1, 0) }, { FVector(100, 0, 0), 150.0f, FVector2D(1, 0) } };
	Params.Railing = ECSStairsRailing::Low;
	Rails.Reset();
	CSStairs::BuildRunRails(Run, Params, [](const FVector2D& XY) { return float(XY.X * 0.5); }, Rails);
	if (TestEqual(TEXT("Two walkway intervals have rails on both sides"), Rails.Num(), 4))
	{
		TestTrue(TEXT("Walkway rail follows raised endpoint tread"), FMath::IsNearlyEqual(Rails[0].Center.Z - 22.5, 40.0));
		TestTrue(TEXT("Last walkway rail follows raised endpoint tread"), FMath::IsNearlyEqual(Rails[1].Center.Z - 22.5, 65.0));
	}
	return true;
}
#endif
