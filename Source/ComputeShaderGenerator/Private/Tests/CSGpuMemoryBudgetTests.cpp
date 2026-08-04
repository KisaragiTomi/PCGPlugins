#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMemoryBudget.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMemoryBudgetCostModelAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.MemoryBudget.CostModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Locks the per-triangle byte constants to the buffer allocations they were derived from.
// If someone changes a CreateBuffer capacity in ComputeShaderMeshBoolean.cpp or
// CSGpuTriangleUtilities.cpp without updating the model, the budget silently starts lying
// about how much VRAM a run needs - which is exactly the failure this check is meant to catch.
bool FCSGpuMemoryBudgetCostModelAutomationTest::RunTest(const FString& Parameters)
{
	// Positions only: 3 verts * FVector4f.
	CSGpuMemoryBudget::FTriangleSoupCostModel Bare;
	Bare.bSourceNormals = false;
	Bare.bSourceTangents = false;
	Bare.bSourceColors = false;
	Bare.bSourceUVs = false;
	Bare.bSourceMaterialIds = false;
	TestEqual(TEXT("Positions-only soup"), Bare.BytesPerSourceTriangle(), int64(48));

	// Full source attribute set: 48 pos + 48 normals + 96 tangent/bitangent + 48 colors
	// + 24 UV + 4 material id.
	CSGpuMemoryBudget::FTriangleSoupCostModel Attributes;
	TestEqual(TEXT("Full attribute soup"), Attributes.BytesPerSourceTriangle(), int64(268));

	// Mesh Boolean defaults: attributes + LBVH(112) + winding(320) + 4 cut segments(128)
	// + 8 output triangles(320).
	CSGpuMemoryBudget::FTriangleSoupCostModel Boolean;
	Boolean.CutSegmentsPerTriangle = 4;
	Boolean.OutputTrianglesPerSource = 8;
	Boolean.bBuildLBVH = true;
	Boolean.bBuildWindingField = true;
	TestEqual(TEXT("Mesh Boolean default cost"), Boolean.BytesPerSourceTriangle(), int64(1148));

	// Welding adds 3 uint32 representatives per output triangle.
	CSGpuMemoryBudget::FTriangleSoupCostModel Welded = Boolean;
	Welded.bWeldOutput = true;
	TestEqual(TEXT("Welded Boolean cost"), Welded.BytesPerSourceTriangle(), int64(1148 + 8 * 12));

	// Arrangement-only (Stage A) drops the winding field, the single most expensive block.
	CSGpuMemoryBudget::FTriangleSoupCostModel StageAOnly = Boolean;
	StageAOnly.bBuildWindingField = false;
	TestEqual(TEXT("Stage A only cost"), StageAOnly.BytesPerSourceTriangle(), int64(1148 - 320));

	// Triangle limit is free VRAM * safety ratio / bytes per triangle.
	CSGpuMemoryBudget::FMemorySnapshot Snapshot;
	Snapshot.TotalVideoMemory = 8ll * 1024 * 1024 * 1024;
	Snapshot.AvailableVideoMemory = int64(1148) * 2000;
	Snapshot.bAvailableIsMeasured = true;
	TestEqual(TEXT("Half of the budget"),
		CSGpuMemoryBudget::EstimateMaxSourceTriangles(Boolean, Snapshot, 0.5f), int64(1000));

	// No VRAM information means no limit can be computed; callers must not be blocked by it.
	CSGpuMemoryBudget::FMemorySnapshot Unknown;
	TestEqual(TEXT("Unknown VRAM yields no limit"),
		CSGpuMemoryBudget::EstimateMaxSourceTriangles(Boolean, Unknown, 0.7f), int64(0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMemoryBudgetSceneEstimateAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.MemoryBudget.SceneEstimate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The pre-flight estimate has to answer before any extraction runs, so it reads render-data
// triangle counts and component bounds only. This covers both halves of that: the count itself,
// and the overlap weighting that keeps a large mesh poking one corner into the query box from
// being charged its full triangle count.
bool FCSGpuMemoryBudgetSceneEstimateAutomationTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;

	// Placed far from anything the default map may contain so the query box isolates this cube.
	const FVector CubeOrigin(100000.0, 0.0, 0.0);
	AStaticMeshActor* SourceActor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("Source StaticMesh actor"), SourceActor)) return false;
	SourceActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
	SourceActor->SetActorLocation(CubeOrigin);
	World->UpdateWorldComponents(true, false);

	const int64 CubeTriangles = int64(CubeMesh->GetRenderData()->LODResources[0].GetNumTriangles());
	TestTrue(TEXT("Cube has triangles"), CubeTriangles > 0);

	// Fully contained: no overlap discount.
	const CSGpuMemoryBudget::FBoxSceneTriangleEstimate Contained =
		CSGpuMemoryBudget::EstimateBoxSceneTriangles(World, FBox(CubeOrigin - FVector(200.0), CubeOrigin + FVector(200.0)));
	TestEqual(TEXT("Contained instance count"), Contained.StaticMeshInstances, 1);
	TestEqual(TEXT("Contained upper bound"), Contained.UpperBoundTriangles, CubeTriangles);
	TestEqual(TEXT("Contained estimate"), Contained.EstimatedTriangles, CubeTriangles);

	// One octant of the cube: volume ratio 1/8, charged as an area ratio (1/8)^(2/3) = 1/4.
	const CSGpuMemoryBudget::FBoxSceneTriangleEstimate Octant =
		CSGpuMemoryBudget::EstimateBoxSceneTriangles(World, FBox(CubeOrigin - FVector(200.0), CubeOrigin));
	TestEqual(TEXT("Octant upper bound"), Octant.UpperBoundTriangles, CubeTriangles);
	// One triangle of slack: the ratio goes through pow(), so the truncated product may land
	// either side of the exact quarter.
	TestTrue(TEXT("Octant estimate is area weighted"),
		FMath::Abs(Octant.EstimatedTriangles - CubeTriangles / 4) <= 1);

	// Disjoint box: the component must not be counted at all.
	const CSGpuMemoryBudget::FBoxSceneTriangleEstimate Disjoint =
		CSGpuMemoryBudget::EstimateBoxSceneTriangles(World, FBox(CubeOrigin + FVector(5000.0), CubeOrigin + FVector(5200.0)));
	TestEqual(TEXT("Disjoint instance count"), Disjoint.StaticMeshInstances, 0);
	TestEqual(TEXT("Disjoint estimate"), Disjoint.EstimatedTriangles, int64(0));

	// Null world / invalid box are guard paths, not errors.
	const CSGpuMemoryBudget::FBoxSceneTriangleEstimate NoWorld =
		CSGpuMemoryBudget::EstimateBoxSceneTriangles(nullptr, FBox(FVector(-100.0), FVector(100.0)));
	TestEqual(TEXT("Null world estimate"), NoWorld.EstimatedTriangles, int64(0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMemoryBudgetSnapshotAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.MemoryBudget.Snapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Reports what this machine actually sees. There is no correct value to assert against, so the
// test checks internal consistency and logs the numbers - which is also how you confirm the DXGI
// path is live rather than silently falling back to the estimate.
bool FCSGpuMemoryBudgetSnapshotAutomationTest::RunTest(const FString& Parameters)
{
	const CSGpuMemoryBudget::FMemorySnapshot Snapshot = CSGpuMemoryBudget::QueryMemorySnapshot();

	AddInfo(FString::Printf(TEXT("VRAM total %.0f MiB, available %.0f MiB (%s), demoted %.0f MiB, host RAM free %.0f MiB"),
		double(Snapshot.TotalVideoMemory) / (1024.0 * 1024.0),
		double(Snapshot.AvailableVideoMemory) / (1024.0 * 1024.0),
		Snapshot.bAvailableIsMeasured ? TEXT("adapter budget") : TEXT("derived"),
		double(Snapshot.DemotedVideoMemory) / (1024.0 * 1024.0),
		double(Snapshot.AvailablePhysicalMemory) / (1024.0 * 1024.0)));

	TestTrue(TEXT("Available VRAM is not negative"), Snapshot.AvailableVideoMemory >= 0);
	TestTrue(TEXT("Available VRAM does not exceed the total"),
		Snapshot.TotalVideoMemory <= 0 || Snapshot.AvailableVideoMemory <= Snapshot.TotalVideoMemory);

	CSGpuMemoryBudget::FTriangleSoupCostModel Boolean;
	Boolean.CutSegmentsPerTriangle = 4;
	Boolean.OutputTrianglesPerSource = 8;
	Boolean.bBuildLBVH = true;
	Boolean.bBuildWindingField = true;
	const int64 MaxTriangles = CSGpuMemoryBudget::EstimateMaxSourceTriangles(Boolean, Snapshot, 0.7f);
	AddInfo(FString::Printf(TEXT("Mesh Boolean ceiling on this device: %lld source triangles (%lld B/triangle)"),
		MaxTriangles, Boolean.BytesPerSourceTriangle()));

	if (Snapshot.AvailableVideoMemory > 0) TestTrue(TEXT("A positive budget yields a positive limit"), MaxTriangles > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
