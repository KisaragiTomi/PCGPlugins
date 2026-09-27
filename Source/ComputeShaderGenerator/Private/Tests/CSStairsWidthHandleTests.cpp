#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CSStairsActor.h"
#include "CSStairsWidthHandleActor.h"
#include "CSHouseActor.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsWidthHandleTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.WidthHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsWidthHandleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSStairsActor* Stairs = World->SpawnActor<ACSStairsActor>();
	Stairs->SetFlags(RF_Transactional);
	Stairs->bFollowGround = false;
	Stairs->bConnectTerraces = false;
	Stairs->Railing = ECSStairsRailing::None;
	Stairs->Width = 150.0f;
	Stairs->GetSpline()->SetSplinePoints({ FVector(0,0,0), FVector(600,0,300) }, ESplineCoordinateSpace::Local);
	Stairs->RebuildStairs();
	const int32 Steps = Stairs->GetStepCount();
	TestFalse(TEXT("Handles are opt-in"), Stairs->IsInResizeMode());
	Stairs->EnterResizeMode();
	TArray<ACSStairsWidthHandleActor*> Handles = Stairs->GetResizeHandles();
	if (!TestEqual(TEXT("Two side handles"), Handles.Num(), 2)) return false;
	ACSStairsWidthHandleActor* Handle = Handles[1];
	TestEqual(TEXT("Host is the stair actor"), Handle->GetHost(), Stairs);
	TestTrue(TEXT("Handles are attached and transient"), Handle->GetAttachParentActor() == Stairs && Handle->HasAnyFlags(RF_Transient));
	UStaticMeshComponent* Arrow = Handle->FindComponentByClass<UStaticMeshComponent>();
	TestTrue(TEXT("Same editor-only, nonblocking prop as house handles"), Arrow && Arrow->bIsEditorOnly && Arrow->GetCollisionEnabled() == ECollisionEnabled::NoCollision && !Arrow->CastShadow);
	Stairs->EnterResizeMode();
	TestTrue(TEXT("Repeated entry reuses the same two handles"), Stairs->GetResizeHandles() == Handles);
	const FVector Before = Handle->GetActorLocation();
	const FVector Outer = Handle->GetOuterNormalWorld();
	Handle->AddActorWorldOffset(Outer * 20.0 + FVector(0,0,75));
	TestTrue(TEXT("Twenty cm drag moves this side twenty cm"), FMath::IsNearlyEqual(Handle->ConsumeDragToHost(false),20.0f,0.01f));
	TestTrue(TEXT("Width grows symmetrically by forty cm"), FMath::IsNearlyEqual(Stairs->Width,190.0f));
	TestTrue(TEXT("Handle snaps off-axis motion away"), Handle->GetActorLocation().Equals(Before + Outer * 20.0,0.01));
	TestTrue(TEXT("Centreline stays authored"), Stairs->GetSpline()->GetLocationAtSplinePoint(1,ESplineCoordinateSpace::Local).Equals(FVector(600,0,300)));
	TestEqual(TEXT("Changing width preserves step count"), Stairs->GetStepCount(), Steps);
	TArray<CSStairs::FRun> Runs;
	Stairs->BuildRuns(Runs);
	TestTrue(TEXT("Generated path consumes the new width"), !Runs.IsEmpty() && FMath::IsNearlyEqual(Runs[0].Samples[0].Width,190.0f));
	const int32 Uploads = Stairs->GetUploadCount();
	Handle->HandleDrag(true);
	TestEqual(TEXT("Release without movement does not reupload"), Stairs->GetUploadCount(), Uploads);
	Stairs->SetStairWidth(45);
	Handle->AddActorWorldOffset(-Outer * 500.0);
	TestTrue(TEXT("Lower limit applies no movement"), FMath::IsNearlyZero(Handle->ConsumeDragToHost(false)));
	Handle->AddActorWorldOffset(Outer * 10.0);
	Handle->HandleDrag(true);
	TestTrue(TEXT("Reverse after clamp has no drag debt"), FMath::IsNearlyEqual(Stairs->Width,65.0f));
	Stairs->SetStairWidth(390);
	Handle->AddActorWorldOffset(Outer * 20.0);
	TestTrue(TEXT("Upper limit reports actual side displacement"), FMath::IsNearlyEqual(Handle->ConsumeDragToHost(true),5.0f));
	TestEqual(TEXT("Maximum width"), Stairs->Width,400.0f);
	Stairs->SetStairWidth(150);
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("Drag stair width")));
		Handle->Modify();
		Handle->AddActorWorldOffset(Outer * 25.0);
		Handle->HandleDrag(true);
	}
	TestEqual(TEXT("Drag participates in the editor transaction"), Stairs->Width,200.0f);
	GEditor->UndoTransaction();
	TestEqual(TEXT("Undo restores width"), Stairs->Width,150.0f);
	for (ACSStairsWidthHandleActor* H : Stairs->GetResizeHandles()) TestTrue(TEXT("Undo restores handle alignment"), H->GetActorLocation().Equals(H->ComputeCanonicalWorldLocation(),0.01));
	GEditor->RedoTransaction();
	TestEqual(TEXT("Redo restores width"), Stairs->Width,200.0f);
	Handles = Stairs->GetResizeHandles();
	World->DestroyActor(Handles[0]);
	TestEqual(TEXT("Deleting one handle unregisters it"), Stairs->GetResizeHandles().Num(),1);
	Stairs->EnterResizeMode();
	TestEqual(TEXT("Entry restores only the missing side"), Stairs->GetResizeHandles().Num(),2);
	Stairs->ExitResizeMode();
	Stairs->ExitResizeMode();
	TestFalse(TEXT("Exit is idempotent"), Stairs->IsInResizeMode());
	Stairs->EnterResizeMode();
	TArray<TWeakObjectPtr<ACSStairsWidthHandleActor>> Weak;
	for (ACSStairsWidthHandleActor* H : Stairs->GetResizeHandles()) Weak.Add(H);
	World->DestroyActor(Stairs);
	for (const auto& H : Weak) TestTrue(TEXT("Deleting the stairs cleans every handle"), !H.IsValid() || H->IsActorBeingDestroyed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsWidthHandleTransformTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.WidthHandleTransform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsWidthHandleTransformTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSStairsActor* Stairs = World->SpawnActor<ACSStairsActor>();
	Stairs->bFollowGround = false;
	Stairs->bConnectTerraces = false;
	Stairs->Width = 150;
	USplineComponent* Spline = Stairs->GetSpline();
	Spline->SetSplinePoints({ FVector(600,0,300), FVector(0,0,0) }, ESplineCoordinateSpace::Local);
	for (int32 I = 0; I < 2; ++I) Spline->SetScaleAtSplinePoint(I,FVector(1,2,1));
	Stairs->EnterResizeMode();
	ACSStairsWidthHandleActor* Handle = Stairs->GetResizeHandles()[0];
	Stairs->SetActorTransform(FTransform(FRotator(0,67,0),FVector(1400,-650,250),FVector(1.6,0.7,1.2)));
	// 故意不先 tick/rebuild：attach 搬动不能被记作用户拖拽。
	TestTrue(TEXT("Parent transform alone is not a width drag"), FMath::IsNearlyZero(Handle->ConsumeDragToHost(false),0.01f));
	TestTrue(TEXT("Parent transform leaves width intact"), FMath::IsNearlyEqual(Stairs->Width,150.0f));
	Stairs->FlushPendingReevaluate();
	FVector Center, Right;
	float WidthScale;
	TestTrue(TEXT("Resolved frame is available"), Stairs->GetWidthHandleFrame(Center,Right,WidthScale));
	TestTrue(TEXT("Frame preserves per-point width scale"), FMath::IsNearlyEqual(WidthScale,2.0f));
	Handle->AddActorWorldOffset(Handle->GetOuterNormalWorld() * 20.0);
	TestTrue(TEXT("Scaled reversed stair still follows a twenty cm drag"), FMath::IsNearlyEqual(Handle->ConsumeDragToHost(true),20.0f,0.01f));
	TestTrue(TEXT("Scaled handle converts to the base width"), FMath::IsNearlyEqual(Stairs->Width,170.0f,0.01f));
	Stairs->SetStairWidth(200);
	for (ACSStairsWidthHandleActor* H : Stairs->GetResizeHandles()) TestTrue(TEXT("Programmatic setter repositions both handles"), H->GetActorLocation().Equals(H->ComputeCanonicalWorldLocation(),0.01));
	Spline->ClearSplinePoints();
	Stairs->RebuildStairs();
	TestFalse(TEXT("Invalid path clears handles"), Stairs->IsInResizeMode());
	Stairs->EnterResizeMode();
	TestFalse(TEXT("Invalid path cannot create floating handles"), Stairs->IsInResizeMode());
	World->DestroyActor(Stairs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSStairsWidthHandleTerraceTest,
	"PCGPlugins.ComputeShaderGenerator.Stairs.WidthHandleTerrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSStairsWidthHandleTerraceTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>();
	House->RoofPitch = 0;
	House->WallHeight = 300;
	House->FootprintSize = FVector2D(600,400);
	House->ReevaluateSite();
	ACSStairsActor* Stairs = World->SpawnActor<ACSStairsActor>();
	Stairs->bFollowGround = false;
	Stairs->Width = 150;
	Stairs->GetSpline()->SetSplinePoints({ FVector(0,-700,15), FVector(0,-400,150), FVector(0,-220,290) }, ESplineCoordinateSpace::World);
	Stairs->EnterResizeMode();
	TArray<FCSStairsTerraceLink> Links;
	Stairs->GetTerraceConnections(Links);
	if (!TestEqual(TEXT("Stairs connect to the terrace"), Links.Num(),1)) return false;
	const float OpeningBefore = Links[0].Connection.Opening.End - Links[0].Connection.Opening.Start;
	ACSStairsWidthHandleActor* Handle = Stairs->GetResizeHandles()[0];
	Handle->AddActorWorldOffset(Handle->GetOuterNormalWorld() * 30.0);
	Handle->HandleDrag(true);
	Stairs->GetTerraceConnections(Links);
	if (!TestEqual(TEXT("Wider stair remains connected"), Links.Num(),1)) return false;
	const float OpeningAfter = Links[0].Connection.Opening.End - Links[0].Connection.Opening.Start;
	TestTrue(TEXT("Terrace opening widens by the full width delta"), FMath::IsNearlyEqual(OpeningAfter - OpeningBefore,60.0f,0.1f));
	TestEqual(TEXT("House keeps one updated entrance"), House->GetTerraceEntranceCount(),1);
	TestTrue(TEXT("Rail geometry is rebuilt with the stairs"), Stairs->GetRailingPieceCount() > 0);
	World->DestroyActor(Stairs);
	World->DestroyActor(House);
	return true;
}
#endif
