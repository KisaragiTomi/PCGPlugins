#include "RoadMeshComponent.h"
#include "RoadBuilderShaders.h"

#include "CSMesh.h"
#include "CSMeshOps.h"

void URoadMeshComponent::SetBuildInput(const FRoadBuildInput& Input, const FTransform& InputToWorld)
{
	if (!RoadGpuMesh) RoadGpuMesh = NewObject<UCSMesh>(this);

	// One entry, not a section table: the whole road surface is one material, and
	// BuildMaterialSections would sort the index buffer and grow the indirect args to split it
	// into exactly one run. The table and MeshMaterial have different jobs — this is where the
	// saved asset's material slot comes from, MeshMaterial is what the single draw batch uses.
	RoadGpuMesh->SetMaterial(0, MeshMaterial);

	GeometryToWorld = InputToWorld;
	if (!BuildRoadGeometryIntoMesh(RoadGpuMesh, Input, InputToWorld))
	{
		// Nothing was built, so what the mesh still holds is the *previous* road. Leaving it drawn
		// next to splines it no longer matches is the worst outcome: it looks like a successful
		// build. Release rather than merely unbind — the allocation was sized for the road that is
		// now gone, and a refused build is exactly when the VRAM is worth handing back.
		//
		// Unbind first, in that order: the proxy borrows the resident buffers, so freeing them
		// while it is still bound leaves it drawing from an index buffer that no longer exists.
		SetGpuMesh(nullptr);
		const FCSMeshResident* Resident = RoadGpuMesh->GetResidentPtr();
		if (Resident && Resident->IsAllocated()) RoadGpuMesh->ReleaseSync();
		return;
	}
	SetGpuMesh(RoadGpuMesh);
}

bool URoadMeshComponent::HasGeneratedGeometry() const
{
	// IsEmpty() answers from the game thread without touching the GPU, and reports non-empty when
	// only the GPU knows the counts — which is always the road's case, since the emitted size
	// depends on the junctions the build finds. That is deliberately not "the road definitely has
	// triangles": the honest cheap answer is "there is an allocation holding a build's output", and
	// paying a GPU stall to sharpen it would put one on every EnsureRoadGeometry() call.
	return RoadGpuMesh && !RoadGpuMesh->IsEmpty();
}

#if WITH_EDITOR
UStaticMesh* URoadMeshComponent::SaveToStaticMesh(const FString& AssetPathAndName, bool bReplaceExistingAsset, bool bSaveAsset)
{
	if (!RoadGpuMesh) return nullptr;

	FCSMeshToStaticMeshOptions Options;
	Options.AssetPath = AssetPathAndName.TrimStartAndEnd();
	Options.bTransient = false; // this entry point's whole purpose is to produce an asset
	Options.bReplaceExisting = bReplaceExistingAsset;
	Options.bSaveToDisk = bSaveAsset;
	// Bake back out of the space the geometry was built in, so the asset holds the same local-space
	// positions the pre-UCSMesh path wrote and still reproduces the road when placed there.
	// GetComponentTransform() would be wrong and quietly so: this component renders with an
	// absolute transform, which makes its component transform identity, and baking with identity
	// would freeze the road at its world coordinates.
	Options.TargetTransform = GeometryToWorld;
	Options.bBakeToLocalSpace = true;

	// Reads the mesh object, not a scene proxy: a road that is hidden or not currently rendered
	// saves exactly the same way.
	return UCSMeshOps::CopyToStaticMesh(RoadGpuMesh, this, GetOwner(), Options);
}
#endif
