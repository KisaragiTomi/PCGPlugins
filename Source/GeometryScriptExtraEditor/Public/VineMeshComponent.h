#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "VineMeshComponent.generated.h"

class UMaterialInterface;
class UStaticMesh;

// Self-owning CPU-prep bundle filled by AVineContainer::GenerateVineGPU.
// Defined at global scope in GeometryEditorActor.cpp — same forward-declaration pattern as
// FVineFusedSCInputs. Held here through a TUniquePtr (PIMPL) so this public header stays free of
// the internal RDG / voxel types the bundle owns.
struct FVineBuildInput;

/**
 * Renders a GPU-generated vine mesh directly through the render pipeline. This IS the vine — the
 * older UDynamicMesh path it once ran beside is gone. The owning AVineContainer feeds it a CPU-prep
 * bundle; the shared vine compute passes then emit straight into the base-owned persistent GPU
 * streams (positions/tangents/texcoords/colors/indices + indirect args + counters) and the result
 * is drawn every frame — vertex/index data never returns to the CPU (except an explicit
 * save-to-StaticMesh).
 *
 * All the generic GPU-mesh draw plumbing lives in UCSGpuMeshComponent / FCSGpuMeshSceneProxy;
 * this leaf only adds the vine material and the build-input hand-off. The mesh renders in world
 * space (the component is kept at an identity world transform and VineWorldToLocal is Identity).
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class GEOMETRYSCRIPTEXTRAEDITOR_API UVineMeshComponent : public UCSGpuMeshComponent
{
	GENERATED_BODY()

public:
	UVineMeshComponent();
	// Out-of-line so the compiler instantiates TUniquePtr<FVineBuildInput>'s destructor where the
	// bundle type is complete (GeometryEditorActor.cpp), not in the UHT-generated TU.
	virtual ~UVineMeshComponent() override;
	// UHT would otherwise auto-define the vtable-helper constructor inside VineMeshComponent.gen.cpp,
	// where FVineBuildInput is only forward-declared — instantiating TUniquePtr<FVineBuildInput>'s
	// destructor on an incomplete type. Declaring it here forces the definition into the .cpp where
	// the bundle is complete. Standard UE PIMPL pattern.
	UVineMeshComponent(FVTableHelper& Helper);

	/** Material used for the whole vine surface. Null falls back to the default surface material.
	 *  Transient mirror, not the place to author it: AVineContainer::VineMaterial owns the
	 *  assignment and pushes it down here (on load, on edit and on every generation), so a value
	 *  written straight onto the component would be overwritten and never saved. */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Transient, Category = "Vine")
	TObjectPtr<UMaterialInterface> VineMaterial;

	/** Hand the component a new GPU-prep bundle (moved) and kick a synchronous proxy rebuild. */
	void SetBuildInput(FVineBuildInput&& Input);

#if WITH_EDITOR
	/** Reads the rendered vine mesh back to the CPU and saves it as a StaticMesh asset via the
	 *  shared CSGpuMeshConvert path. Vine vertices are already in world space, so
	 *  bConvertToActorLocalSpace defaults to false. Editor only. */
	UStaticMesh* SaveToStaticMesh(const FString& AssetPathAndName = TEXT(""), bool bReplaceExistingAsset = true,
		bool bSaveAsset = false, bool bConvertToActorLocalSpace = false);
#endif

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

protected:
	//~ UCSGpuMeshComponent interface
	virtual UMaterialInterface* GetRenderMaterial() const override { return VineMaterial; }

private:
	// Snapshot taken by CreateSceneProxy; a rebuild recreates the proxy. Not a UPROPERTY
	// (holds render resources / raw arrays); PIMPL keeps the internal bundle type out of this header.
	TUniquePtr<FVineBuildInput> PendingInput;
};
