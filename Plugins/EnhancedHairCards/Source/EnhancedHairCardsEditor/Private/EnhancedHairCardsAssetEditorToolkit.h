#pragma once

#include "CoreMinimal.h"
#include "PropertyEditorDelegates.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Widgets/Views/SListView.h"

class FPreviewScene;
class IDetailsView;
class IToolkitHost;
class SEnhancedHairCardsEditorViewport;
class SListViewBase;
class UEnhancedHairCardsAsset;
class UEnhancedHairCardsComponent;
class UEnhancedHairCardsPartEditorObject;
class UGroomComponent;

struct FEnhancedHairCardsPart;

class FEnhancedHairCardsAssetEditorToolkit : public FAssetEditorToolkit, public FGCObject
{
public:
	void InitEnhancedHairCardsAssetEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UEnhancedHairCardsAsset* InAsset);

	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

	UEnhancedHairCardsAsset* GetAsset() const { return HairCardsAsset; }
	FPreviewScene& GetPreviewScene() const { return *PreviewScene; }
	void RebuildPreviewComponents();
	void FocusViewport();
	void SelectPart(int32 PartIndex);

private:
	TSharedRef<SDockTab> SpawnRenderTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnCardsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnMaterialTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnGuidesTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnPreviewTab(const FSpawnTabArgs& Args);

	TSharedRef<SWidget> BuildCardsPanel();
	void CreateDetailsViews();
	void ExtendToolbar();
	void AddToolbarButtons(FToolBarBuilder& ToolbarBuilder);
	void HandleFinishedChangingProperties(const FPropertyChangedEvent& PropertyChangedEvent);
	bool IsPartPropertyVisible(const FPropertyAndParent& PropertyAndParent) const;
	bool IsMaterialPropertyVisible(const FPropertyAndParent& PropertyAndParent) const;
	bool IsGuidesPropertyVisible(const FPropertyAndParent& PropertyAndParent) const;
	bool IsPreviewPropertyVisible(const FPropertyAndParent& PropertyAndParent) const;
	bool IsPreviewSettingsPropertyChange(const FPropertyChangedEvent& PropertyChangedEvent) const;
	bool IsCardSettingsPropertyChange(const FPropertyChangedEvent& PropertyChangedEvent) const;
	void SyncSelectedPartFromEditorObject();
	void ApplyPreviewVisibility();
	void ApplySelectedPartSettingsToPreview();
	void RefreshPreviewViewport();
	void RefreshLevelActorsFromAsset(bool bForceRebuild = false);
	void RefreshPartList();
	void RebuildFromGroom();
	void ToggleOriginalGroom();
	void ToggleEnhancedCards();
	void ToggleGuides();
	void ExportBlueprint();
	FText GetPartRowText(int32 PartIndex) const;

	TObjectPtr<UEnhancedHairCardsAsset> HairCardsAsset = nullptr;
	TObjectPtr<UEnhancedHairCardsPartEditorObject> PartEditorObject = nullptr;
	int32 SelectedPartIndex = INDEX_NONE;

	TSharedPtr<FPreviewScene> PreviewScene;
	TObjectPtr<UGroomComponent> PreviewGroomComponent = nullptr;
	TArray<TObjectPtr<UEnhancedHairCardsComponent>> PreviewCardComponents;

	TSharedPtr<SEnhancedHairCardsEditorViewport> ViewportWidget;
	TSharedPtr<IDetailsView> AssetDetailsView;
	TSharedPtr<IDetailsView> PartDetailsView;
	TSharedPtr<IDetailsView> MaterialDetailsView;
	TSharedPtr<IDetailsView> GuidesDetailsView;
	TArray<TSharedPtr<int32>> PartItems;
	TSharedPtr<SListView<TSharedPtr<int32>>> PartsListView;
};
