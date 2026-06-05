#include "EnhancedHairCardsAssetTypeActions.h"

#include "EnhancedHairCardsAsset.h"
#include "EnhancedHairCardsAssetEditorToolkit.h"

#define LOCTEXT_NAMESPACE "EnhancedHairCardsAssetTypeActions"

FEnhancedHairCardsAssetTypeActions::FEnhancedHairCardsAssetTypeActions(EAssetTypeCategories::Type InAssetCategory)
	: AssetCategory(InAssetCategory)
{
}

FText FEnhancedHairCardsAssetTypeActions::GetName() const
{
	return LOCTEXT("EnhancedHairCardsAssetName", "Enhanced Hair Cards");
}

FColor FEnhancedHairCardsAssetTypeActions::GetTypeColor() const
{
	return FColor(38, 168, 214);
}

UClass* FEnhancedHairCardsAssetTypeActions::GetSupportedClass() const
{
	return UEnhancedHairCardsAsset::StaticClass();
}

uint32 FEnhancedHairCardsAssetTypeActions::GetCategories()
{
	return AssetCategory;
}

void FEnhancedHairCardsAssetTypeActions::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	const EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;
	for (UObject* Object : InObjects)
	{
		if (UEnhancedHairCardsAsset* HairCardsAsset = Cast<UEnhancedHairCardsAsset>(Object))
		{
			TSharedRef<FEnhancedHairCardsAssetEditorToolkit> Editor = MakeShared<FEnhancedHairCardsAssetEditorToolkit>();
			Editor->InitEnhancedHairCardsAssetEditor(Mode, EditWithinLevelEditor, HairCardsAsset);
		}
	}
}

#undef LOCTEXT_NAMESPACE
