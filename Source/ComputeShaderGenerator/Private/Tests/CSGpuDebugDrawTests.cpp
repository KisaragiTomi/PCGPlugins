#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSMeshGeneratorDebugComponent.h"
#include "ComputeShaderDebugParams.h"
#include "ComputeShaderMeshGenerator.h"

#include "Components/BoxComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuDebugDrawSubmitAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuDebugDraw.SubmitVoxelDebug",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Drives both GPU debug visuals end to end: retained voxel buffers -> debug compute passes ->
// position stream + vertex factory -> indirect draw submission.
//
// The vertex-factory half is the part worth a test. A position-only FLocalVertexFactory still
// has to hand FLocalVertexFactoryUniformShaderParameters a positions SRV (and non-null tangent /
// texcoord / colour dummies) because manual vertex fetch reads positions from the SRV, not from
// the vertex stream; getting that wrong trips a uniform-buffer resource assertion the moment the
// proxy is created, which is exactly what FlushRenderingCommands below forces.
bool FCSGpuDebugDrawSubmitAutomationTest::RunTest(const FString& Parameters)
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return true;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;

	AStaticMeshActor* SourceActor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("Source StaticMesh actor"), SourceActor)) return false;
	SourceActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);

	AComputeShaderMeshGenerator* Generator = World->SpawnActor<AComputeShaderMeshGenerator>();
	if (!TestNotNull(TEXT("Compute shader mesh generator"), Generator)) return false;
	Generator->GeneratorBounds->SetBoxExtent(FVector(75.0));
	UCSMeshGeneratorDebugComponent* Debug = Generator->MeshGeneratorDebugComponent;
	if (!TestNotNull(TEXT("GPU debug component"), Debug)) return false;
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	// --- direction lines + centre points, drawn from the retained voxel buffers.
	FCSDebugBoxVoxelDirectionOptions DirectionOptions;
	DirectionOptions.VoxelSize = 10.0f;
	DirectionOptions.bDrawPoints = true;
	DirectionOptions.bPersistentLines = true;
	const int32 SubmittedDirections = Generator->DrawDebugBoxSceneSurfaceVoxelDirections(DirectionOptions);
	TestTrue(TEXT("Surface-voxel directions submitted"), SubmittedDirections > 0);
	FlushRenderingCommands();
	TestNotNull(TEXT("Direction debug proxy created"), Debug->SceneProxy);
	TestTrue(TEXT("Direction bounds cover the cube"), Debug->Bounds.GetBox().IsValid != 0);

	// --- isolated quads: same buffers, triangle geometry instead of lines.
	TestTrue(TEXT("Isolated quads submitted"), Generator->SurfaceVoxelsToIsolatedQuadsDebug(10.0f, false));
	FlushRenderingCommands();
	TestNotNull(TEXT("Isolated quad debug proxy created"), Debug->SceneProxy);

	Generator->ClearMeshGeneratorGPUDebug();
	FlushRenderingCommands();
	TestNull(TEXT("Cleared component drops its debug proxy"), Debug->SceneProxy);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
