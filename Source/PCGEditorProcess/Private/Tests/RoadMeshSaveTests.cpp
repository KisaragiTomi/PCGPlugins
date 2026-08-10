#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "RoadMeshComponent.h"
#include "RoadTypes.h"

#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "MeshDescription.h"
#include "RenderingThread.h"
#include "EditorAssetLibrary.h"
#include "Tests/AutomationEditorCommon.h"

// Exercises the road (indexed, V != I) side of the unified GPU-mesh save path end to end:
// a synthetic straight-spline road is built entirely on the GPU, then read back via the
// hoisted ReadbackMeshSync + MeshCounters and saved to a StaticMesh. The key assertion is
// that the saved mesh is a real indexed mesh (fewer vertices than the index count), which
// only holds if the vertex count and index count were read back independently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMeshSaveGPUAutomationTest,
	"PCGPlugins.PCGEditorProcess.RoadMesh.SaveStaticMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FRoadMeshSaveGPUAutomationTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Road host actor"), Actor)) return false;

	USceneComponent* Root = NewObject<USceneComponent>(Actor);
	if (!TestNotNull(TEXT("Root component"), Root)) return false;
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();

	URoadMeshComponent* Road = NewObject<URoadMeshComponent>(Actor);
	if (!TestNotNull(TEXT("Road mesh component"), Road)) return false;
	Road->SetupAttachment(Root);
	Road->RegisterComponent();

	// Synthetic straight road: one spline, 5 resampled points along +X (300cm wide, 400cm long).
	// No junctions -> the road builder emits a plain ribbon: 2 verts/sample, 6 indices/segment,
	// so vertexCount (10) != indexCount (24) -- the indexed case the readback must handle.
	FRoadBuildInput Input;
	Input.SampleStep = 100.0f;
	Input.RoadHalfWidth = 150.0f;
	Input.IntersectionMergeRadius = 1.0f;
	Input.UVLengthScale = 0.001f;

	FRoadSplineRange Range;
	Range.FirstPoint = 0;
	Range.NumPoints = 5;
	Range.Length = 400.0f;
	Range.HalfWidth = 150.0f;
	for (int32 i = 0; i < 5; ++i)
	{
		FRoadSplinePoint Point;
		Point.Position = FVector3f(i * 100.0f, 0.0f, 0.0f);
		Point.DistanceAlongSpline = i * 100.0f;
		Input.Points.Add(Point);
	}
	Input.Splines.Add(Range);

	Input.MaxIntersections = 16;
	Input.MaxVertices = 4096; // conservative over-provision (only bounds the GPU buffers)
	Input.MaxIndices = Input.MaxVertices * 3;
	Input.LocalBounds = FBox(FVector(-50.0, -200.0, -50.0), FVector(450.0, 200.0, 50.0));

	Road->SetBuildInput(MoveTemp(Input));
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	const FString TestAssetPath = FString::Printf(
		TEXT("/Game/Automation/RoadMesh/SM_RoadGPUReadback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	// Save via the road component's editor-only wrapper (delegates to the shared CSGpuMeshConvert).
	UStaticMesh* SavedMesh = Road->SaveToStaticMesh(TestAssetPath, /*bReplace*/ true, /*bSaveAsset*/ false, /*bConvertToLocal*/ false);
	if (!TestNotNull(TEXT("StaticMesh created from road GPU buffers"), SavedMesh)) return false;

	const FMeshDescription* MeshDescription = SavedMesh->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Saved road mesh LOD0 MeshDescription"), MeshDescription)) return false;

	const int32 NumTriangles = MeshDescription->Triangles().Num();
	const int32 NumVertices = MeshDescription->Vertices().Num();
	TestTrue(TEXT("Road mesh has triangles"), NumTriangles > 0);
	TestTrue(TEXT("Road mesh has vertices"), NumVertices >= 3);
	// Indexed mesh: shared vertices => strictly fewer vertices than the index count.
	// This can only hold if vertexCount and indexCount were read back independently (V != I).
	TestTrue(
		FString::Printf(TEXT("Road is an indexed mesh (verts %d < indices %d)"), NumVertices, NumTriangles * 3),
		NumVertices < NumTriangles * 3);

	const bool bDeletedTestAsset = UEditorAssetLibrary::DeleteAsset(TestAssetPath);
	TestTrue(TEXT("Temporary StaticMesh asset cleaned up"), bDeletedTestAsset);
	return true;
}

#endif
