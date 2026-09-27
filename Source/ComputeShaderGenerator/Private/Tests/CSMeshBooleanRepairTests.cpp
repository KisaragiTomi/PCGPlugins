#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "ComputeShaderMeshBoolean.h"

#include "Components/BoxComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// GPU weld + repair of a Union (MeshBooleanRepair), on the two scenes that motivated it.
//
// A is a 100 cm box with its top at z = 0; B is a 23 x 50 x 20 box standing on it.
//  - Flush: B's bottom lies exactly on A's top. Coplanar pairs are never cut, so the A top
//    triangle whose centroid falls inside B's footprint is deleted whole and the part of it
//    outside the footprint is a visible hole. Restoration must put it back, and nothing else:
//    B's bottom meets A's top at a right angle to B's walls, outside the bend limit. The
//    closed result is A's six faces + B's walls + B's top = 64070 cm2, and the only open
//    edges left are B's bottom perimeter, a zero-width contact lying on A's top.
//  - Sunk by 1 cm: a properly cut seam. The analytic union is 62774 cm2 and, once welded,
//    it has no open edge at all.
// Edges are matched by exact position: welded corners are written from one representative
// each, so coincident corners are bit-identical. The sunk case's slits are opened into strips
// of NudgeDistance before they are zipped; on a 90 degree crease the diagonal fill is shorter
// than the two face strips it replaces, so the area may drift by up to 2 x NudgeDistance x the
// crack length (about 1030 cm of open edges before the repair here), i.e. some 20 cm2.
// -----------------------------------------------------------------------------

namespace
{
// Unity builds share a TU, so file-local names carry a per-file prefix.
struct FCSMeshBooleanRepair_Result
{
	double Area = 0.0;
	int32 Triangles = 0;
	TArray<TPair<FVector3f, FVector3f>> OpenEdges;
};

bool CSMeshBooleanRepair_Measure(UCSMesh* Mesh, FCSMeshBooleanRepair_Result& Out)
{
	FCSGpuMeshCPUData Data;
	if (!Mesh || !Mesh->ReadbackMeshSync(Data)) return false;
	TMap<TPair<FVector3f, FVector3f>, int32> Directed;
	for (int32 Corner = 0; Corner + 2 < Data.Indices.Num(); Corner += 3)
	{
		const FVector3f P[3] =
		{
			Data.Positions[Data.Indices[Corner + 0]],
			Data.Positions[Data.Indices[Corner + 1]],
			Data.Positions[Data.Indices[Corner + 2]]
		};
		Out.Area += 0.5 * FVector3d(FVector3d(P[1] - P[0]).Cross(FVector3d(P[2] - P[0]))).Length();
		++Out.Triangles;
		for (int32 K = 0; K < 3; ++K) ++Directed.FindOrAdd(TPair<FVector3f, FVector3f>(P[K], P[(K + 1) % 3]));
	}
	for (const TPair<TPair<FVector3f, FVector3f>, int32>& Edge : Directed)
	{
		if (Directed.Contains(TPair<FVector3f, FVector3f>(Edge.Key.Value, Edge.Key.Key))) continue;
		for (int32 N = 0; N < Edge.Value; ++N) Out.OpenEdges.Add(Edge.Key);
	}
	return true;
}

bool CSMeshBooleanRepair_OnFootprintPerimeter(const FVector3f& P)
{
	constexpr float Tolerance = 0.2f;
	if (FMath::Abs(P.Z) > Tolerance) return false;
	const bool bInsideX = P.X > 55.0f - Tolerance && P.X < 78.0f + Tolerance;
	const bool bInsideY = P.Y > 25.0f - Tolerance && P.Y < 75.0f + Tolerance;
	const bool bOnX = FMath::Abs(P.X - 55.0f) < Tolerance || FMath::Abs(P.X - 78.0f) < Tolerance;
	const bool bOnY = FMath::Abs(P.Y - 25.0f) < Tolerance || FMath::Abs(P.Y - 75.0f) < Tolerance;
	return bInsideX && bInsideY && (bOnX || bOnY);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSMeshBooleanGpuRepairTest,
	"PCGPlugins.ComputeShaderGenerator.MeshBoolean.GpuRepair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSMeshBooleanGpuRepairTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;

	auto SpawnBox = [World, CubeMesh](const FVector& Min, const FVector& Max)
	{
		AStaticMeshActor* Box = World->SpawnActor<AStaticMeshActor>();
		// The engine cube is 100 cm, centred on its pivot.
		Box->SetActorTransform(FTransform(FQuat::Identity, (Min + Max) * 0.5, (Max - Min) / 100.0));
		Box->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		// 布尔的场景收集是白名单制：没挂 Pick 的 actor 不进 soup。
		Box->Tags.Add(TEXT("Pick"));
		return Box;
	};

	AComputeShaderMeshBoolean* Generator = World->SpawnActor<AComputeShaderMeshBoolean>();
	if (!TestNotNull(TEXT("Mesh boolean generator"), Generator)) return false;
	Generator->SetActorTransform(FTransform(FVector(50.0, 50.0, -40.0)));
	Generator->GeneratorBounds->SetBoxExtent(FVector(150.0));
	Generator->MaxTriangles = 4096;
	Generator->bReadLandscape = false;
	Generator->VertexWeldDistance = 0.1f;

	// ExpectedArea < 0 means "less than the closed surface by a whole triangle": without
	// restoration the A top triangle under B's centroid test stays deleted.
	struct FCase { const TCHAR* Name; double BottomZ; bool bRestore; double ExpectedArea; double AreaTolerance; bool bOnlyFootprintOpen; };
	const FCase Cases[] = {
		{ TEXT("Flush"), 0.0, true, 64070.0, 1.0, true },
		{ TEXT("FlushNoRestore"), 0.0, false, -64070.0, 0.0, false },
		{ TEXT("Sunk1cm"), -1.0, true, 62774.0, 25.0, false },
	};

	for (const FCase& Case : Cases)
	{
		AStaticMeshActor* A = SpawnBox(FVector(0.0, 0.0, -100.0), FVector(100.0, 100.0, 0.0));
		AStaticMeshActor* B = SpawnBox(FVector(55.0, 25.0, Case.BottomZ), FVector(78.0, 75.0, Case.BottomZ + 20.0));
		Generator->bRestoreHoleFragments = Case.bRestore;
		World->UpdateWorldComponents(true, false);
		FlushRenderingCommands();

		UCSMesh* Mesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		FCSMeshBooleanRepair_Result Result;
		const bool bRan = Mesh && Generator->RunBooleanToGpuMesh(ECSMeshBooleanOp::Union, Generator->MakeBooleanOptions(), Mesh);
		if (TestTrue(*FString::Printf(TEXT("[%s] welded GPU Boolean produced a mesh"), Case.Name), bRan)
			&& TestTrue(*FString::Printf(TEXT("[%s] readback"), Case.Name), CSMeshBooleanRepair_Measure(Mesh, Result)))
		{
			if (Case.ExpectedArea < 0.0)
			{
				TestTrue(*FString::Printf(TEXT("[%s] area %.2f shows the hole (closed surface %.0f)"), Case.Name, Result.Area, -Case.ExpectedArea),
					Result.Area < -Case.ExpectedArea - 500.0);
			}
			else
			{
				TestTrue(*FString::Printf(TEXT("[%s] area %.2f matches the analytic surface %.0f"), Case.Name, Result.Area, Case.ExpectedArea),
					FMath::Abs(Result.Area - Case.ExpectedArea) < Case.AreaTolerance);
			}
			if (Case.bOnlyFootprintOpen)
			{
				int32 Stray = 0;
				for (const TPair<FVector3f, FVector3f>& Edge : Result.OpenEdges)
				{
					if (!CSMeshBooleanRepair_OnFootprintPerimeter(Edge.Key) || !CSMeshBooleanRepair_OnFootprintPerimeter(Edge.Value)) ++Stray;
				}
				TestEqual(*FString::Printf(TEXT("[%s] open edges off B's contact perimeter (of %d open)"), Case.Name, Result.OpenEdges.Num()), Stray, 0);
			}
			else if (Case.bRestore)
			{
				TestEqual(*FString::Printf(TEXT("[%s] open edges"), Case.Name), Result.OpenEdges.Num(), 0);
			}
		}
		A->Destroy();
		B->Destroy();
	}

	Generator->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
