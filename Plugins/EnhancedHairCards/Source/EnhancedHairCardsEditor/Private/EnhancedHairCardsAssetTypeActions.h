#pragma once

#include "AssetTypeActions_Base.h"

class UEnhancedHairCardsAsset;

class FEnhancedHairCardsAssetTypeActions : public FAssetTypeActions_Base
{
public:
	explicit FEnhancedHairCardsAssetTypeActions(EAssetTypeCategories::Type InAssetCategory);

	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor = TSharedPtr<IToolkitHost>()) override;

private:
	EAssetTypeCategories::Type AssetCategory;
};
