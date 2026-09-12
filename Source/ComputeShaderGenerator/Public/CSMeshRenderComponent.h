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
 * 显存各归各：网格的 Outer 是本组件时它**归本组件**，组件销毁时自己把它的显存还掉
 * （OnComponentDestroyed，不阻塞）；生产方挂在自己身上、只借本组件显示的网格一个字节都不动。
 * 这与 UCSGpuInstancedMeshComponent 对它自己那张常驻网格的做法同一条纪律 —— 拥有方不必
 * 在销毁前替 gpumesh 收拾。
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
	 *
	 * bEnableNanite 一路透传到 BuildFromMeshDescriptions —— 必须在那次构建之前设置，建完再改只会
	 * 标脏、不会真的产出 Nanite 数据。默认 false 保持既有行为；GPU 生成的网格常是百万级三角的一次性
	 * 产物，需要时由生产方显式打开。
	 *
	 * SourceMeshOverride 指定要存的几何，留空则用本组件当前绑定的那份。
	 * **生产方若把 UCSMesh 存在自己身上（AComputeShaderMeshGenerator::DirectGpuMesh 就是），
	 * 必须显式传进来**：组件的绑定是渲染用的，解绑（SetGpuMesh(nullptr)）只该让它不再显示，
	 * 不该让生产方连自己的几何都存不出来 —— 而这正是"不需要任何东西正在渲染"这条契约的含义。
	 */
	UStaticMesh* SaveToStaticMesh(const FTransform& BakeSpace, const FString& AssetPathAndName = TEXT(""),
		bool bReplaceExistingAsset = true, bool bSaveAsset = false, bool bBakeToLocalSpace = true,
		bool bEnableNanite = false, UCSMesh* SourceMeshOverride = nullptr);
#endif

	/** One step of the surface-cache poll, exactly as the core ticker runs it; returns whether the
	 *  poll wants another. For tests: a synchronous test never reaches the next frame. */
	bool TickSurfaceCacheRefreshForTest() { return TickSurfaceCacheRefresh(0.0f); }

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

	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void BeginDestroy() override;

private:
	void BindMeshDelegate();
	void UnbindMeshDelegate();
	void HandleMeshChanged(UCSMesh* ChangedMesh);

	/**
	 * 变化事件落在帧末组件更新（SendAllEndOfFrameUpdates）中途时，推迟到下一次 core tick 再对账。
	 *
	 * 渲染线程 flush 会顺手泵游戏线程任务 —— 异步编辑的尾巴（广播本组件的变化事件）就在里面；而引擎
	 * 自己也会在帧末组件更新的中途 flush（例如新建的代理给材质补 usage 标志时走 FMaterialUpdateContext）。
	 * 那一刻碰渲染状态是 check 崩（UWorld::MarkActorComponentForNeededEndOfFrameUpdate 的
	 * !bPostTickComponentUpdate）。晚一帧对上画面，代价只是这一帧的包围盒 / 代理还是旧的。
	 */
	void DeferMeshChanged();
	bool bMeshChangedDeferred = false;

	/**
	 * Lumen surface cache upkeep (the proxy side is FCSGpuMeshSceneProxy::DrawStaticElements).
	 *
	 * The proxy's card-capture batches carry the published draw counts, and the engine collects
	 * static batches only when the primitive is added or its transform is updated — while a landed
	 * readback has no game-thread event of its own. So after every edit, and after every proxy
	 * creation (whose batches may have been collected before any count was known), this polls the
	 * resident set's publish serial. When it moves: a transform update, which makes the engine
	 * collect the batches again with the landed counts, and a surface-cache invalidation, which
	 * recaptures the cards from them.
	 *
	 * Geometry that grew past the bounds the live proxy's cards were built from gets a new proxy
	 * instead. Lumen builds a primitive's card set once and afterwards only moves it
	 * (FLumenSceneData::UpdateMeshCards), so no transform update can widen it.
	 *
	 * A poll rather than a flag the render thread raises: this component's state is game-thread
	 * state, and the only ways back from the render thread are the task hops DeferMeshChanged
	 * already documents as unsafe. Armed only while a publication is expected; costs one lock per
	 * frame while it is. Same ticker discipline as DeferMeshChanged: a weak lambda and a flag, no
	 * handle — a collected component unbinds the delegate and the core ticker drops it.
	 */
	void ArmSurfaceCacheRefresh();
	bool TickSurfaceCacheRefresh(float DeltaTime);
	bool bSurfaceCacheTickerArmed = false;

	/** Publish serial the live proxy's capture batches were last collected against. */
	uint32 SurfaceCacheServedSerial = 0;

	/** Bounds the live proxy built its cards from — its local bounds when it joined the scene. */
	FBox SurfaceCacheCardBounds = FBox(ForceInit);

	/** When the poll was last armed. A readback that publishes nothing leaves the counts unknown
	 *  until the next edit (which re-arms), so the wait is bounded instead of permanent. */
	double SurfaceCacheArmedAt = 0.0;

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
