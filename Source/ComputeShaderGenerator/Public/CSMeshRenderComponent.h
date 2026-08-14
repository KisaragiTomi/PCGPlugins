#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "CSMeshRenderComponent.generated.h"

class UCSMesh;
class UMaterialInterface;

/**
 * Renders a UCSMesh. The GPU counterpart of UDynamicMeshComponent: it draws a mesh object
 * someone else owns and edits, and generates nothing itself.
 *
 * The other leaves of UCSGpuMeshComponent own their buffers through the scene proxy, which
 * means every render-state recreation re-runs that leaf's generation compute and the geometry's
 * lifetime is the render state's lifetime. This leaf inverts that: the buffers belong to the
 * UCSMesh, the proxy only binds them, and a recreation costs a rebind.
 *
 * The migration onto this leaf is in progress: the box-scene display path and the road are done
 * (URoadMeshComponent now derives from this class), vine and instanced still own their buffers.
 *
 * The data is world-space, so the component renders with an absolute transform: its own
 * placement in the level does not move the geometry.
 *
 * A mesh that carries a section table draws one FMeshBatch per section, each out of its own
 * DrawIndexedIndirect arg set — the material sort already made every material's triangles
 * contiguous, so the whole split lives in the args and the batches share one index buffer. A
 * mesh with no table (everything that never met the section builder) still draws as exactly one
 * batch with MeshMaterial, which is what the rest of the subsystem assumes of it.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSMeshRenderComponent : public UCSGpuMeshComponent
{
	GENERATED_BODY()

public:
	UCSMeshRenderComponent();

	/** Material for the whole mesh, used while the bound mesh carries no section table. Null falls
	 *  back to the engine default surface material. Once sections exist each batch draws with its
	 *  own entry from UCSMesh::Materials and this is no longer a draw material — but it stays the
	 *  component's declared material, so GetUsedMaterials keeps reporting it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	TObjectPtr<UMaterialInterface> MeshMaterial;

	/** Binds a mesh object. Passing null clears the display. The component listens for the
	 *  mesh's change event, so later operators show up without another call. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	void SetGpuMesh(UCSMesh* InMesh);

	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	UCSMesh* GetGpuMesh() const { return GpuMesh; }

	/**
	 * 这儿有没有几何——生成方在决定"要不要重跑一次生成"时问的那个问题。
	 *
	 * 问的是网格对象，因为几何归它所有。"有没有 scene proxy"从渲染状态重建变成重新绑定的那
	 * 一刻起就不再是证据了。
	 *
	 * IsEmpty() 在 game thread 上就能答，不碰 GPU；当只有 GPU 知道计数时它报非空——这正是
	 * 生成结果尺寸由 GPU 决定那些路径（藤蔓的 SC 求解、道路的路口数）的常态。所以它刻意不是
	 * "确定有三角形"，而是"有一块装着某次生成产物的分配"：把它问准就得在每次调用上挂一次 GPU
	 * 停顿，而这类调用每次关卡加载都会发生一遍。
	 */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	bool HasGeneratedGeometry() const;

#if WITH_EDITOR
	/**
	 * 把当前绑定的这份几何存成 StaticMesh 资产。
	 *
	 * BakeSpace 是烘焙用的局部空间：常驻数据是世界空间的，资产按它烘回去，摆在那个变换上就能
	 * 复现画面上的东西。生产方知道自己是在哪个空间里建的几何（道路是 spline 重采样时的那个
	 * InputToWorld，藤蔓是 actor 变换），所以由调用方给。
	 *
	 * 不需要任何东西正在渲染这份几何——读的是网格对象，不是 scene proxy。
	 * AssetPathAndName 留空则由落盘层兜底到当前关卡旁的 AutoResult 目录。
	 * bSaveAsset=false（默认）只把资产标脏，留给手动保存。
	 * bBakeToLocalSpace=false 表示常驻数据本来就是局部空间的，别再变换（此时 BakeSpace 不参与）。
	 */
	UStaticMesh* SaveToStaticMesh(const FTransform& BakeSpace, const FString& AssetPathAndName = TEXT(""),
		bool bReplaceExistingAsset = true, bool bSaveAsset = false, bool bBakeToLocalSpace = true);
#endif

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

	/** Every material the bound mesh can draw with, not just GetRenderMaterial(). The base only
	 *  knows the component's single material, and a section material nothing ever reported is one
	 *  the engine never prepared shaders, texture streaming or editor usage queries for. */
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

protected:
	//~ UCSGpuMeshComponent interface
	virtual UMaterialInterface* GetRenderMaterial() const override { return MeshMaterial; }
	virtual bool IsGpuMeshProxyActive() const override;

	virtual void BeginDestroy() override;

private:
	void BindMeshDelegate();
	void UnbindMeshDelegate();
	void HandleMeshChanged(UCSMesh* ChangedMesh);

	/**
	 * The material of every draw batch, in batch order — batch i draws from indirect arg set i.
	 * One entry (MeshMaterial, or the default surface material) when the mesh has no section
	 * table, one per section otherwise. Never contains null and never comes back empty, so the
	 * proxy has nothing to second-guess and the mesh cannot end up with no batch to draw.
	 *
	 * Deriving the list rather than caching a "sections changed" flag is what closes the hole:
	 * the answer is recomputed from the current table on every change event, so no publication
	 * path can be the one that forgot to raise the flag. Game thread only.
	 */
	void ResolveBatchMaterials(TArray<TObjectPtr<UMaterialInterface>>& OutMaterials) const;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> GpuMesh;

	FDelegateHandle MeshChangedHandle;

	// What the live proxy was built from, and the only thing HandleMeshChanged compares against.
	// Both are written by CreateSceneProxy — on its no-proxy path too, so a mesh that becomes
	// allocated later is still seen as a change — and re-stated by HandleMeshChanged for the
	// components CreateSceneProxy never runs for.

	/** Allocation generation the current proxy was built against. A content edit reuses the
	 *  same buffers and only needs new bounds; a reallocation (capacity growth, release) has
	 *  to rebuild the proxy or it keeps drawing from freed buffers. */
	uint32 BoundAllocationGeneration = 0;

	/** The proxy's batch materials, and the reason this is a UPROPERTY rather than a bare array:
	 *  the proxy holds them as raw pointers, and an entry of UCSMesh::Materials can be replaced
	 *  with no event at all — without a reference here that would leave the render thread
	 *  dereferencing a collected material. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> BoundBatchMaterials;
};
