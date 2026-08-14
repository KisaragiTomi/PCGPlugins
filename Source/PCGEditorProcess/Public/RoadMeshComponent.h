#pragma once

#include "CoreMinimal.h"
#include "CSMeshRenderComponent.h"
#include "RoadTypes.h"
#include "RoadMeshComponent.generated.h"

class UCSMesh;
class UStaticMesh;

/**
 * Renders a GPU-generated road network.
 * The owning actor feeds it resampled spline data via SetBuildInput(); mesh generation runs
 * entirely in compute shaders and the result is drawn through an indirect draw — vertex/index
 * data never returns to the CPU.
 *
 * The geometry lives in a UCSMesh this component owns, not in its scene proxy. That is the whole
 * point of the class now: a render-state recreation (transform edit, visibility toggle, a
 * construction rerun) is a rebind rather than a full re-run of the road compute, and the road can
 * be saved whether or not anything is currently drawing it. Regenerating is the owner's call —
 * ACSLandscapeRoad::EnsureRoadGeometry() / RebuildRoad() — and nothing else's.
 *
 * The resident data is world space, so the base renders with an absolute transform: this
 * component's own placement does not move the road. Three consequences worth stating, because
 * every one of them is silent when got wrong:
 *   - SetBuildInput takes the space its samples are in as an explicit argument. It cannot be
 *     derived from the component, whose transform is now identity by construction.
 *   - GetComponentTransform() is therefore useless for baking the save back to local space;
 *     GetGeometryToWorld() is what that transform is.
 *   - Moving the owning actor no longer moves the road by itself. The geometry is baked, so a
 *     rebuild is what re-bakes it — in the editor that is the construction rerun the move
 *     already triggers. This is inherent to a world-space resident set, and the same trade the
 *     display path made; it is not a bug to be papered over with a component transform.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class PCGEDITORPROCESS_API URoadMeshComponent : public UCSMeshRenderComponent
{
	GENERATED_BODY()

public:
	/**
	 * Build the road from a spline snapshot and draw it. Synchronous: the geometry exists by the
	 * time this returns, so a save right after it needs no round trip through the renderer.
	 *
	 * InputToWorld is the space Input.Points are expressed in — the caller resampled them into it
	 * and is the only thing that knows which one it was. The geometry is baked into world space
	 * with it, and remembered so SaveToStaticMesh can bake back out.
	 *
	 * The road draws with MeshMaterial (one batch, no sections — the whole surface is one
	 * material) and the mesh's material table gets that same material as its only slot, which is
	 * what the saved asset's single material slot comes from.
	 *
	 * A build that cannot run — no splines, an RHI below SM5, a refused allocation — clears the
	 * display rather than leaving the previous road on screen next to splines it no longer matches.
	 */
	void SetBuildInput(const FRoadBuildInput& Input, const FTransform& InputToWorld);

	/** Whether road geometry already exists here — the question ACSLandscapeRoad::EnsureRoadGeometry()
	 *  asks before deciding to run a generation.
	 *
	 *  Asked of the mesh object, which is what owns the geometry: "is there a scene proxy" stopped
	 *  being evidence of anything the moment a recreation became a rebind, and the input snapshot it
	 *  used to consult is not kept any more. Note this answers about the geometry, not about whether
	 *  it is currently on screen — the component owns its road mesh and separately binds it for
	 *  drawing, so an inherited SetGpuMesh(nullptr) would stop the draw without losing the road. */
	bool HasGeneratedGeometry() const;

	/** The transform the current geometry was baked out of — the last InputToWorld handed to
	 *  SetBuildInput. Place the saved StaticMesh here and it lands back on the drawn road. */
	const FTransform& GetGeometryToWorld() const { return GeometryToWorld; }

#if WITH_EDITOR
	/** Reads the road mesh back and saves it as a StaticMesh asset via the shared CSGpuMeshConvert
	 *  path. The asset is baked into GetGeometryToWorld()'s local space, so it reproduces the road
	 *  when placed on that transform — the same asset the pre-UCSMesh path produced, which wrote
	 *  the raw (then already local-space) GPU positions.
	 *
	 *  Needs nothing to be rendering the mesh. Leave AssetPathAndName empty to default to an
	 *  "AutoResult" folder next to the current level (/Game/.../AutoResult/SM_<owner>). The asset is
	 *  marked dirty; bSaveAsset=false (default) leaves it unsaved for a manual Save. Editor only. */
	UStaticMesh* SaveToStaticMesh(const FString& AssetPathAndName = TEXT(""), bool bReplaceExistingAsset = true,
		bool bSaveAsset = false);
#endif

private:
	/** The road geometry. Transient because GPU data does not survive a level reload; the property
	 *  exists to hold the object against GC, not to serialize it. */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> RoadGpuMesh;

	FTransform GeometryToWorld = FTransform::Identity;
};
