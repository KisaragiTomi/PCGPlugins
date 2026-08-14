#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshBuild.h"
#include "CSMeshOps.h"
#include "CSMeshPool.h"
#include "CSMeshRenderComponent.h"
#include "ComputeShaderMeshGenerator.h"

#include "Components/BoxComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "Tests/AutomationEditorCommon.h"
#include "UDynamicMesh.h"

#include <limits>

// -----------------------------------------------------------------------------
// Stream-descriptor contract (no RHI needed)
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshStreamContractTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.StreamContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGpuMeshStreamContractTest::RunTest(const FString& Parameters)
{
	// The access states are what keep a mesh drawable after an operator has written it.
	// RDG's default epilogue (SRVMask) is illegal for index / indirect usage, so these three
	// must never silently become the default.
	TestEqual(TEXT("Index streams stay index-readable"),
		CSGpuMeshStreams::FinalAccessForRole(ECSGpuStreamRole::Index), ERHIAccess::VertexOrIndexBuffer);
	TestEqual(TEXT("Indirect args stay indirect-readable"),
		CSGpuMeshStreams::FinalAccessForRole(ECSGpuStreamRole::IndirectArgs), ERHIAccess::IndirectArgs);
	TestEqual(TEXT("Counters stay copyable for readback"),
		CSGpuMeshStreams::FinalAccessForRole(ECSGpuStreamRole::MeshCounters), ERHIAccess::CopySrc);
	TestEqual(TEXT("Vertex streams are drawn from and manually fetched"),
		CSGpuMeshStreams::FinalAccessForRole(ECSGpuStreamRole::Position),
		ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);

	// PerTriangle used to be declared but never resolved, so a per-face stream would have
	// been sized as if it were per-vertex.
	TestEqual(TEXT("PerTriangle resolves to index/3"),
		CSGpuMeshStreams::UnitsForCountSource(ECSGpuCountSource::PerTriangle, 100, 300), 100u);
	TestEqual(TEXT("PerVertex resolves to the vertex count"),
		CSGpuMeshStreams::UnitsForCountSource(ECSGpuCountSource::PerVertex, 100, 300), 100u);
	TestEqual(TEXT("PerIndex resolves to the index count"),
		CSGpuMeshStreams::UnitsForCountSource(ECSGpuCountSource::PerIndex, 100, 300), 300u);
	TestEqual(TEXT("Fixed ignores both counts"),
		CSGpuMeshStreams::UnitsForCountSource(ECSGpuCountSource::Fixed, 100, 300), 1u);

	// The proxy-owned leaves must not gain a material-id readback stream they never fill.
	TArray<FCSGpuStreamDesc> ProxyDescs;
	CSGpuMeshStreams::BuildStandardTriangleStreamDescs(ProxyDescs);
	TestEqual(TEXT("Proxy standard set keeps its seven streams"), ProxyDescs.Num(), 7);
	for (const FCSGpuStreamDesc& Desc : ProxyDescs)
		TestTrue(TEXT("Proxy set carries no material-id semantic"), Desc.CpuSemantic != ECSGpuMeshSemantic::MaterialId);

	CSGpuMeshStreams::FStandardStreamOptions ResidentOptions;
	ResidentOptions.bMaterialIds = true;
	ResidentOptions.bReadbackColors = true;
	TArray<FCSGpuStreamDesc> ResidentDescs;
	CSGpuMeshStreams::BuildStandardTriangleStreamDescs(ResidentDescs, ResidentOptions);
	TestEqual(TEXT("Resident set adds the material-id stream"), ResidentDescs.Num(), 8);

	bool bFoundMaterialId = false;
	bool bColorReadback = false;
	for (const FCSGpuStreamDesc& Desc : ResidentDescs)
	{
		if (Desc.CpuSemantic == ECSGpuMeshSemantic::MaterialId)
		{
			bFoundMaterialId = true;
			TestEqual(TEXT("Material ids are per triangle"), Desc.CountSource, ECSGpuCountSource::PerTriangle);
		}
		if (Desc.Role == ECSGpuStreamRole::Color) bColorReadback = Desc.bReadback;
	}
	TestTrue(TEXT("Resident set exposes per-triangle material ids"), bFoundMaterialId);
	TestTrue(TEXT("Resident set reads vertex colours back"), bColorReadback);
	return true;
}

// -----------------------------------------------------------------------------
// StaticMesh -> UCSMesh -> readback -> StaticMesh round trip
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshObjectRoundTripTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.StaticMeshRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshObjectRoundTripTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;
	const FStaticMeshRenderData* RenderData = CubeMesh->GetRenderData();
	if (!TestNotNull(TEXT("Engine cube render data"), RenderData)) return false;
	if (!TestTrue(TEXT("Engine cube has LOD0"), RenderData->LODResources.Num() > 0)) return false;

	const int32 SourceTriangles = RenderData->LODResources[0].GetNumTriangles();
	const int32 SourceVertices = RenderData->LODResources[0].GetNumVertices();
	if (!TestTrue(TEXT("Engine cube has geometry"), SourceTriangles > 0 && SourceVertices > 0)) return false;

	// A transform with rotation and translation, so a mistake in the world-space contract
	// (the resident set is world space) shows up as displaced geometry rather than passing.
	const FTransform SourceTransform(FRotator(0.0, 30.0, 0.0), FVector(1200.0, -800.0, 350.0));

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;
	TestTrue(TEXT("A freshly allocated mesh is empty"), GpuMesh->IsEmpty());

	FCSMeshFromStaticMeshOptions FromOptions;
	FromOptions.Transform = SourceTransform;
	UCSMeshOps::CopyFromStaticMesh(GpuMesh, CubeMesh, FromOptions);

	TestEqual(TEXT("Upload grew the vertex capacity"), GpuMesh->GetVertexCapacity() >= SourceVertices, true);
	TestFalse(TEXT("An uploaded mesh is no longer empty"), GpuMesh->IsEmpty());
	TestEqual(TEXT("Triangle count survives the upload"), GpuMesh->GetTriangleCountSync(), SourceTriangles);
	TestEqual(TEXT("Material slots came across"), GpuMesh->Materials.Num(), CubeMesh->GetStaticMaterials().Num());

	// --- readback parity
	FCSGpuMeshCPUData Snapshot;
	if (!TestTrue(TEXT("Readback needs no component and no scene proxy"), GpuMesh->ReadbackMeshSync(Snapshot))) return false;
	TestEqual(TEXT("Readback vertex count"), Snapshot.Positions.Num(), SourceVertices);
	TestEqual(TEXT("Readback index count"), Snapshot.Indices.Num(), SourceTriangles * 3);
	TestEqual(TEXT("Per-triangle material slots read back"), Snapshot.TriangleMaterialSlots.Num(), SourceTriangles);
	TestEqual(TEXT("Vertex colours read back"), Snapshot.Colors.Num(), SourceVertices);

	FBox ReadBounds(ForceInit);
	for (const FVector3f& Position : Snapshot.Positions) ReadBounds += FVector(Position);
	const FBox ExpectedBounds = CubeMesh->GetBoundingBox().TransformBy(SourceTransform);
	TestTrue(TEXT("Readback positions are in world space"),
		ReadBounds.IsValid && ReadBounds.GetCenter().Equals(ExpectedBounds.GetCenter(), 1.0)
		&& ReadBounds.GetExtent().Equals(ExpectedBounds.GetExtent(), 1.0));
	TestTrue(TEXT("Approximate bounds track the geometry"),
		GpuMesh->GetWorldBoundsApprox().IsValid
		&& GpuMesh->GetWorldBoundsApprox().GetCenter().Equals(ExpectedBounds.GetCenter(), 1.0));

	// --- sink: back to a StaticMesh asset, baked into the source transform's local space
	const FString TestAssetPath = FString::Printf(
		TEXT("/Game/Automation/CSGpuMeshObject/SM_RoundTrip_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	FCSMeshToStaticMeshOptions ToOptions;
	ToOptions.AssetPath = TestAssetPath;
	ToOptions.bTransient = false;
	ToOptions.bSaveToDisk = false;
	ToOptions.TargetTransform = SourceTransform;
	ToOptions.bBakeToLocalSpace = true;
	UStaticMesh* Result = UCSMeshOps::CopyToStaticMesh(GpuMesh, World, nullptr, ToOptions);
	if (!TestNotNull(TEXT("StaticMesh rebuilt from the GPU mesh"), Result)) return false;

	const FMeshDescription* ResultDescription = Result->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Result LOD0 MeshDescription"), ResultDescription)) return false;
	TestEqual(TEXT("Triangle count survives the round trip"), ResultDescription->Triangles().Num(), SourceTriangles);
	TestEqual(TEXT("Shared vertices survive the round trip"), ResultDescription->Vertices().Num(), SourceVertices);

	FStaticMeshConstAttributes ResultAttributes(*ResultDescription);
	TVertexAttributesConstRef<FVector3f> ResultPositions = ResultAttributes.GetVertexPositions();
	FBox ResultBounds(ForceInit);
	for (const FVertexID VertexID : ResultDescription->Vertices().GetElementIDs())
		ResultBounds += FVector(ResultPositions[VertexID]);
	const FBox SourceLocalBounds = CubeMesh->GetBoundingBox();
	TestTrue(TEXT("Round-tripped positions return to the source local space"),
		ResultBounds.IsValid
		&& ResultBounds.GetCenter().Equals(SourceLocalBounds.GetCenter(), 0.5)
		&& ResultBounds.GetExtent().Equals(SourceLocalBounds.GetExtent(), 0.5));

	TestTrue(TEXT("Round-trip asset cleaned up"), UEditorAssetLibrary::DeleteAsset(TestAssetPath));
	return true;
}

// -----------------------------------------------------------------------------
// Operators: chaining, reset, and the DynamicMesh sink
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshObjectOperatorTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.Operators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshObjectOperatorTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;
	const FStaticMeshRenderData* RenderData = CubeMesh->GetRenderData();
	if (!TestNotNull(TEXT("Engine cube render data"), RenderData)) return false;
	const int32 SourceTriangles = RenderData->LODResources[0].GetNumTriangles();

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;

	UCSMeshOps::CopyFromStaticMesh(GpuMesh, CubeMesh, FCSMeshFromStaticMeshOptions());
	const uint32 GenerationAfterUpload = GpuMesh->GetGeneration();

	// Chained operators stay entirely on the GPU: no readback happens between these calls.
	const FVector Offset(500.0, 0.0, 0.0);
	UCSMeshOps::SetVertexColors(UCSMeshOps::TranslateMesh(GpuMesh, Offset), FLinearColor::Red);
	TestTrue(TEXT("Each operator bumps the generation"), GpuMesh->GetGeneration() > GenerationAfterUpload);

	FCSGpuMeshCPUData Snapshot;
	if (!TestTrue(TEXT("Readback after the operator chain"), GpuMesh->ReadbackMeshSync(Snapshot))) return false;

	FBox MovedBounds(ForceInit);
	for (const FVector3f& Position : Snapshot.Positions) MovedBounds += FVector(Position);
	const FBox ExpectedBounds = CubeMesh->GetBoundingBox().ShiftBy(Offset);
	TestTrue(TEXT("TranslateMesh moved the geometry on the GPU"),
		MovedBounds.IsValid && MovedBounds.GetCenter().Equals(ExpectedBounds.GetCenter(), 1.0));
	TestTrue(TEXT("SetVertexColors reached every vertex"),
		Snapshot.Colors.Num() == Snapshot.Positions.Num()
		&& Snapshot.Colors.Num() > 0
		&& Snapshot.Colors[0].X > 0.9f && Snapshot.Colors[0].Y < 0.1f && Snapshot.Colors[0].Z < 0.1f);

	// --- DynamicMesh sink (the previous plan's "sink B")
	UDynamicMesh* DynamicMesh = UCSMeshOps::CopyToDynamicMesh(GpuMesh, nullptr, World, FCSMeshToDynamicMeshOptions());
	if (!TestNotNull(TEXT("DynamicMesh sink produced a mesh"), DynamicMesh)) return false;
	DynamicMesh->ProcessMesh([this, SourceTriangles](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		TestEqual(TEXT("DynamicMesh keeps every triangle"), Mesh.TriangleCount(), SourceTriangles);
		TestTrue(TEXT("DynamicMesh carries attributes"), Mesh.HasAttributes());
		if (Mesh.HasAttributes())
		{
			TestTrue(TEXT("DynamicMesh carries per-triangle material ids"), Mesh.Attributes()->HasMaterialID());
			TestTrue(TEXT("DynamicMesh carries vertex colours"), Mesh.Attributes()->HasPrimaryColors());
		}
	});

	// --- Reset keeps the allocation but empties the mesh
	const int32 CapacityBeforeReset = GpuMesh->GetVertexCapacity();
	GpuMesh->Reset();
	TestEqual(TEXT("Reset keeps the allocation"), GpuMesh->GetVertexCapacity(), CapacityBeforeReset);
	TestTrue(TEXT("Reset empties the mesh"), GpuMesh->IsEmpty());
	TestEqual(TEXT("Reset zeroes the GPU triangle count"), GpuMesh->GetTriangleCountSync(), 0);

	// --- Release frees the GPU memory outright
	GpuMesh->ReleaseSync();
	TestEqual(TEXT("Release drops the capacity"), GpuMesh->GetVertexCapacity(), 0);
	return true;
}

// -----------------------------------------------------------------------------
// Weld: co-located corners collapse onto shared vertices, entirely on the GPU
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshWeldTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.Weld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshWeldTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// Two triangles sharing an edge, but stored as an unwelded soup: six corners, six
	// vertices, two coincident pairs. Welding must leave four distinct positions in use.
	FCSGpuMeshCPUData Soup;
	const FVector3f Quad[4] = {
		FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(100, 100, 0), FVector3f(0, 100, 0) };
	const int32 Corners[6] = { 0, 1, 2, 0, 2, 3 };
	for (int32 Corner = 0; Corner < 6; ++Corner)
	{
		Soup.Positions.Add(Quad[Corners[Corner]]);
		Soup.Normals.Add(FVector3f::UnitZ());
		Soup.Tangents.Add(FVector3f::UnitX());
		Soup.TexCoords().Add(FVector2f::ZeroVector);
		Soup.Indices.Add(uint32(Corner));
	}
	Soup.TriangleMaterialSlots = { 0, 0 };

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

	FCSGpuMeshCPUData BeforeWeld;
	if (!TestTrue(TEXT("Readback before weld"), GpuMesh->ReadbackMeshSync(BeforeWeld))) return false;
	TSet<uint32> DistinctBefore;
	for (uint32 Index : BeforeWeld.Indices) DistinctBefore.Add(Index);
	TestEqual(TEXT("The unwelded soup uses six distinct vertices"), DistinctBefore.Num(), 6);

	UCSMeshOps::WeldVertices(GpuMesh, 1.0f);

	FCSGpuMeshCPUData AfterWeld;
	if (!TestTrue(TEXT("Readback after weld"), GpuMesh->ReadbackMeshSync(AfterWeld))) return false;
	TestEqual(TEXT("Welding does not change the index count"), AfterWeld.Indices.Num(), BeforeWeld.Indices.Num());

	TSet<uint32> DistinctAfter;
	for (uint32 Index : AfterWeld.Indices) DistinctAfter.Add(Index);
	TestEqual(TEXT("Coincident corners collapse onto four shared vertices"), DistinctAfter.Num(), 4);

	// Welding must not move geometry: every referenced position still has to be one of the
	// four quad corners.
	bool bPositionsPreserved = true;
	for (uint32 Index : AfterWeld.Indices)
	{
		if (!AfterWeld.Positions.IsValidIndex(int32(Index))) { bPositionsPreserved = false; break; }
		const FVector3f Position = AfterWeld.Positions[int32(Index)];
		bool bMatched = false;
		for (const FVector3f& Corner : Quad) bMatched |= Position.Equals(Corner, 0.01f);
		bPositionsPreserved &= bMatched;
	}
	TestTrue(TEXT("Welding shares vertices without moving them"), bPositionsPreserved);
	return true;
}

// -----------------------------------------------------------------------------
// Pool: reuse keeps the allocation, and the ceiling is VRAM rather than a count
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshPoolTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.Pool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshPoolTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UCSMeshPool* Pool = NewObject<UCSMeshPool>(World);
	if (!TestNotNull(TEXT("Mesh pool"), Pool)) return false;

	UCSMesh* First = Pool->RequestMesh(1024, 1024);
	if (!TestNotNull(TEXT("Pool hands out a mesh"), First)) return false;
	TestEqual(TEXT("Handed-out meshes are tracked"), Pool->GetActiveMeshCount(), 1);
	TestTrue(TEXT("The mesh carries the requested capacity"), First->GetVertexCapacity() >= 1024);
	const int64 FirstBytes = First->GetResidentPtr()->GetAllocatedBytes();
	TestTrue(TEXT("An allocated mesh reports its VRAM"), FirstBytes > 0);

	Pool->ReturnMesh(First);
	TestEqual(TEXT("Returning moves the mesh to the idle set"), Pool->GetActiveMeshCount(), 0);
	TestEqual(TEXT("Idle meshes stay in the pool"), Pool->GetCachedMeshCount(), 1);
	// The GPU pool's whole reason to exist: a returned mesh keeps its allocation, unlike the
	// CPU pool where returning frees the storage.
	TestEqual(TEXT("Returning keeps the allocation"), Pool->GetCachedBytes(), FirstBytes);

	UCSMesh* Second = Pool->RequestMesh(512, 512);
	TestEqual(TEXT("A smaller request reuses the idle allocation"), Second, First);
	TestEqual(TEXT("Reuse hands back an empty mesh"), Second->GetTriangleCountSync(), 0);
	TestEqual(TEXT("Reuse empties the idle set"), Pool->GetCachedMeshCount(), 0);

	UCSMesh* Third = Pool->RequestMesh(1024, 1024);
	TestTrue(TEXT("A second concurrent request gets a different object"), Third != Second);
	TestEqual(TEXT("Both are tracked as active"), Pool->GetActiveMeshCount(), 2);

	Pool->ReturnAllMeshes();
	TestEqual(TEXT("ReturnAllMeshes empties the active set"), Pool->GetActiveMeshCount(), 0);
	TestEqual(TEXT("ReturnAllMeshes fills the idle set"), Pool->GetCachedMeshCount(), 2);

	// The safeguard is VRAM, not object count: a one-byte ceiling must evict everything, and
	// lowering the ceiling has to take effect without waiting for the next return.
	Pool->MaxCachedBytesOverride = 1;
	Pool->EnforceMemoryLimit();
	TestEqual(TEXT("The VRAM ceiling evicts idle meshes"), Pool->GetCachedMeshCount(), 0);

	Pool->MaxCachedBytesOverride = 0;
	Pool->FreeAllMeshes();
	TestEqual(TEXT("FreeAllMeshes leaves nothing cached"), Pool->GetCachedMeshCount(), 0);
	TestEqual(TEXT("FreeAllMeshes releases the VRAM"), Pool->GetTotalBytes(), (int64)0);
	return true;
}

// -----------------------------------------------------------------------------
// Scene extraction: the object path against the proxy path it replaces
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshBoxSceneParityTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.BoxSceneParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshBoxSceneParityTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;
	const int32 CubeTriangles = CubeMesh->GetRenderData()->LODResources[0].GetNumTriangles();

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

	// The generator's own submit entry point. It draws through a UCSMesh now rather than through
	// a proxy that owned its buffers, so this is a smoke check that the entry point still extracts
	// — not the "old half" of the parity comparison it used to be.
	if (!TestTrue(TEXT("The generator's submit entry point extracts the cube"),
		Generator->SubmitBoxSceneTrianglesToRenderPipeline(nullptr, 64, 0.0f)))
	{
		return false;
	}

	// The parity that still means something: the CPU-readback extraction against the GPU-resident
	// one. These are genuinely different code paths — one lands in a CPU triangle array, the other
	// never leaves the GPU — so agreeing on the geometry is a real cross-check.
	const FCSTriangleMeshData LegacyData = Generator->GetBoxSceneTrianglesFromGPUFiltered(0.0f);
	TestEqual(TEXT("CPU-readback extraction sees the cube"), LegacyData.VertexCount, CubeTriangles * 3);

	// The GPU-resident path: the same extraction writes a mesh object, with no component involved.
	// The operator no longer reads the generator, so the inputs the CPU-readback path takes off the
	// actor are restated here — same box, same excluded actor — which is what keeps this a parity
	// comparison rather than two different queries.
	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	FCSMeshBoxSceneOptions SceneOptions;
	SceneOptions.QueryBox = Generator->GetGeneratorBoundsWorldBox();
	SceneOptions.ExcludedActor = Generator;
	SceneOptions.MaxTriangles = 64;
	SceneOptions.bIncludeLandscape = false;
	UCSMeshOps::AppendBoxSceneTriangles(GpuMesh, Generator, SceneOptions);

	TestEqual(TEXT("Object path extracts the same triangle count"), GpuMesh->GetTriangleCountSync(), CubeTriangles);

	FCSGpuMeshCPUData Snapshot;
	if (!TestTrue(TEXT("Object path readback"), GpuMesh->ReadbackMeshSync(Snapshot))) return false;
	TestEqual(TEXT("Soup keeps one vertex per corner"), Snapshot.Positions.Num(), CubeTriangles * 3);

	FBox NewBounds(ForceInit);
	for (const FVector3f& Position : Snapshot.Positions) NewBounds += FVector(Position);
	FBox LegacyBounds(ForceInit);
	for (int32 Index = 0; Index < LegacyData.VertexCount && Index < LegacyData.Vertices.Num(); ++Index)
		LegacyBounds += LegacyData.Vertices[Index];
	TestTrue(TEXT("CPU-readback and GPU-resident extraction agree on the world-space geometry"),
		NewBounds.IsValid && LegacyBounds.IsValid
		&& NewBounds.GetCenter().Equals(LegacyBounds.GetCenter(), 0.5)
		&& NewBounds.GetExtent().Equals(LegacyBounds.GetExtent(), 0.5));

	// The extraction's material registry has to land in the object's material table, since
	// the per-triangle ids index it.
	TestTrue(TEXT("Extraction filled the material table"), GpuMesh->Materials.Num() > 0);

	// Appending again must accumulate rather than overwrite — that is what "Append" promises.
	UCSMeshOps::AppendBoxSceneTriangles(GpuMesh, Generator, SceneOptions);
	TestEqual(TEXT("A second append accumulates"), GpuMesh->GetTriangleCountSync(), CubeTriangles * 2);

	SourceActor->Destroy();
	Generator->Destroy();
	return true;
}

// -----------------------------------------------------------------------------
// Render component: binds the resident set instead of regenerating it
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshRenderComponentTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.RenderComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshRenderComponentTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;
	const int32 SourceTriangles = CubeMesh->GetRenderData()->LODResources[0].GetNumTriangles();

	const FTransform SourceTransform(FQuat::Identity, FVector(400.0, 200.0, 100.0));
	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	FCSMeshFromStaticMeshOptions FromOptions;
	FromOptions.Transform = SourceTransform;
	UCSMeshOps::CopyFromStaticMesh(GpuMesh, CubeMesh, FromOptions);

	AActor* HostActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Host actor"), HostActor)) return false;

	UCSMeshRenderComponent* RenderComponent = NewObject<UCSMeshRenderComponent>(HostActor);
	HostActor->SetRootComponent(RenderComponent);
	RenderComponent->RegisterComponent();
	RenderComponent->SetGpuMesh(GpuMesh);
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	TestEqual(TEXT("Component reports the mesh it was given"), RenderComponent->GetGpuMesh(), GpuMesh);
	const FBoxSphereBounds ComponentBounds = RenderComponent->CalcBounds(RenderComponent->GetComponentTransform());
	TestTrue(TEXT("Component bounds come from the resident set"),
		ComponentBounds.GetBox().GetCenter().Equals(GpuMesh->GetWorldBoundsApprox().GetCenter(), 1.0));

	// Saving through the component must agree with reading the object directly — that is the
	// point of lifting readback off the proxy.
	FCSGpuMeshCPUData ViaObject;
	if (!TestTrue(TEXT("Object-side readback"), GpuMesh->ReadbackMeshSync(ViaObject))) return false;
	FCSGpuMeshCPUData ViaComponent;
	if (!TestTrue(TEXT("Component-side readback"), RenderComponent->ReadbackMeshSync(ViaComponent))) return false;

	TestEqual(TEXT("Both readbacks see the same vertex count"), ViaComponent.Positions.Num(), ViaObject.Positions.Num());
	TestEqual(TEXT("Both readbacks see the same index count"), ViaComponent.Indices.Num(), ViaObject.Indices.Num());
	TestEqual(TEXT("Both readbacks see every triangle"), ViaComponent.Indices.Num() / 3, SourceTriangles);
	bool bPositionsMatch = ViaComponent.Positions.Num() == ViaObject.Positions.Num();
	for (int32 Index = 0; bPositionsMatch && Index < ViaObject.Positions.Num(); ++Index)
		bPositionsMatch = ViaComponent.Positions[Index].Equals(ViaObject.Positions[Index], 0.01f);
	TestTrue(TEXT("Both readbacks see identical positions"), bPositionsMatch);

	// Re-editing the mesh must reach the component without the component regenerating anything.
	const uint32 GenerationBefore = GpuMesh->GetGeneration();
	UCSMeshOps::TranslateMesh(GpuMesh, FVector(100.0, 0.0, 0.0));
	FlushRenderingCommands();
	TestTrue(TEXT("The change event advanced the generation"), GpuMesh->GetGeneration() > GenerationBefore);

	FCSGpuMeshCPUData AfterEdit;
	if (!TestTrue(TEXT("Component readback after an external edit"), RenderComponent->ReadbackMeshSync(AfterEdit))) return false;
	TestTrue(TEXT("The component sees the edited geometry"),
		AfterEdit.Positions.Num() == ViaObject.Positions.Num()
		&& !AfterEdit.Positions[0].Equals(ViaObject.Positions[0], 0.01f));

	HostActor->Destroy();
	return true;
}

// -----------------------------------------------------------------------------
// Helpers for the material-section, world-bounds and per-section-rendering suites.
//
// Unity/jumbo builds share a translation unit, so every file-local name here carries a
// CSGpuMeshTests_ prefix (the same rule CSMesh.cpp / CSMeshOps.cpp follow).
// -----------------------------------------------------------------------------

namespace
{
/**
 * A triangle soup with one unique vertex per corner, one entry of TriangleSlots per triangle.
 * Triangle T is parked at X = T * 1000, so after the sort has permuted the index buffer each
 * triangle is still identifiable by the positions it points at — which is what makes "no triangle
 * was lost or duplicated" checkable at all.
 */
void CSGpuMeshTests_BuildTaggedSoup(const TArray<int32>& TriangleSlots, FCSGpuMeshCPUData& OutSoup)
{
	OutSoup.Reset();
	for (int32 Triangle = 0; Triangle < TriangleSlots.Num(); ++Triangle)
	{
		const float BaseX = float(Triangle) * 1000.0f;
		const FVector3f Corners[3] = {
			FVector3f(BaseX, 0.0f, 0.0f),
			FVector3f(BaseX + 100.0f, 0.0f, 0.0f),
			FVector3f(BaseX, 100.0f, 0.0f) };
		for (const FVector3f& Corner : Corners)
		{
			OutSoup.Indices.Add(uint32(OutSoup.Positions.Num()));
			OutSoup.Positions.Add(Corner);
			OutSoup.Normals.Add(FVector3f::UnitZ());
			OutSoup.Tangents.Add(FVector3f::UnitX());
			OutSoup.TexCoords().Add(FVector2f::ZeroVector);
		}
		OutSoup.TriangleMaterialSlots.Add(TriangleSlots[Triangle]);
	}
}

/** One triangle's three world positions with corner order removed, so a re-wound or re-based
 *  triangle still keys the same. */
FString CSGpuMeshTests_TriangleKey(const FCSGpuMeshCPUData& Data, int32 Triangle)
{
	TArray<FString> Corners;
	Corners.Reserve(3);
	for (int32 Corner = 0; Corner < 3; ++Corner)
	{
		const int32 Slot = Triangle * 3 + Corner;
		const int32 Vertex = Data.Indices.IsValidIndex(Slot) ? int32(Data.Indices[Slot]) : INDEX_NONE;
		if (!Data.Positions.IsValidIndex(Vertex)) { Corners.Add(TEXT("<invalid>")); continue; }
		const FVector3f Position = Data.Positions[Vertex];
		Corners.Add(FString::Printf(TEXT("%.2f/%.2f/%.2f"), Position.X, Position.Y, Position.Z));
	}
	Corners.Sort();
	return FString::Join(Corners, TEXT("|"));
}

/** The mesh's triangles as a sorted multiset of keys. A correct re-order is a permutation, so this
 *  array must come out identical before and after. */
void CSGpuMeshTests_SortedTriangleKeys(const FCSGpuMeshCPUData& Data, TArray<FString>& OutKeys)
{
	OutKeys.Reset();
	const int32 TriangleCount = Data.Indices.Num() / 3;
	for (int32 Triangle = 0; Triangle < TriangleCount; ++Triangle) OutKeys.Add(CSGpuMeshTests_TriangleKey(Data, Triangle));
	OutKeys.Sort();
}

FString CSGpuMeshTests_JoinInts(const TArray<int32>& Values)
{
	TArray<FString> Parts;
	Parts.Reserve(Values.Num());
	for (int32 Value : Values) Parts.Add(FString::FromInt(Value));
	return FString::Join(Parts, TEXT(","));
}

/**
 * Reads a whole resident stream back as raw uints.
 *
 * The mesh readback loop only visits streams that carry a CpuSemantic, so the two kinds of stream
 * a test most wants to look at — the DrawIndexedIndirect args, and an aux stream the targeted
 * resize operates on — have no other way of being observed. Same shape as
 * CSMeshReadback::ReadCountersSync, including the final access state: leaving the args buffer in
 * RDG's SRVMask epilogue is illegal for indirect use and would silently stop the mesh drawing.
 */
bool CSGpuMeshTests_ReadStreamUints(const UCSMesh* Mesh, ECSGpuStreamRole Role, uint8 Slot, TArray<uint32>& OutValues)
{
	OutValues.Reset();
	if (!IsInGameThread()) return false;

	const FCSMeshResident* Resident = Mesh ? Mesh->GetResidentPtr() : nullptr;
	if (!Resident) return false;
	const FCSMeshResident::FStream* Stream = Resident->FindStream(Role, Slot);
	if (!Stream || !Stream->Pooled.IsValid()) return false;

	const uint32 Units = FMath::Max(CSGpuMeshStreams::UnitsForCountSource(
		Stream->Desc.CountSource, FMath::Max(Resident->VertexCapacity, 1u), FMath::Max(Resident->IndexCapacity, 1u)), 1u);
	const uint32 NumUints = Units * Stream->Desc.ElementsPerUnit * Stream->Desc.BytesPerElement / uint32(sizeof(uint32));
	const uint32 Bytes = NumUints * uint32(sizeof(uint32));
	if (NumUints == 0) return false;

	FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("CSGpuMeshTests.StreamReadback"));
	TRefCountPtr<FRDGPooledBuffer> Pooled = Stream->Pooled;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshTestsEnqueueStream)(
		[Pooled, Readback, Bytes, Role](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGpuMeshTests.ReadbackStream"));
			FRDGBufferRef StreamRDG = GraphBuilder.RegisterExternalBuffer(Pooled);
			AddEnqueueCopyPass(GraphBuilder, Readback, StreamRDG, Bytes);
			CSGpuMeshStreams::SetStreamAccessFinal(GraphBuilder, StreamRDG, Role);
			GraphBuilder.Execute();
		});
	FlushRenderingCommands();

	TArray<uint32> Values;
	Values.SetNumZeroed(int32(NumUints));
	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshTestsConsumeArgs)(
		[Readback, &Values, Bytes, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			// The flush above only waits for the render thread; the copy itself is still in flight.
			if (!Readback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (Readback->IsReady() && Readback->GetGPUSizeBytes() >= Bytes)
			{
				if (const uint32* Raw = static_cast<const uint32*>(Readback->Lock(Bytes)))
				{
					FMemory::Memcpy(Values.GetData(), Raw, Bytes);
					Readback->Unlock();
					bRead = true;
				}
			}
			delete Readback;
		});
	FlushRenderingCommands();

	if (!bRead) return false;
	OutValues = MoveTemp(Values);
	return true;
}

/** The whole DrawIndexedIndirect args buffer (5 uints per arg set). */
bool CSGpuMeshTests_ReadIndirectArgs(const UCSMesh* Mesh, TArray<uint32>& OutArgs)
{
	return CSGpuMeshTests_ReadStreamUints(Mesh, ECSGpuStreamRole::IndirectArgs, 0, OutArgs);
}

/** Uploads raw uints over the front of one resident stream, through the sanctioned edit path.
 *  A pattern planted this way is how "the resize handed the stream back zeroed" becomes
 *  distinguishable from "the stream was already zero". */
bool CSGpuMeshTests_WriteStreamUints(UCSMesh* Mesh, ECSGpuStreamRole Role, uint8 Slot, const TArray<uint32>& Values)
{
	if (!Mesh || Values.Num() == 0) return false;

	const uint64 Bytes = uint64(Values.Num()) * sizeof(uint32);
	return Mesh->EditMeshSync([&Values, Bytes, Role, Slot](FCSMeshEditContext& Context)
	{
		FRDGBufferRef Stream = Context.Find(Role, Slot);
		if (!Stream) return;
		void* Copy = Context.GraphBuilder.Alloc(Bytes, 16);
		FMemory::Memcpy(Copy, Values.GetData(), Bytes);
		Context.GraphBuilder.QueueBufferUpload(Stream, Copy, Bytes, ERDGInitialDataFlags::None);
	});
}

/**
 * Overwrites the per-triangle material-id stream with raw ids.
 *
 * The snapshot upload cannot carry these: FCSGpuMeshCPUData::TriangleMaterialSlots is int32 and
 * CopyFromMeshSnapshot clamps negatives to zero, so CS_NO_MATERIAL_ID (0xFFFFFFFF) can only be
 * planted by writing the stream itself.
 */
bool CSGpuMeshTests_WriteTriangleMaterialIds(UCSMesh* Mesh, const TArray<uint32>& Ids)
{
	const FCSMeshResident* Resident = Mesh ? Mesh->GetResidentPtr() : nullptr;
	if (!Resident) return false;
	if (Ids.Num() > int32(Resident->IndexCapacity / 3u)) return false;

	// The standard set parks the material ids on AuxVertex slot 0, which is why this leaf's own
	// aux slots start at 16 (see ECSGpuInstancedAuxSlot).
	return CSGpuMeshTests_WriteStreamUints(Mesh, ECSGpuStreamRole::AuxVertex, 0, Ids);
}

/** Distinct dynamic material instances. Distinctness matters: a "reports every section material"
 *  assertion would pass on a table whose entries all happen to be the same object. */
void CSGpuMeshTests_MakeDistinctMaterials(UObject* Outer, int32 Count, TArray<TObjectPtr<UMaterialInterface>>& OutMaterials)
{
	OutMaterials.Reset();
	UMaterialInterface* Parent = UMaterial::GetDefaultMaterial(MD_Surface);
	for (int32 Index = 0; Index < Count; ++Index) OutMaterials.Add(UMaterialInstanceDynamic::Create(Parent, Outer));
}
}

// -----------------------------------------------------------------------------
// BuildMaterialSections: the counting sort, its section table and its indirect args
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshMaterialSectionSortTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.MaterialSectionSort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshMaterialSectionSortTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// Deliberately interleaved so "sorted" and "left alone" are visibly different arrays, and with
	// unequal run lengths (3 / 2 / 4) so an off-by-one in the prefix scan lands on a wrong offset
	// instead of on a coincidentally identical one.
	const TArray<int32> InputSlots = { 2, 0, 1, 2, 0, 2, 1, 2, 0 };
	const int32 TriangleCount = InputSlots.Num();
	const int32 ExpectedPerSlot[3] = { 3, 2, 4 };

	FCSGpuMeshCPUData Soup;
	CSGpuMeshTests_BuildTaggedSoup(InputSlots, Soup);

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;
	CSGpuMeshTests_MakeDistinctMaterials(World, 3, GpuMesh->Materials);
	if (!TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

	FCSGpuMeshCPUData Before;
	if (!TestTrue(TEXT("Readback before the sort"), GpuMesh->ReadbackMeshSync(Before))) return false;
	if (!TestTrue(*FString::Printf(TEXT("Upload carried %d triangles, expected %d"),
		Before.TriangleMaterialSlots.Num(), TriangleCount), Before.TriangleMaterialSlots.Num() == TriangleCount))
	{
		return false;
	}
	// The premise the rest of the suite rests on: the ids really do arrive interleaved, so a sort
	// that did nothing cannot pass the contiguity checks below by accident.
	TestTrue(*FString::Printf(TEXT("Uploaded material ids keep the input order: got [%s], expected [%s]"),
		*CSGpuMeshTests_JoinInts(Before.TriangleMaterialSlots), *CSGpuMeshTests_JoinInts(InputSlots)),
		Before.TriangleMaterialSlots == InputSlots);

	TArray<FString> KeysBefore;
	CSGpuMeshTests_SortedTriangleKeys(Before, KeysBefore);

	UCSMeshOps::BuildMaterialSections(GpuMesh);

	TestEqual(TEXT("The args buffer grew to one arg set per material slot"), GpuMesh->GetIndirectDrawCount(), 3);

	const TArray<FCSMeshSection> Sections = GpuMesh->GetSections();
	if (!TestTrue(*FString::Printf(TEXT("Published %d sections, expected one per material slot (3)"), Sections.Num()),
		Sections.Num() == 3))
	{
		return false;
	}
	for (int32 Slot = 0; Slot < Sections.Num(); ++Slot)
		TestEqual(*FString::Printf(TEXT("Section %d draws material slot"), Slot), Sections[Slot].MaterialIndex, Slot);

	FCSGpuMeshCPUData After;
	if (!TestTrue(TEXT("Readback after the sort"), GpuMesh->ReadbackMeshSync(After))) return false;
	TestEqual(TEXT("The sort adds and removes no triangles"), After.TriangleMaterialSlots.Num(), TriangleCount);

	// Contiguity is the whole product of the sort: one arg set per slot can only describe a run,
	// so a single out-of-order triangle means some slot's draw covers a triangle of another slot.
	int32 FirstBreak = INDEX_NONE;
	for (int32 Triangle = 1; Triangle < After.TriangleMaterialSlots.Num(); ++Triangle)
	{
		if (After.TriangleMaterialSlots[Triangle] >= After.TriangleMaterialSlots[Triangle - 1]) continue;
		FirstBreak = Triangle;
		break;
	}
	TestTrue(*FString::Printf(TEXT("Material ids are non-decreasing after the sort (first drop at triangle %d, sequence [%s])"),
		FirstBreak, *CSGpuMeshTests_JoinInts(After.TriangleMaterialSlots)), FirstBreak == INDEX_NONE);

	int32 ActualPerSlot[3] = { 0, 0, 0 };
	bool bAllSlotsInRange = true;
	for (int32 Slot : After.TriangleMaterialSlots)
	{
		if (Slot >= 0 && Slot < 3) ++ActualPerSlot[Slot];
		else bAllSlotsInRange = false;
	}
	TestTrue(*FString::Printf(TEXT("Every sorted triangle carries a slot in [0,3): sequence [%s]"),
		*CSGpuMeshTests_JoinInts(After.TriangleMaterialSlots)), bAllSlotsInRange);
	for (int32 Slot = 0; Slot < 3; ++Slot)
		TestEqual(*FString::Printf(TEXT("Slot %d keeps the triangle count it went in with"), Slot), ActualPerSlot[Slot], ExpectedPerSlot[Slot]);

	// The assertion that catches an off-by-one in the prefix scan or a lost InterlockedAdd: both
	// leave the triangle *count* intact and only show up as one triangle written twice while
	// another was never written at all.
	TArray<FString> KeysAfter;
	CSGpuMeshTests_SortedTriangleKeys(After, KeysAfter);
	FString MissingKey = TEXT("<none>");
	FString ExtraKey = TEXT("<none>");
	for (int32 Index = 0; Index < FMath::Max(KeysBefore.Num(), KeysAfter.Num()); ++Index)
	{
		const FString Expected = KeysBefore.IsValidIndex(Index) ? KeysBefore[Index] : FString(TEXT("<past the end>"));
		const FString Actual = KeysAfter.IsValidIndex(Index) ? KeysAfter[Index] : FString(TEXT("<past the end>"));
		if (Expected == Actual) continue;
		MissingKey = Expected;
		ExtraKey = Actual;
		break;
	}
	TestTrue(*FString::Printf(TEXT("The sort is a permutation of the input triangles (%d in, %d out; expected '%s' where '%s' turned up)"),
		KeysBefore.Num(), KeysAfter.Num(), *MissingKey, *ExtraKey), KeysAfter == KeysBefore);

	// The arg sets are what the draw actually consumes; the section table only names materials.
	TArray<uint32> IndirectArgs;
	if (!TestTrue(TEXT("Indirect args readback"), CSGpuMeshTests_ReadIndirectArgs(GpuMesh, IndirectArgs))) return false;
	if (!TestTrue(*FString::Printf(TEXT("The args buffer holds 5 uints per set: %d uints for %d sets"),
		IndirectArgs.Num(), GpuMesh->GetIndirectDrawCount()), IndirectArgs.Num() >= 15))
	{
		return false;
	}

	auto CheckArgSet = [this, &IndirectArgs](int32 SetIndex, uint32 ExpectedIndexCount, uint32 ExpectedStartIndex)
	{
		const int32 Base = SetIndex * 5;
		if (!IndirectArgs.IsValidIndex(Base + 4))
		{
			AddError(FString::Printf(TEXT("Arg set %d is past the end of a %d-uint args buffer."), SetIndex, IndirectArgs.Num()));
			return;
		}
		TestEqual(*FString::Printf(TEXT("Arg set %d IndexCountPerInstance"), SetIndex), IndirectArgs[Base + 0], ExpectedIndexCount);
		TestEqual(*FString::Printf(TEXT("Arg set %d InstanceCount"), SetIndex), IndirectArgs[Base + 1], 1u);
		TestEqual(*FString::Printf(TEXT("Arg set %d StartIndexLocation"), SetIndex), IndirectArgs[Base + 2], ExpectedStartIndex);
		// Resident indices are absolute, so a non-zero base vertex would shift the whole run.
		TestEqual(*FString::Printf(TEXT("Arg set %d BaseVertexLocation"), SetIndex), IndirectArgs[Base + 3], 0u);
		TestEqual(*FString::Printf(TEXT("Arg set %d StartInstanceLocation"), SetIndex), IndirectArgs[Base + 4], 0u);
	};

	uint32 ExpectedStart = 0;
	for (int32 Slot = 0; Slot < 3; ++Slot)
	{
		CheckArgSet(Slot, uint32(ExpectedPerSlot[Slot]) * 3u, ExpectedStart);
		ExpectedStart += uint32(ExpectedPerSlot[Slot]) * 3u;
	}

	// The operator documents itself as safe to re-run. A second pass whose runs came out different
	// would mean the sort reads state its own previous pass left behind.
	UCSMeshOps::BuildMaterialSections(GpuMesh);
	TArray<uint32> ArgsAgain;
	if (TestTrue(TEXT("Indirect args readback after a second sort"), CSGpuMeshTests_ReadIndirectArgs(GpuMesh, ArgsAgain)))
	{
		bool bSameArgs = ArgsAgain.Num() == IndirectArgs.Num();
		for (int32 Index = 0; bSameArgs && Index < IndirectArgs.Num(); ++Index) bSameArgs = ArgsAgain[Index] == IndirectArgs[Index];
		TestTrue(TEXT("Re-sorting an already-sorted mesh reproduces the same arg sets"), bSameArgs);
	}
	TestEqual(TEXT("Re-sorting republishes the same section count"), GpuMesh->GetSections().Num(), 3);
	return true;
}

// -----------------------------------------------------------------------------
// BuildMaterialSections: the inputs that have no triangles, no table, or no valid id
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshMaterialSectionEdgeCaseTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.MaterialSectionEdgeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshMaterialSectionEdgeCaseTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	auto CheckArgSet = [this](const TArray<uint32>& Args, int32 SetIndex, uint32 ExpectedIndexCount, uint32 ExpectedStartIndex)
	{
		const int32 Base = SetIndex * 5;
		if (!Args.IsValidIndex(Base + 4))
		{
			AddError(FString::Printf(TEXT("Arg set %d is past the end of a %d-uint args buffer."), SetIndex, Args.Num()));
			return;
		}
		TestEqual(*FString::Printf(TEXT("Arg set %d IndexCountPerInstance"), SetIndex), Args[Base + 0], ExpectedIndexCount);
		TestEqual(*FString::Printf(TEXT("Arg set %d InstanceCount"), SetIndex), Args[Base + 1], 1u);
		TestEqual(*FString::Printf(TEXT("Arg set %d StartIndexLocation"), SetIndex), Args[Base + 2], ExpectedStartIndex);
	};

	// --- a slot no triangle uses. Its arg set has to be a no-op draw sitting at the right offset,
	//     because dropping it would break "section i == arg set i == material slot i".
	{
		const TArray<int32> SlotsWithHole = { 0, 0, 2, 2, 2 }; // slot 1 is never referenced
		FCSGpuMeshCPUData Soup;
		CSGpuMeshTests_BuildTaggedSoup(SlotsWithHole, Soup);

		UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		if (!TestNotNull(TEXT("GPU mesh object (empty slot)"), GpuMesh)) return false;
		CSGpuMeshTests_MakeDistinctMaterials(World, 3, GpuMesh->Materials);
		if (!TestTrue(TEXT("Snapshot upload (empty slot)"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

		UCSMeshOps::BuildMaterialSections(GpuMesh);
		TestEqual(TEXT("An unused slot still gets a section"), GpuMesh->GetSections().Num(), 3);
		TestEqual(TEXT("An unused slot still gets an arg set"), GpuMesh->GetIndirectDrawCount(), 3);

		TArray<uint32> Args;
		if (!TestTrue(TEXT("Indirect args readback (empty slot)"), CSGpuMeshTests_ReadIndirectArgs(GpuMesh, Args))) return false;
		CheckArgSet(Args, 0, 6u, 0u);
		// The one that matters: zero indices, not garbage from whatever the buffer pool last held,
		// and an offset that still accounts for the triangles before it.
		CheckArgSet(Args, 1, 0u, 6u);
		CheckArgSet(Args, 2, 9u, 6u);
	}

	// --- no material table at all. One slot minimum, and an empty section table rather than a
	//     one-entry one, because empty already means "one whole-mesh batch from arg set 0".
	{
		const TArray<int32> Slots = { 0, 1, 0, 1 };
		FCSGpuMeshCPUData Soup;
		CSGpuMeshTests_BuildTaggedSoup(Slots, Soup);

		UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		if (!TestNotNull(TEXT("GPU mesh object (no materials)"), GpuMesh)) return false;
		GpuMesh->Materials.Reset();
		if (!TestTrue(TEXT("Snapshot upload (no materials)"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

		UCSMeshOps::BuildMaterialSections(GpuMesh);
		TestEqual(TEXT("An empty material table publishes no sections"), GpuMesh->GetSections().Num(), 0);
		TestEqual(TEXT("An empty material table keeps the single whole-mesh arg set"), GpuMesh->GetIndirectDrawCount(), 1);

		TArray<uint32> Args;
		if (!TestTrue(TEXT("Indirect args readback (no materials)"), CSGpuMeshTests_ReadIndirectArgs(GpuMesh, Args))) return false;
		CheckArgSet(Args, 0, 12u, 0u);

		FCSGpuMeshCPUData After;
		if (!TestTrue(TEXT("Readback after the sort (no materials)"), GpuMesh->ReadbackMeshSync(After))) return false;
		bool bAllSlotZero = After.TriangleMaterialSlots.Num() == 4;
		for (int32 Slot : After.TriangleMaterialSlots) bAllSlotZero &= Slot == 0;
		TestTrue(*FString::Printf(TEXT("Ids past a one-slot table resolve to slot 0: got [%s]"),
			*CSGpuMeshTests_JoinInts(After.TriangleMaterialSlots)), bAllSlotZero);
	}

	// --- CS_NO_MATERIAL_ID and ids past the end of the table. Both must land in slot 0; anything
	//     else indexes the per-slot counter buffer out of bounds.
	{
		const TArray<int32> Placeholder = { 0, 0, 0, 0, 0 };
		FCSGpuMeshCPUData Soup;
		CSGpuMeshTests_BuildTaggedSoup(Placeholder, Soup);

		UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		if (!TestNotNull(TEXT("GPU mesh object (bad ids)"), GpuMesh)) return false;
		CSGpuMeshTests_MakeDistinctMaterials(World, 2, GpuMesh->Materials);
		if (!TestTrue(TEXT("Snapshot upload (bad ids)"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

		// 4 ids resolve to slot 0 (two sentinels, one past the end, one genuine), 1 to slot 1.
		const TArray<uint32> RawIds = { CS_NO_MATERIAL_ID, 1u, 7u, 0u, CS_NO_MATERIAL_ID };
		if (!TestTrue(TEXT("Raw material-id upload"), CSGpuMeshTests_WriteTriangleMaterialIds(GpuMesh, RawIds))) return false;

		FCSGpuMeshCPUData Before;
		if (!TestTrue(TEXT("Readback before the sort (bad ids)"), GpuMesh->ReadbackMeshSync(Before))) return false;
		// Proves the raw write actually reached the stream: 7 has no other way of getting there.
		TestTrue(*FString::Printf(TEXT("The out-of-range id survived the upload: got [%s], wanted a 7 at triangle 2"),
			*CSGpuMeshTests_JoinInts(Before.TriangleMaterialSlots)),
			Before.TriangleMaterialSlots.IsValidIndex(2) && Before.TriangleMaterialSlots[2] == 7);

		UCSMeshOps::BuildMaterialSections(GpuMesh);
		TestEqual(TEXT("Bad ids do not change the section count"), GpuMesh->GetSections().Num(), 2);

		TArray<uint32> Args;
		if (!TestTrue(TEXT("Indirect args readback (bad ids)"), CSGpuMeshTests_ReadIndirectArgs(GpuMesh, Args))) return false;
		CheckArgSet(Args, 0, 12u, 0u);
		CheckArgSet(Args, 1, 3u, 12u);

		FCSGpuMeshCPUData After;
		if (!TestTrue(TEXT("Readback after the sort (bad ids)"), GpuMesh->ReadbackMeshSync(After))) return false;
		const TArray<int32> ExpectedSlots = { 0, 0, 0, 0, 1 };
		// The stream must carry the *resolved* slot afterwards: an unresolved sentinel left in it
		// would make the id stream disagree with the section the triangle now draws in.
		TestTrue(*FString::Printf(TEXT("Sentinel and out-of-range ids resolve to slot 0: got [%s], expected [%s]"),
			*CSGpuMeshTests_JoinInts(After.TriangleMaterialSlots), *CSGpuMeshTests_JoinInts(ExpectedSlots)),
			After.TriangleMaterialSlots == ExpectedSlots);
	}
	return true;
}

// -----------------------------------------------------------------------------
// Section invalidation: who drops the table and who must not
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshSectionInvalidationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.SectionInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshSectionInvalidationTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	const TArray<int32> Slots = { 0, 1, 2, 0, 1, 2 };
	FCSGpuMeshCPUData Soup;
	CSGpuMeshTests_BuildTaggedSoup(Slots, Soup);

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;

	// Oversized up front so nothing below can reallocate. That is what isolates the mechanism under
	// test: a reallocation drops the table through MarkBuffersChanged, which would make the counter
	// pass look like it invalidated when it never ran.
	if (!TestTrue(TEXT("Capacity reserved up front"), GpuMesh->EnsureCapacitySync(4096, 4096))) return false;
	CSGpuMeshTests_MakeDistinctMaterials(World, 3, GpuMesh->Materials);
	if (!TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

	UCSMeshOps::BuildMaterialSections(GpuMesh);
	if (!TestTrue(*FString::Printf(TEXT("Sections published: got %d, expected 3"), GpuMesh->GetSections().Num()),
		GpuMesh->GetSections().Num() == 3))
	{
		return false;
	}
	const FCSMeshResident* Resident = GpuMesh->GetResidentPtr();
	if (!TestTrue(TEXT("Resident set available"), Resident != nullptr)) return false;
	const uint32 AllocationGenerationAfterSort = Resident->AllocationGeneration;

	// --- the deliberate non-invalidators. Both rewrite index *values* or corner order without
	//     moving a triangle between slots, so the runs the table describes still describe the same
	//     triangles and dropping the table would cost the mesh its materials for no reason.
	UCSMeshOps::WeldVertices(GpuMesh, 1.0f);
	TestEqual(TEXT("WeldVertices keeps the section table"), GpuMesh->GetSections().Num(), 3);
	TestEqual(TEXT("WeldVertices does not reallocate"), Resident->AllocationGeneration, AllocationGenerationAfterSort);

	UCSMeshOps::FlipNormals(GpuMesh);
	TestEqual(TEXT("FlipNormals keeps the section table"), GpuMesh->GetSections().Num(), 3);
	TestEqual(TEXT("FlipNormals does not reallocate"), Resident->AllocationGeneration, AllocationGenerationAfterSort);

	// --- the invalidator. Same dimensions as the upload already in place, so EnsureCapacitySync
	//     cannot grow: the only thing left that can drop the table is AddSetCountersPass, whose
	//     kernel rewrites arg set 0 alone and would otherwise leave sets 1..N-1 describing a
	//     triangle layout that no longer exists.
	if (!TestTrue(TEXT("Snapshot re-upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;
	TestEqual(TEXT("The re-upload did not reallocate"), Resident->AllocationGeneration, AllocationGenerationAfterSort);
	TestEqual(TEXT("A counter-writing operator drops the section table"), GpuMesh->GetSections().Num(), 0);

	// --- the same contract through the other documented route into AddSetCountersPass.
	UCSMeshOps::BuildMaterialSections(GpuMesh);
	if (!TestTrue(*FString::Printf(TEXT("Sections rebuilt: got %d, expected 3"), GpuMesh->GetSections().Num()),
		GpuMesh->GetSections().Num() == 3))
	{
		return false;
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (TestNotNull(TEXT("Engine cube mesh"), CubeMesh))
	{
		UCSMeshOps::CopyFromStaticMesh(GpuMesh, CubeMesh, FCSMeshFromStaticMeshOptions());
		TestEqual(TEXT("The StaticMesh upload did not reallocate"), Resident->AllocationGeneration, AllocationGenerationAfterSort);
		TestEqual(TEXT("CopyFromStaticMesh drops the section table"), GpuMesh->GetSections().Num(), 0);
	}

	// --- and Reset, which zeroes the very arg sets the table indexes.
	UCSMeshOps::BuildMaterialSections(GpuMesh);
	if (TestTrue(*FString::Printf(TEXT("Sections rebuilt over the cube: got %d, expected %d"),
		GpuMesh->GetSections().Num(), GpuMesh->Materials.Num()), GpuMesh->GetSections().Num() == GpuMesh->Materials.Num()))
	{
		GpuMesh->Reset();
		TestEqual(TEXT("Reset drops the section table with the geometry it described"), GpuMesh->GetSections().Num(), 0);
	}
	return true;
}

// -----------------------------------------------------------------------------
// ComputeWorldBoundsSync: the ordered-float reduction
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshWorldBoundsReductionTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.WorldBoundsReduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshWorldBoundsReductionTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// Straddles the origin with negative coordinates on all three axes. This is the case a naive
	// InterlockedMin/Max over asuint(float) gets exactly backwards — negative bit patterns run
	// upwards — so a test that stayed in the positive octant would pass against a broken reduction.
	const FVector3f TrueMin(-1234.5f, -678.25f, -90.125f);
	const FVector3f TrueMax(2345.75f, 456.5f, 78.375f);
	TestTrue(TEXT("The test data really is negative on all three axes"),
		TrueMin.X < 0.0f && TrueMin.Y < 0.0f && TrueMin.Z < 0.0f);

	const FVector3f Corners[6] = {
		FVector3f(TrueMin.X, TrueMin.Y, TrueMin.Z),
		FVector3f(TrueMax.X, TrueMin.Y, TrueMax.Z),
		FVector3f(TrueMin.X, TrueMax.Y, TrueMin.Z),
		FVector3f(TrueMax.X, TrueMax.Y, TrueMax.Z),
		FVector3f(0.0f, 0.0f, 0.0f),
		FVector3f(TrueMin.X, 0.0f, TrueMax.Z) };

	FCSGpuMeshCPUData Soup;
	for (const FVector3f& Corner : Corners)
	{
		Soup.Indices.Add(uint32(Soup.Positions.Num()));
		Soup.Positions.Add(Corner);
		Soup.Normals.Add(FVector3f::UnitZ());
		Soup.Tangents.Add(FVector3f::UnitX());
		Soup.TexCoords().Add(FVector2f::ZeroVector);
	}
	Soup.TriangleMaterialSlots = { 0, 0 };

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;
	if (!TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

	// A deliberately oversized bound standing in for the conservative estimate the GPU-sized
	// operators leave behind (for the scene extraction it is the whole query box).
	const FBox Oversized(FVector(-1.0e6), FVector(1.0e6));
	if (!TestTrue(TEXT("Planting the conservative bound"),
		GpuMesh->EditMeshSync([Oversized](FCSMeshEditContext& Context) { Context.Resident.WorldBounds = Oversized; })))
	{
		return false;
	}

	UCSMeshOps::ComputeWorldBoundsSync(GpuMesh);
	const FBox Reduced = GpuMesh->GetWorldBoundsApprox();
	if (!TestTrue(TEXT("The reduction produced a valid box"), Reduced.IsValid != 0)) return false;

	const FVector ExpectedMin(TrueMin);
	const FVector ExpectedMax(TrueMax);
	// Exact, not approximate: the same float bits go in and come back out through the flip, so a
	// mismatch here is a wrong reduction rather than accumulated error.
	TestTrue(*FString::Printf(TEXT("Reduced min is the true minimum: got %s, expected %s"),
		*Reduced.Min.ToString(), *ExpectedMin.ToString()), Reduced.Min.Equals(ExpectedMin, 0.01));
	TestTrue(*FString::Printf(TEXT("Reduced max is the true maximum: got %s, expected %s"),
		*Reduced.Max.ToString(), *ExpectedMax.ToString()), Reduced.Max.Equals(ExpectedMax, 0.01));

	// And it is genuinely tighter than what it replaced, on every axis.
	TestTrue(*FString::Printf(TEXT("The reduction tightened the conservative bound: extent %s against %s"),
		*Reduced.GetExtent().ToString(), *Oversized.GetExtent().ToString()),
		Reduced.Min.X > Oversized.Min.X && Reduced.Min.Y > Oversized.Min.Y && Reduced.Min.Z > Oversized.Min.Z
		&& Reduced.Max.X < Oversized.Max.X && Reduced.Max.Y < Oversized.Max.Y && Reduced.Max.Z < Oversized.Max.Z);

	// --- nothing contributed. Reset keeps the allocation and zeroes the counters, so the reduction
	//     runs over no vertices at all and the buffer is still at its clear value.
	GpuMesh->Reset();
	TestEqual(TEXT("Reset zeroed the GPU triangle count"), GpuMesh->GetTriangleCountSync(), 0);

	const FBox Preserved(FVector(-500.0, -250.0, -125.0), FVector(750.0, 300.0, 175.0));
	if (!TestTrue(TEXT("Planting the bound the empty reduction must not touch"),
		GpuMesh->EditMeshSync([Preserved](FCSMeshEditContext& Context) { Context.Resident.WorldBounds = Preserved; })))
	{
		return false;
	}

	UCSMeshOps::ComputeWorldBoundsSync(GpuMesh);
	const FBox AfterEmpty = GpuMesh->GetWorldBoundsApprox();
	// The clear value has to be a true identity in the flipped ordering. If it were not, "nothing
	// contributed" would be indistinguishable from a real result and the caller's estimate would be
	// replaced by a degenerate box near the origin — which then gets blamed on the geometry.
	TestTrue(*FString::Printf(TEXT("An empty mesh keeps the previous bounds: got %s, expected %s"),
		*AfterEmpty.ToString(), *Preserved.ToString()),
		AfterEmpty.IsValid != 0 && AfterEmpty.Min.Equals(Preserved.Min, 0.01) && AfterEmpty.Max.Equals(Preserved.Max, 0.01));
	return true;
}

// -----------------------------------------------------------------------------
// bComputeExactBounds: the extraction's bound against the box it searched
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshExactSceneBoundsTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.ExactSceneBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshExactSceneBoundsTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;

	const FTransform TestTransform(FRotator(0.0, 27.0, 0.0), FVector(100000.0, -100000.0, 50000.0));
	AStaticMeshActor* SourceActor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("Source StaticMesh actor"), SourceActor)) return false;
	SourceActor->SetActorTransform(TestTransform);
	SourceActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);

	AComputeShaderMeshGenerator* Generator = World->SpawnActor<AComputeShaderMeshGenerator>();
	if (!TestNotNull(TEXT("Compute shader mesh generator"), Generator)) return false;
	Generator->SetActorTransform(TestTransform);
	// A query box deliberately bigger than the geometry inside it: that gap is what the exact
	// reduction exists to close, and with a matching box the two answers would be the same.
	Generator->GeneratorBounds->SetBoxExtent(FVector(120.0));
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	const FBox QueryBox = Generator->GetGeneratorBoundsWorldBox();
	if (!TestTrue(TEXT("The generator has a valid query box"), QueryBox.IsValid != 0)) return false;

	FCSMeshBoxSceneOptions SceneOptions;
	SceneOptions.QueryBox = QueryBox;
	SceneOptions.ExcludedActor = Generator;
	SceneOptions.MaxTriangles = 64;
	SceneOptions.bIncludeLandscape = false;

	// --- the conservative fallback: the bound is the box that was searched, most of which is empty.
	UCSMesh* ConservativeMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	SceneOptions.bComputeExactBounds = false;
	UCSMeshOps::AppendBoxSceneTriangles(ConservativeMesh, Generator, SceneOptions);
	const FBox Conservative = ConservativeMesh->GetWorldBoundsApprox();
	const int32 ConservativeTriangles = ConservativeMesh->GetTriangleCountSync();
	if (!TestTrue(*FString::Printf(TEXT("The extraction found geometry inside the query box: %d triangles"), ConservativeTriangles),
		ConservativeTriangles > 0))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("Without bComputeExactBounds the bound is the query box: got %s, expected %s"),
		*Conservative.ToString(), *QueryBox.ToString()),
		Conservative.IsValid != 0 && Conservative.Min.Equals(QueryBox.Min, 1.0) && Conservative.Max.Equals(QueryBox.Max, 1.0));

	// --- the same extraction with the reduction switched on.
	UCSMesh* ExactMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	SceneOptions.bComputeExactBounds = true;
	UCSMeshOps::AppendBoxSceneTriangles(ExactMesh, Generator, SceneOptions);
	const FBox Exact = ExactMesh->GetWorldBoundsApprox();
	TestEqual(TEXT("Both extractions found the same geometry"), ExactMesh->GetTriangleCountSync(), ConservativeTriangles);

	FCSGpuMeshCPUData Snapshot;
	if (!TestTrue(TEXT("Readback of the extracted geometry"), ExactMesh->ReadbackMeshSync(Snapshot))) return false;
	FBox FromPositions(ForceInit);
	for (const FVector3f& Position : Snapshot.Positions) FromPositions += FVector(Position);

	// The GPU reduction against the same positions added up on the CPU. A wrong flip, a group that
	// failed to reach the barrier, or a missed tail thread all show up here as a box that does not
	// contain the geometry it was reduced from.
	TestTrue(*FString::Printf(TEXT("The exact bound matches the extracted positions: got %s, expected %s"),
		*Exact.ToString(), *FromPositions.ToString()),
		Exact.IsValid != 0 && FromPositions.IsValid != 0
		&& Exact.Min.Equals(FromPositions.Min, 0.01) && Exact.Max.Equals(FromPositions.Max, 0.01));

	TestTrue(*FString::Printf(TEXT("The exact bound is tighter than the query box: extent %s against %s"),
		*Exact.GetExtent().ToString(), *Conservative.GetExtent().ToString()),
		Exact.GetExtent().X < Conservative.GetExtent().X
		&& Exact.GetExtent().Y < Conservative.GetExtent().Y
		&& Exact.GetExtent().Z < Conservative.GetExtent().Z);

	SourceActor->Destroy();
	Generator->Destroy();
	return true;
}

// -----------------------------------------------------------------------------
// Per-section rendering: batch materials, the unsectioned default, and the rebind
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshSectionRenderingTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.SectionRendering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshSectionRenderingTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	const TArray<int32> Slots = { 0, 1, 2, 0, 1, 2 };
	FCSGpuMeshCPUData Soup;
	CSGpuMeshTests_BuildTaggedSoup(Slots, Soup);

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;

	TArray<TObjectPtr<UMaterialInterface>> SectionMaterials;
	CSGpuMeshTests_MakeDistinctMaterials(World, 3, SectionMaterials);
	GpuMesh->Materials = SectionMaterials;
	if (!TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

	UCSMeshOps::BuildMaterialSections(GpuMesh);
	if (!TestTrue(*FString::Printf(TEXT("Sections published: got %d, expected 3"), GpuMesh->GetSections().Num()),
		GpuMesh->GetSections().Num() == 3))
	{
		return false;
	}

	AActor* HostActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Host actor"), HostActor)) return false;

	TArray<TObjectPtr<UMaterialInterface>> ComponentMaterials;
	CSGpuMeshTests_MakeDistinctMaterials(HostActor, 2, ComponentMaterials);

	UCSMeshRenderComponent* RenderComponent = NewObject<UCSMeshRenderComponent>(HostActor);
	RenderComponent->MeshMaterial = ComponentMaterials[0];
	HostActor->SetRootComponent(RenderComponent);
	RenderComponent->RegisterComponent();
	RenderComponent->SetGpuMesh(GpuMesh);
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	// A section material nothing ever reported is one the engine prepared no shaders, no texture
	// streaming and no editor usage query for — it draws as the default and nobody knows why.
	TArray<UMaterialInterface*> Used;
	RenderComponent->GetUsedMaterials(Used);
	TestEqual(TEXT("Every section material plus MeshMaterial is reported"), Used.Num(), 4);
	for (int32 Slot = 0; Slot < 3; ++Slot)
		TestTrue(*FString::Printf(TEXT("Section material %d is reported"), Slot), Used.Contains(SectionMaterials[Slot].Get()));
	// MeshMaterial stays in the list even though sections are drawing instead of it: the table is
	// transient GPU state that any reallocation drops, and "which components use this material"
	// must not depend on when it was asked.
	TestTrue(TEXT("MeshMaterial stays reported while sections are drawing"), Used.Contains(RenderComponent->MeshMaterial.Get()));

	// --- the unsectioned path, which every mesh that never meets the section builder still takes.
	const TArray<int32> PlainSlots = { 0, 0 };
	FCSGpuMeshCPUData PlainSoup;
	CSGpuMeshTests_BuildTaggedSoup(PlainSlots, PlainSoup);
	UCSMesh* PlainMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("Unsectioned GPU mesh object"), PlainMesh)) return false;
	if (!TestTrue(TEXT("Unsectioned snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(PlainMesh, PlainSoup))) return false;
	TestEqual(TEXT("An unsectioned mesh publishes no section table"), PlainMesh->GetSections().Num(), 0);

	UCSMeshRenderComponent* PlainComponent = NewObject<UCSMeshRenderComponent>(HostActor);
	PlainComponent->MeshMaterial = ComponentMaterials[1];
	PlainComponent->SetupAttachment(RenderComponent);
	PlainComponent->RegisterComponent();
	PlainComponent->SetGpuMesh(PlainMesh);
	FlushRenderingCommands();

	TArray<UMaterialInterface*> PlainUsed;
	PlainComponent->GetUsedMaterials(PlainUsed);
	TestEqual(TEXT("An empty section table reports exactly one material"), PlainUsed.Num(), 1);
	if (PlainUsed.Num() == 1) TestEqual(TEXT("...and it is MeshMaterial"), PlainUsed[0], PlainComponent->MeshMaterial.Get());

	PlainComponent->MeshMaterial = nullptr;
	TArray<UMaterialInterface*> NullMeshMaterialUsed;
	PlainComponent->GetUsedMaterials(NullMeshMaterialUsed);
	TestEqual(TEXT("A null MeshMaterial still reports one material"), NullMeshMaterialUsed.Num(), 1);
	if (NullMeshMaterialUsed.Num() == 1)
		TestEqual(TEXT("...the engine default surface material"), NullMeshMaterialUsed[0], UCSGpuMeshComponent::GetDefaultSurfaceMaterial());

	// --- the section table changing with no reallocation behind it.
	FPrimitiveSceneProxy* ProxyBeforeChange = RenderComponent->GetSceneProxy();
	if (!TestTrue(TEXT("The sectioned component has a live scene proxy"), ProxyBeforeChange != nullptr)) return false;

	const int32 ArgSetsBefore = GpuMesh->GetIndirectDrawCount();
	const FCSMeshResident* Resident = GpuMesh->GetResidentPtr();
	if (!TestTrue(TEXT("Resident set available"), Resident != nullptr)) return false;
	const uint32 AllocationGenerationBefore = Resident->AllocationGeneration;

	TArray<FCSMeshSection> FewerSections;
	FewerSections.AddDefaulted_GetRef().MaterialIndex = 0;
	FewerSections.AddDefaulted_GetRef().MaterialIndex = 2;
	if (!TestTrue(TEXT("SetSections accepted the shorter table"), GpuMesh->SetSections(FewerSections))) return false;
	FlushRenderingCommands();

	// The premise: the args buffer was already big enough, so AllocationGeneration cannot carry the
	// signal and only the recomputed batch-material list can.
	TestEqual(TEXT("Publishing a shorter table did not reallocate the args buffer"), GpuMesh->GetIndirectDrawCount(), ArgSetsBefore);
	TestEqual(TEXT("Publishing a shorter table did not bump the allocation generation"),
		Resident->AllocationGeneration, AllocationGenerationBefore);

	// The proxy freezes its batch list at construction, so a surviving proxy is one drawing three
	// materials for a mesh that now has two batches — with nothing at all to indicate it.
	FPrimitiveSceneProxy* ProxyAfterChange = RenderComponent->GetSceneProxy();
	TestTrue(TEXT("A section-table change with no reallocation still rebuilt the scene proxy"),
		ProxyAfterChange != nullptr && ProxyAfterChange != ProxyBeforeChange);

	TArray<UMaterialInterface*> UsedAfterChange;
	RenderComponent->GetUsedMaterials(UsedAfterChange);
	TestEqual(TEXT("The shorter table's materials are what gets reported"), UsedAfterChange.Num(), 3);
	TestFalse(TEXT("The dropped section's material is no longer a batch material"),
		UsedAfterChange.Contains(SectionMaterials[1].Get()));

	// --- a null slot and an index past the end of the material table. Both are normal transient
	//     states (the sort usually runs before the caller has finished filling Materials), so they
	//     have to degrade to the default surface material rather than drop the batch or crash.
	GpuMesh->Materials[1] = nullptr;
	TArray<FCSMeshSection> AwkwardSections;
	AwkwardSections.AddDefaulted_GetRef().MaterialIndex = 0;
	AwkwardSections.AddDefaulted_GetRef().MaterialIndex = 1;  // a null entry in the table
	AwkwardSections.AddDefaulted_GetRef().MaterialIndex = 99; // past the end of a 3-entry table
	if (!TestTrue(TEXT("SetSections accepted the table with unresolvable materials"), GpuMesh->SetSections(AwkwardSections))) return false;
	FlushRenderingCommands();

	TArray<UMaterialInterface*> UsedAwkward;
	RenderComponent->GetUsedMaterials(UsedAwkward);
	bool bNoNulls = UsedAwkward.Num() > 0;
	for (UMaterialInterface* Material : UsedAwkward) bNoNulls &= Material != nullptr;
	// A null here is what the render thread would call GetRenderProxy() on mid-frame.
	TestTrue(*FString::Printf(TEXT("GetUsedMaterials never reports a null material (%d entries)"), UsedAwkward.Num()), bNoNulls);
	TestTrue(TEXT("A null slot and an out-of-range index both fall back to the default surface material"),
		UsedAwkward.Contains(UCSGpuMeshComponent::GetDefaultSurfaceMaterial()));
	TestTrue(TEXT("The one resolvable section material is still reported"), UsedAwkward.Contains(SectionMaterials[0].Get()));
	// Rebuilt again: three unresolvable-or-not sections still mean three batches, so an unfilled
	// material slot costs a default-coloured batch rather than missing geometry.
	TestTrue(TEXT("The table with unresolvable materials rebuilt the scene proxy too"),
		RenderComponent->GetSceneProxy() != nullptr && RenderComponent->GetSceneProxy() != ProxyAfterChange);

	// --- material-table edits. Section 0 resolves through Materials[0], so swapping that entry
	//     changes what the mesh draws without touching a buffer, a count or the section table —
	//     the change event is the only thing that can carry it.
	FPrimitiveSceneProxy* ProxyBeforeMaterialEdit = RenderComponent->GetSceneProxy();
	UMaterialInterface* const DefaultSurface = UCSGpuMeshComponent::GetDefaultSurfaceMaterial();

	// Element assignment on a TArray UPROPERTY cannot be intercepted, so this genuinely does not
	// notify. Pinned as a test because the field's doc comment promises exactly this: it is the
	// reason SetMaterial exists, and if it ever starts notifying that comment is the thing to fix.
	GpuMesh->Materials[0] = DefaultSurface;
	FlushRenderingCommands();
	TestEqual(TEXT("A direct Materials[i] write does not rebuild the proxy (hence SetMaterial)"),
		RenderComponent->GetSceneProxy(), ProxyBeforeMaterialEdit);

	if (!TestTrue(TEXT("NotifyMaterialsChanged reached the component"),
		[&]{ GpuMesh->NotifyMaterialsChanged(); FlushRenderingCommands();
		     return RenderComponent->GetSceneProxy() != nullptr
		         && RenderComponent->GetSceneProxy() != ProxyBeforeMaterialEdit; }())) return false;

	TArray<UMaterialInterface*> UsedAfterNotify;
	RenderComponent->GetUsedMaterials(UsedAfterNotify);
	TestFalse(TEXT("The replaced section material is no longer reported"),
		UsedAfterNotify.Contains(SectionMaterials[0].Get()));

	// SetMaterial is the same edit without the separate notify, and it must grow the table rather
	// than refuse: the sort routinely runs before a producer has finished filling Materials.
	FPrimitiveSceneProxy* ProxyBeforeSetMaterial = RenderComponent->GetSceneProxy();
	GpuMesh->SetMaterial(0, SectionMaterials[0].Get());
	FlushRenderingCommands();
	TestTrue(TEXT("SetMaterial notifies on its own"),
		RenderComponent->GetSceneProxy() != nullptr && RenderComponent->GetSceneProxy() != ProxyBeforeSetMaterial);

	const int32 MaterialCountBeforeGrow = GpuMesh->Materials.Num();
	GpuMesh->SetMaterial(MaterialCountBeforeGrow + 2, DefaultSurface);
	TestEqual(TEXT("SetMaterial past the end grows the table to fit"),
		GpuMesh->Materials.Num(), MaterialCountBeforeGrow + 3);

	// A no-op assignment must not churn the proxy; a render-state recreation per redundant set
	// would make filling the table slot-by-slot quadratic in proxy rebuilds.
	FPrimitiveSceneProxy* ProxyBeforeRedundantSet = RenderComponent->GetSceneProxy();
	GpuMesh->SetMaterial(0, SectionMaterials[0].Get());
	FlushRenderingCommands();
	TestEqual(TEXT("Assigning the value a slot already holds does not rebuild the proxy"),
		RenderComponent->GetSceneProxy(), ProxyBeforeRedundantSet);

	HostActor->Destroy();
	return true;
}

// -----------------------------------------------------------------------------
// ResizeStreamsSync: one stream moves, nothing else does
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshStreamResizeTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.StreamResize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshStreamResizeTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// Clear of the standard set, which owns AuxVertex slot 0 for its per-triangle material ids.
	constexpr uint8 AuxSlot = 16;
	constexpr int32 InitialElements = 64;

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;

	FCSMeshStreamLayout Layout;
	FCSGpuStreamDesc Aux;
	Aux.DebugName = TEXT("CSGpuMeshTests.Resizable");
	Aux.Role = ECSGpuStreamRole::AuxVertex;
	Aux.BytesPerElement = sizeof(uint32);
	Aux.ElementsPerUnit = uint32(InitialElements);
	Aux.CountSource = ECSGpuCountSource::Fixed;
	Aux.SrvFormat = PF_R32_UINT;
	Aux.TexCoordIndex = AuxSlot;
	Layout.ExtraStreams.Add(Aux);
	if (!TestTrue(TEXT("The mesh accepts a resizable extra stream"), GpuMesh->SetStreamLayoutSync(Layout))) return false;

	// Geometry worth losing: not losing it is the whole product of a targeted resize.
	const TArray<int32> Slots = { 0, 0, 0 };
	FCSGpuMeshCPUData Soup;
	CSGpuMeshTests_BuildTaggedSoup(Slots, Soup);
	if (!TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;

	FCSGpuMeshCPUData Before;
	if (!TestTrue(TEXT("Readback before the resize"), GpuMesh->ReadbackMeshSync(Before))) return false;

	TArray<uint32> Pattern;
	for (int32 Index = 0; Index < InitialElements; ++Index) Pattern.Add(uint32(Index) + 1u);
	if (!TestTrue(TEXT("Aux stream seeded with a pattern"),
		CSGpuMeshTests_WriteStreamUints(GpuMesh, ECSGpuStreamRole::AuxVertex, AuxSlot, Pattern)))
	{
		return false;
	}

	FCSMeshResident* Resident = GpuMesh->GetResidentPtr();
	if (!TestNotNull(TEXT("Resident set"), Resident)) return false;
	const int32 AuxIndex = Resident->FindStreamIndex(ECSGpuStreamRole::AuxVertex, AuxSlot);
	if (!TestTrue(TEXT("The extra stream is in the resident set"), AuxIndex != INDEX_NONE)) return false;

	// Held as references, not bare addresses: a released pooled buffer goes back to the pool, and a
	// recycled address would make "this is a different buffer" pass by coincidence.
	TArray<TRefCountPtr<FRDGPooledBuffer>> PooledBefore;
	for (const FCSMeshResident::FStream& Stream : Resident->Streams) PooledBefore.Add(Stream.Pooled);
	const uint32 AllocationGenerationBefore = Resident->AllocationGeneration;

	auto SameBuffer = [&PooledBefore, Resident](int32 StreamIndex)
	{
		return Resident->Streams[StreamIndex].Pooled.GetReference() == PooledBefore[StreamIndex].GetReference();
	};

	// --- grow
	if (!TestTrue(TEXT("Growing one extra stream"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::AuxVertex, AuxSlot, InitialElements * 2)))
	{
		return false;
	}

	TestEqual(TEXT("The resized stream carries the new element count"),
		int32(Resident->Streams[AuxIndex].Desc.ElementsPerUnit), InitialElements * 2);
	TestFalse(TEXT("The resized stream got a new buffer"), SameBuffer(AuxIndex));
	TestTrue(TEXT("A reallocation moved the allocation generation, so a bound proxy rebinds"),
		Resident->AllocationGeneration != AllocationGenerationBefore);

	// The assertion the whole entry point exists for. Before this, changing one aux stream's size
	// meant re-declaring the layout, which reallocates and copies every stream in the set.
	int32 FirstMovedStream = INDEX_NONE;
	for (int32 Index = 0; Index < Resident->Streams.Num(); ++Index)
	{
		if (Index == AuxIndex || SameBuffer(Index)) continue;
		FirstMovedStream = Index;
		break;
	}
	TestTrue(*FString::Printf(TEXT("Every other stream kept the buffer it had (stream %d moved)"), FirstMovedStream),
		FirstMovedStream == INDEX_NONE && Resident->Streams.Num() == PooledBefore.Num());

	FCSGpuMeshCPUData After;
	if (!TestTrue(TEXT("Readback after the resize"), GpuMesh->ReadbackMeshSync(After))) return false;
	bool bGeometryIntact = After.Positions.Num() == Before.Positions.Num() && After.Indices == Before.Indices;
	for (int32 Index = 0; bGeometryIntact && Index < Before.Positions.Num(); ++Index) bGeometryIntact = After.Positions[Index].Equals(Before.Positions[Index], 0.01f);
	TestTrue(TEXT("Resizing one stream leaves every other stream's contents alone"), bGeometryIntact);

	TArray<uint32> Grown;
	if (TestTrue(TEXT("Aux stream readback after the grow"),
		CSGpuMeshTests_ReadStreamUints(GpuMesh, ECSGpuStreamRole::AuxVertex, AuxSlot, Grown)))
	{
		TestEqual(TEXT("The grown stream is twice as long"), Grown.Num(), InitialElements * 2);
		// The documented contract. Left alone, the buffer would hold whatever the pool's previous
		// tenant wrote, and a consumer reading that has no way to tell that is what it is reading.
		int32 FirstNonZero = INDEX_NONE;
		for (int32 Index = 0; Index < Grown.Num(); ++Index)
		{
			if (Grown[Index] == 0u) continue;
			FirstNonZero = Index;
			break;
		}
		TestTrue(*FString::Printf(TEXT("A resized stream comes back zeroed (element %d survived)"), FirstNonZero),
			FirstNonZero == INDEX_NONE);
	}

	// The property the instanced leaf's rebuild depends on: after a targeted resize the resident set
	// IS the layout a re-declaration at that size would ask for, so the declaration a consumer
	// repeats on every rebuild costs nothing instead of reallocating and copying the whole set.
	FCSMeshStreamLayout GrownDeclaration;
	FCSGpuStreamDesc GrownAux = Aux;
	GrownAux.ElementsPerUnit = uint32(InitialElements) * 2u;
	GrownDeclaration.ExtraStreams.Add(GrownAux);
	const uint32 GenerationBeforeRedeclare = Resident->AllocationGeneration;
	TestTrue(TEXT("Re-declaring the layout the resize arrived at is accepted"),
		GpuMesh->SetStreamLayoutSync(GrownDeclaration));
	TestEqual(TEXT("...and reallocates nothing"), Resident->AllocationGeneration, GenerationBeforeRedeclare);

	// --- shrink. The same entry point both ways: hysteresis belongs to the caller, which is the
	//     only thing that knows how its counts move.
	if (TestTrue(TEXT("Shrinking the same stream"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::AuxVertex, AuxSlot, InitialElements / 4)))
	{
		TestEqual(TEXT("The shrunk stream is exactly what was asked for, not rounded up"),
			int32(Resident->Streams[AuxIndex].Desc.ElementsPerUnit), InitialElements / 4);
		TArray<uint32> Shrunk;
		if (TestTrue(TEXT("Aux stream readback after the shrink"),
			CSGpuMeshTests_ReadStreamUints(GpuMesh, ECSGpuStreamRole::AuxVertex, AuxSlot, Shrunk)))
		{
			TestEqual(TEXT("The shrunk stream really is shorter"), Shrunk.Num(), InitialElements / 4);
		}
	}

	// Re-asking for the size it already has must not reallocate: a consumer re-declares its sizes on
	// every rebuild, and a rebuild that changed nothing has to cost nothing.
	const uint32 GenerationBeforeNoOp = Resident->AllocationGeneration;
	TestTrue(TEXT("Resizing to the size the stream already has is accepted"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::AuxVertex, AuxSlot, InitialElements / 4));
	TestEqual(TEXT("...and reallocates nothing"), Resident->AllocationGeneration, GenerationBeforeNoOp);

	// --- the refusals. Each must leave the mesh exactly as it was: a resize that rejected an entry
	//     after applying an earlier one leaves half the streams sized for the new count and half for
	//     the old, and the CPU-side strides that index them can only be right for one of the two.
	const int32 AuxElementsBeforeRefusals = int32(Resident->Streams[AuxIndex].Desc.ElementsPerUnit);
	const uint32 GenerationBeforeRefusals = Resident->AllocationGeneration;

	TestFalse(TEXT("A stream nothing declares is refused"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::AuxVertex, 200, 32));
	TestFalse(TEXT("Zero elements is refused: it would allocate nothing and report the mesh unallocated"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::AuxVertex, AuxSlot, 0));
	// Position's ElementsPerUnit is the xyz stride the vertex factory decodes by, not a size.
	TestFalse(TEXT("A capacity-sized stream is refused"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::Position, 0, 128));
	// The args keep NumIndirectDraws alongside the descriptor; SetSections is checked against it.
	TestFalse(TEXT("The indirect args are refused, and belong to EnsureIndirectDrawCapacitySync"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::IndirectArgs, 0, 10));
	TestFalse(TEXT("The counters stream is refused: the readback copies exactly two uints out of it"),
		GpuMesh->ResizeStreamSync(ECSGpuStreamRole::MeshCounters, 0, 8));

	TArray<FCSMeshStreamResize> Duplicated;
	for (int32 Repeat = 0; Repeat < 2; ++Repeat)
	{
		FCSMeshStreamResize& Entry = Duplicated.AddDefaulted_GetRef();
		Entry.Role = ECSGpuStreamRole::AuxVertex;
		Entry.SlotIndex = AuxSlot;
		Entry.ElementCount = uint32(InitialElements) * uint32(Repeat + 2);
	}
	TestFalse(TEXT("Naming one stream twice in a batch is refused: which size survived would be array order"),
		GpuMesh->ResizeStreamsSync(Duplicated));

	// A batch is all or nothing, which is only observable when one entry would have worked.
	TArray<FCSMeshStreamResize> Mixed;
	FCSMeshStreamResize& Good = Mixed.AddDefaulted_GetRef();
	Good.Role = ECSGpuStreamRole::AuxVertex;
	Good.SlotIndex = AuxSlot;
	Good.ElementCount = uint32(InitialElements) * 8u;
	FCSMeshStreamResize& Bad = Mixed.AddDefaulted_GetRef();
	Bad.Role = ECSGpuStreamRole::MeshCounters;
	Bad.SlotIndex = 0;
	Bad.ElementCount = 8u;
	TestFalse(TEXT("A batch with one bad entry is refused whole"), GpuMesh->ResizeStreamsSync(Mixed));
	TestEqual(TEXT("...and the entry that would have worked was not applied"),
		int32(Resident->Streams[AuxIndex].Desc.ElementsPerUnit), AuxElementsBeforeRefusals);

	TestEqual(TEXT("No refusal reallocated anything"), Resident->AllocationGeneration, GenerationBeforeRefusals);
	TestEqual(TEXT("No refusal touched the draw layout"), GpuMesh->GetIndirectDrawCount(), 1);
	TestEqual(TEXT("The mesh still draws the triangles it went in with"), GpuMesh->GetTriangleCountSync(), Slots.Num());
	return true;
}

// -----------------------------------------------------------------------------
// FCSMeshRenderThreadEdit: an edit inside a graph the caller owns
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshRenderThreadEditTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.RenderThreadEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshRenderThreadEditTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	const TArray<int32> Slots = { 0, 0, 0 };
	FCSGpuMeshCPUData Soup;
	CSGpuMeshTests_BuildTaggedSoup(Slots, Soup);

	UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
	if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;
	if (!TestTrue(TEXT("Snapshot upload"), UCSMeshOps::CopyFromMeshSnapshot(GpuMesh, Soup))) return false;
	if (!TestEqual(TEXT("The upload landed"), GpuMesh->GetTriangleCountSync(), Slots.Num())) return false;

	const FCSMeshResidentRef Resident = GpuMesh->GetResident();
	if (!TestTrue(TEXT("Resident set"), Resident.IsValid())) return false;
	const int32 NumStreams = Resident->Streams.Num();

	// Two of the three triangles, so the write is visible and the mesh stays readable afterwards.
	const uint32 EditedCounters[2] = { 6u, 6u };

	int32 RegisteredStreams = 0;
	bool bNamedStreamsResolve = false;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshTestsRenderThreadEdit)(
		[Resident, &RegisteredStreams, &bNamedStreamsResolve, &EditedCounters](FRHICommandListImmediate& RHICmdList)
		{
			// The caller's graph: built here, executed here, never flushed from inside the edit.
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGpuMeshTests.RenderThreadEdit"));
			{
				FCSMeshRenderThreadEdit Edit(GraphBuilder, *Resident);

				// The registration half of the contract: the edit hands over the whole resident set,
				// not the streams it guessed this caller would want.
				for (FRDGBufferRef Buffer : Edit->StreamBuffers) if (Buffer) ++RegisteredStreams;
				bNamedStreamsResolve = Edit->Positions() && Edit->Tangents() && Edit->TexCoords() && Edit->Colors()
					&& Edit->Indices() && Edit->IndirectArgs() && Edit->Counters() && Edit->MaterialIds();

				if (FRDGBufferRef Counters = Edit->Counters())
					GraphBuilder.QueueBufferUpload(Counters, EditedCounters, sizeof(EditedCounters), ERDGInitialDataFlags::None);
			}
			// Both of these belong to the caller and neither happened inside the scope above: the
			// edit added its access-mode transitions and stopped there.
			GraphBuilder.Execute();
		});
	FlushRenderingCommands();

	TestTrue(TEXT("A render-thread edit resolves every stream an operator can name"), bNamedStreamsResolve);
	TestEqual(TEXT("...over the whole resident set"), RegisteredStreams, NumStreams);

	// The write landed, and the counters stream came out of the edit in CopySrc: the readback copies
	// straight out of it and would find it in the wrong state otherwise.
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	if (TestTrue(TEXT("Counters read back after the render-thread edit"), GpuMesh->GetCountsSync(VertexCount, IndexCount)))
	{
		TestEqual(TEXT("The edit's write reached the vertex counter"), VertexCount, EditedCounters[0]);
		TestEqual(TEXT("...and the index counter"), IndexCount, EditedCounters[1]);
	}
	TestEqual(TEXT("The GPU-side triangle count is what the render-thread edit wrote"), GpuMesh->GetTriangleCountSync(), 2);

	// The other half of the contract, and the one that fails silently when it is broken: every
	// stream came out usable. The readback is what notices — it copies out of the index stream,
	// which RDG's default epilogue (SRVMask) would have left illegal for index use.
	FCSGpuMeshCPUData After;
	if (TestTrue(TEXT("The mesh still reads back after a render-thread edit"), GpuMesh->ReadbackMeshSync(After)))
	{
		TestEqual(TEXT("...at the counts the edit published"), After.Positions.Num(), int32(EditedCounters[0]));
		bool bGeometryIntact = After.Positions.Num() <= Soup.Positions.Num();
		for (int32 Index = 0; bGeometryIntact && Index < After.Positions.Num(); ++Index) bGeometryIntact = After.Positions[Index].Equals(Soup.Positions[Index], 0.01f);
		TestTrue(TEXT("...over geometry the edit never touched"), bGeometryIntact);
	}

	// A game-thread edit still works on the same mesh afterwards, which is what says the streams were
	// handed back rather than left in the external access mode the render-thread edit put them in.
	if (TestTrue(TEXT("A game-thread edit follows a render-thread one"),
		GpuMesh->EditMeshSync([](FCSMeshEditContext& Context) { UCSMeshOps::AddSetCountersPass(Context, 9u, 9u); })))
	{
		TestEqual(TEXT("...and takes effect"), GpuMesh->GetTriangleCountSync(), Slots.Num());
	}

	// The game-thread state a render-thread edit must not publish. Nothing fences a write of these
	// against the game thread reading them, and there is no flush here to publish them behind.
	const int32 KnownVerticesBefore = Resident->KnownVertexCount;
	ENQUEUE_RENDER_COMMAND(CSGpuMeshTestsRenderThreadCounts)(
		[Resident](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGpuMeshTests.RenderThreadCounts"));
			{
				FCSMeshRenderThreadEdit Edit(GraphBuilder, *Resident);
				Edit->SetKnownCounts(12345, 12345);
			}
			GraphBuilder.Execute();
		});
	FlushRenderingCommands();
	TestEqual(TEXT("A render-thread edit cannot publish the counts the game thread reads"),
		Resident->KnownVertexCount, KnownVerticesBefore);
	return true;
}

// -----------------------------------------------------------------------------
// CS triangle data -> UCSMesh -> UDynamicMesh: the CPU producers' only way out
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshTriangleDataBridgeTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.TriangleDataBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshTriangleDataBridgeTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	const double NotANumber = std::numeric_limits<double>::quiet_NaN();

	// The NaN sits at index 0 on purpose: dropping it shifts every surviving vertex down by one,
	// so an index buffer that was not renumbered with it addresses past the end of the compacted
	// positions. That is the failure mode of this bridge, and it reads back as displaced geometry
	// rather than as a crash, so the geometry assertions below are what catch it.
	const FVector Quad[4] = {
		FVector(0, 0, 0), FVector(100, 0, 0), FVector(100, 100, 0), FVector(0, 100, 0) };
	TArray<FVector> Vertices = { FVector(NotANumber, 0, 0), Quad[0], Quad[1], Quad[2], Quad[3] };
	TArray<FVector> Normals;
	for (int32 Index = 0; Index < Vertices.Num(); ++Index) Normals.Add(FVector::UpVector);
	TArray<int32> Indices = {
		1, 2, 3,      // kept
		1, 3, 4,      // kept
		0, 1, 2,      // references the NaN vertex: dropped
		1, 2, 2 };    // repeated corner: dropped

	UCSMesh* GpuMesh = CSMeshBuild::BuildGpuMeshFromCSTriangleData(
		nullptr, World, Vertices, Indices, Normals, -1, -1, false, true, false);
	if (!TestNotNull(TEXT("The builder allocates a mesh when handed none"), GpuMesh)) return false;
	TestEqual(TEXT("Only the two valid triangles survive"), GpuMesh->GetTriangleCountSync(), 2);

	FCSGpuMeshCPUData Snapshot;
	if (!TestTrue(TEXT("Readback of the built mesh"), GpuMesh->ReadbackMeshSync(Snapshot))) return false;
	TestEqual(TEXT("Dropping the NaN vertex compacts the positions"), Snapshot.Positions.Num(), 4);

	bool bIndicesInRange = true;
	for (uint32 Index : Snapshot.Indices) bIndicesInRange &= Snapshot.Positions.IsValidIndex(int32(Index));
	TestTrue(TEXT("...and renumbers the indices with it"), bIndicesInRange);

	FBox Bounds(ForceInit);
	for (const FVector3f& Position : Snapshot.Positions) Bounds += FVector(Position);
	TestTrue(TEXT("The surviving geometry is the quad"),
		Bounds.IsValid && Bounds.Min.Equals(FVector(0, 0, 0), 0.01) && Bounds.Max.Equals(FVector(100, 100, 0), 0.01));

	// The point of the whole exercise: one bridge out. The GPU mesh converts to a DynamicMesh
	// through CopyToDynamicMesh rather than through a second builder.
	UDynamicMesh* DynamicMesh = UCSMeshOps::CopyToDynamicMesh(GpuMesh, nullptr, World, FCSMeshToDynamicMeshOptions());
	if (!TestNotNull(TEXT("The single bridge converts the built mesh"), DynamicMesh)) return false;
	DynamicMesh->ProcessMesh([this](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		TestEqual(TEXT("Both triangles reach the DynamicMesh"), Mesh.TriangleCount(), 2);
	});

	// bRecomputeNormals must ignore the supplied normals entirely. +Z was supplied above; a quad
	// wound the other way has to come back facing -Z, which a "keep what you were given" bug cannot.
	TArray<int32> ReversedIndices = { 0, 2, 1, 0, 3, 2 };
	UCSMesh* FlippedMesh = CSMeshBuild::BuildGpuMeshFromCSTriangleData(
		nullptr, World, TArray<FVector>(Quad, 4), ReversedIndices, Normals, -1, -1, false, true, true);
	FCSGpuMeshCPUData FlippedSnapshot;
	if (TestTrue(TEXT("Readback of the recomputed-normal mesh"), FlippedMesh && FlippedMesh->ReadbackMeshSync(FlippedSnapshot)))
	{
		bool bFacesDown = !FlippedSnapshot.Normals.IsEmpty();
		for (const FVector3f& Normal : FlippedSnapshot.Normals) bFacesDown &= Normal.Z < -0.5f;
		TestTrue(TEXT("Recomputed normals come from the winding, not from the input"), bFacesDown);
	}

	// Empty input empties the mesh instead of leaving the previous contents drawable.
	CSMeshBuild::BuildGpuMeshFromCSTriangleData(GpuMesh, World, TArray<FVector>(), TArray<int32>(), TArray<FVector>(),
		-1, -1, false, true, false);
	TestEqual(TEXT("Rebuilding from nothing empties the mesh"), GpuMesh->GetTriangleCountSync(), 0);
	return true;
}

// -----------------------------------------------------------------------------
// VDB particles -> UCSMesh (no scene, no generator state)
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuMeshVDBParticleTest,
	"PCGPlugins.ComputeShaderGenerator.GpuMeshObject.VDBParticles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGpuMeshVDBParticleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// A solid block of particles one voxel apart, so the isosurface is a closed blob rather than
	// a scatter of disconnected spheres.
	const float VoxelSize = 10.0f;
	TArray<FVector> Particles;
	for (int32 X = 0; X < 4; ++X) for (int32 Y = 0; Y < 4; ++Y) for (int32 Z = 0; Z < 4; ++Z) Particles.Add(FVector(X, Y, Z) * VoxelSize);

	UCSMesh* SmoothMesh = AComputeShaderMeshGenerator::VDBParticlesToGpuMesh(
		nullptr, World, Particles, VoxelSize, 2.0f, true);
	if (!TestNotNull(TEXT("The VDB bridge allocates a mesh when handed none"), SmoothMesh)) return false;
	const int32 SmoothTriangles = SmoothMesh->GetTriangleCountSync();
	TestTrue(TEXT("The particle blob meshes into geometry"), SmoothTriangles > 0);

	FCSGpuMeshCPUData SmoothSnapshot;
	if (TestTrue(TEXT("Readback of the smoothed isosurface"), SmoothMesh->ReadbackMeshSync(SmoothSnapshot)))
	{
		// Shared vertices are what "smooth" means here: a soup would have exactly 3 per triangle.
		TestTrue(TEXT("The smooth path shares vertices between triangles"),
			SmoothSnapshot.Positions.Num() < SmoothTriangles * 3);

		// The isosurface has to enclose the particles it was built from. Radius is
		// (Rmin+0.1)*VoxelSize*RadiusMult, so the blob is a good deal larger than the point cloud;
		// only the "did it land in the right place at all" half is worth asserting.
		FBox Bounds(ForceInit);
		for (const FVector3f& Position : SmoothSnapshot.Positions) Bounds += FVector(Position);
		FBox ParticleBounds(Particles);
		TestTrue(TEXT("The isosurface encloses the particles"), Bounds.IsValid && Bounds.IsInside(ParticleBounds));
	}

	// bRecomputeNormals=false keeps VDB's per-face normals, which is only representable as a soup:
	// a shared vertex carries one normal, and flat shading needs one per face.
	UCSMesh* FlatMesh = AComputeShaderMeshGenerator::VDBParticlesToGpuMesh(
		nullptr, World, Particles, VoxelSize, 2.0f, false);
	if (TestNotNull(TEXT("The flat-shaded VDB mesh exists"), FlatMesh))
	{
		FCSGpuMeshCPUData FlatSnapshot;
		if (TestTrue(TEXT("Readback of the flat-shaded isosurface"), FlatMesh->ReadbackMeshSync(FlatSnapshot)))
		{
			// The three corners of a triangle must agree on one normal. Tolerance covers the
			// snorm8888 packing the resident tangent stream uses (~1/127 per component).
			bool bFlatPerTriangle = FlatSnapshot.Indices.Num() >= 3;
			for (int32 Corner = 0; Corner + 2 < FlatSnapshot.Indices.Num(); Corner += 3)
			{
				const int32 I0 = int32(FlatSnapshot.Indices[Corner + 0]);
				const int32 I1 = int32(FlatSnapshot.Indices[Corner + 1]);
				const int32 I2 = int32(FlatSnapshot.Indices[Corner + 2]);
				if (!FlatSnapshot.Normals.IsValidIndex(I0) || !FlatSnapshot.Normals.IsValidIndex(I1) || !FlatSnapshot.Normals.IsValidIndex(I2))
				{
					bFlatPerTriangle = false;
					break;
				}
				bFlatPerTriangle &= FlatSnapshot.Normals[I0].Equals(FlatSnapshot.Normals[I1], 0.03f)
					&& FlatSnapshot.Normals[I1].Equals(FlatSnapshot.Normals[I2], 0.03f);
			}
			TestTrue(TEXT("Flat shading gives every triangle one normal across its corners"), bFlatPerTriangle);
		}
	}

	// No particles is not an error, and must not leave stale geometry behind.
	AComputeShaderMeshGenerator::VDBParticlesToGpuMesh(SmoothMesh, World, TArray<FVector>(), VoxelSize, 2.0f, true);
	TestEqual(TEXT("An empty particle set empties the mesh"), SmoothMesh->GetTriangleCountSync(), 0);
	return true;
}

#endif
