#include "AssetToolsModule.h"
#include "EnhancedHairCardsAssetTypeActions.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

class FEnhancedHairCardsEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		const EAssetTypeCategories::Type AssetCategory = AssetTools.RegisterAdvancedAssetCategory(
			TEXT("EnhancedHairCards"),
			NSLOCTEXT("EnhancedHairCardsEditor", "EnhancedHairCardsCategory", "Enhanced Hair Cards"));

		HairCardsAssetTypeActions = MakeShared<FEnhancedHairCardsAssetTypeActions>(AssetCategory);
		AssetTools.RegisterAssetTypeActions(HairCardsAssetTypeActions.ToSharedRef());
	}

	virtual void ShutdownModule() override
	{
		if (!FModuleManager::Get().IsModuleLoaded("AssetTools"))
		{
			HairCardsAssetTypeActions.Reset();
			return;
		}

		if (HairCardsAssetTypeActions.IsValid())
		{
			IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
			AssetTools.UnregisterAssetTypeActions(HairCardsAssetTypeActions.ToSharedRef());
			HairCardsAssetTypeActions.Reset();
		}
	}

private:
	TSharedPtr<IAssetTypeActions> HairCardsAssetTypeActions;
};

IMPLEMENT_MODULE(FEnhancedHairCardsEditorModule, EnhancedHairCardsEditor)
