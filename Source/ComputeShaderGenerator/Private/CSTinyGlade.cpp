#include "CSTinyGlade.h"

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"

ACSTinyGlade::ACSTinyGlade()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	TinyGladeMeshComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("TinyGladeMesh"));
	TinyGladeMeshComponent->SetupAttachment(Root);
}

bool ACSTinyGlade::UploadTinyGladeSnapshot(const FCSGpuMeshCPUData& Snapshot, const TArray<TObjectPtr<UMaterialInterface>>& Materials)
{
	if (!TinyGladeMeshComponent || !Snapshot.IsValid()) return false;

	if (!TinyGladeMesh) TinyGladeMesh = NewObject<UCSMesh>(this);

	BindTinyGladeMaterials(Materials);

	if (!UCSMeshOps::CopyFromMeshSnapshot(TinyGladeMesh, Snapshot)) return false;
	TinyGladeMeshComponent->SetGpuMesh(TinyGladeMesh);
	return true;
}

void ACSTinyGlade::BindTinyGladeMaterials(const TArray<TObjectPtr<UMaterialInterface>>& Materials)
{
	if (!TinyGladeMeshComponent || !TinyGladeMesh) return;

	// 直写数组 + 一次 NotifyMaterialsChanged（UCSMesh::Materials 的元素赋值拦不住，注释里
	// 指定的批量写法就是这条）；组件的 MeshMaterial 也参与 ResolveBatchMaterials，所以要在
	// 广播之前更新 —— 无 section 表时它就是那唯一一个绘制批次的材质。
	if (TinyGladeMesh->Materials.Num() < Materials.Num()) TinyGladeMesh->Materials.SetNum(Materials.Num());
	for (int32 Slot = 0; Slot < Materials.Num(); ++Slot) TinyGladeMesh->Materials[Slot] = Materials[Slot];
	TinyGladeMeshComponent->MeshMaterial = Materials.IsValidIndex(0) ? Materials[0].Get() : nullptr;
	TinyGladeMesh->NotifyMaterialsChanged();
}
