#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "RoadTypes.h"
#include "RoadMeshComponent.generated.h"

class FRoadMeshSceneProxy;

/**
 * Renders a GPU-generated road network.
 * The owning actor feeds it resampled spline data via SetBuildInput();
 * mesh generation runs entirely in compute shaders and the result is drawn
 * through an indirect draw — vertex/index data never returns to the CPU.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class PCGEDITORPROCESS_API URoadMeshComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	URoadMeshComponent();

	/** Material used for the whole road surface (roads + intersections). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	TObjectPtr<UMaterialInterface> RoadMaterial;

	/** Hand the component a new spline snapshot and kick a GPU rebuild. */
	void SetBuildInput(FRoadBuildInput&& Input);

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

private:
	// Snapshot taken by CreateSceneProxy; a rebuild recreates the proxy.
	FRoadBuildInput PendingInput;
	FBox LocalBounds = FBox(ForceInit);
};
