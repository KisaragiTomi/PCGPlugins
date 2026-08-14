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

	/** The transform the current geometry was baked out of — the last InputToWorld handed to
	 *  SetBuildInput. Place the saved StaticMesh here and it lands back on the drawn road.
	 *
	 *  存盘走基类的 UCSMeshRenderComponent::SaveToStaticMesh，把这个变换作为 BakeSpace 传进去；
	 *  道路这边没有自己的落盘实现，它和"存这个组件画的东西"没有任何区别。 */
	const FTransform& GetGeometryToWorld() const { return GeometryToWorld; }

private:
	FTransform GeometryToWorld = FTransform::Identity;
};
