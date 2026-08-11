#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuInstancedMeshComponent.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
