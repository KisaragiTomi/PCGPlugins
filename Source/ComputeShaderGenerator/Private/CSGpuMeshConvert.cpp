#include "CSGpuMeshConvert.h"

#include "CSGpuMeshSave.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#endif

namespace
{
FName ConvertMaterialSlotName(int32 Slot)
{
	return FName(*FString::Printf(TEXT("MaterialSlot_%d"), Slot));
}
}

UMaterialInterface* CSGpuMeshConvert::GetDefaultSurfaceMaterial()
{
	return UMaterial::GetDefaultMaterial(MD_Surface);
}

bool CSGpuMeshConvert::BuildMeshDescription(
	const FCSGpuMeshCPUData& MeshData,
	const FConvertOptions& Options,
	FMeshDescription& OutMeshDescription)
{
	// 属性装配仍复用已验证的实现（绕序、切线正交化、退化面过滤、per-corner/per-vertex 判别
	// 都在里面），这里只负责把统一的选项翻译过去。后续把那份实现搬进本文件时，调用方无感。
	//
	// 只有当数据确实是世界空间、且调用方要求烘到局部空间时才做变换。以前靠单个
	// bConvertToActorLocalSpace 表达，数据本就是局部空间的 leaf 只能靠默认 false 绕开，
	// 语义含混；现在由 MeshData.SourceSpace 说明数据自己是什么空间。
	const bool bNeedsBake = Options.bBakeToLocalSpace
		&& MeshData.SourceSpace == FCSGpuMeshCPUData::ESpace::World;
	return CSGpuMeshSave::BuildGpuMeshDescription(
		MeshData, Options.TargetTransform, bNeedsBake, OutMeshDescription);
}

UStaticMesh* CSGpuMeshConvert::BuildStaticMesh(
	UObject* Outer,
	const AActor* OwnerActor,
	const FCSGpuMeshCPUData& MeshData,
	const TArray<UMaterialInterface*>& Materials,
	const FConvertOptions& Options,
	const FAssetOptions& AssetOptions)
{
	// 材质表优先取数据自带的 MeshData.Materials；调用方仍可用 Materials 参数覆盖（迁移期的
	// 旧调用点走这条）。两者都空时下面的兜底会按槽位数补默认材质。
	TArray<UMaterialInterface*> ResolvedMaterials;
	if (Materials.Num() > 0)
	{
		ResolvedMaterials = Materials;
	}
	else
	{
		ResolvedMaterials.Reserve(MeshData.Materials.Num());
		for (const TObjectPtr<UMaterialInterface>& Material : MeshData.Materials) ResolvedMaterials.Add(Material.Get());
	}

	// 槽位数至少要覆盖 TriangleMaterialSlots 里出现过的最大槽号，否则落盘时会因槽位缺失而丢面。
	int32 RequiredSlots = ResolvedMaterials.Num();
	for (int32 Slot : MeshData.TriangleMaterialSlots) RequiredSlots = FMath::Max(RequiredSlots, Slot + 1);
	if (RequiredSlots > ResolvedMaterials.Num()) ResolvedMaterials.SetNumZeroed(RequiredSlots);

	// 空槽兜底：注册表允许材质为空（源网格未指定），但输出网格带空槽会渲染成默认灰，
	// 且在编辑器里无法区分「未指定」与「指定了灰材质」。统一填引擎默认表面材质，
	// 槽位数量和顺序不变，用户仍可逐槽替换。
	if (Options.bFillEmptySlotsWithDefaultMaterial)
	{
		UMaterialInterface* DefaultMaterial = GetDefaultSurfaceMaterial();
		for (UMaterialInterface*& Material : ResolvedMaterials)
		{
			if (!Material) Material = DefaultMaterial;
		}
	}

	// 与 BuildMeshDescription 同一判据：数据已是局部空间就不再变换。
	const bool bNeedsBake = Options.bBakeToLocalSpace
		&& MeshData.SourceSpace == FCSGpuMeshCPUData::ESpace::World;

	if (AssetOptions.bTransient)
	{
		return CSGpuMeshSave::BuildTransientStaticMesh(
			Outer, MeshData, ResolvedMaterials, Options.TargetTransform, bNeedsBake);
	}

#if WITH_EDITOR
	return CSGpuMeshSave::SaveGpuMeshDataToStaticMesh(
		OwnerActor, MeshData, ResolvedMaterials, Options.TargetTransform, bNeedsBake,
		AssetOptions.AssetPath, AssetOptions.bReplaceExisting, AssetOptions.bSaveToDisk);
#else
	// 非编辑器构建没有资产系统，只能退回 transient。
	return CSGpuMeshSave::BuildTransientStaticMesh(
		Outer, MeshData, ResolvedMaterials, Options.TargetTransform, bNeedsBake);
#endif
}
