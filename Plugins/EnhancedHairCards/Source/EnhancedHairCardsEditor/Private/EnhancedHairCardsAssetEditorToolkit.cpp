#include "EnhancedHairCardsAssetEditorToolkit.h"

#include "AdvancedPreviewScene.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "EnhancedHairCardsActor.h"
#include "EnhancedHairCardsAsset.h"
#include "EnhancedHairCardsComponent.h"
#include "EnhancedHairCardsConverterLibrary.h"
#include "EnhancedHairCardsEditorObjects.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GroomComponent.h"
#include "IDetailsView.h"
#include "Input/Reply.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "PropertyEditorModule.h"
#include "SEditorViewport.h"
#include "Styling/AppStyle.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "EnhancedHairCardsAssetEditor"

namespace EnhancedHairCardsEditor
{
	static const FName AppIdentifier(TEXT("EnhancedHairCardsAssetEditorApp"));
	static const FName RenderTabId(TEXT("EnhancedHairCardsAssetEditor_Render"));
	static const FName CardsTabId(TEXT("EnhancedHairCardsAssetEditor_Cards"));
	static const FName MaterialTabId(TEXT("EnhancedHairCardsAssetEditor_Material"));
	static const FName GuidesTabId(TEXT("EnhancedHairCardsAssetEditor_Guides"));
	static const FName PreviewTabId(TEXT("EnhancedHairCardsAssetEditor_Preview"));

	static bool IsNamedOrChildProperty(const FPropertyAndParent& PropertyAndParent, const TSet<FName>& RootNames)
	{
		if (RootNames.Contains(PropertyAndParent.Property.GetFName()))
		{
			return true;
		}

		for (const FProperty* ParentProperty : PropertyAndParent.ParentProperties)
		{
			if (ParentProperty && RootNames.Contains(ParentProperty->GetFName()))
			{
				return true;
			}
		}

		return false;
	}

	static bool IsRootOrSelectedNestedProperty(
		const FPropertyAndParent& PropertyAndParent,
		FName RootName,
		const TSet<FName>& NestedNames)
	{
		const FName PropertyName = PropertyAndParent.Property.GetFName();
		if (PropertyName == RootName || NestedNames.Contains(PropertyName))
		{
			return true;
		}

		for (const FProperty* ParentProperty : PropertyAndParent.ParentProperties)
		{
			if (ParentProperty && NestedNames.Contains(ParentProperty->GetFName()))
			{
				return true;
			}
		}

		return false;
	}

	class FEnhancedHairCardsViewportClient : public FEditorViewportClient
	{
	public:
		FEnhancedHairCardsViewportClient(FPreviewScene& InPreviewScene, const TSharedRef<SEditorViewport>& InViewport)
			: FEditorViewportClient(nullptr, &InPreviewScene, InViewport)
		{
			SetViewportType(LVT_Perspective);
			SetViewMode(VMI_Lit);
			SetViewLocation(FVector(-220.0, -320.0, 180.0));
			SetViewRotation(FRotator(-18.0, 38.0, 0.0));
			bSetListenerPosition = false;
			bUsingOrbitCamera = true;

			EngineShowFlags.SetGrid(false);
			EngineShowFlags.SetSnap(false);
			EngineShowFlags.SetCompositeEditorPrimitives(true);
			EngineShowFlags.SetSelectionOutline(false);
		}

		virtual void Tick(float DeltaSeconds) override
		{
			FEditorViewportClient::Tick(DeltaSeconds);
			if (PreviewScene)
			{
				PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
			}
		}

		virtual bool ShouldOrbitCamera() const override
		{
			return true;
		}
	};
}

class SEnhancedHairCardsEditorViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SEnhancedHairCardsEditorViewport) {}
		SLATE_ARGUMENT(TWeakPtr<FEnhancedHairCardsAssetEditorToolkit>, EditorToolkit)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		EditorToolkit = InArgs._EditorToolkit;
		SEditorViewport::Construct(SEditorViewport::FArguments());
	}

	void FocusOnPreview()
	{
		if (EditorViewportClient.IsValid())
		{
			EditorViewportClient->FocusViewportOnBox(CachedPreviewBounds, true);
		}
	}

	void SetPreviewBounds(const FBox& InBounds)
	{
		CachedPreviewBounds = InBounds.IsValid ? InBounds : FBox(FVector(-50.0), FVector(50.0));
	}

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
	{
		TSharedPtr<FEnhancedHairCardsAssetEditorToolkit> Toolkit = EditorToolkit.Pin();
		check(Toolkit.IsValid());

		EditorViewportClient = MakeShared<EnhancedHairCardsEditor::FEnhancedHairCardsViewportClient>(
			Toolkit->GetPreviewScene(),
			SharedThis(this));
		EditorViewportClient->VisibilityDelegate.BindSP(this, &SEnhancedHairCardsEditorViewport::IsVisible);
		return EditorViewportClient.ToSharedRef();
	}

	virtual TSharedPtr<SWidget> BuildViewportToolbar() override
	{
		return SNullWidget::NullWidget;
	}

	virtual void OnFocusViewportToSelection() override
	{
		FocusOnPreview();
	}

private:
	TWeakPtr<FEnhancedHairCardsAssetEditorToolkit> EditorToolkit;
	TSharedPtr<EnhancedHairCardsEditor::FEnhancedHairCardsViewportClient> EditorViewportClient;
	FBox CachedPreviewBounds = FBox(FVector(-50.0), FVector(50.0));
};

void FEnhancedHairCardsAssetEditorToolkit::InitEnhancedHairCardsAssetEditor(
	EToolkitMode::Type Mode,
	const TSharedPtr<IToolkitHost>& InitToolkitHost,
	UEnhancedHairCardsAsset* InAsset)
{
	HairCardsAsset = InAsset;
	SelectedPartIndex = HairCardsAsset && HairCardsAsset->Parts.Num() > 0 ? 0 : INDEX_NONE;

	PreviewScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
	PartEditorObject = NewObject<UEnhancedHairCardsPartEditorObject>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);

	CreateDetailsViews();
	ExtendToolbar();

	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout(TEXT("Standalone_EnhancedHairCardsAssetEditor_Layout_v1"))
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
			->Split
			(
				FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.64f)
					->AddTab(EnhancedHairCardsEditor::RenderTabId, ETabState::OpenedTab)
					->SetHideTabWell(true)
				)
				->Split
				(
					FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)
					->SetSizeCoefficient(0.36f)
					->Split
					(
						FTabManager::NewStack()
						->SetSizeCoefficient(0.56f)
						->AddTab(EnhancedHairCardsEditor::CardsTabId, ETabState::OpenedTab)
					)
					->Split
					(
						FTabManager::NewStack()
						->SetSizeCoefficient(0.44f)
						->AddTab(EnhancedHairCardsEditor::PreviewTabId, ETabState::OpenedTab)
						->AddTab(EnhancedHairCardsEditor::MaterialTabId, ETabState::ClosedTab)
						->AddTab(EnhancedHairCardsEditor::GuidesTabId, ETabState::ClosedTab)
					)
				)
			)
		);

	InitAssetEditor(
		Mode,
		InitToolkitHost,
		EnhancedHairCardsEditor::AppIdentifier,
		StandaloneDefaultLayout,
		true,
		true,
		InAsset);

	RegenerateMenusAndToolbars();
	SelectPart(SelectedPartIndex);
	RebuildPreviewComponents();
}

FName FEnhancedHairCardsAssetEditorToolkit::GetToolkitFName() const
{
	return FName(TEXT("EnhancedHairCardsAssetEditor"));
}

FText FEnhancedHairCardsAssetEditorToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "Enhanced Hair Cards");
}

FString FEnhancedHairCardsAssetEditorToolkit::GetWorldCentricTabPrefix() const
{
	return TEXT("Enhanced Hair Cards");
}

FLinearColor FEnhancedHairCardsAssetEditorToolkit::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.15f, 0.66f, 0.84f, 0.5f);
}

void FEnhancedHairCardsAssetEditorToolkit::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu", "Enhanced Hair Cards"));
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(EnhancedHairCardsEditor::RenderTabId, FOnSpawnTab::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::SpawnRenderTab))
		.SetDisplayName(LOCTEXT("RenderTab", "Render"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

	InTabManager->RegisterTabSpawner(EnhancedHairCardsEditor::CardsTabId, FOnSpawnTab::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::SpawnCardsTab))
		.SetDisplayName(LOCTEXT("CardsTab", "Cards"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.StaticMesh"));

	InTabManager->RegisterTabSpawner(EnhancedHairCardsEditor::MaterialTabId, FOnSpawnTab::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::SpawnMaterialTab))
		.SetDisplayName(LOCTEXT("MaterialTab", "Material"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.Material"));

	InTabManager->RegisterTabSpawner(EnhancedHairCardsEditor::GuidesTabId, FOnSpawnTab::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::SpawnGuidesTab))
		.SetDisplayName(LOCTEXT("GuidesTab", "Guides"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Show"));

	InTabManager->RegisterTabSpawner(EnhancedHairCardsEditor::PreviewTabId, FOnSpawnTab::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::SpawnPreviewTab))
		.SetDisplayName(LOCTEXT("PreviewTab", "Preview"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FEnhancedHairCardsAssetEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(EnhancedHairCardsEditor::RenderTabId);
	InTabManager->UnregisterTabSpawner(EnhancedHairCardsEditor::CardsTabId);
	InTabManager->UnregisterTabSpawner(EnhancedHairCardsEditor::MaterialTabId);
	InTabManager->UnregisterTabSpawner(EnhancedHairCardsEditor::GuidesTabId);
	InTabManager->UnregisterTabSpawner(EnhancedHairCardsEditor::PreviewTabId);
}

void FEnhancedHairCardsAssetEditorToolkit::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(HairCardsAsset);
	Collector.AddReferencedObject(PartEditorObject);
	Collector.AddReferencedObject(PreviewGroomComponent);
	Collector.AddReferencedObjects(PreviewCardComponents);
}

FString FEnhancedHairCardsAssetEditorToolkit::GetReferencerName() const
{
	return TEXT("FEnhancedHairCardsAssetEditorToolkit");
}

TSharedRef<SDockTab> FEnhancedHairCardsAssetEditorToolkit::SpawnRenderTab(const FSpawnTabArgs& Args)
{
	if (!ViewportWidget.IsValid())
	{
		ViewportWidget = SNew(SEnhancedHairCardsEditorViewport)
			.EditorToolkit(StaticCastSharedRef<FEnhancedHairCardsAssetEditorToolkit>(AsShared()));
	}

	return SNew(SDockTab)
		.Label(LOCTEXT("RenderTabLabel", "Render"))
		[
			ViewportWidget.ToSharedRef()
		];
}

TSharedRef<SDockTab> FEnhancedHairCardsAssetEditorToolkit::SpawnCardsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("CardsTabLabel", "Cards"))
		[
			BuildCardsPanel()
		];
}

TSharedRef<SDockTab> FEnhancedHairCardsAssetEditorToolkit::SpawnMaterialTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("MaterialTabLabel", "Material"))
		[
			MaterialDetailsView.IsValid() ? MaterialDetailsView.ToSharedRef() : SNullWidget::NullWidget
		];
}

TSharedRef<SDockTab> FEnhancedHairCardsAssetEditorToolkit::SpawnGuidesTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("GuidesTabLabel", "Guides"))
		[
			GuidesDetailsView.IsValid() ? GuidesDetailsView.ToSharedRef() : SNullWidget::NullWidget
		];
}

TSharedRef<SDockTab> FEnhancedHairCardsAssetEditorToolkit::SpawnPreviewTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("PreviewTabLabel", "Preview"))
		[
			AssetDetailsView.IsValid() ? AssetDetailsView.ToSharedRef() : SNullWidget::NullWidget
		];
}

TSharedRef<SWidget> FEnhancedHairCardsAssetEditorToolkit::BuildCardsPanel()
{
	PartItems.Reset();
	if (HairCardsAsset)
	{
		for (int32 PartIndex = 0; PartIndex < HairCardsAsset->Parts.Num(); ++PartIndex)
		{
			PartItems.Add(MakeShared<int32>(PartIndex));
		}
	}

	PartsListView = SNew(SListView<TSharedPtr<int32>>)
		.ListItemsSource(&PartItems)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow_Lambda([this](TSharedPtr<int32> Item, const TSharedRef<STableViewBase>& OwnerTable)
		{
			const int32 PartIndex = Item.IsValid() ? *Item : INDEX_NONE;
			return SNew(STableRow<TSharedPtr<int32>>, OwnerTable)
				[
					SNew(STextBlock)
					.Text(GetPartRowText(PartIndex))
				];
		})
		.OnSelectionChanged_Lambda([this](TSharedPtr<int32> Item, ESelectInfo::Type)
		{
			SelectPart(Item.IsValid() ? *Item : INDEX_NONE);
		});

	if (PartItems.IsValidIndex(SelectedPartIndex))
	{
		PartsListView->SetSelection(PartItems[SelectedPartIndex]);
	}

	return SNew(SSplitter)
		.Orientation(Orient_Vertical)
		+ SSplitter::Slot()
		.Value(0.32f)
		[
			SNew(SBorder)
			.Padding(4.0f)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				PartsListView.ToSharedRef()
			]
		]
		+ SSplitter::Slot()
		.Value(0.68f)
		[
			PartDetailsView.IsValid() ? PartDetailsView.ToSharedRef() : SNullWidget::NullWidget
		];
}

void FEnhancedHairCardsAssetEditorToolkit::CreateDetailsViews()
{
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs AssetDetailsArgs;
	AssetDetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	AssetDetailsArgs.bHideSelectionTip = true;
	AssetDetailsArgs.bAllowSearch = true;
	AssetDetailsView = PropertyEditorModule.CreateDetailView(AssetDetailsArgs);
	AssetDetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::IsPreviewPropertyVisible));
	AssetDetailsView->OnFinishedChangingProperties().AddSP(this, &FEnhancedHairCardsAssetEditorToolkit::HandleFinishedChangingProperties);
	AssetDetailsView->SetObject(HairCardsAsset);

	FDetailsViewArgs PartDetailsArgs;
	PartDetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	PartDetailsArgs.bHideSelectionTip = true;
	PartDetailsArgs.bAllowSearch = true;
	PartDetailsView = PropertyEditorModule.CreateDetailView(PartDetailsArgs);
	PartDetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::IsPartPropertyVisible));
	PartDetailsView->OnFinishedChangingProperties().AddSP(this, &FEnhancedHairCardsAssetEditorToolkit::HandleFinishedChangingProperties);
	PartDetailsView->SetObject(PartEditorObject);

	MaterialDetailsView = PropertyEditorModule.CreateDetailView(PartDetailsArgs);
	MaterialDetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::IsMaterialPropertyVisible));
	MaterialDetailsView->OnFinishedChangingProperties().AddSP(this, &FEnhancedHairCardsAssetEditorToolkit::HandleFinishedChangingProperties);
	MaterialDetailsView->SetObject(PartEditorObject);

	GuidesDetailsView = PropertyEditorModule.CreateDetailView(PartDetailsArgs);
	GuidesDetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::IsGuidesPropertyVisible));
	GuidesDetailsView->OnFinishedChangingProperties().AddSP(this, &FEnhancedHairCardsAssetEditorToolkit::HandleFinishedChangingProperties);
	GuidesDetailsView->SetObject(PartEditorObject);
}

void FEnhancedHairCardsAssetEditorToolkit::ExtendToolbar()
{
	TSharedRef<FExtender> ToolbarExtender = MakeShared<FExtender>();
	ToolbarExtender->AddToolBarExtension(
		"Asset",
		EExtensionHook::After,
		GetToolkitCommands(),
		FToolBarExtensionDelegate::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::AddToolbarButtons));
	AddToolbarExtender(ToolbarExtender);
}

void FEnhancedHairCardsAssetEditorToolkit::AddToolbarButtons(FToolBarBuilder& ToolbarBuilder)
{
	ToolbarBuilder.BeginSection(TEXT("EnhancedHairCardsBuild"));
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::RebuildFromGroom)),
		NAME_None,
		LOCTEXT("RebuildFromGroom", "Rebuild From Groom"),
		LOCTEXT("RebuildFromGroomTooltip", "Rebuild this asset's card parts from its source Groom asset."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"));
	ToolbarBuilder.EndSection();

	ToolbarBuilder.BeginSection(TEXT("EnhancedHairCardsPreview"));
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::ToggleOriginalGroom)),
		NAME_None,
		LOCTEXT("ToggleOriginalGroom", "Original"),
		LOCTEXT("ToggleOriginalGroomTooltip", "Toggle the source Groom component in the preview."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.GroomAsset"));

	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::ToggleEnhancedCards)),
		NAME_None,
		LOCTEXT("ToggleEnhancedCards", "Cards"),
		LOCTEXT("ToggleEnhancedCardsTooltip", "Toggle Enhanced Hair Cards preview components."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.StaticMesh"));

	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::ToggleGuides)),
		NAME_None,
		LOCTEXT("ToggleGuides", "Guides"),
		LOCTEXT("ToggleGuidesTooltip", "Toggle stored guide curve debug drawing."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Show"));
	ToolbarBuilder.EndSection();

	ToolbarBuilder.BeginSection(TEXT("EnhancedHairCardsExport"));
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FEnhancedHairCardsAssetEditorToolkit::ExportBlueprint)),
		NAME_None,
		LOCTEXT("ExportBlueprint", "Export Blueprint"),
		LOCTEXT("ExportBlueprintTooltip", "Export this Enhanced Hair Cards asset to a compatible Actor Blueprint."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "BlueprintEditor.Tabs.Components"));
	ToolbarBuilder.EndSection();
}

void FEnhancedHairCardsAssetEditorToolkit::HandleFinishedChangingProperties(const FPropertyChangedEvent& PropertyChangedEvent)
{
	SyncSelectedPartFromEditorObject();
	if (HairCardsAsset)
	{
		HairCardsAsset->MarkPackageDirty();
	}

	if (IsPreviewSettingsPropertyChange(PropertyChangedEvent))
	{
		ApplyPreviewVisibility();
		RefreshLevelActorsFromAsset(false);
		return;
	}

	if (IsCardSettingsPropertyChange(PropertyChangedEvent))
	{
		ApplySelectedPartSettingsToPreview();
		RefreshLevelActorsFromAsset(false);
		return;
	}

	RebuildPreviewComponents();
	RefreshLevelActorsFromAsset(true);
}

bool FEnhancedHairCardsAssetEditorToolkit::IsPartPropertyVisible(const FPropertyAndParent& PropertyAndParent) const
{
	static const TSet<FName> HiddenRootNames =
	{
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPart, Material),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPart, GuideCurves)
	};

	return !EnhancedHairCardsEditor::IsNamedOrChildProperty(PropertyAndParent, HiddenRootNames);
}

bool FEnhancedHairCardsAssetEditorToolkit::IsMaterialPropertyVisible(const FPropertyAndParent& PropertyAndParent) const
{
	static const TSet<FName> VisibleRootNames =
	{
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPart, Material)
	};
	return EnhancedHairCardsEditor::IsNamedOrChildProperty(PropertyAndParent, VisibleRootNames);
}

bool FEnhancedHairCardsAssetEditorToolkit::IsGuidesPropertyVisible(const FPropertyAndParent& PropertyAndParent) const
{
	static const TSet<FName> VisibleRootNames =
	{
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPart, GuideCurves)
	};
	static const TSet<FName> VisibleCardSettingsNames =
	{
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsSettings, Dynamics),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsSettings, GuideDebug)
	};

	return EnhancedHairCardsEditor::IsNamedOrChildProperty(PropertyAndParent, VisibleRootNames)
		|| EnhancedHairCardsEditor::IsRootOrSelectedNestedProperty(
			PropertyAndParent,
			GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPart, CardSettings),
			VisibleCardSettingsNames);
}

bool FEnhancedHairCardsAssetEditorToolkit::IsPreviewPropertyVisible(const FPropertyAndParent& PropertyAndParent) const
{
	static const TSet<FName> VisibleRootNames =
	{
		GET_MEMBER_NAME_CHECKED(UEnhancedHairCardsAsset, SourceGroom),
		GET_MEMBER_NAME_CHECKED(UEnhancedHairCardsAsset, PreviewSettings)
	};

	return EnhancedHairCardsEditor::IsNamedOrChildProperty(PropertyAndParent, VisibleRootNames);
}

bool FEnhancedHairCardsAssetEditorToolkit::IsPreviewSettingsPropertyChange(const FPropertyChangedEvent& PropertyChangedEvent) const
{
	const FProperty* Property = PropertyChangedEvent.Property;
	const FProperty* MemberProperty = PropertyChangedEvent.MemberProperty;
	if (!Property && !MemberProperty)
	{
		return false;
	}

	static const TSet<FName> PreviewSettingNames =
	{
		GET_MEMBER_NAME_CHECKED(UEnhancedHairCardsAsset, PreviewSettings),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPreviewSettings, bShowOriginalGroom),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPreviewSettings, bKeepSourceGroomVisibleForSimulation),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPreviewSettings, bShowEnhancedCards),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPreviewSettings, bShowGuides),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPreviewSettings, bRenderEnhancedCardsMesh)
	};

	return (Property && PreviewSettingNames.Contains(Property->GetFName()))
		|| (MemberProperty && PreviewSettingNames.Contains(MemberProperty->GetFName()));
}

bool FEnhancedHairCardsAssetEditorToolkit::IsCardSettingsPropertyChange(const FPropertyChangedEvent& PropertyChangedEvent) const
{
	const FProperty* Property = PropertyChangedEvent.Property;
	const FProperty* MemberProperty = PropertyChangedEvent.MemberProperty;
	if (!Property && !MemberProperty)
	{
		return false;
	}

	static const TSet<FName> CardSettingsNames =
	{
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsPart, CardSettings),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsSettings, Dynamics),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsSettings, GuideDebug),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsSettings, bRenderCardsMesh),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsSettings, bInvertAtlasV),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsSettings, NumUVChannels),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, bEnabled),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, bGuideSimulationEnabled),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, Strength),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, LocalWindDirection),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, WindStrength),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, FlutterStrength),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, FlutterFrequency),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, TipPower),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, GravityStrength),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, bInvertRootToTip),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, GuideStiffness),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, GuideDamping),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, GuideConstraintIterations),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, GuideMotionInertia),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, bGuideRotationEnabled),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, GuideRotationStrength),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsDynamicsSettings, BoundsExtension),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsGuideDebugSettings, bDrawGuides),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsGuideDebugSettings, bDrawRoots),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsGuideDebugSettings, bDrawInForeground),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsGuideDebugSettings, LineThickness),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsGuideDebugSettings, RootTickSize),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsGuideDebugSettings, GuideColor),
		GET_MEMBER_NAME_CHECKED(FEnhancedHairCardsGuideDebugSettings, RootColor)
	};

	return (Property && CardSettingsNames.Contains(Property->GetFName()))
		|| (MemberProperty && CardSettingsNames.Contains(MemberProperty->GetFName()));
}

void FEnhancedHairCardsAssetEditorToolkit::SyncSelectedPartFromEditorObject()
{
	if (HairCardsAsset && PartEditorObject && HairCardsAsset->Parts.IsValidIndex(SelectedPartIndex))
	{
		HairCardsAsset->Modify();
		HairCardsAsset->Parts[SelectedPartIndex] = PartEditorObject->Part;
	}
}

void FEnhancedHairCardsAssetEditorToolkit::ApplyPreviewVisibility()
{
	if (!HairCardsAsset)
	{
		return;
	}

	if (PreviewGroomComponent)
	{
		const bool bKeepSourceGroomVisibleForSimulation =
			HairCardsAsset->PreviewSettings.bKeepSourceGroomVisibleForSimulation
			&& HairCardsAsset->SourceGroom
			&& PreviewCardComponents.ContainsByPredicate([](const TObjectPtr<UEnhancedHairCardsComponent>& Component)
			{
				return Component && Component->bUseSourceGroomSimulation;
			});
		const bool bShowSourceGroom =
			bKeepSourceGroomVisibleForSimulation
			||
			HairCardsAsset->PreviewSettings.bShowOriginalGroom;
		PreviewGroomComponent->SetVisibility(bShowSourceGroom);
		PreviewGroomComponent->SetHiddenInGame(!bShowSourceGroom);
		PreviewGroomComponent->MarkRenderStateDirty();
	}

	for (UEnhancedHairCardsComponent* Component : PreviewCardComponents)
	{
		if (!Component)
		{
			continue;
		}

		Component->SetVisibility(HairCardsAsset->PreviewSettings.bShowEnhancedCards);
		Component->SetHiddenInGame(!HairCardsAsset->PreviewSettings.bShowEnhancedCards);
		Component->CardSettings.bRenderCardsMesh = HairCardsAsset->PreviewSettings.bRenderEnhancedCardsMesh;
		Component->CardSettings.GuideDebug.bDrawGuides = HairCardsAsset->PreviewSettings.bShowGuides;
		Component->MarkRenderStateDirty();
	}

	RefreshPreviewViewport();
}

void FEnhancedHairCardsAssetEditorToolkit::ApplySelectedPartSettingsToPreview()
{
	if (!HairCardsAsset || !HairCardsAsset->Parts.IsValidIndex(SelectedPartIndex))
	{
		return;
	}

	if (PreviewCardComponents.IsValidIndex(SelectedPartIndex))
	{
		if (UEnhancedHairCardsComponent* Component = PreviewCardComponents[SelectedPartIndex])
		{
			Component->ApplyCardSettings(HairCardsAsset->Parts[SelectedPartIndex].CardSettings, false);
			Component->SetMaterial(0, HairCardsAsset->Parts[SelectedPartIndex].Material);
		}
	}

	ApplyPreviewVisibility();
}

void FEnhancedHairCardsAssetEditorToolkit::RefreshPreviewViewport()
{
	if (PreviewScene.IsValid())
	{
		PreviewScene->UpdateCaptureContents();
	}

	if (ViewportWidget.IsValid())
	{
		ViewportWidget->Invalidate();
	}
}

void FEnhancedHairCardsAssetEditorToolkit::RefreshLevelActorsFromAsset(bool bForceRebuild)
{
	if (!HairCardsAsset || !GEditor)
	{
		return;
	}

	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld)
	{
		return;
	}

	bool bRefreshedAnyActor = false;
	for (TActorIterator<AEnhancedHairCardsActor> It(EditorWorld); It; ++It)
	{
		AEnhancedHairCardsActor* HairCardsActor = *It;
		if (!HairCardsActor || HairCardsActor->HairCardsAsset != HairCardsAsset)
		{
			continue;
		}

		HairCardsActor->RefreshComponentsFromAsset(bForceRebuild);
		bRefreshedAnyActor = true;
	}

	if (bRefreshedAnyActor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
}

void FEnhancedHairCardsAssetEditorToolkit::RebuildPreviewComponents()
{
	if (!PreviewScene.IsValid() || !HairCardsAsset)
	{
		return;
	}

	if (PreviewGroomComponent)
	{
		PreviewScene->RemoveComponent(PreviewGroomComponent);
		PreviewGroomComponent = nullptr;
	}

	for (UEnhancedHairCardsComponent* Component : PreviewCardComponents)
	{
		if (Component)
		{
			PreviewScene->RemoveComponent(Component);
		}
	}
	PreviewCardComponents.Reset();

	if (HairCardsAsset->SourceGroom)
	{
		PreviewGroomComponent = NewObject<UGroomComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		PreviewGroomComponent->SetUseCards(true);
		PreviewGroomComponent->SetGroomAsset(HairCardsAsset->SourceGroom);
		PreviewScene->AddComponent(PreviewGroomComponent, FTransform::Identity);
	}

	FBox PreviewBounds(ForceInit);
	for (const FEnhancedHairCardsPart& Part : HairCardsAsset->Parts)
	{
		UEnhancedHairCardsComponent* Component = NewObject<UEnhancedHairCardsComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->ApplyHairCardsPart(Part);
		PreviewCardComponents.Add(Component);
		PreviewScene->AddComponent(Component, FTransform::Identity);
		PreviewBounds += Component->CalcBounds(FTransform::Identity).GetBox();
	}

	ApplyPreviewVisibility();
	PreviewScene->UpdateCaptureContents();

	if (ViewportWidget.IsValid())
	{
		ViewportWidget->SetPreviewBounds(PreviewBounds);
		ViewportWidget->FocusOnPreview();
		ViewportWidget->Invalidate();
	}
}

void FEnhancedHairCardsAssetEditorToolkit::FocusViewport()
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->FocusOnPreview();
	}
}

void FEnhancedHairCardsAssetEditorToolkit::SelectPart(int32 PartIndex)
{
	SelectedPartIndex = HairCardsAsset && HairCardsAsset->Parts.IsValidIndex(PartIndex) ? PartIndex : INDEX_NONE;
	if (PartEditorObject)
	{
		PartEditorObject->Modify();
		PartEditorObject->Part = SelectedPartIndex != INDEX_NONE ? HairCardsAsset->Parts[SelectedPartIndex] : FEnhancedHairCardsPart();
	}

	if (PartDetailsView.IsValid())
	{
		PartDetailsView->SetObject(PartEditorObject, true);
	}
	if (MaterialDetailsView.IsValid())
	{
		MaterialDetailsView->SetObject(PartEditorObject, true);
	}
	if (GuidesDetailsView.IsValid())
	{
		GuidesDetailsView->SetObject(PartEditorObject, true);
	}
}

void FEnhancedHairCardsAssetEditorToolkit::RefreshPartList()
{
	PartItems.Reset();
	if (HairCardsAsset)
	{
		for (int32 PartIndex = 0; PartIndex < HairCardsAsset->Parts.Num(); ++PartIndex)
		{
			PartItems.Add(MakeShared<int32>(PartIndex));
		}
	}

	if (PartsListView.IsValid())
	{
		PartsListView->RequestListRefresh();
		if (PartItems.IsValidIndex(SelectedPartIndex))
		{
			PartsListView->SetSelection(PartItems[SelectedPartIndex]);
		}
	}
}

void FEnhancedHairCardsAssetEditorToolkit::RebuildFromGroom()
{
	if (!HairCardsAsset)
	{
		return;
	}

	SyncSelectedPartFromEditorObject();
	if (!UEnhancedHairCardsConverterLibrary::RebuildEnhancedHairCardsAssetFromGroom(HairCardsAsset))
	{
		return;
	}

	SelectedPartIndex = HairCardsAsset->Parts.Num() > 0 ? 0 : INDEX_NONE;
	RefreshPartList();
	SelectPart(SelectedPartIndex);

	if (AssetDetailsView.IsValid())
	{
		AssetDetailsView->SetObject(HairCardsAsset, true);
	}

	RebuildPreviewComponents();
	RefreshLevelActorsFromAsset(true);
}

void FEnhancedHairCardsAssetEditorToolkit::ToggleOriginalGroom()
{
	if (HairCardsAsset)
	{
		HairCardsAsset->Modify();
		HairCardsAsset->PreviewSettings.bShowOriginalGroom = !HairCardsAsset->PreviewSettings.bShowOriginalGroom;
		HairCardsAsset->MarkPackageDirty();
		ApplyPreviewVisibility();
		RefreshLevelActorsFromAsset(false);
	}
}

void FEnhancedHairCardsAssetEditorToolkit::ToggleEnhancedCards()
{
	if (HairCardsAsset)
	{
		HairCardsAsset->Modify();
		HairCardsAsset->PreviewSettings.bShowEnhancedCards = !HairCardsAsset->PreviewSettings.bShowEnhancedCards;
		HairCardsAsset->MarkPackageDirty();
		ApplyPreviewVisibility();
		RefreshLevelActorsFromAsset(false);
	}
}

void FEnhancedHairCardsAssetEditorToolkit::ToggleGuides()
{
	if (HairCardsAsset)
	{
		HairCardsAsset->Modify();
		HairCardsAsset->PreviewSettings.bShowGuides = !HairCardsAsset->PreviewSettings.bShowGuides;
		HairCardsAsset->MarkPackageDirty();
		ApplyPreviewVisibility();
		RefreshLevelActorsFromAsset(false);
	}
}

void FEnhancedHairCardsAssetEditorToolkit::ExportBlueprint()
{
	if (!HairCardsAsset)
	{
		return;
	}

	const FString AssetPackageName = HairCardsAsset->GetOutermost()->GetName();
	const FString PackagePath = FPackageName::GetLongPackagePath(AssetPackageName);
	const FString AssetName = HairCardsAsset->GetName();
	const FString OutputPackagePath = PackagePath / FString::Printf(TEXT("BP_%s"), *AssetName);
	UEnhancedHairCardsConverterLibrary::ExportEnhancedHairCardsAssetToBlueprint(HairCardsAsset, OutputPackagePath, true);
}

FText FEnhancedHairCardsAssetEditorToolkit::GetPartRowText(int32 PartIndex) const
{
	if (!HairCardsAsset || !HairCardsAsset->Parts.IsValidIndex(PartIndex))
	{
		return LOCTEXT("InvalidPartRow", "Invalid Part");
	}

	const FEnhancedHairCardsPart& Part = HairCardsAsset->Parts[PartIndex];
	const FString MeshName = Part.SourceMesh ? Part.SourceMesh->GetName() : FString(TEXT("No Mesh"));
	return FText::FromString(FString::Printf(
		TEXT("%02d  G%d  L%d  %s  Guides:%d"),
		PartIndex,
		Part.SourceGroupIndex,
		Part.SourceLODIndex,
		*MeshName,
		Part.GuideCurves.Num()));
}

#undef LOCTEXT_NAMESPACE
