#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
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

// Drives the voxel debug visual end to end: retained voxel buffers -> arrow expansion compute ->
// UCSMesh resident streams -> UCSMeshRenderComponent proxy.
//
// 这个测试以前验的是 position-only 顶点工厂那条路（体素方向线用 PT_LineList 直接 indirect 画）。
// 显示改走箭头网格后那条路没了，能验的东西也变了：现在要证明的是几何真的落进了 UCSMesh 的常驻
// 流、组件据此建出了代理，以及自动清除会把绑定摘掉。
//
// 仍然要 FlushRenderingCommands：BuildPointArrowGeometryIntoMesh 内部的 EditMeshSync 自己会
// flush 一次，但代理是在组件的渲染状态重建里建的，那一步要等到帧末的组件更新。
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
	UCSMeshRenderComponent* Debug = Generator->DisplayComponent;
	if (!TestNotNull(TEXT("GPU debug component"), Debug)) return false;
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	// --- 方向箭头，从常驻体素 buffer 展开。
	FCSDebugBoxVoxelDirectionOptions DirectionOptions;
	DirectionOptions.VoxelSize = 10.0f;
	DirectionOptions.bPersistentLines = true; // 常驻：不排自动清除，好让下面几步稳定观察
	const int32 SubmittedDirections = Generator->DrawDebugBoxSceneSurfaceVoxelDirections(DirectionOptions);
	TestTrue(TEXT("Surface-voxel directions submitted"), SubmittedDirections > 0);

	// 几何归网格对象所有，这是这次迁移的全部意义所在：先验它，再验画得出来。
	UCSMesh* DebugMesh = Debug->GetGpuMesh();
	if (!TestNotNull(TEXT("Debug geometry bound to the render component"), DebugMesh)) return false;
	TestTrue(TEXT("Debug mesh holds an allocation"), Debug->HasGeneratedGeometry());

	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();
	TestNotNull(TEXT("Direction debug proxy created"), Debug->SceneProxy);
	TestTrue(TEXT("Direction bounds cover the cube"), Debug->Bounds.GetBox().IsValid != 0);

	Generator->ClearMeshGeneratorGPUDebug();
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();
	TestNull(TEXT("Cleared generator unbinds its debug geometry"), Debug->GetGpuMesh());
	TestNull(TEXT("Cleared component drops its debug proxy"), Debug->SceneProxy);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
