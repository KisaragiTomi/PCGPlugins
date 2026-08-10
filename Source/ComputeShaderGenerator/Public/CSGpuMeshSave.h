#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshTypes.h"

class AActor;
class UCSGpuMeshComponent;
class UMaterialInterface;
class UStaticMesh;
struct FMeshDescription;

// Shared "GPU triangles -> StaticMesh" conversion used by every UCSGpuMeshComponent leaf and
// by compute passes that already own a final GPU readback snapshot. No leaf/actor coupling.
namespace CSGpuMeshSave
{
	/** Builds an FMeshDescription (LOD0) from a GPU-mesh CPU snapshot. Handles indexed meshes
	 *  (V != I), per-corner attributes, and per-triangle material slots. Optionally converts
	 *  world-space positions/normals/tangents into ActorTransform's local space. */
	COMPUTESHADERGENERATOR_API bool BuildGpuMeshDescription(
		const FCSGpuMeshCPUData& MeshData,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace,
		FMeshDescription& OutMeshDescription);

	/** Builds a transient UStaticMesh directly from a final GPU readback snapshot. */
	COMPUTESHADERGENERATOR_API UStaticMesh* BuildTransientStaticMesh(
		UObject* Outer,
		const FCSGpuMeshCPUData& MeshData,
		const TArray<UMaterialInterface*>& Materials,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace = true,
		bool bEnableNanite = false);

#if WITH_EDITOR
	/** Builds "<OwnerActor's level folder>/AutoResult/SM_<owner><NameSuffix>". Producers that emit
	 *  more than one result from the same actor (an intermediate stage and the final mesh, or
	 *  repeated runs) must pass distinct suffixes — a timestamp works — otherwise each run
	 *  replaces the previous asset.
	 *  Returns an empty string when the owning level has no content path (unsaved map). */
	COMPUTESHADERGENERATOR_API FString BuildResultAssetPath(
		const AActor* OwnerActor,
		const FString& NameSuffix = FString());

	/** Saves an already-read-back GPU mesh snapshot as a StaticMesh asset, preserving every
	 *  material slot. With an empty AssetPathAndName the location defaults to an "AutoResult" folder
	 *  next to OwnerActor's level (e.g. level /Game/Maps/L_Foo -> /Game/Maps/AutoResult/SM_<owner>).
	 *  The asset and its package are always marked dirty; bSaveAsset additionally writes it to
	 *  disk (default false = leave it dirty for a manual Save-All). */
	COMPUTESHADERGENERATOR_API UStaticMesh* SaveGpuMeshDataToStaticMesh(
		const AActor* OwnerActor,
		const FCSGpuMeshCPUData& MeshData,
		const TArray<UMaterialInterface*>& Materials,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace = true,
		const FString& AssetPathAndName = FString(),
		bool bReplaceExistingAsset = true,
		bool bSaveAsset = false,
		bool bEnableNanite = false);

	/** Reads Component's rendered GPU mesh back to the CPU (one blocking readback) and saves it
	 *  as a StaticMesh asset. The created asset is always marked dirty; bSaveAsset controls whether
	 *  it is also written to disk (default false = leave it dirty for a manual/Save-All).
	 *  Pass an empty AssetPathAndName to default the location to an "AutoResult" folder next to the
	 *  current level (e.g. /Game/Maps/AutoResult/SM_<owner>). Returns the created mesh (possibly unsaved)
	 *  or nullptr on any failure. */
	COMPUTESHADERGENERATOR_API UStaticMesh* SaveGpuMeshComponentToStaticMesh(
		UCSGpuMeshComponent* Component,
		const FString& AssetPathAndName,
		UMaterialInterface* Material,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace = true,
		bool bReplaceExistingAsset = true,
		bool bSaveAsset = false);
#endif
}
