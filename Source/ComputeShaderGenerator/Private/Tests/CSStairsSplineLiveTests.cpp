#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CSStairsActor.h"
#include "CSStairsWidthHandleActor.h"
#include "Components/SplineComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsSplineLiveTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.SplineLiveRebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsSplineLiveTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSStairsActor* Stairs = World->SpawnActor<ACSStairsActor>();
	Stairs->bFollowGround = false;
	Stairs->bConnectTerraces = false;
	USplineComponent* Spline = Stairs->GetSpline();
	Spline->SetSplinePoints({ FVector(0,0,15), FVector(350,0,200), FVector(700,0,400) },ESplineCoordinateSpace::Local);
	Stairs->RebuildStairs();
	Stairs->EnterResizeMode();
	ACSStairsWidthHandleActor* Handle = Stairs->GetResizeHandles()[0];
	const FVector InitialHandle = Handle->GetActorLocation();
	int32 Uploads = Stairs->GetUploadCount();
	TestFalse(TEXT("Idle stairs do not tick"), Stairs->IsActorTickEnabled());
	for (int32 I = 1; I <= 3; ++I)
	{
		// 编辑器拖动的真实组合：更新曲线，再发尚未松手的 PostEditMove(false)。
		Spline->SetLocationAtSplinePoint(1,FVector(350,70 * I,200 + 15 * I),ESplineCoordinateSpace::Local,true);
		Stairs->PostEditMove(false);
	}
	TestTrue(TEXT("Interactive edits wake the rebuild queue before release"), Stairs->IsActorTickEnabled());
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Three edits in one frame upload the final shape once"), Stairs->GetUploadCount(),Uploads + 1);
	TestFalse(TEXT("A processed frame goes idle again"), Stairs->IsActorTickEnabled());
	TestFalse(TEXT("Visible width handle follows the moved spline"), Handle->GetActorLocation().Equals(InitialHandle,1.0));
	TestTrue(TEXT("Handle matches its new canonical location"), Handle->GetActorLocation().Equals(Handle->ComputeCanonicalWorldLocation(),0.01));
	bool bBentBricks = false;
	for (const CSStairs::FBrick& B : Stairs->GetBricks()) bBentBricks |= B.Center.Y > 100;
	TestTrue(TEXT("Visible bricks bend during the drag, before mouse release"), bBentBricks);
	Uploads = Stairs->GetUploadCount();
	Stairs->PostEditMove(true);
	TestEqual(TEXT("Release with no further change does not reupload"), Stairs->GetUploadCount(),Uploads);

	// 蓝图/script setter 不经过 actor 构造脚本或 PostEditMove，仍应从样条通知重建。
	for (int32 I = 0; I < 3; ++I) Spline->SetScaleAtSplinePoint(I,FVector(1,1.5,1),true);
	TestTrue(TEXT("Spline scale changes wake stairs without PostEditMove"), Stairs->IsActorTickEnabled());
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Scale setters coalesce to one geometry upload"), Stairs->GetUploadCount(),Uploads + 1);
	Uploads = Stairs->GetUploadCount();
	Spline->SetTangentsAtSplinePoint(1,FVector(200,400,0),FVector(200,-400,0),ESplineCoordinateSpace::Local,true);
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Changing tangents rebuilds the bent path"), Stairs->GetUploadCount(),Uploads + 1);
	Uploads = Stairs->GetUploadCount();
	Spline->SetRelativeLocation(FVector(0,300,0));
	TestTrue(TEXT("Moving just the spline component is observed"), Stairs->IsActorTickEnabled());
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Component translation uploads new brick placement"), Stairs->GetUploadCount(),Uploads + 1);
	Uploads = Stairs->GetUploadCount();
	Spline->AddSplinePoint(FVector(1100,0,500),ESplineCoordinateSpace::Local,true);
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Adding a point rebuilds automatically"), Stairs->GetUploadCount(),Uploads + 1);
	Spline->RemoveSplinePoint(3,true);
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Removing a point rebuilds automatically"), Stairs->GetUploadCount(),Uploads + 2);
	Uploads = Stairs->GetUploadCount();
	for (int32 I = 0; I < 3; ++I) Spline->UpdateSpline();
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Unchanged spline updates do not upload geometry"), Stairs->GetUploadCount(),Uploads);
	TestFalse(TEXT("Repeated no-op notifications do not leave ticking enabled"), Stairs->IsActorTickEnabled());
	World->DestroyActor(Stairs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsSplineUndoTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.SplineComponentUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsSplineUndoTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSStairsActor* Stairs = World->SpawnActor<ACSStairsActor>();
	Stairs->bFollowGround = false;
	Stairs->bConnectTerraces = false;
	Stairs->RebuildStairs();
	USplineComponent* Spline = Stairs->GetSpline();
	Spline->SetFlags(RF_Transactional);
	const FVector Original = Spline->GetLocationAtSplinePoint(1,ESplineCoordinateSpace::Local);
	const int32 OriginalBricks = Stairs->GetBrickCount();
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("Edit only the stair spline")));
		Spline->Modify();
		Spline->SetLocationAtSplinePoint(1,Original + FVector(0,300,120),ESplineCoordinateSpace::Local,true);
	}
	Stairs->Tick(1.0f / 60.0f);
	const int32 Uploads = Stairs->GetUploadCount();
	GEditor->UndoTransaction();
	TestTrue(TEXT("Component-only undo wakes the stair actor"), Stairs->IsActorTickEnabled());
	Stairs->Tick(1.0f / 60.0f);
	TestTrue(TEXT("Undo restores the spline point"), Stairs->GetSpline()->GetLocationAtSplinePoint(1,ESplineCoordinateSpace::Local).Equals(Original,0.01));
	TestEqual(TEXT("Undo rebuilds the original geometry"), Stairs->GetBrickCount(),OriginalBricks);
	TestEqual(TEXT("Undo uploads once without manually rebuilding"), Stairs->GetUploadCount(),Uploads + 1);
	GEditor->RedoTransaction();
	Stairs->Tick(1.0f / 60.0f);
	TestEqual(TEXT("Redo also rebuilds automatically"), Stairs->GetUploadCount(),Uploads + 2);
	World->DestroyActor(Stairs);
	return true;
}
#endif
