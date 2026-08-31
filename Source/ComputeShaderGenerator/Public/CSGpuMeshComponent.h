#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGpuMeshComponent.generated.h"

class UMaterialInterface;
struct FMeshDescription;   // BuildGpuMeshDescription 的 out 参数；unity 构建下靠邻居 TU 的 include 兜住，单文件编译会露馅

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
	 *  Leaves back this with their own UPROPERTY to avoid changing serialized data. Null falls
	 *  back to the default surface material in the proxy. */
	virtual UMaterialInterface* GetRenderMaterial() const { return nullptr; }

	/** ReadbackMeshSync 会把当前代理无检查地 static_cast 成 FCSGpuMeshSceneProxy。
	 *  只创建本基座代理的叶子保持默认 true 即可；任何可能创建其他代理、或可能没有代理的叶子
	 *  必须覆写，否则那次 cast 是 UB。现存的覆写者是 UCSMeshRenderComponent（按网格对象有没有
	 *  分配来答）。曾经的例子 UCSDisplayComponent 已不再派生自本基座。 */
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

private:
	// BuildStaticMesh 的两个落地分支。它们只被上面那个分发器调用（同类、同一个 .cpp），
	// 对外暴露只会让人以为存在第二条合法入口——落盘的策略位（transient? 烘焙空间? 材质来源?）
	// 全在 BuildStaticMesh 里合成，绕过它直接调这两个就会绕过那些合成。
	//
	// 包/资产生命周期（路径校验、删旧、建包、注册、写盘）已抽到 CSStaticMeshAssetSink.h，
	// 由这里和 DynamicMesh 侧共用；这两个函数剩下的只是"喂什么数据进去"。

	/** Builds a transient UStaticMesh directly from a final GPU readback snapshot. */
	static UStaticMesh* BuildTransientStaticMesh(
		UObject* Outer,
		const FCSGpuMeshCPUData& MeshData,
		const TArray<UMaterialInterface*>& Materials,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace = true,
		bool bEnableNanite = false);

#if WITH_EDITOR
	/** Saves an already-read-back GPU mesh snapshot as a StaticMesh asset, preserving every
	 *  material slot. With an empty AssetPathAndName the location defaults to an "AutoResult" folder
	 *  next to OwnerActor's level (e.g. level /Game/Maps/L_Foo -> /Game/Maps/AutoResult/SM_<owner>). */
	static UStaticMesh* SaveGpuMeshDataToStaticMesh(
		const AActor* OwnerActor,
		const FCSGpuMeshCPUData& MeshData,
		const TArray<UMaterialInterface*>& Materials,
		const FTransform& ActorTransform,
		bool bConvertToActorLocalSpace,
		const FString& AssetPathAndName,
		bool bReplaceExistingAsset,
		bool bSaveAsset,
		bool bEnableNanite);
#endif
};
