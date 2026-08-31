#include "CSMeshRenderComponent.h"

#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderSceneProxy.h"

#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "SceneInterface.h"

// -----------------------------------------------------------------------------
// FCSMeshRenderSceneProxy
// -----------------------------------------------------------------------------

FCSMeshRenderSceneProxy::FCSMeshRenderSceneProxy(UCSMeshRenderComponent* Component, const FCSMeshResidentRef& InResident,
	const TArray<TObjectPtr<UMaterialInterface>>& InBatchMaterials)
	: FCSGpuMeshSceneProxy(Component, Component->MeshMaterial, "FCSMeshRenderSceneProxy")
{
	// Holding the shared reference (not a raw pointer) is what makes both teardown orders
	// safe: the component can be destroyed first, or the mesh object can be collected first.
	SetExternalStreams(InResident);

	// The TObjectPtr side of the fence ends here: the render thread only ever sees the raw
	// pointers below. A null slot would become a null GetRenderProxy() mid-frame, so it takes the
	// same default the base falls back to rather than costing a batch.
	UMaterialInterface* const DefaultMaterial = UCSGpuMeshComponent::GetDefaultSurfaceMaterial();
	BatchMaterials.Reserve(InBatchMaterials.Num());
	for (const TObjectPtr<UMaterialInterface>& Mat : InBatchMaterials) BatchMaterials.Add(Mat ? Mat.Get() : DefaultMaterial);

	// The base derived relevance from MeshMaterial alone, which is a lie the moment a second
	// material draws: a translucent section on an otherwise opaque mesh would be gathered into no
	// pass that accepts it and simply not appear. Union rather than replace — over-reporting only
	// costs a pass that gathers batches it then filters out, under-reporting costs the geometry.
	const EShaderPlatform ShaderPlatform = GetScene().GetShaderPlatform();
	for (UMaterialInterface* Mat : BatchMaterials) MaterialRelevance |= Mat->GetRelevance_Concurrent(ShaderPlatform);
}

SIZE_T FCSMeshRenderSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FCSMeshRenderSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory) return;

	// A proxy with no batches would draw nothing while looking perfectly healthy, and an
	// invisible mesh is the symptom nobody traces back to a list; the base's single batch is the
	// floor under that.
	if (BatchMaterials.Num() == 0) return FCSGpuMeshSceneProxy::GetDynamicMeshElements(Views, ViewFamily, VisibilityMap, Collector);

	// Every batch shares the index buffer and differs only in which arg set it draws from: the
	// sort made each material's triangles contiguous and wrote that run's counts into its own arg
	// set. The section's FirstTriangle/TriangleCount are deliberately never read — a batch built
	// from remembered counts keeps drawing after the args it belongs to have been regenerated.
	for (int32 BatchIndex = 0; BatchIndex < BatchMaterials.Num(); ++BatchIndex)
	{
		FMaterialRenderProxy* MaterialProxy = BatchMaterials[BatchIndex]->GetRenderProxy();
		SubmitGpuBufferDraw(*this, Views, VisibilityMap, Collector, *VertexFactory, *MaterialProxy,
			*DrawDesc.IndexBuffer, PT_TriangleList, DrawDesc.NumPrimitives, DrawDesc.MaxVertexIndex,
			bBatchCastShadow, DrawDesc.IndirectArgsBuffer, DrawDesc.IndirectArgsOffset + uint32(BatchIndex) * IndirectArgsSetBytes);
	}
}

// -----------------------------------------------------------------------------
// UCSMeshRenderComponent
// -----------------------------------------------------------------------------

UCSMeshRenderComponent::UCSMeshRenderComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Resident data is world space; rendering with an absolute transform makes local space
	// equal world space so positions draw 1:1 regardless of where the host actor sits.
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);

	// 2026-08-30 实测（出图对照）：房体 / 地面 / 柱子这条路与实例化那条**结论完全一样** ——
	// CSM（r.Shadow.Virtual.Enable 0）下投影正常、自阴影与屋檐投墙都对；VSM（项目级设置 =1）
	// 下一点影子都没有。两条路失效于同一个地方，只是理由不同：
	//   · 这条路用引擎自己的 FLocalVertexFactory，它**声明**了 SupportsPrimitiveIdStream，
	//     于是 MeshPassProcessor.cpp:1304 的 bDoOverrideArgs 为 true，VSM 会拿 GPU-Scene 的
	//     DrawIndirectArgsRDG 顶掉我们写的那份；而我们是 dynamic relevance +
	//     FDynamicPrimitiveUniformBuffer，GPU-Scene 里没有对应实例 ⇒ 实例数 0。
	//   · 实例化那条（UCSGpuInstancedMeshComponent）不声明该 flag，args 不被覆盖，
	//     但同样进不了 VSM 的实例剔除表 —— 详见那边的注释。
	// 所以"实例化路能投影、非实例化路不能"这条**推断被实测推翻**：在 VSM 下两条都不能，
	// 在 CSM 下两条都能。
	//
	// 留 true 而不是退回 false：CSM 下确凿正确，VSM 下一个影子像素都画不出来
	// （改前/改后同机位逐像素比过）。
	CastShadow = true;
	bReceivesDecals = false;
}

void UCSMeshRenderComponent::SetGpuMesh(UCSMesh* InMesh)
{
	if (GpuMesh == InMesh)
	{
		// Same object, but it may have been reallocated since the proxy bound it.
		HandleMeshChanged(InMesh);
		return;
	}

	UnbindMeshDelegate();
	GpuMesh = InMesh;
	BindMeshDelegate();

	const FCSMeshResident* Resident = GpuMesh ? GpuMesh->GetResidentPtr() : nullptr;
	LocalBounds = Resident ? Resident->WorldBounds : FBox(ForceInit);

	// A caller that submits and immediately saves must see the new proxy; MarkRenderStateDirty
	// defers the rebuild to the end-of-frame update, which a bare FlushRenderingCommands does
	// not run (the display component learned the same lesson).
	if (IsRegistered()) RecreateRenderState_Concurrent();
	UpdateBounds();
}

bool UCSMeshRenderComponent::HasGeneratedGeometry() const
{
	return GpuMesh != nullptr && !GpuMesh->IsEmpty();
}

#if WITH_EDITOR
UStaticMesh* UCSMeshRenderComponent::SaveToStaticMesh(const FTransform& BakeSpace,
	const FString& AssetPathAndName, bool bReplaceExistingAsset, bool bSaveAsset, bool bBakeToLocalSpace,
	bool bEnableNanite, UCSMesh* SourceMeshOverride)
{
	// 要存的几何优先取调用方给的那份：本组件的绑定只决定"画什么"，而落盘读的是网格对象。
	// 生产方自己持有 UCSMesh 时（generator 的 DirectGpuMesh），组件被解绑不该连累落盘。
	UCSMesh* SourceMesh = SourceMeshOverride ? SourceMeshOverride : GpuMesh.Get();
	if (!SourceMesh) return nullptr;

	FCSMeshToStaticMeshOptions Options;
	Options.AssetPath = AssetPathAndName.TrimStartAndEnd();
	Options.bTransient = false; // this entry point's whole purpose is to produce an asset
	Options.bReplaceExisting = bReplaceExistingAsset;
	Options.bSaveToDisk = bSaveAsset;
	Options.bEnableNanite = bEnableNanite;
	// 烘回 BakeSpace 的局部空间，资产摆在那个变换上就能复现画面上的东西。
	// GetComponentTransform() 在这里是错的而且错得无声：本组件以绝对变换渲染，组件变换恒为单位，
	// 按它烘等于把几何冻结在世界坐标上。
	Options.TargetTransform = BakeSpace;
	Options.bBakeToLocalSpace = bBakeToLocalSpace;

	// 读的是网格对象而不是 scene proxy：隐藏的、未注册的、甚至已经解绑的组件，存出来的东西完全一样。
	return UCSMeshOps::CopyToStaticMesh(SourceMesh, this, GetOwner(), Options);
}
#endif

FPrimitiveSceneProxy* UCSMeshRenderComponent::CreateSceneProxy()
{
	// The bound state is recorded on the no-proxy path too. Recording it only alongside a real
	// proxy would leave it describing a proxy two rebuilds ago, and every later comparison would
	// be against something that never existed.
	FCSMeshResidentRef Resident = GpuMesh ? GpuMesh->GetResident() : FCSMeshResidentRef();
	BoundAllocationGeneration = Resident.IsValid() ? Resident->AllocationGeneration : 0;
	ResolveBatchMaterials(BoundBatchMaterials);

	if (!Resident.IsValid() || !Resident->IsAllocated()) return nullptr;
	return new FCSMeshRenderSceneProxy(this, Resident, BoundBatchMaterials);
}

void UCSMeshRenderComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// Resolved live rather than read off BoundBatchMaterials: this is asked before any proxy
	// exists (shader compilation, streaming, the editor's material-usage queries) and has to
	// answer for the mesh as it is now, not for the proxy as it was.
	TArray<TObjectPtr<UMaterialInterface>> BatchMaterials;
	ResolveBatchMaterials(BatchMaterials);
	for (const TObjectPtr<UMaterialInterface>& Mat : BatchMaterials) OutMaterials.AddUnique(Mat.Get());

	// MeshMaterial stays in the list even while sections are drawing instead of it. It is the
	// component's declared material, and the section table is transient GPU state that any
	// reallocation drops — "which components use this material" must not depend on when it was
	// asked. An extra entry costs nothing; a missing one is a material nothing prepared for.
	if (MeshMaterial) OutMaterials.AddUnique(MeshMaterial.Get());
}

void UCSMeshRenderComponent::ResolveBatchMaterials(TArray<TObjectPtr<UMaterialInterface>>& OutMaterials) const
{
	OutMaterials.Reset();

	UMaterialInterface* const DefaultMaterial = UCSGpuMeshComponent::GetDefaultSurfaceMaterial();
	const FCSMeshResident* Resident = GpuMesh ? GpuMesh->GetResidentPtr() : nullptr;
	const int32 NumSections = Resident ? Resident->Sections.Num() : 0;

	// Not a degenerate case but the default: every mesh that never met the section builder must
	// keep drawing as one batch of MeshMaterial out of arg set 0, exactly as it did before
	// sections existed.
	if (NumSections == 0)
	{
		OutMaterials.Add(MeshMaterial ? MeshMaterial.Get() : DefaultMaterial);
		return;
	}

	// A table longer than the arg buffer would issue draws out of arg sets that do not exist —
	// a read past the end of an indirect buffer, which is a device fault rather than a wrong
	// colour. UCSMesh::SetSections refuses that, but the table may also be written straight into
	// the resident set inside an edit, where nothing checks it.
	const int32 MaxBatches = int32(Resident->NumIndirectDraws);
	const int32 NumBatches = FMath::Min(NumSections, MaxBatches);
	if (NumBatches < NumSections) UE_LOG(LogTemp, Warning, TEXT("[CSMeshRender] %d sections against %d indirect arg sets; the surplus is not drawn."), NumSections, MaxBatches);

	OutMaterials.Reserve(NumBatches);
	for (int32 BatchIndex = 0; BatchIndex < NumBatches; ++BatchIndex)
	{
		const int32 MaterialIndex = Resident->Sections[BatchIndex].MaterialIndex;
		UMaterialInterface* SectionMaterial = GpuMesh->Materials.IsValidIndex(MaterialIndex) ? GpuMesh->Materials[MaterialIndex].Get() : nullptr;

		// An id past the end of the material table, or a slot nobody has filled yet, is a normal
		// transient state — the sort usually runs before the caller has finished the table — so
		// the batch draws in the default material. Dropping it instead would hide geometry, and
		// missing triangles do not lead anyone back to an unfilled material slot.
		OutMaterials.Add(SectionMaterial ? SectionMaterial : DefaultMaterial);
	}
}

FBoxSphereBounds UCSMeshRenderComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FCSMeshResident* Resident = GpuMesh ? GpuMesh->GetResidentPtr() : nullptr;
	const FBox Box = (Resident && Resident->WorldBounds.IsValid)
		? Resident->WorldBounds
		: (LocalBounds.IsValid ? LocalBounds : FBox(FVector(-100.0), FVector(100.0)));
	return FBoxSphereBounds(Box.TransformBy(LocalToWorld));
}

bool UCSMeshRenderComponent::IsGpuMeshProxyActive() const
{
	// This leaf only ever creates the base proxy, and only when the mesh has an allocation.
	const FCSMeshResident* Resident = GpuMesh ? GpuMesh->GetResidentPtr() : nullptr;
	return Resident != nullptr && Resident->IsAllocated();
}

void UCSMeshRenderComponent::BeginDestroy()
{
	UnbindMeshDelegate();
	Super::BeginDestroy();
}

void UCSMeshRenderComponent::BindMeshDelegate()
{
	if (!GpuMesh || MeshChangedHandle.IsValid()) return;
	MeshChangedHandle = GpuMesh->OnMeshChanged.AddUObject(this, &UCSMeshRenderComponent::HandleMeshChanged);
}

void UCSMeshRenderComponent::UnbindMeshDelegate()
{
	if (GpuMesh && MeshChangedHandle.IsValid()) GpuMesh->OnMeshChanged.Remove(MeshChangedHandle);
	MeshChangedHandle.Reset();
}

void UCSMeshRenderComponent::HandleMeshChanged(UCSMesh* ChangedMesh)
{
	if (ChangedMesh != GpuMesh) return;

	const FCSMeshResident* Resident = GpuMesh ? GpuMesh->GetResidentPtr() : nullptr;
	LocalBounds = Resident ? Resident->WorldBounds : FBox(ForceInit);

	// The GPU-side advantage of this mode: an ordinary content edit leaves the buffers and the
	// vertex-factory binding untouched — the counts live on the GPU — so all the render side
	// needs is fresh bounds. Only a reallocation forces a proxy rebuild.
	const uint32 AllocationGeneration = Resident ? Resident->AllocationGeneration : 0;
	if (!IsRegistered()) return;

	// ...and a section-table change, which is the one edit that changes what the proxy draws
	// without changing a buffer. AllocationGeneration cannot carry that signal: the section
	// builder only reallocates when the arg buffer is too small, so the common case publishes a
	// whole new material split under an unchanged generation, and a proxy holding the old split
	// would keep drawing the wrong materials with nothing to indicate it.
	//
	// The check is a recomputation, not a flag, and that is the point: every route that *reaches*
	// this handler ends in the same comparison — UCSMesh::SetSections, an operator dropping the
	// table inside an edit (UCSMeshOps::InvalidateSections, which every counter-writing pass
	// calls), UCSMesh::SetMaterial — so none of them can be the one that forgot to signal.
	//
	// What it cannot cover is a write that never broadcasts at all: UCSMesh::Materials is a public
	// UPROPERTY array, and assigning an element in place fires nothing. That is why the mesh
	// exposes SetMaterial / NotifyMaterialsChanged and why the field's own comment calls the direct
	// write out — the gap is on the writing side, not here.
	//
	// A rebuild rather than a per-frame derivation, because a batch needs a material render proxy
	// and the render thread may not read a UObject to find one; because Sections is a game-thread
	// TArray that a per-frame read would race against; and because relevance and GetUsedMaterials
	// are construction-time facts the scene has already cached from the old material set. In this
	// leaf a rebuild is a rebind — no allocation, no compute — which is what the retained design
	// bought.
	TArray<TObjectPtr<UMaterialInterface>> BatchMaterials;
	ResolveBatchMaterials(BatchMaterials);

	if (AllocationGeneration != BoundAllocationGeneration || BatchMaterials != BoundBatchMaterials)
	{
		// Recorded here as well as in CreateSceneProxy, which cannot disagree (same resolve, same
		// call stack) but also does not always run: a registered component that is hidden or
		// otherwise not added to the scene still receives change events, and without this it would
		// recreate its render state on every edit for the rest of its life.
		BoundAllocationGeneration = AllocationGeneration;
		BoundBatchMaterials = MoveTemp(BatchMaterials);
		RecreateRenderState_Concurrent();
		UpdateBounds();
		return;
	}

	UpdateBounds();
	MarkRenderTransformDirty();
}
