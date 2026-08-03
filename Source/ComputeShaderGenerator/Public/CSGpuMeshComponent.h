#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGpuMeshComponent.generated.h"

class UMaterialInterface;

/**
 * Abstract base for components that draw a GPU-generated mesh directly through the
 * render pipeline via an FCSGpuMeshSceneProxy subclass. Owns the shared local-space
 * bounds and material reporting; leaves implement CreateSceneProxy() and provide the
 * render material through GetRenderMaterial().
 */
UCLASS(Abstract, ClassGroup = Rendering)
class COMPUTESHADERGENERATOR_API UCSGpuMeshComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UCSGpuMeshComponent();

	//~ UPrimitiveComponent interface
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

	/** Explicitly reads the currently rendered GPU mesh back to CPU memory. Blocks the game
	 *  thread (FlushRenderingCommands + GPU stalls) and is intended for asset saving only;
	 *  normal rendering stays GPU-only. Works for any FCSGpuMeshSceneProxy leaf (soup or
	 *  indexed): vertex streams are sized to the GPU vertex count and indices to the index
	 *  count, both read from the proxy's MeshCounters buffer. */
	bool ReadbackMeshSync(FCSGpuMeshCPUData& OutMeshData) const;

protected:
	/** Render material reported to GetUsedMaterials and used by the scene proxy.
	 *  Leaves back this with their own UPROPERTY (e.g. RoadMaterial) to avoid changing
	 *  serialized data. Null falls back to the default surface material in the proxy. */
	virtual UMaterialInterface* GetRenderMaterial() const { return nullptr; }

	// Local-space bounds used by CalcBounds; leaves update this when their geometry changes.
	FBox LocalBounds = FBox(ForceInit);
};
