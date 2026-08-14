#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "RoadMeshComponent.h"
#include "RoadTypes.h"

#include "CSGpuMeshComponent.h"
#include "CSMesh.h"
#include "CSMeshOps.h"

#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "RenderingThread.h"
#include "StaticMeshAttributes.h"
#include "EditorAssetLibrary.h"
#include "Tests/AutomationEditorCommon.h"

// Exercises the road (indexed, V != I) side of the unified GPU-mesh save path end to end:
// a synthetic straight-spline road is built entirely on the GPU into a retained UCSMesh, then
// read back and saved to a StaticMesh. Two things are asserted that nothing else covers:
//
//   * the saved mesh is a real indexed mesh (fewer vertices than the index count), which only
//     holds if the vertex count and index count were read back independently;
//   * PLACEMENT. The host actor sits far from the origin and rotated, because that is the only
//     configuration in which the world-space resident set and the local-space asset can be told
//     apart. A road that never got baked into world space draws in the wrong place, and one that
//     never got baked back out saves in the wrong place — and both failures are silent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMeshSaveGPUAutomationTest,
	"PCGPlugins.PCGEditorProcess.RoadMesh.SaveStaticMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FRoadMeshSaveGPUAutomationTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// Rotated and far from the origin on purpose: at the origin every space is the same space, so
	// an identity-placed road cannot distinguish a correct bake from no bake at all.
	const FTransform RoadTransform(FRotator(0.0, 35.0, 0.0), FVector(12000.0, -8000.0, 500.0));

	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Road host actor"), Actor)) return false;

	USceneComponent* Root = NewObject<USceneComponent>(Actor);
	if (!TestNotNull(TEXT("Root component"), Root)) return false;
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();
	Actor->SetActorTransform(RoadTransform);

	URoadMeshComponent* Road = NewObject<URoadMeshComponent>(Actor);
	if (!TestNotNull(TEXT("Road mesh component"), Road)) return false;
	Road->SetupAttachment(Root);
	Road->RegisterComponent();

	UMaterialInterface* RoadMaterial = UCSGpuMeshComponent::GetDefaultSurfaceMaterial();
	Road->MeshMaterial = RoadMaterial;

	// Synthetic straight road: one spline, 5 resampled points along +X (300cm wide, 400cm long),
	// in the actor's local space. No junctions -> the road builder emits a plain ribbon: 2
	// verts/sample, 6 indices/segment, so vertexCount (10) != indexCount (24) -- the indexed case
	// the readback must handle. Local centre is therefore (200, 0, 0).
	const FVector LocalRoadCentre(200.0, 0.0, 0.0);
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

	Road->SetBuildInput(Input, RoadTransform);
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	// --- what the build is now supposed to leave behind: a retained mesh object the component
	//     owns and binds, instead of geometry that exists only for as long as some scene proxy does.
	if (!TestTrue(TEXT("The component reports road geometry"), Road->HasGeneratedGeometry())) return false;
	UCSMesh* GpuMesh = Road->GetGpuMesh();
	if (!TestNotNull(TEXT("The component is bound to a retained GPU mesh"), GpuMesh)) return false;
	TestTrue(TEXT("The retained mesh holds triangles"), GpuMesh->GetTriangleCountSync() > 0);
	TestTrue(TEXT("The road is remembered as built out of the actor transform"),
		Road->GetGeometryToWorld().Equals(RoadTransform));

	// One material, one batch. Sections exist to split a mesh across material slots; a road has
	// nothing to split, and an empty table is what makes it draw as a single whole-mesh batch.
	TestEqual(TEXT("The road mesh carries exactly one material slot"), GpuMesh->Materials.Num(), 1);
	if (GpuMesh->Materials.Num() == 1) TestEqual(TEXT("That slot is the road material"), GpuMesh->Materials[0].Get(), RoadMaterial);
	TestEqual(TEXT("A single-material road publishes no section table"), GpuMesh->GetSections().Num(), 0);

	// The conservative bounds have to travel with the geometry, or the road is culled against a box
	// still sitting at the origin.
	const FBox ExpectedWorldBounds = Input.LocalBounds.TransformBy(RoadTransform);
	const FBox ApproxWorldBounds = GpuMesh->GetWorldBoundsApprox();
	TestTrue(TEXT("The mesh's approximate bounds are the input bounds in world space"),
		ApproxWorldBounds.IsValid && ApproxWorldBounds.Min.Equals(ExpectedWorldBounds.Min, 1.0)
			&& ApproxWorldBounds.Max.Equals(ExpectedWorldBounds.Max, 1.0));

	// The exact AABB of the resident positions. Its centre is the road's centre whatever the
	// rotation, so comparing centres avoids the AABB-of-a-rotated-AABB inflation entirely.
	UCSMeshOps::ComputeWorldBoundsSync(GpuMesh);
	const FBox ExactWorldBounds = GpuMesh->GetWorldBoundsApprox();
	TestTrue(TEXT("The resident geometry really is in world space, at the actor's placement"),
		ExactWorldBounds.IsValid
			&& ExactWorldBounds.GetCenter().Equals(RoadTransform.TransformPosition(LocalRoadCentre), 1.0));

	const FString TestAssetPath = FString::Printf(
		TEXT("/Game/Automation/RoadMesh/SM_RoadGPUReadback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	// Save via the road component's editor-only wrapper (delegates to the shared CSGpuMeshConvert).
	UStaticMesh* SavedMesh = Road->SaveToStaticMesh(Road->GetGeometryToWorld(), TestAssetPath, /*bReplace*/ true, /*bSaveAsset*/ false);
	if (!TestNotNull(TEXT("StaticMesh created from road GPU buffers"), SavedMesh)) return false;
	TestEqual(TEXT("The road's single material slot reaches the saved asset"), SavedMesh->GetStaticMaterials().Num(), 1);

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

	// The asset must be placement-independent: the same local-space mesh the pre-UCSMesh path
	// produced, which reproduces the drawn road when placed on GetGeometryToWorld(). If the bake
	// back out were missing, this box would sit ~144m away and both checks below would fail.
	FStaticMeshConstAttributes Attributes(*MeshDescription);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	FBox SavedLocalBounds(ForceInit);
	for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs()) SavedLocalBounds += FVector(Positions[VertexID]);
	TestTrue(TEXT("Saved positions are back in the space the road was built from"),
		SavedLocalBounds.IsValid && SavedLocalBounds.GetCenter().Equals(LocalRoadCentre, 1.0));
	TestTrue(TEXT("Saved positions stay inside the road's declared local bounds"),
		SavedLocalBounds.IsValid && Input.LocalBounds.IsInside(SavedLocalBounds));

	// Lifting the geometry off the scene proxy means the save no longer needs anything to be
	// rendering it — the old path rejected an unregistered component outright. Same save call,
	// replacing the same asset in place.
	Road->UnregisterComponent();
	FlushRenderingCommands();
	UStaticMesh* SavedWithoutProxy = Road->SaveToStaticMesh(Road->GetGeometryToWorld(), TestAssetPath, /*bReplace*/ true, /*bSaveAsset*/ false);
	if (TestNotNull(TEXT("StaticMesh saved with nothing rendering the road"), SavedWithoutProxy))
	{
		const FMeshDescription* ProxylessDescription = SavedWithoutProxy->GetMeshDescription(0);
		if (TestNotNull(TEXT("Proxy-free save LOD0 MeshDescription"), ProxylessDescription)) TestEqual(TEXT("Proxy-free save keeps every triangle"), ProxylessDescription->Triangles().Num(), NumTriangles);
	}

	const bool bDeletedTestAsset = UEditorAssetLibrary::DeleteAsset(TestAssetPath);
	TestTrue(TEXT("Temporary StaticMesh asset cleaned up"), bDeletedTestAsset);
	return true;
}

#endif
