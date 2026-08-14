#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedMeshSceneProxy.h" // ECSGpuInstancedAuxSlot + the aux stream declaration
#include "CSMesh.h"

#include "UObject/Package.h"

namespace
{
	/** Smallest mesh that passes FCSGpuMeshCPUData::IsValid(): one triangle, per-vertex attributes. */
	FCSGpuMeshCPUData MakeUnitTriangle()
	{
		FCSGpuMeshCPUData Data;
		Data.Positions = { FVector3f(0.0f, 0.0f, 0.0f), FVector3f(100.0f, 0.0f, 0.0f), FVector3f(0.0f, 100.0f, 0.0f) };
		Data.Normals = { FVector3f::ZAxisVector, FVector3f::ZAxisVector, FVector3f::ZAxisVector };
		Data.Tangents = { FVector3f::XAxisVector, FVector3f::XAxisVector, FVector3f::XAxisVector };
		Data.TexCoordChannels[0] = { FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(0.0f, 1.0f) };
		Data.Indices = { 0, 1, 2 };
		Data.SourceSpace = FCSGpuMeshCPUData::ESpace::ComponentLocal;
		Data.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
		return Data;
	}

	UCSGpuInstancedMeshComponent* MakeComponent(int32 InstancesPerCluster)
	{
		UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Component->InstancesPerCluster = InstancesPerCluster;
		Component->SetBaseMeshFromGpuData(MakeUnitTriangle());
		return Component;
	}

	/** Rebuilds the instance-to-component matrix the way LocalVertexFactory.ush's
	 *  GetInstanceTransform() does, from the 5-float4 packed layout. */
	FMatrix44f UnpackInstanceTransform(const TArray<FVector4f>& Packed, int32 Slot)
	{
		const FVector4f R0 = Packed[Slot * 5 + 0];
		const FVector4f R1 = Packed[Slot * 5 + 1];
		const FVector4f R2 = Packed[Slot * 5 + 2];
		const FVector4f Origin = Packed[Slot * 5 + 3];
		return FMatrix44f(
			FPlane4f(R0.X, R0.Y, R0.Z, 0.0f),
			FPlane4f(R1.X, R1.Y, R1.Z, 0.0f),
			FPlane4f(R2.X, R2.Y, R2.Z, 0.0f),
			FPlane4f(Origin.X, Origin.Y, Origin.Z, 1.0f));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshPackingAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.Packing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The packed instance buffer is consumed by a shader we do not control: LocalVertexFactory.ush
// reads three float4 rows plus an origin and rebuilds the instance matrix from them. Get the row
// order or the .w slots wrong and nothing fails to compile - the instances just render in the
// wrong place, or (Transform1.w feeding GetInstanceSelected) vanish entirely. This pins the layout.
bool FCSGpuInstancedMeshPackingAutomationTest::RunTest(const FString& Parameters)
{
	UCSGpuInstancedMeshComponent* Component = MakeComponent(/*InstancesPerCluster*/ 64);
	if (!TestNotNull(TEXT("Component"), Component)) return false;

	TArray<FTransform> Sources;
	Sources.Add(FTransform(FRotator(0.0f, 45.0f, 0.0f), FVector(500.0, -200.0, 30.0), FVector(2.0, 2.0, 2.0)));
	Sources.Add(FTransform(FRotator(10.0f, 0.0f, -25.0f), FVector(-1000.0, 800.0, -50.0), FVector(1.0, 0.5, 3.0)));
	Sources.Add(FTransform::Identity);
	Component->SetInstances(Sources);

	TestEqual(TEXT("Instance count"), Component->GetInstanceCount(), Sources.Num());

	const TArray<FVector4f>& Packed = Component->GetPackedInstances();
	if (!TestEqual(TEXT("Packed float4 count"), Packed.Num(), Sources.Num() * 5)) return false;

	// Instances are stored in Morton order, so match each source by its origin rather than assuming
	// the array order survived the sort.
	for (const FTransform& Source : Sources)
	{
		const FVector3f ExpectedOrigin = FVector3f(Source.GetLocation());

		int32 FoundSlot = INDEX_NONE;
		for (int32 Slot = 0; Slot < Sources.Num(); ++Slot)
		{
			const FVector4f Origin = Packed[Slot * 5 + 3];
			if (FVector3f(Origin.X, Origin.Y, Origin.Z).Equals(ExpectedOrigin, 0.01f))
			{
				FoundSlot = Slot;
				break;
			}
		}
		if (!TestTrue(FString::Printf(TEXT("Instance at %s is present"), *Source.GetLocation().ToString()), FoundSlot != INDEX_NONE)) continue;

		const FMatrix44f Expected = FMatrix44f(Source.ToMatrixWithScale());
		TestTrue(TEXT("Unpacked instance matrix matches the source transform"),
			UnpackInstanceTransform(Packed, FoundSlot).Equals(Expected, 0.01f));

		// Transform1.w is hitproxy + 256 * selected in the shader; anything non-zero would make
		// GetInstanceSelected() report a selection this component never has.
		TestEqual(TEXT("Transform row 0 .w is zero"), Packed[FoundSlot * 5 + 0].W, 0.0f);
		TestEqual(TEXT("Transform row 1 .w is zero"), Packed[FoundSlot * 5 + 1].W, 0.0f);
		TestEqual(TEXT("Transform row 2 .w is zero"), Packed[FoundSlot * 5 + 2].W, 0.0f);

		// Origin.w is the material's PerInstanceRandom.
		const float Random = Packed[FoundSlot * 5 + 3].W;
		TestTrue(TEXT("Per-instance random is in [0,1)"), Random >= 0.0f && Random < 1.0f);

		// The cull sphere has to contain the transformed base mesh, or instances pop at the edges.
		const FVector4f Sphere = Packed[FoundSlot * 5 + 4];
		TestTrue(TEXT("Cull sphere has a positive radius"), Sphere.W > 0.0f);
		for (const FVector3f& P : Component->GetBaseMeshSnapshot().Positions)
		{
			const FVector3f World = FVector3f(Source.TransformPosition(FVector(P)));
			TestTrue(TEXT("Cull sphere contains every transformed base vertex"),
				(World - FVector3f(Sphere.X, Sphere.Y, Sphere.Z)).Size() <= Sphere.W + 0.01f);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshClusterAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.Clusters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The coarse cull level rejects a whole cluster in one test, so a cluster sphere that does not
// enclose its members would drop visible instances. The shader derives an instance's cluster as
// InstanceIndex / ClusterSize, which only holds while clusters are contiguous fixed-size runs -
// both invariants are checked here.
bool FCSGpuInstancedMeshClusterAutomationTest::RunTest(const FString& Parameters)
{
	constexpr int32 ClusterSize = 8;
	constexpr int32 NumInstances = 37; // deliberately not a multiple of ClusterSize

	UCSGpuInstancedMeshComponent* Component = MakeComponent(ClusterSize);
	if (!TestNotNull(TEXT("Component"), Component)) return false;

	TArray<FTransform> Sources;
	Sources.Reserve(NumInstances);
	for (int32 i = 0; i < NumInstances; ++i)
	{
		// Spread over a 3D lattice so the Morton sort actually has something to order.
		const FVector Location(double(i % 5) * 400.0, double((i / 5) % 5) * 400.0, double(i / 25) * 400.0);
		Sources.Add(FTransform(FRotator::ZeroRotator, Location, FVector(1.0 + 0.1 * i)));
	}
	Component->SetInstances(Sources);

	const TArray<FVector4f>& Packed = Component->GetPackedInstances();
	const TArray<FVector4f>& Clusters = Component->GetClusterBounds();

	const int32 ExpectedClusters = FMath::DivideAndRoundUp(NumInstances, ClusterSize);
	TestEqual(TEXT("Cluster count"), Clusters.Num(), ExpectedClusters);
	TestEqual(TEXT("Packed float4 count"), Packed.Num(), NumInstances * 5);

	for (int32 Instance = 0; Instance < NumInstances; ++Instance)
	{
		// Exactly the mapping InstanceCullCS uses.
		const int32 Cluster = FMath::Min(Instance / ClusterSize, Clusters.Num() - 1);

		const FVector4f Sphere = Packed[Instance * 5 + 4];
		const FVector4f ClusterSphere = Clusters[Cluster];
		const float CentreDistance = (FVector3f(Sphere.X, Sphere.Y, Sphere.Z) - FVector3f(ClusterSphere.X, ClusterSphere.Y, ClusterSphere.Z)).Size();

		TestTrue(FString::Printf(TEXT("Cluster %d encloses instance %d"), Cluster, Instance),
			CentreDistance + Sphere.W <= ClusterSphere.W + 0.01f);
	}

	// Clearing has to leave nothing behind for the proxy to allocate capacity from.
	Component->ClearInstances();
	TestEqual(TEXT("Cleared instance count"), Component->GetInstanceCount(), 0);
	TestEqual(TEXT("Cleared packed data"), Component->GetPackedInstances().Num(), 0);
	TestEqual(TEXT("Cleared clusters"), Component->GetClusterBounds().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshStreamsAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.Streams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// This leaf's seven aux streams live in a UCSMesh alongside the standard set, addressed by
// (Role, TexCoordIndex). The standard set owns AuxVertex slot 0 for its per-triangle material ids
// and offers no way to turn that off, so a leaf slot numbered from 0 collides — and a collision is
// not loud: SetStreamLayoutSync refuses the layout and returns false, after which the vertex factory
// is handed null instance SRVs. Nothing in the rendered result says which stream went missing.
//
// Declaring a layout on a mesh that has never been allocated is pure bookkeeping (no RHI, no render
// commands), so the whole contract can be pinned without a GPU.
bool FCSGpuInstancedMeshStreamsAutomationTest::RunTest(const FString& Parameters)
{
	FCSGpuInstancedGpuLayout Layout;
	Layout.NumLODs = 3;
	Layout.InstanceCapacity = 128;
	Layout.NumSourceInstances = 100;
	Layout.ClusterSize = 8;
	Layout.NumClusters = 13;

	TArray<FCSGpuStreamDesc> AuxStreams;
	CSGpuInstancedBuildAuxStreamDescs(AuxStreams, Layout, /*bExternalPackedSource*/ false);
	if (!TestEqual(TEXT("Aux stream count"), AuxStreams.Num(), 7)) return false;

	UCSMesh* Mesh = NewObject<UCSMesh>(GetTransientPackage());
	if (!TestNotNull(TEXT("Mesh"), Mesh)) return false;

	const FCSMeshResident* Resident = Mesh->GetResidentPtr();
	if (!TestNotNull(TEXT("Resident set"), Resident)) return false;
	// Counted rather than hard-coded: the point is that all seven survive on top of whatever the
	// standard set happens to be, not that the standard set has a particular size today.
	const int32 NumStandardStreams = Resident->Streams.Num();

	FCSMeshStreamLayout StreamLayout;
	StreamLayout.NumIndirectDraws = Layout.NumLODs;
	CSGpuInstancedBuildAuxStreamDescs(StreamLayout.ExtraStreams, Layout, /*bExternalPackedSource*/ false);
	if (!TestTrue(TEXT("The mesh accepts this leaf's stream layout"), Mesh->SetStreamLayoutSync(StreamLayout))) return false;

	TestEqual(TEXT("Every aux stream was appended to the standard set"), Resident->Streams.Num(), NumStandardStreams + 7);
	TestEqual(TEXT("One indirect arg set per LOD"), Mesh->GetIndirectDrawCount(), int32(Layout.NumLODs));

	// The material-id stream is what the leaf slots must not land on, so its survival is half the
	// assertion — a layout that displaced it would leave the operator layer without it.
	TestNotNull(TEXT("The standard material-id stream still owns AuxVertex slot 0"),
		Resident->FindStream(ECSGpuStreamRole::AuxVertex, 0));

	const ECSGpuInstancedAuxSlot Slots[] = {
		ECSGpuInstancedAuxSlot::SourceInstances,
		ECSGpuInstancedAuxSlot::ClusterBounds,
		ECSGpuInstancedAuxSlot::ClusterVisible,
		ECSGpuInstancedAuxSlot::VisibleTransforms,
		ECSGpuInstancedAuxSlot::VisibleOrigins,
		ECSGpuInstancedAuxSlot::VisibleLightmap,
		ECSGpuInstancedAuxSlot::LodCounters,
	};
	for (ECSGpuInstancedAuxSlot Slot : Slots)
	{
		TestTrue(FString::Printf(TEXT("Aux slot %u is clear of the standard set"), uint32(Slot)), uint8(Slot) != 0);
		TestNotNull(FString::Printf(TEXT("Aux slot %u resolves in the resident set"), uint32(Slot)),
			Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(Slot)));
	}

	// Each LOD draws out of its own fixed-size region and the vertex factory offsets into it by
	// Lod * InstanceCapacity, so a region short of the capacity would have the cull compacting
	// survivors into the next LOD's slots.
	const int32 VisibleSlots = int32(Layout.InstanceCapacity * Layout.NumLODs);
	auto AuxElementCount = [Resident](ECSGpuInstancedAuxSlot Slot)
	{
		const FCSMeshResident::FStream* Stream = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(Slot));
		return Stream ? int32(Stream->Desc.ElementsPerUnit) : 0;
	};
	TestEqual(TEXT("Visible origins cover every LOD region"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleOrigins), VisibleSlots);
	TestEqual(TEXT("Visible lightmap covers every LOD region"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleLightmap), VisibleSlots);
	TestEqual(TEXT("Visible transforms are three float4 per slot"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleTransforms), VisibleSlots * 3);
	// The source rows are five float4 per instance, over the capacity rather than the live count —
	// the buffer ratchets, so the cull's MaxSourceInstances is the capacity.
	TestEqual(TEXT("Source rows cover the instance capacity"), AuxElementCount(ECSGpuInstancedAuxSlot::SourceInstances), int32(Layout.InstanceCapacity) * 5);
	// Cluster spheres are sized from the capacity too, but only NumClusters of them are ever filled.
	TestTrue(TEXT("Cluster spheres cover the live cluster count"),
		AuxElementCount(ECSGpuInstancedAuxSlot::ClusterBounds) >= int32(Layout.NumClusters));

	// A packed GPU source brings its own row buffer, so the mesh must not size one for it as well —
	// at large instance counts that is tens of megabytes nothing ever reads.
	TArray<FCSGpuStreamDesc> ExternalSourceStreams;
	CSGpuInstancedBuildAuxStreamDescs(ExternalSourceStreams, Layout, /*bExternalPackedSource*/ true);
	for (const FCSGpuStreamDesc& Desc : ExternalSourceStreams)
	{
		if (Desc.TexCoordIndex != uint8(ECSGpuInstancedAuxSlot::SourceInstances)) continue;
		TestEqual(TEXT("An external packed source leaves only a placeholder behind"), int32(Desc.ElementsPerUnit), 1);
	}

	// Nothing may declare zero bytes: the mesh refuses such a stream, which takes the whole layout
	// with it and leaves the component unallocated for a reason no log connects to the empty slot.
	for (const FCSGpuStreamDesc& Desc : AuxStreams)
	{
		TestTrue(TEXT("Every aux stream allocates something"), Desc.BytesPerElement > 0 && Desc.ElementsPerUnit > 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshStreamResizeAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.StreamResize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Another instance changes six of this leaf's aux streams and nothing else. Saying so used to mean
// re-declaring the whole layout, which reallocates and copies every resident stream — the base mesh
// included — so an AddInstance copied the entire mesh to make one buffer bigger.
// UCSMesh::ResizeStreamsSync is what the component uses instead, and this pins the bookkeeping half
// of it: that a capacity change lands exactly where CSGpuInstancedBuildAuxStreamDescs says it should,
// that the standard set and the draw layout do not move with it, and that the resident set ends up
// byte-identical to what a re-declaration at that capacity would have asked for — which is precisely
// the condition under which the component's next rebuild costs nothing.
//
// Declaring and resizing on a mesh that has never been allocated is pure bookkeeping (no RHI, no
// render commands), the same reason the Streams suite above needs no GPU.
bool FCSGpuInstancedMeshStreamResizeAutomationTest::RunTest(const FString& Parameters)
{
	FCSGpuInstancedGpuLayout Layout;
	Layout.NumLODs = 3;
	Layout.InstanceCapacity = 128;
	Layout.NumSourceInstances = 100;
	Layout.ClusterSize = 8;
	Layout.NumClusters = 13;

	UCSMesh* Mesh = NewObject<UCSMesh>(GetTransientPackage());
	if (!TestNotNull(TEXT("Mesh"), Mesh)) return false;

	FCSMeshStreamLayout StreamLayout;
	StreamLayout.NumIndirectDraws = Layout.NumLODs;
	CSGpuInstancedBuildAuxStreamDescs(StreamLayout.ExtraStreams, Layout, /*bExternalPackedSource*/ false);
	if (!TestTrue(TEXT("The mesh accepts this leaf's stream layout"), Mesh->SetStreamLayoutSync(StreamLayout))) return false;

	const FCSMeshResident* Resident = Mesh->GetResidentPtr();
	if (!TestNotNull(TEXT("Resident set"), Resident)) return false;

	// Whatever the standard set happens to be today, verbatim: the resize must not touch a single
	// element of it, and the aux slots start at 16 precisely so it cannot.
	TArray<uint32> ElementsBefore;
	for (const FCSMeshResident::FStream& Stream : Resident->Streams) ElementsBefore.Add(Stream.Desc.ElementsPerUnit);
	const int32 StreamCountBefore = Resident->Streams.Num();

	// The instance set grew past its capacity; everything else about the component is unchanged.
	FCSGpuInstancedGpuLayout Grown = Layout;
	Grown.InstanceCapacity = 512;
	Grown.NumSourceInstances = 400;
	Grown.NumClusters = 50;

	TArray<FCSGpuStreamDesc> WantedAux;
	CSGpuInstancedBuildAuxStreamDescs(WantedAux, Grown, /*bExternalPackedSource*/ false);

	TArray<FCSMeshStreamResize> Resizes;
	for (const FCSGpuStreamDesc& Desc : WantedAux)
	{
		FCSMeshStreamResize& Resize = Resizes.AddDefaulted_GetRef();
		Resize.Role = Desc.Role;
		Resize.SlotIndex = Desc.TexCoordIndex;
		Resize.ElementCount = Desc.ElementsPerUnit;
	}
	if (!TestTrue(TEXT("A capacity change goes through as a per-stream resize"), Mesh->ResizeStreamsSync(Resizes))) return false;

	TestEqual(TEXT("A resize adds and removes no streams"), Resident->Streams.Num(), StreamCountBefore);
	for (const FCSGpuStreamDesc& Desc : WantedAux)
	{
		const FCSMeshResident::FStream* Stream = Resident->FindStream(Desc.Role, Desc.TexCoordIndex);
		if (!TestNotNull(*FString::Printf(TEXT("Aux slot %u survives the resize"), uint32(Desc.TexCoordIndex)), Stream)) continue;
		// Byte-identical to a fresh declaration at the grown capacity — which is what makes the
		// component's next SetStreamLayoutSync a no-op instead of a whole-mesh reallocation.
		TestEqual(*FString::Printf(TEXT("Aux slot %u is sized for the grown capacity"), uint32(Desc.TexCoordIndex)),
			int32(Stream->Desc.ElementsPerUnit), int32(Desc.ElementsPerUnit));
	}

	// The visible-instance regions are what the vertex factory strides through by Lod * capacity,
	// so a region short of the new capacity would have the cull compacting into the next LOD's slots.
	const int32 GrownVisibleSlots = int32(Grown.InstanceCapacity * Grown.NumLODs);
	auto AuxElementCount = [Resident](ECSGpuInstancedAuxSlot Slot)
	{
		const FCSMeshResident::FStream* Stream = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(Slot));
		return Stream ? int32(Stream->Desc.ElementsPerUnit) : 0;
	};
	TestEqual(TEXT("Visible origins grew with the capacity"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleOrigins), GrownVisibleSlots);
	TestEqual(TEXT("Visible transforms grew with the capacity"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleTransforms), GrownVisibleSlots * 3);
	TestEqual(TEXT("Source rows grew with the capacity"), AuxElementCount(ECSGpuInstancedAuxSlot::SourceInstances), int32(Grown.InstanceCapacity) * 5);
	// Fixed at CS_GPU_INSTANCED_MAX_LODS on purpose, so it never appears in a resize at all.
	TestEqual(TEXT("The LOD counters do not follow the instance capacity"),
		AuxElementCount(ECSGpuInstancedAuxSlot::LodCounters), CS_GPU_INSTANCED_MAX_LODS);

	// The half that used to be collateral damage.
	const uint8 AuxSlotFloor = uint8(ECSGpuInstancedAuxSlot::SourceInstances);
	bool bStandardSetIntact = Resident->Streams.Num() == ElementsBefore.Num();
	for (int32 Index = 0; bStandardSetIntact && Index < Resident->Streams.Num(); ++Index)
	{
		const FCSGpuStreamDesc& Desc = Resident->Streams[Index].Desc;
		if (Desc.Role == ECSGpuStreamRole::AuxVertex && Desc.TexCoordIndex >= AuxSlotFloor) continue;
		bStandardSetIntact = Desc.ElementsPerUnit == ElementsBefore[Index];
	}
	TestTrue(TEXT("An instance-capacity change leaves the standard streams untouched"), bStandardSetIntact);
	TestNotNull(TEXT("The material-id stream still owns AuxVertex slot 0"),
		Resident->FindStream(ECSGpuStreamRole::AuxVertex, 0));
	TestEqual(TEXT("An instance-capacity change leaves the draw layout alone"),
		Mesh->GetIndirectDrawCount(), int32(Layout.NumLODs));

	// Re-declaring at the capacity the resize arrived at is the path RebuildGpuMesh takes on every
	// rebuild after the first, so it has to be accepted and leave the set alone. That it also costs
	// no reallocation only shows on an allocated mesh, which is what GpuMeshObject.StreamResize pins.
	FCSMeshStreamLayout GrownLayout;
	GrownLayout.NumIndirectDraws = Grown.NumLODs;
	CSGpuInstancedBuildAuxStreamDescs(GrownLayout.ExtraStreams, Grown, /*bExternalPackedSource*/ false);
	TestTrue(TEXT("Declaring the layout the resize arrived at is accepted"), Mesh->SetStreamLayoutSync(GrownLayout));
	TestEqual(TEXT("...and changes nothing about the stream set"), Resident->Streams.Num(), StreamCountBefore);
	TestEqual(TEXT("...nor about the sizes the resize established"),
		AuxElementCount(ECSGpuInstancedAuxSlot::VisibleOrigins), GrownVisibleSlots);

	// Shrinking back is the same call: hysteresis is ResolveInstanceCapacity's job, not the mesh's.
	TArray<FCSMeshStreamResize> Shrink;
	FCSMeshStreamResize& ShrinkOrigins = Shrink.AddDefaulted_GetRef();
	ShrinkOrigins.Role = ECSGpuStreamRole::AuxVertex;
	ShrinkOrigins.SlotIndex = uint8(ECSGpuInstancedAuxSlot::VisibleOrigins);
	ShrinkOrigins.ElementCount = uint32(Layout.InstanceCapacity * Layout.NumLODs);
	if (TestTrue(TEXT("Shrinking a stream back uses the same entry point"), Mesh->ResizeStreamsSync(Shrink)))
	{
		TestEqual(TEXT("A shrink is exact, not rounded up"),
			AuxElementCount(ECSGpuInstancedAuxSlot::VisibleOrigins), int32(Layout.InstanceCapacity * Layout.NumLODs));
	}

	// The standard stream this leaf's slot numbering exists to protect. It is per-triangle, so its
	// size follows the mesh capacity and a resize of it would corrupt the readback's per-face stride.
	TestFalse(TEXT("The standard material-id stream cannot be resized this way"),
		Mesh->ResizeStreamSync(ECSGpuStreamRole::AuxVertex, 0, 32));
	TestFalse(TEXT("Neither can the per-LOD indirect args"),
		Mesh->ResizeStreamSync(ECSGpuStreamRole::IndirectArgs, 0, 5 * 4));
	TestEqual(TEXT("A refused resize leaves the draw layout alone"), Mesh->GetIndirectDrawCount(), int32(Layout.NumLODs));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
