#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSPointBrushActor.h"

#include "CSMesh.h"
#include "CSMeshRenderComponent.h"

#include "Engine/World.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSPointBrushBufferAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.PointBrush.BufferLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
TArray<FCSBrushPoint> MakeTestBrushPoints(int32 Count)
{
	TArray<FCSBrushPoint> Points;
	Points.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FCSBrushPoint& Point = Points.AddDefaulted_GetRef();
		Point.Position = FVector(double(Index) * 25.0, 0.0, 0.0);
		Point.Normal = FVector::UpVector;
	}
	return Points;
}
}

// Drives the painted-point path end to end: CPU points -> pooled GPU buffers -> the arrow compute
// passes -> a retained UCSMesh -> indirect draw submission, and back out again.
//
// The upload half is worth a test because the arrow passes read the three buffers as typed
// Buffer<float4> / Buffer<uint> views; allocating them as structured buffers instead compiles fine
// and only fails when those passes actually run, which the build's own EditMeshSync flush forces
// during AppendBrushPoints. The lifetime half is the other half, and retention made it wider: the
// actor's pooled refs are the only owners of the source outside the RDG pool, and the arrow mesh now
// holds a second allocation of its own, so release, rebuild and destroy have to give up both without
// asserting against an in-flight draw.
bool FCSPointBrushBufferAutomationTest::RunTest(const FString& Parameters)
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return true;

	// The point-cap section below deliberately overfills the actor twice.
	AddExpectedMessagePlain(TEXT("point cap"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSPointBrushActor* BrushActor = World->SpawnActor<ACSPointBrushActor>();
	if (!TestNotNull(TEXT("Point brush actor"), BrushActor)) return false;
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	UCSMeshRenderComponent* Arrows = BrushActor->FindComponentByClass<UCSMeshRenderComponent>();
	if (!TestNotNull(TEXT("Point arrow render component"), Arrows)) return false;
	TestFalse(TEXT("A fresh actor holds no GPU buffer"), BrushActor->HasPointBuffer());
	TestNull(TEXT("A fresh actor has no arrow mesh"), BrushActor->GetPointArrowMesh());

	// --- adding points through the CPU API: the array is seeded into a cap-sized GPU buffer, whose
	// tail is the room the depth brush's compute pass appends into.
	TestEqual(TEXT("Every painted point was accepted"), BrushActor->AppendBrushPoints(MakeTestBrushPoints(8)), 8);
	TestEqual(TEXT("Painted points are stored on the actor"), BrushActor->GetBrushPointCount(), 8);
	TestTrue(TEXT("Committing built the GPU buffer"), BrushActor->HasPointBuffer());
	TestEqual(TEXT("GPU capacity is the point cap, not the painted count"),
		BrushActor->GetPointBuffers().Capacity, BrushActor->MaxPointCount);

	FlushRenderingCommands();
	TestNotNull(TEXT("Painted points produced an arrow proxy"), Arrows->SceneProxy);
	TestTrue(TEXT("Arrow bounds cover the painted points"), Arrows->Bounds.GetBox().IsValid != 0);

	// The arrows live in a mesh object now, and the component draws that object rather than owning
	// the geometry — so the binding, not just the proxy, is what the display depends on.
	UCSMesh* ArrowMesh = BrushActor->GetPointArrowMesh();
	if (!TestNotNull(TEXT("Painting built an arrow mesh"), ArrowMesh)) return false;
	TestTrue(TEXT("The arrow mesh is what the component draws"), Arrows->GetGpuMesh() == ArrowMesh);
	TestTrue(TEXT("The arrow mesh has GPU capacity"), ArrowMesh->GetVertexCapacity() > 0);

	// --- a second stroke appends and re-uploads the whole set.
	BrushActor->AppendBrushPoints(MakeTestBrushPoints(4));
	FlushRenderingCommands();
	TestEqual(TEXT("Second stroke appends to the same point set"), BrushActor->GetBrushPointCount(), 12);
	TestNotNull(TEXT("Second stroke still has an arrow proxy"), Arrows->SceneProxy);

	// --- the point cap is a hard limit, not a suggestion.
	BrushActor->MaxPointCount = 13;
	TestEqual(TEXT("Only the points that fit under the cap are added"),
		BrushActor->AppendBrushPoints(MakeTestBrushPoints(5)), 1);
	TestEqual(TEXT("Nothing is added once the cap is reached"),
		BrushActor->AppendBrushPoints(MakeTestBrushPoints(5)), 0);
	TestEqual(TEXT("The cap bounds the stored point set"), BrushActor->GetBrushPointCount(), 13);

	// --- explicit release drops the GPU side and leaves the CPU side alone.
	BrushActor->ReleasePointBuffer();
	FlushRenderingCommands();
	TestFalse(TEXT("Release drops the GPU buffer"), BrushActor->HasPointBuffer());
	TestEqual(TEXT("Release keeps the painted points"), BrushActor->GetBrushPointCount(), 13);
	TestNull(TEXT("Release drops the arrow proxy"), Arrows->SceneProxy);
	// Retention makes this a separate claim from the proxy: unbinding alone would leave the arrow
	// allocation held for the rest of the session with nothing drawing it.
	TestTrue(TEXT("Release hands the arrow mesh's VRAM back"), ArrowMesh->IsEmpty());

	// --- and it rebuilds from the points that survived.
	TestTrue(TEXT("Rebuild restores the GPU buffer"), BrushActor->RebuildPointBuffer());
	FlushRenderingCommands();
	TestNotNull(TEXT("Rebuild restores the arrow proxy"), Arrows->SceneProxy);
	TestTrue(TEXT("Rebuild reuses the same arrow mesh object"), BrushActor->GetPointArrowMesh() == ArrowMesh);

	// --- clearing wipes both sides.
	BrushActor->ClearBrushPoints();
	FlushRenderingCommands();
	TestEqual(TEXT("Clear empties the painted points"), BrushActor->GetBrushPointCount(), 0);
	TestFalse(TEXT("Clear drops the GPU buffer"), BrushActor->HasPointBuffer());

	// --- destroying an actor that still owns a buffer is the remaining release path.
	ACSPointBrushActor* DestroyedActor = World->SpawnActor<ACSPointBrushActor>();
	if (!TestNotNull(TEXT("Second point brush actor"), DestroyedActor)) return false;
	DestroyedActor->AppendBrushPoints(MakeTestBrushPoints(3));
	FlushRenderingCommands();
	TestTrue(TEXT("Second actor owns a GPU buffer before destruction"), DestroyedActor->HasPointBuffer());
	World->DestroyActor(DestroyedActor);
	FlushRenderingCommands();
	TestFalse(TEXT("Destroying the actor releases its GPU buffer"), DestroyedActor->HasPointBuffer());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
