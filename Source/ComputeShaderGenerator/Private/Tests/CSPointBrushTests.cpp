#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSPointBrushActor.h"

#include "CSMeshGeneratorDebugComponent.h"

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

// Drives the painted-point path end to end: CPU points -> pooled GPU buffers -> debug compute
// passes -> indirect draw submission, and back out again.
//
// The upload half is worth a test because the debug passes read the three buffers as typed
// Buffer<float4> / Buffer<uint> views; allocating them as structured buffers instead compiles
// fine and only fails when the proxy is built, which is what FlushRenderingCommands forces here.
// The lifetime half is the other half: the actor's pooled refs are the only owners outside the
// RDG pool, so release, rebuild and destroy all have to give them up without asserting against
// an in-flight draw.
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

	UCSMeshGeneratorDebugComponent* Debug = BrushActor->FindComponentByClass<UCSMeshGeneratorDebugComponent>();
	if (!TestNotNull(TEXT("Point debug component"), Debug)) return false;
	TestFalse(TEXT("A fresh actor holds no GPU buffer"), BrushActor->HasPointBuffer());

	// --- committing a stroke: the CPU array is the truth, the GPU buffer is rebuilt from it.
	TestEqual(TEXT("Every painted point was accepted"), BrushActor->AppendBrushPoints(MakeTestBrushPoints(8)), 8);
	TestEqual(TEXT("Painted points are stored on the actor"), BrushActor->GetBrushPointCount(), 8);
	TestTrue(TEXT("Committing built the GPU buffer"), BrushActor->HasPointBuffer());
	TestEqual(TEXT("GPU capacity matches the painted count"), BrushActor->GetPointBuffers().Capacity, 8);

	FlushRenderingCommands();
	TestNotNull(TEXT("Painted points produced a debug proxy"), Debug->SceneProxy);
	TestTrue(TEXT("Debug bounds cover the painted points"), Debug->Bounds.GetBox().IsValid != 0);

	// --- a second stroke appends and re-uploads the whole set.
	BrushActor->AppendBrushPoints(MakeTestBrushPoints(4));
	FlushRenderingCommands();
	TestEqual(TEXT("Second stroke appends to the same point set"), BrushActor->GetBrushPointCount(), 12);
	TestEqual(TEXT("GPU capacity tracks the grown point set"), BrushActor->GetPointBuffers().Capacity, 12);
	TestNotNull(TEXT("Second stroke still has a debug proxy"), Debug->SceneProxy);

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
	TestNull(TEXT("Release drops the debug proxy"), Debug->SceneProxy);

	// --- and it rebuilds from the points that survived.
	TestTrue(TEXT("Rebuild restores the GPU buffer"), BrushActor->RebuildPointBuffer());
	FlushRenderingCommands();
	TestNotNull(TEXT("Rebuild restores the debug proxy"), Debug->SceneProxy);

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
