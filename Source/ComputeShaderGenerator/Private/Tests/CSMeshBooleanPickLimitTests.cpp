#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "ComputeShaderMeshBoolean.h"

#include "Components/BoxComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// MaxPickTriangles gate: which geometry counts, and what a refused run leaves behind.
//
// Only the count and the refusal are exercised. A run that passes the gate goes on into the GPU
// pipeline, which MeshBoolean.GpuParity already covers; asserting on it here would only make this
// test need an RHI.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSMeshBooleanPickTriangleLimitTest,
	"PCGPlugins.ComputeShaderGenerator.MeshBoolean.PickTriangleLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSMeshBooleanPickTriangleLimitTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;
	const int64 CubeTriangles = CubeMesh->GetNumTriangles(0);
	if (!TestTrue(TEXT("Engine cube has triangles"), CubeTriangles > 0)) return false;

	auto SpawnCube = [World, CubeMesh](const FVector& Location, FName Tag)
	{
		AStaticMeshActor* Cube = World->SpawnActor<AStaticMeshActor>();
		Cube->SetActorTransform(FTransform(Location));
		Cube->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		if (!Tag.IsNone()) Cube->Tags.Add(Tag);
	};
	// Only the two Pick cubes inside the box count. The untagged and the Ref cube are in the box
	// but are not output geometry, and the far Pick cube never reaches the source soup.
	SpawnCube(FVector(0.0, 0.0, 0.0), TEXT("Pick"));
	SpawnCube(FVector(120.0, 0.0, 0.0), TEXT("Pick"));
	SpawnCube(FVector(0.0, 120.0, 0.0), NAME_None);
	SpawnCube(FVector(0.0, -120.0, 0.0), TEXT("Ref"));
	SpawnCube(FVector(5000.0, 0.0, 0.0), TEXT("Pick"));

	AComputeShaderMeshBoolean* Generator = World->SpawnActor<AComputeShaderMeshBoolean>();
	if (!TestNotNull(TEXT("Mesh boolean generator"), Generator)) return false;
	Generator->GeneratorBounds->SetBoxExtent(FVector(300.0));
	World->UpdateWorldComponents(true, false);

	TestEqual(TEXT("Pick triangles are the two tagged cubes inside the box"), Generator->CountPickTriangles(), 2 * CubeTriangles);

	// Reaching the limit refuses the run. Unattended mode is what suppresses the dialog, so the
	// text fallback has to appear instead - and only once, however often the run is refused.
	Generator->MaxPickTriangles = int32(2 * CubeTriangles);
	AddExpectedMessagePlain(TEXT("本次 Boolean 未执行"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		TestNull(TEXT("Run is refused at the limit"), Generator->BooleanBoxScene(ECSMeshBooleanOp::Union));
		TestNull(TEXT("Run is refused again"), Generator->BooleanBoxScene(ECSMeshBooleanOp::Union));
	}

	TArray<AActor*> AttachedActors;
	Generator->GetAttachedActors(AttachedActors);
	int32 LimitTexts = 0;
	for (const AActor* Attached : AttachedActors)
	{
		const ATextRenderActor* TextActor = Cast<ATextRenderActor>(Attached);
		if (!TextActor) continue;
		++LimitTexts;
		const FString Text = TextActor->GetTextRender()->Text.ToString();
		TestTrue(TEXT("Limit text shows the count"), Text.Contains(FString::Printf(TEXT("%lld"), 2 * CubeTriangles)));
		TestTrue(TEXT("Limit text is not saved with the level"), TextActor->HasAnyFlags(RF_Transient));
	}
	TestEqual(TEXT("A refused run leaves exactly one limit text"), LimitTexts, 1);
	return true;
}

#endif
