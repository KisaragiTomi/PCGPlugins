#include "CSTinyGlade.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundShaperSteps.h"   // ReserveCount —— 容量台阶与房体 / 柱原来那两份同一个口径
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogTinyGlade, Log, All);

ACSTinyGlade::ACSTinyGlade()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	TinyGladeMeshComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("TinyGladeMesh"));
	TinyGladeMeshComponent->SetupAttachment(Root);
}

// -----------------------------------------------------------------------------
// 网格槽
// -----------------------------------------------------------------------------

bool ACSTinyGlade::UploadTinyGladeSnapshot(const FCSGpuMeshCPUData& Snapshot, const TArray<TObjectPtr<UMaterialInterface>>& Materials)
{
	if (!TinyGladeMeshComponent || !Snapshot.IsValid()) return false;

	EnsureSlotMesh(TinyGladeMeshComponent, TinyGladeMesh);

	BindTinyGladeMaterials(Materials);

	if (!UCSMeshOps::CopyFromMeshSnapshot(TinyGladeMesh, Snapshot)) return false;
	TinyGladeMeshComponent->SetGpuMesh(TinyGladeMesh);
	// 多槽才需要分批绘制；单槽时"整网格一个批次"本来就是正确语义。
	if (Materials.Num() > 1) UCSMeshOps::BuildMaterialSections(TinyGladeMesh);
	return true;
}

void ACSTinyGlade::BindTinyGladeMaterials(const TArray<TObjectPtr<UMaterialInterface>>& Materials)
{
	if (!TinyGladeMesh) return;
	BindMeshSlotMaterials(TinyGladeMeshComponent, TinyGladeMesh, Materials);
}

void ACSTinyGlade::BindMeshSlotMaterials(UCSMeshRenderComponent* Component, UCSMesh* Mesh,
	const TArray<TObjectPtr<UMaterialInterface>>& Materials, UMaterialInterface* DrawMaterial)
{
	if (!Component) return;

	// 组件的 MeshMaterial 也参与 ResolveBatchMaterials，所以要在广播之前更新 —— 无 section 表时
	// 它就是那唯一一个绘制批次的材质。
	UMaterialInterface* Draw = DrawMaterial ? DrawMaterial : (Materials.IsValidIndex(0) ? Materials[0].Get() : nullptr);
	const bool bDrawChanged = Component->MeshMaterial != Draw;
	if (bDrawChanged) Component->MeshMaterial = Draw;

	bool bSlotsChanged = false;
	if (Mesh)
	{
		// 直写数组 + 一次 NotifyMaterialsChanged（UCSMesh::Materials 的元素赋值拦不住，注释里
		// 指定的批量写法就是这条）。
		if (Mesh->Materials.Num() < Materials.Num())
		{
			Mesh->Materials.SetNum(Materials.Num());
			bSlotsChanged = true;
		}
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			if (Mesh->Materials[Slot] == Materials[Slot]) continue;
			Mesh->Materials[Slot] = Materials[Slot];
			bSlotsChanged = true;
		}
	}

	if (Mesh && (bDrawChanged || bSlotsChanged)) Mesh->NotifyMaterialsChanged();
	// 没有网格就没有广播可发，而组件代理在构造时就把材质抄走了 —— 光写属性看不出变化。
	else if (bDrawChanged) Component->MarkRenderStateDirty();
}

UCSMesh* ACSTinyGlade::EnsureSlotMesh(UCSMeshRenderComponent* Component, TObjectPtr<UCSMesh>& Mesh)
{
	UObject* MeshOuter = Component ? static_cast<UObject*>(Component) : static_cast<UObject*>(this);
	if (!Mesh || Mesh->GetOuter() != MeshOuter) Mesh = NewObject<UCSMesh>(MeshOuter);
	return Mesh;
}

bool ACSTinyGlade::IsSlotMeshLive(const UCSMesh* Mesh)
{
	const FCSMeshResident* Resident = Mesh ? Mesh->GetResidentPtr() : nullptr;
	return Resident && Resident->IsAllocated();
}

void ACSTinyGlade::SubmitMeshSlotAsync(UCSMeshRenderComponent* Component, TObjectPtr<UCSMesh>& Mesh, FCSMeshSlotState& Slot,
	const TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe>& Snapshot, const FCSMeshSlotUpload& Upload,
	TFunction<void()> OnSettled)
{
	if (!Snapshot.IsValid() || !Component) return;

	UCSMesh* Target = EnsureSlotMesh(Component, Mesh);

	// 材质表要在录图之前就绑好：分段数取自 Materials.Num()，排序 pass 要在同一张图里录进去。
	BindMeshSlotMaterials(Component, Target, Upload.Materials);

	// UV 组数是逐 mesh 的流布局变体，别人不为它付显存；同组数时这一步一点工作都不做。
	// 必须在容量与上传之前 —— 它会重建流集合。
	if (Upload.NumTexCoordSets > 1) UCSMeshOps::EnsureTexCoordSets(Target, Upload.NumTexCoordSets);

	// 在途 → 只留最新态。拖拽期间这条常态命中，被吸收掉的中间帧本来也没人看得见。
	if (ParkIfInFlight(Target, Slot.Pending, Snapshot)) return;

	UCSMeshOps::FCSMeshUploadPayload Payload;
	if (!UCSMeshOps::BuildUploadPayload(*Snapshot, Payload, UCSMeshOps::GetTexCoordSets(Target)))
	{
		UE_LOG(LogTinyGlade, Warning, TEXT("[TinyGlade] %s mesh upload failed (tris=%d)"),
			*GetName(), Snapshot->Indices.Num() / 3);
		return;
	}

	// 两条同步扩容都带早退，稳态下一次 enqueue 都没有；且必须在异步编辑**之前** ——
	// 在途时发起的同步路径虽然 FIFO 有序、结果正确，但会一直阻塞到两者都跑完，
	// 等于把省下的 flush 又还回去。
	// 按 CSShaperSteps::ReserveCount 预留而不是按精确数要：拖尺寸时顶点数是 FootprintSize 的连续函数，
	// 精确要就等于每一帧重新分配 + 拷贝一整份网格（EnsureCapacitySync 只涨不缩，但"涨"本身
	// 就是一次阻塞刷新）。多要的容量不画任何东西 —— 画多少由计数器决定，这正是这套 GPU 网格
	// 的设计前提。
	const bool bSort = Upload.bSortSections;
	const int32 NumSlots = FMath::Max(Target->Materials.Num(), 1);
	Target->EnsureCapacitySync(CSShaperSteps::ReserveCount(Payload.VertexCount), CSShaperSteps::ReserveCount(Payload.IndexCount));
	if (bSort) Target->EnsureIndirectDrawCapacitySync(NumSlots);
	Component->SetGpuMesh(Target);

	// 排序是否真的录进去了，只有渲染线程录图那一刻知道；游戏线程尾巴靠这个共享标志读。
	// 直接读一个 lambda 体内置位的裸 bool 会永远读到 false ⇒ 网格只画一个材质且不报错。
	TSharedPtr<bool, ESPMode::ThreadSafe> Sorted = MakeShared<bool, ESPMode::ThreadSafe>(false);
	// EditFunc 是 owned（TFunction 移入），它读到的一切也必须由它拥有 —— payload 因此走
	// 共享指针按值捕获，而不是 EditMeshSync 那种"捕获栈上快照的裸指针"（在这里是 use-after-free）。
	TSharedPtr<UCSMeshOps::FCSMeshUploadPayload, ESPMode::ThreadSafe> Owned =
		MakeShared<UCSMeshOps::FCSMeshUploadPayload, ESPMode::ThreadSafe>(MoveTemp(Payload));

	const bool bAccepted = Target->EditMeshAsync(
		[Owned, NumSlots, Sorted, bSort](FCSMeshEditContext& Context)
		{
			UCSMeshOps::AddCopyFromSnapshotPasses(Context, *Owned);
			if (bSort) *Sorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots);
		},
		[WeakThis = TWeakObjectPtr<ACSTinyGlade>(this), WeakMesh = TWeakObjectPtr<UCSMesh>(Target), Sorted,
			OnSettled = MoveTemp(OnSettled)](bool /*bMeshAlive*/)
		{
			if (!WeakThis.IsValid()) return;
			// SetSections 必须落在这条游戏线程尾巴上（异步化时最容易静默失效的一处，详见
			// UCSMeshOps::PublishMaterialSections 的注释），而且要赶在补发 pending 之前。
			UCSMesh* Settled = WeakMesh.Get();
			if (*Sorted && Settled) UCSMeshOps::PublishMaterialSections(Settled);
			if (OnSettled) OnSettled();
		});

	if (!bAccepted)
	{
		// 走到这里说明被拒的原因不是"在途"（那条上面已经拦了），而是某种意外状态。
		// 存进 pending 会永远没人来补发（OnComplete 不会触发），所以退回同步一次 ——
		// 宁可付一次 flush，也不能让网格静默地没有几何。
		UE_LOG(LogTinyGlade, Warning, TEXT("[TinyGlade] %s async mesh edit refused; falling back to a sync upload."), *GetName());
		UCSMeshOps::CopyFromMeshSnapshot(Target, *Snapshot);
		if (bSort) UCSMeshOps::BuildMaterialSections(Target);
	}
}

bool ACSTinyGlade::ApplyMeshSlotPlacement(UCSMesh* Mesh, FCSMeshSlotState& Slot, const FTransform& NewWorld, TFunction<void()> OnSettled)
{
	if (!Mesh) return false;

	// 常驻流是世界空间、渲染组件用绝对变换 ⇒ SetActorLocation 不会带动已生成的几何，
	// 得自己把它搬过去。变换 pass 同时改 Positions 与 Tangents（逆转置法线）并变换
	// WorldBounds，平移与 yaw 都支持。
	//
	// UE 的合成口径是 "C = A * B 先 A 后 B"，所以增量 = 先撤旧变换、再上新变换；写反了
	// 网格会在远离原点处飞走（旋转分量作用在未撤销的世界坐标上）。
	const FTransform Delta = Slot.BuiltAt.Inverse() * NewWorld;
	if (Delta.Equals(FTransform::Identity, 1.0e-4)) return true;   // 已经在位
	if (Mesh->IsEditInFlight()) return false;                      // 在途：留给完成回调重试

	const bool bAccepted = Mesh->EditMeshAsync(
		[Delta](FCSMeshEditContext& Context) { UCSMeshOps::AddTransformPasses(Context, Delta); },
		// 变换只改顶点、不动索引与材质分段，所以尾巴不发布分段表。
		[WeakThis = TWeakObjectPtr<ACSTinyGlade>(this), OnSettled = MoveTemp(OnSettled)](bool /*bMeshAlive*/)
		{
			if (WeakThis.IsValid() && OnSettled) OnSettled();
		});
	if (!bAccepted) return false;

	Slot.BuiltAt = NewWorld;
	return true;
}

void ACSTinyGlade::ReconcileMeshSlot(FCSMeshSlotState& Slot, uint32 ShapeHash, uint32 PlacementHash, bool bRebuild,
	TFunctionRef<void()> Rebuild, TFunctionRef<bool()> Place)
{
	if (bRebuild || ShapeHash != Slot.ShapeHash)
	{
		Rebuild();
		Slot.ShapeHash = ShapeHash;
		Slot.PlacementHash = PlacementHash;
		return;
	}
	// 只有真送出去了才推进哈希 —— 在途被拒时推进等于把这次移动丢掉。
	if (PlacementHash != Slot.PlacementHash && Place()) Slot.PlacementHash = PlacementHash;
}

bool ACSTinyGlade::IsMeshEditInFlight(const UCSMesh* Mesh)
{
	return Mesh && Mesh->IsEditInFlight();
}

void ACSTinyGlade::UnbindMeshComponent(UCSMeshRenderComponent* Component)
{
	if (Component) Component->SetGpuMesh(nullptr);
}

// -----------------------------------------------------------------------------
// 实例族：诊断 / 烘焙
// -----------------------------------------------------------------------------

FString ACSTinyGlade::DebugGetGpuAssetMismatchSync() const
{
	TArray<FCSInstancedFamily> Families;
	GetInstancedFamilies(Families);
	for (const FCSInstancedFamily& Family : Families)
	{
		// 没有组件、或这一轮按设计不画，都不算"画错了" —— 那是 `Is*Drawable` 那一族的职责范围，
		// 这里只答"画的是不是那个"。闲置组件没分配 GPU 网格，不跳过就会被误报。
		if (!Family.bExpectDrawn || !IsValid(Family.Component)) continue;
		const FString Reason = Family.Component->DebugGetDrawnAssetMismatchSync();
		if (!Reason.IsEmpty()) return FString::Printf(TEXT("%s：%s"), *Family.Label, *Reason);
	}
	return FString();
}

#if WITH_EDITOR
int32 ACSTinyGlade::SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets)
{
	const FString Folder = BakeFolder.TrimStartAndEnd().IsEmpty()
		? FString::Printf(TEXT("/Game/TinyGladeBake/%s"), *GetName())
		: BakeFolder.TrimStartAndEnd();

	TArray<FCSInstancedFamily> Families;
	GetInstancedFamilies(Families);

	int32 Saved = 0;
	for (const FCSInstancedFamily& Family : Families)
	{
		if (!IsValid(Family.Component)) continue;
		const FString Path = FString::Printf(TEXT("%s/SM_%s_%s"), *Folder, *GetName(), *Family.AssetSuffix);
		// 烘回本 actor 的局部空间：资产摆在同一个变换上就复现画面（同岩壳 / 道路那两条出口）。
		if (Family.Component->SaveToStaticMesh(GetActorTransform(), Path, /*bReplaceExistingAsset*/ true, bSaveAssets)) ++Saved;
	}

	UE_LOG(LogTinyGlade, Log, TEXT("[TinyGlade] %s 实例路烘焙：%d 张资产 -> %s"), *GetName(), Saved, *Folder);
	return Saved;
}
#endif

// -----------------------------------------------------------------------------
// AActor
// -----------------------------------------------------------------------------

void ACSTinyGlade::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// 构造脚本重跑 = 一次唤醒：改属性、撤销、松手，以及**蓝图**实例的逐帧拖动（引擎在
	// AActor::PostEditChangeProperty / PostEditMove 里重跑构造脚本）。原生类的逐帧拖动**不**重跑
	// （AActor::bRunConstructionScriptOnDrag 默认关），那条由派生类自己的 PostEditMove 兜。
	ReevaluateSite();
}

void ACSTinyGlade::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 只放 actor 自己分配的那份；组件那一份由组件在 OnComponentDestroyed 里自己放（见类注释）。
	// 覆盖 PIE 结束与关卡卸载 —— 这两条 `Destroyed` 收不到。
	ReleaseInstancedBuffers();
	Super::EndPlay(EndPlayReason);
}

void ACSTinyGlade::Destroyed()
{
	// **编辑器 world 里只有这一条会来**（那个 world 没有 begun play，`DestroyActor` 不发 `EndPlay`）。
	ReleaseInstancedBuffers();
	Super::Destroyed();
}
