#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshComponent.h"
#include "ComputeShaderMeshGenerator.h"

#include "Components/BoxComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "MeshDescription.h"
#include "RenderingThread.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshDescriptionSharedVertexAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.DirectGPUMesh.SharedStaticMeshConverter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGpuMeshDescriptionSharedVertexAutomationTest::RunTest(const FString& Parameters)
{
	const FTransform ActorTransform(FQuat::Identity, FVector(120.0, -80.0, 35.0), FVector(-2.0, 3.0, 1.5));
	const FVector LocalPositions[4] =
	{
		FVector(0.0, 0.0, 0.0),
		FVector(10.0, 0.0, 0.0),
		FVector(10.0, 10.0, 0.0),
		FVector(0.0, 10.0, 0.0)
	};

	FCSGpuMeshCPUData MeshData;
	for (const FVector& Position : LocalPositions) MeshData.Positions.Add(FVector3f(ActorTransform.TransformPosition(Position)));
	MeshData.Indices = { 0, 1, 2, 0, 2, 3 };
	MeshData.TexCoords() =
	{
		FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(1.0f, 1.0f),
		FVector2f(0.25f, 0.25f), FVector2f(0.75f, 0.75f), FVector2f(0.0f, 1.0f)
	};
	MeshData.Colors =
	{
		FVector4f(1, 0, 0, 1), FVector4f(0, 1, 0, 1), FVector4f(0, 0, 1, 1),
		FVector4f(1, 1, 0, 1), FVector4f(0, 1, 1, 1), FVector4f(1, 0, 1, 1)
	};
	const FVector3f WorldNormal(ActorTransform.TransformVectorNoScale(FVector::UpVector).GetSafeNormal());
	const FVector3f WorldTangent(ActorTransform.TransformVector(FVector::ForwardVector).GetSafeNormal());
	MeshData.Normals.Init(WorldNormal, MeshData.Indices.Num());
	MeshData.Tangents.Init(WorldTangent, MeshData.Indices.Num());
	MeshData.BinormalSigns.Init(-1.0f, MeshData.Indices.Num());
	MeshData.TriangleMaterialSlots = { 0, 1 };

	FMeshDescription MeshDescription;
	if (!TestTrue(TEXT("Shared converter builds MeshDescription"),
		UCSGpuMeshComponent::BuildGpuMeshDescription(MeshData, ActorTransform, true, MeshDescription)))
	{
		return false;
	}

	TestEqual(TEXT("Shared positions remain shared"), MeshDescription.Vertices().Num(), 4);
	TestEqual(TEXT("Each indexed corner becomes a vertex instance"), MeshDescription.VertexInstances().Num(), 6);
	TestEqual(TEXT("Both triangles are created"), MeshDescription.Triangles().Num(), 2);
	TestEqual(TEXT("Per-triangle material slots create polygon groups"), MeshDescription.PolygonGroups().Num(), 2);

	FStaticMeshConstAttributes Attributes(MeshDescription);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	for (int32 VertexIndex = 0; VertexIndex < UE_ARRAY_COUNT(LocalPositions); ++VertexIndex)
	{
		const FVector3f ActualPosition = Positions[FVertexID(VertexIndex)];
		TestTrue(TEXT("Negative-scale world positions return to actor-local space"),
			FVector(ActualPosition).Equals(LocalPositions[VertexIndex], 0.001));
	}

	TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesConstRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	for (int32 Corner = 0; Corner < MeshData.Indices.Num(); ++Corner)
	{
		const FVertexInstanceID VertexInstanceID(Corner);
		TestTrue(TEXT("Per-corner UV survives shared-position conversion"),
			UVs.Get(VertexInstanceID, 0).Equals(MeshData.TexCoords()[Corner], UE_SMALL_NUMBER));
		TestTrue(TEXT("Per-corner color survives shared-position conversion"),
			Colors[VertexInstanceID].Equals(MeshData.Colors[Corner], UE_SMALL_NUMBER));
		TestEqual(TEXT("Negative scale restores local tangent handedness"), BinormalSigns[VertexInstanceID], 1.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSDirectMeshSaveGPUAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.DirectGPUMesh.SaveStaticMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSDirectMeshSaveGPUAutomationTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;
	const FStaticMeshRenderData* CubeRenderData = CubeMesh->GetRenderData();
	if (!TestNotNull(TEXT("Engine cube render data"), CubeRenderData)) return false;
	if (!TestTrue(TEXT("Engine cube has LOD0 render resources"), CubeRenderData->LODResources.Num() > 0)) return false;
	const int32 ExpectedTriangleCount = CubeRenderData->LODResources[0].GetNumTriangles();
	if (!TestTrue(TEXT("Engine cube has render triangles"), ExpectedTriangleCount > 0)) return false;
	const int32 ExpectedVertexCount = ExpectedTriangleCount * 3;

	const FTransform TestTransform(FRotator(0.0, 27.0, 0.0), FVector(100000.0, -100000.0, 50000.0));
	AStaticMeshActor* SourceActor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("Source StaticMesh actor"), SourceActor)) return false;
	SourceActor->SetActorTransform(TestTransform);
	SourceActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);

	AComputeShaderMeshGenerator* Generator = World->SpawnActor<AComputeShaderMeshGenerator>();
	if (!TestNotNull(TEXT("Compute shader mesh generator"), Generator)) return false;
	Generator->SetActorTransform(TestTransform);
	Generator->GeneratorBounds->SetBoxExtent(FVector(75.0));
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	if (!TestTrue(TEXT("Submit cube triangles to direct GPU renderer"),
		Generator->SubmitBoxSceneTrianglesToRenderPipeline(nullptr, 64, 0.0f)))
	{
		return false;
	}

	const FCSTriangleMeshData DiagnosticTriangleData = Generator->GetBoxSceneTrianglesFromGPUFiltered(0.0f);
	TestEqual(TEXT("Existing GPU triangle extraction sees the cube"), DiagnosticTriangleData.VertexCount, ExpectedVertexCount);

	const FString TestAssetPath = FString::Printf(
		TEXT("/Game/Automation/CSDirectMesh/SM_GPUReadback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UStaticMesh* SavedMesh = Generator->SaveDirectGPUMeshToStaticMesh(TestAssetPath, true, false, true);
	if (!TestNotNull(TEXT("StaticMesh created from GPU buffers"), SavedMesh)) return false;

	const FMeshDescription* MeshDescription = SavedMesh->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Saved mesh LOD0 MeshDescription"), MeshDescription)) return false;
	TestEqual(TEXT("Cube triangle count survives GPU save"), MeshDescription->Triangles().Num(), ExpectedTriangleCount);
	TestEqual(TEXT("Direct triangle soup vertex count"), MeshDescription->Vertices().Num(), ExpectedVertexCount);

	FStaticMeshConstAttributes Attributes(*MeshDescription);
	TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	FBox LocalBounds(ForceInit);
	for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
		LocalBounds += FVector(Positions[VertexID]);
	TestTrue(TEXT("Saved positions were converted back to generator-local space"),
		LocalBounds.IsValid && LocalBounds.GetCenter().IsNearlyZero(0.1) && LocalBounds.GetExtent().Equals(FVector(50.0), 0.1));

	const bool bDeletedTestAsset = UEditorAssetLibrary::DeleteAsset(TestAssetPath);
	TestTrue(TEXT("Temporary StaticMesh asset cleaned up"), bDeletedTestAsset);
	return true;
}

#endif
