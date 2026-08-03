#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "ComputeShaderMeshGenerator.h" // FCSBoxScenePreparedData
#include "CSDirectTriangleMeshComponent.generated.h"

class UMaterialInterface;

/**
 * Draws a GPU triangle soup extracted by AComputeShaderMeshGenerator directly through
 * the render pipeline. The owning generator hands it a game-thread-prepared box-scene
 * snapshot; the scene proxy runs the extraction + a pack pass on the render thread and
 * draws the result with DrawIndexedIndirect. Vertex/index data never returns to the CPU
 * and no UDynamicMesh is created.
 *
 * The triangle soup is in world space, so this component renders with an absolute
 * (world-origin) identity transform — its local space equals world space.
 *
 * Buffer set, CPU readback (ReadbackMeshSync) and save-to-StaticMesh all live in the
 * UCSGpuMeshComponent / FCSGpuMeshSceneProxy base; this leaf only adds the material and
 * the box-scene producer API.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSDirectTriangleMeshComponent : public UCSGpuMeshComponent
{
	GENERATED_BODY()

public:
	UCSDirectTriangleMeshComponent();

	/** Material drawn for the whole triangle soup. Null uses the engine default surface material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Direct Mesh")
	TObjectPtr<UMaterialInterface> MeshMaterial;

	/** Hand the component a game-thread-prepared box-scene triangle snapshot and rebuild the proxy.
	 *  VertexCapacity = triangle capacity * 3 (bounds the persistent GPU buffers). WorldBounds is the
	 *  world-space bounds of the geometry (used directly as local bounds — see class comment). */
	void SetTriangleSource(const FCSBoxScenePreparedData& InPrepared, uint32 InVertexCapacity, const FBox& InWorldBounds);

	/** Releases the prepared source and removes the GPU triangle proxy. */
	void ClearTriangleSource();

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

protected:
	//~ UCSGpuMeshComponent interface
	virtual UMaterialInterface* GetRenderMaterial() const override { return MeshMaterial; }

private:
	// Snapshot taken by CreateSceneProxy; a resubmit recreates the proxy.
	FCSBoxScenePreparedData PendingPrepared;
	uint32 PendingVertexCapacity = 0;
};
