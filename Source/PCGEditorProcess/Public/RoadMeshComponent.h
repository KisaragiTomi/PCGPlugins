#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "RoadTypes.h"
#include "RoadMeshComponent.generated.h"

class FRoadMeshSceneProxy;
class UStaticMesh;

/**
 * Renders a GPU-generated road network.
 * The owning actor feeds it resampled spline data via SetBuildInput();
 * mesh generation runs entirely in compute shaders and the result is drawn
 * through an indirect draw — vertex/index data never returns to the CPU.
 *
 * The generic GPU-mesh draw plumbing lives in UCSGpuMeshComponent /
 * FCSGpuMeshSceneProxy; this class only adds the road material and spline input.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class PCGEDITORPROCESS_API URoadMeshComponent : public UCSGpuMeshComponent
{
	GENERATED_BODY()

public:
	URoadMeshComponent();

	/** Material used for the whole road surface (roads + intersections). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road")
	TObjectPtr<UMaterialInterface> RoadMaterial;

	/** Hand the component a new spline snapshot and kick a GPU rebuild. */
	void SetBuildInput(FRoadBuildInput&& Input);

#if WITH_EDITOR
	/** Reads the rendered road mesh back to the CPU and saves it as a StaticMesh asset via the
	 *  shared CSGpuMeshConvert path. Road vertices are already in the component's local space, so
	 *  bConvertToActorLocalSpace defaults to false (the asset reproduces the road when placed at
	 *  this component's transform). Leave AssetPathAndName empty to default to an "AutoResult" folder next
	 *  to the current level (/Game/.../AutoResult/SM_<owner>). The asset is marked dirty; bSaveAsset=false
	 *  (default) leaves it unsaved for a manual Save. Editor only. */
	UStaticMesh* SaveToStaticMesh(const FString& AssetPathAndName = TEXT(""), bool bReplaceExistingAsset = true,
		bool bSaveAsset = false, bool bConvertToActorLocalSpace = false);
#endif

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

protected:
	//~ UCSGpuMeshComponent interface
	virtual UMaterialInterface* GetRenderMaterial() const override { return RoadMaterial; }

private:
	// Snapshot taken by CreateSceneProxy; a rebuild recreates the proxy.
	FRoadBuildInput PendingInput;
};
