#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CSMeshPool.generated.h"

class UCSMesh;

/**
 * Reuse pool for UCSMesh, with the same four-function API as UDynamicMeshPool.
 *
 * The reason a GPU pool exists is the opposite of the CPU one's. UDynamicMeshPool returns a
 * mesh by clearing it, which frees the memory — the pool saves the UObject, not the storage.
 * Here the storage *is* the expensive part: a returned mesh keeps its buffers, so a request
 * that fits an existing allocation costs a counter reset instead of a VRAM allocation. That
 * inverts the safeguard too: capping the pool by object count (geometry.DynamicMesh.MaxPoolSize)
 * would say nothing about the memory held, so the cap here is VRAM, checked against the live
 * budget (CSGpuMemoryBudget) rather than a fixed number.
 */
UCLASS(BlueprintType)
class COMPUTESHADERGENERATOR_API UCSMeshPool : public UObject
{
	GENERATED_BODY()

public:
	/** Hands out a mesh with at least this capacity — an idle one when the pool has a fit,
	 *  otherwise a new object. The result is always empty (counters zeroed). */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Pool")
	UCSMesh* RequestMesh(int32 VertexCapacity = 3, int32 IndexCapacity = 3);

	/** Returns a mesh for reuse. The geometry is cleared but the allocation is kept, which is
	 *  the entire point; the VRAM cap decides whether it survives. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Pool")
	void ReturnMesh(UCSMesh* Mesh);

	/** Returns everything this pool ever handed out. Callers must have stopped using them. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Pool")
	void ReturnAllMeshes();

	/** Returns everything and frees the GPU memory outright. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Pool")
	void FreeAllMeshes();

	/** Applies the current VRAM ceiling now, evicting idle meshes until the pool fits under
	 *  it. Returns and requests do this automatically; call it directly after lowering the
	 *  ceiling, or when something else on the device suddenly needs the memory. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Pool")
	void EnforceMemoryLimit();

	/** VRAM currently held by idle (returned) meshes. */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Pool")
	int64 GetCachedBytes() const;

	/** VRAM held by both idle and handed-out meshes. */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Pool")
	int64 GetTotalBytes() const;

	/** The current eviction ceiling: MaxCachedBytesOverride when positive, otherwise
	 *  MaxCachedVideoMemoryRatio of the device's available VRAM. */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Pool")
	int64 GetCachedBytesLimit() const;

	/** Hard ceiling on idle VRAM in bytes. <= 0 derives it from the live budget instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh|Pool")
	int64 MaxCachedBytesOverride = 0;

	/** Share of currently available VRAM the idle set may hold when no override is set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh|Pool", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxCachedVideoMemoryRatio = 0.25f;

	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Pool")
	int32 GetCachedMeshCount() const { return CachedMeshes.Num(); }

	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Pool")
	int32 GetActiveMeshCount() const { return ActiveMeshes.Num(); }

private:
	/** Idle, still allocated, waiting to be handed out again. */
	UPROPERTY()
	TArray<TObjectPtr<UCSMesh>> CachedMeshes;

	/** Handed out and not yet returned. Kept referenced so ReturnAllMeshes can find them. */
	UPROPERTY()
	TArray<TObjectPtr<UCSMesh>> ActiveMeshes;
};
