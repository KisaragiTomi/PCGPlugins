#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderLandscape.h"
#include "ComputeShaderLandscapeRoad.generated.h"

class URoadMeshComponent;
class USplineComponent;
class UMaterialInterface;
struct FRoadBuildInput;

/**
 * A CS-Landscape edit-layer actor that also owns a spline road generator. It collects spline
 * components (its own + attached child actors), builds the road network on the GPU via the shared
 * road compute pipeline, renders it through a URoadMeshComponent, and keeps a resident render
 * target (RT_RoadHeight) holding the road surface height for downstream landscape blending.
 *
 * Inherits AComputeShaderMeshGenerator (through ACSLandscape), so the road triangles are turned
 * into a heightmap by reusing FExtractStaticMeshTrianglesCS -> RasterizeTriangleSoupToHeightmapRDG.
 */
UCLASS()
class PCGEDITORPROCESS_API ACSLandscapeRoad : public ACSLandscape
{
	GENERATED_BODY()

public:
	ACSLandscapeRoad();

	// --- Road shape (mirrors ARoadGeneratorActor) ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Rendering")
	TObjectPtr<UMaterialInterface> RoadMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Shape", meta = (ClampMin = "10.0", Units = "cm"))
	float RoadWidth = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Shape", meta = (ClampMin = "10.0", Units = "cm"))
	float SampleStep = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Shape", meta = (ClampMin = "0.5"))
	float IntersectionMergeFactor = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Rendering", meta = (ClampMin = "1.0", Units = "cm"))
	float UVTileLength = 1000.0f;

	/** Resident road-height render target (R = CameraHeight - WorldZ, matching the soup rasterizer).
	 *  Sized to the landscape's sampling density over the road bounds (1:1 with the heightmap). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Road|Heightmap")
	TObjectPtr<UTextureRenderTarget2D> RT_RoadHeight;

	// --- How the road heightmap deforms the terrain (all Blueprint-settable) ---

	/** How strongly the terrain conforms to the road (0 = no effect, 1 = full conform). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Terrain", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RoadInfluence = 1.0f;

	/** Raise (+) or lower (-) the road relative to its geometry, in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Terrain", meta = (Units = "cm"))
	float RoadHeightOffset = 0.0f;

	/** Feathered shoulder width in cm: the terrain ramps to the road height over this distance
	 *  around the road edge (0 = hard edge, higher = softer/wider blend into the terrain). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road|Terrain", meta = (ClampMin = "0.0", Units = "cm"))
	float RoadEdgeFalloff = 0.0f;

	/** Re-collect splines, rebuild the road mesh, and refresh RT_RoadHeight. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Road")
	void RebuildRoad();

	/** Idempotent "the road I own must exist" entry point: rebuilds only when there is no road
	 *  geometry, and returns whether there is one by the time the call is over. Safe to call from
	 *  any lifecycle event and any number of times — the whole point is that the owner, not a
	 *  render-state recreation, decides when the road is generated.
	 *
	 *  Now that URoadMeshComponent draws a UCSMesh, a render-state recreation is a rebind and
	 *  regenerates nothing: this and RebuildRoad() are the only two things that produce road
	 *  geometry, and on a level load this is the one that runs. */
	UFUNCTION(BlueprintCallable, Category = "Road")
	bool EnsureRoadGeometry();

	/** The resident road-height RT for downstream landscape editing. */
	UFUNCTION(BlueprintPure, Category = "Road|Heightmap")
	UTextureRenderTarget2D* GetRoadHeightRT() const { return RT_RoadHeight; }

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;

private:
	/** Queues one EnsureRoadGeometry() for the next engine tick. See the rationale on the
	 *  definition for why the restore is hooked here and why it is not run inline. */
	void ScheduleEnsureRoadGeometry();

	/** Latched at the first schedule and never cleared: the automatic restore is one shot per actor
	 *  lifetime. Re-arming it on every re-registration would stack tickers, and — because a
	 *  generation that keeps failing leaves the geometry absent — would retry the whole heavy path
	 *  every frame. Anything past the first attempt is the owner's call (RebuildRoad /
	 *  EnsureRoadGeometry are both public). */
	bool bRoadGeometryRestoreAttempted = false;

	UPROPERTY(VisibleAnywhere, Category = "Road")
	TObjectPtr<URoadMeshComponent> RoadMesh;

	void CollectSplines(TArray<USplineComponent*>& OutSplines) const;

	/** Resamples the collected splines into a FRoadBuildInput; false when there are no usable splines. */
	bool BuildRoadInput(FRoadBuildInput& OutInput) const;

	/** (Re)creates RT_RoadHeight at RoadHeightRTSize with UAV support. */
	void EnsureRoadHeightRT();

	/** GPU: build the road geometry then rasterize it into RT_RoadHeight (render-thread, blocks). */
	void BuildRoadHeightRT(const FRoadBuildInput& Input, const FBox& WorldBounds, float CameraHeight);
};
