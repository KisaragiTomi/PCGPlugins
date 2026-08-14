#pragma once

#include "CoreMinimal.h"
#include "CSMeshRenderComponent.h"
#include "VineMeshComponent.generated.h"

class UCSMesh;

/**
 * Draws the GPU-generated vine mesh. This IS the vine — the older UDynamicMesh path it once ran
 * beside is gone — but it no longer *builds* one: the geometry lives in a UCSMesh that
 * AVineContainer owns, and this component only binds it.
 *
 * That inversion is the whole point of the class now. The vine used to own its buffers through
 * FVineMeshSceneProxy, which meant every render-state recreation (a material edit, a visibility
 * toggle, a transform change) re-ran the surface voxelization and the space-colonization solve —
 * by far the most expensive thing this actor does. A recreation is now a rebind, and the solve runs
 * exactly when AVineContainer asks for a vine (GenerateVineGPU / EnsureVineGeometry) and never
 * otherwise.
 *
 * The vine surface is one material, so there is no section table: the base draws one batch out of
 * indirect arg set 0 with MeshMaterial, which AVineContainer::VineMaterial is pushed down into.
 *
 * The resident data is world space, so the base renders with an absolute transform — this
 * component's own placement does not move the vine, and moving the owning actor does not carry the
 * geometry with it (a regeneration is what re-bakes it). Same trade the road and display paths made.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class GEOMETRYSCRIPTEXTRAEDITOR_API UVineMeshComponent : public UCSMeshRenderComponent
{
	GENERATED_BODY()

public:
	/** Whether vine geometry already exists here — the question AVineContainer::EnsureVineGeometry()
	 *  asks before deciding to run a generation.
	 *
	 *  Asked of the mesh object, which is what owns the geometry. "Is there a scene proxy" stopped
	 *  being evidence of anything the moment a recreation became a rebind, and the build bundle this
	 *  used to consult is gone — it is consumed by the build operator and dropped.
	 *
	 *  Note this answers about the geometry, not about whether it is currently on screen: an
	 *  inherited SetGpuMesh(nullptr) would stop the draw without the owner losing its vine. */
	bool HasGeneratedGeometry() const;
};
