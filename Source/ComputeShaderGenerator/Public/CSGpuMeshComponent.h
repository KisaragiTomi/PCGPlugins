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

	/** ReadbackMeshSync 会把当前代理无检查地 static_cast 成 FCSGpuMeshSceneProxy。
	 *  只创建本基座代理的叶子（road / vine）保持默认 true 即可；能按模式创建其他代理的
	 *  叶子（UCSDisplayComponent）必须覆写，否则那次 cast 是 UB。 */
	virtual bool IsGpuMeshProxyActive() const { return true; }

	// Local-space bounds used by CalcBounds; leaves update this when their geometry changes.
	FBox LocalBounds = FBox(ForceInit);

public:
	// -------------------------------------------------------------------------
	// GPU 快照 -> StaticMesh 的转换与落地（原 namespace CSGpuMeshConvert，
	// 2026-08 并入本类）。只吃 FCSGpuMeshCPUData 的部分是静态成员，不需要组件实例。
	// -------------------------------------------------------------------------

	/** 返回引擎默认表面材质，供空槽兜底。 */
	static UMaterialInterface* GetDefaultSurfaceMaterial();

	/**
	 * 由 GPU 回读快照装配 LOD0 的 FMeshDescription。支持索引网格（顶点数 != 角点数）、
	 * per-corner 属性与 per-triangle 材质槽。不触碰任何资产系统。
	 */
	static bool BuildMeshDescription(
		const FCSGpuMeshCPUData& MeshData,
		const FCSGpuMeshConvertOptions& Options,
		FMeshDescription& OutMeshDescription);

	/**
	 * 由 GPU 回读快照产出 UStaticMesh。Materials 为空槽时按 Options 兜底默认材质。
	 * bTransient 时在 Outer 下建临时网格（不提交 MeshDescription，省下每网格约 1.5 GiB 常驻）；
	 * 否则建成资产并标脏，bSaveToDisk 决定是否立即写盘。
	 */
	static UStaticMesh* BuildStaticMesh(
		UObject* Outer,
		const AActor* OwnerActor,
		const FCSGpuMeshCPUData& MeshData,
		const TArray<UMaterialInterface*>& Materials,
		const FCSGpuMeshConvertOptions& Options,
		const FCSGpuMeshAssetOptions& AssetOptions);

	// -------------------------------------------------------------------------
	// 回读快照 -> StaticMesh 的落地实现（原 CSGpuMeshSave，2026-08 并入本命名空间）。
	// 两个文件此前互相调用形成循环依赖，且各自持有一份逐字相同的材质槽命名 helper。
	// -------------------------------------------------------------------------
	/** Builds an FMeshDescription (LOD0) from a GPU-mesh CPU snapshot. Handles indexed meshes
	 *  (V != I), per-corner attributes, and per-triangle material slots. Optionally converts
	 *  world-space positions/normals/tangents into ActorTransform's local space. */
	static bool BuildGpuMeshDescription(
		const FCSGpuMeshCPUData& MeshData,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace,
		FMeshDescription& OutMeshDescription);

	/** Builds a transient UStaticMesh directly from a final GPU readback snapshot. */
	static UStaticMesh* BuildTransientStaticMesh(
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
	static FString BuildResultAssetPath(
		const AActor* OwnerActor,
		const FString& NameSuffix = FString());

	/** Saves an already-read-back GPU mesh snapshot as a StaticMesh asset, preserving every
	 *  material slot. With an empty AssetPathAndName the location defaults to an "AutoResult" folder
	 *  next to OwnerActor's level (e.g. level /Game/Maps/L_Foo -> /Game/Maps/AutoResult/SM_<owner>).
	 *  The asset and its package are always marked dirty; bSaveAsset additionally writes it to
	 *  disk (default false = leave it dirty for a manual Save-All). */
	static UStaticMesh* SaveGpuMeshDataToStaticMesh(
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
	UStaticMesh* SaveRenderedMeshToStaticMesh(
		const FString& AssetPathAndName,
		UMaterialInterface* Material,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace = true,
		bool bReplaceExistingAsset = true,
		bool bSaveAsset = false);
#endif
};
