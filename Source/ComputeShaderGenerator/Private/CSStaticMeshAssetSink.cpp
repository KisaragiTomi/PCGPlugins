#include "CSStaticMeshAssetSink.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshAttributes.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#endif

namespace CSStaticMeshAsset
{

FName MaterialSlotName(int32 Slot)
{
	return FName(*FString::Printf(TEXT("MaterialSlot_%d"), Slot));
}

bool PopulateFromDescription(
	UStaticMesh* StaticMesh,
	FMeshDescription& MeshDescription,
	const TArray<UMaterialInterface*>& Materials,
	TArrayView<const int32> TriangleMaterialSlots,
	bool bCommitMeshDescription,
	bool bEnableNanite)
{
	if (!StaticMesh) return false;

	// 必须在 BuildFromMeshDescriptions 之前设置：Nanite 数据是在那次构建里生成的，
	// 建完再改设置只会标脏，不会真的产出 Nanite 数据。
	{
		FMeshNaniteSettings NaniteSettings = StaticMesh->GetNaniteSettings();
		NaniteSettings.bEnabled = bEnableNanite;
		StaticMesh->SetNaniteSettings(NaniteSettings);
	}

	int32 RequiredMaterialSlots = FMath::Max(1, Materials.Num());
	for (int32 Slot : TriangleMaterialSlots) RequiredMaterialSlots = FMath::Max(RequiredMaterialSlots, Slot + 1);
	for (int32 Slot = 0; Slot < RequiredMaterialSlots; ++Slot)
	{
		UMaterialInterface* Material = Materials.IsValidIndex(Slot) ? Materials[Slot] : nullptr;
		const FName SlotName = MaterialSlotName(Slot);
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(Material, SlotName, SlotName));
	}

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bBuildSimpleCollision = false;
	// The fast path (UStaticMesh::BuildFromMeshDescription) emits one render vertex per vertex
	// instance and never merges them, so a perfectly welded MeshDescription still renders with
	// 3 vertices per triangle - welding looks like it did nothing and the buffers stay large.
	// Saved assets therefore take the full build, which merges vertices by attribute equality.
	// Transient results are throwaway previews, so they keep the fast path.
	BuildParams.bFastBuild = !bCommitMeshDescription;
	// A saved asset must keep its editable source data; a transient one must not pay for it.
	BuildParams.bCommitMeshDescription = bCommitMeshDescription;
	return StaticMesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams);
}

#if WITH_EDITOR

bool ResolveTarget(
	const FString& AssetPath,
	bool bReplaceExisting,
	const TCHAR* LogPrefix,
	FCSStaticMeshAssetTarget& OutTarget)
{
	OutTarget = FCSStaticMeshAssetTarget();

	const FString Trimmed = AssetPath.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s Save failed: empty asset path."), LogPrefix);
		return false;
	}

	OutTarget.SanitizedPath = UPackageTools::SanitizePackageName(Trimmed);
	if (!FPackageName::IsValidLongPackageName(OutTarget.SanitizedPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("%s Save failed: invalid asset path '%s'."), LogPrefix, *OutTarget.SanitizedPath);
		return false;
	}

	OutTarget.AssetName = FPackageName::GetLongPackageAssetName(OutTarget.SanitizedPath);
	if (OutTarget.AssetName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s Save failed: '%s' has no asset name."), LogPrefix, *OutTarget.SanitizedPath);
		return false;
	}

	const FString AssetFolderPath = FPackageName::GetLongPackagePath(OutTarget.SanitizedPath);
	if (!AssetFolderPath.IsEmpty()
		&& !UEditorAssetLibrary::DoesDirectoryExist(AssetFolderPath)
		&& !UEditorAssetLibrary::MakeDirectory(AssetFolderPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("%s Save failed: could not create asset folder '%s'."), LogPrefix, *AssetFolderPath);
		return false;
	}

	// 同名资产已存在时优先「就地重建」而不是删掉重建：删除会打断所有已有引用（生成方持有的
	// OutputStaticMesh、场上的 StaticMeshComponent、其他关卡），而且只要还有人在内存里引用它，
	// DeleteAsset 本身就会失败，重跑等于永远存不上。就地重建保留同一个 UObject，引用自然跟着更新。
	if (UEditorAssetLibrary::DoesAssetExist(OutTarget.SanitizedPath))
	{
		if (!bReplaceExisting)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s Save skipped: asset already exists at '%s'."), LogPrefix, *OutTarget.SanitizedPath);
			return false;
		}
		OutTarget.ExistingMesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(OutTarget.SanitizedPath));
		// 同名的不是 StaticMesh（材质、贴图……）就没法就地重建，只能按老路子删掉。
		// 真正的删除推迟到 PrepareMesh，这样几何构建失败时不会白白毁掉一个资产。
		OutTarget.bMustDeleteConflicting = OutTarget.ExistingMesh == nullptr;
	}

	return true;
}

UStaticMesh* PrepareMesh(const FCSStaticMeshAssetTarget& Target, const TCHAR* LogPrefix)
{
	if (!Target.IsValid()) return nullptr;

	if (Target.bMustDeleteConflicting && !UEditorAssetLibrary::DeleteAsset(Target.SanitizedPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("%s Save failed: could not replace '%s'."), LogPrefix, *Target.SanitizedPath);
		return nullptr;
	}

	UPackage* Package = Target.ExistingMesh ? Target.ExistingMesh->GetPackage() : CreatePackage(*Target.SanitizedPath);
	if (!Package)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s Save failed: could not create package for '%s'."), LogPrefix, *Target.SanitizedPath);
		return nullptr;
	}
	Package->FullyLoad();

	if (UStaticMesh* Existing = Target.ExistingMesh)
	{
		Existing->Modify();
		// 材质槽与 section 映射是按本次网格重新装配的，先清空，否则上一轮的槽位会残留在前面。
		Existing->GetStaticMaterials().Empty();
		Existing->GetSectionInfoMap().Clear();
		return Existing;
	}

	return NewObject<UStaticMesh>(Package, *Target.AssetName, RF_Public | RF_Standalone);
}

void Finalize(
	UStaticMesh* StaticMesh,
	const FCSStaticMeshAssetTarget& Target,
	bool bSaveToDisk,
	const TCHAR* LogPrefix)
{
	if (!StaticMesh) return;

	// 就地重建的资产 registry 里本来就有，再报一次 AssetCreated 会多出一条重复记录。
	if (!Target.ExistingMesh) FAssetRegistryModule::AssetCreated(StaticMesh);
	StaticMesh->MarkPackageDirty();
	if (UPackage* Package = StaticMesh->GetPackage()) Package->SetDirtyFlag(true);

	if (bSaveToDisk && !UEditorAssetLibrary::SaveLoadedAsset(StaticMesh, false))
	{
		UE_LOG(LogTemp, Warning, TEXT("%s Created '%s' but failed to write the package to disk."),
			LogPrefix, *Target.SanitizedPath);
	}
}

#endif // WITH_EDITOR

} // namespace CSStaticMeshAsset
