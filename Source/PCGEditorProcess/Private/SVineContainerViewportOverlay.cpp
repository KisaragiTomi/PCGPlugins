#include "SVineContainerViewportOverlay.h"

#include "VineGenerator.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SVineContainerViewportOverlay"

void SVineContainerViewportOverlay::Construct(const FArguments& InArgs)
{
	VineContainer = InArgs._VineContainer;
	DetailsCategoryWidget = InArgs._DetailsCategoryWidget;
	SetVisibility(EVisibility::SelfHitTestInvisible);

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(16.0f)
		[
			SNew(SBox)
			.WidthOverride(280.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
				.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.68f))
				.Padding(FMargin(10.0f, 8.0f))
				[
					SNew(SVerticalBox)

					// Actor label
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SVineContainerViewportOverlay::GetActorLabelText)
						.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
						.ColorAndOpacity(FLinearColor::White)
					]

					// Line count info
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 8.0f)
					[
						SNew(STextBlock)
						.Text(this, &SVineContainerViewportOverlay::GetLineCountText)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FLinearColor(0.78f, 0.82f, 0.86f, 1.0f))
					]

					// Row: Fetch Foliage / Revert Foliage
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("FetchFoliage", "Fetch Foliage"))
							.HAlign(HAlign_Center)
							.OnClicked(this, &SVineContainerViewportOverlay::OnFetchFoliageClicked)
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(4.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("RevertFoliage", "Revert Foliage"))
							.HAlign(HAlign_Center)
							.OnClicked(this, &SVineContainerViewportOverlay::OnRevertFoliageClicked)
						]
					]

					// Row: Generate Vine / Clean All
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("GenerateVine", "Generate Vine"))
							.HAlign(HAlign_Center)
							.OnClicked(this, &SVineContainerViewportOverlay::OnGenerateVineActionClicked)
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.Padding(4.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("CleanAll", "Clean All"))
							.HAlign(HAlign_Center)
							.OnClicked(this, &SVineContainerViewportOverlay::OnCleanAllClicked)
						]
					]

					// Row: Save Staticmesh
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("SaveStaticmesh", "Save Mesh"))
						.HAlign(HAlign_Center)
						.OnClicked(this, &SVineContainerViewportOverlay::OnSaveStaticmeshClicked)
					]
				]
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		[
			DetailsCategoryWidget.IsValid() ? DetailsCategoryWidget.ToSharedRef() : SNullWidget::NullWidget
		]
	];
}

FText SVineContainerViewportOverlay::GetActorLabelText() const
{
	if (const AVineContainer* Container = VineContainer.Get())
	{
		return FText::FromString(Container->GetActorLabel());
	}

	return LOCTEXT("MissingActor", "Vine Container");
}

FText SVineContainerViewportOverlay::GetLineCountText() const
{
	if (const AVineContainer* Container = VineContainer.Get())
	{
		return FText::Format(
			LOCTEXT("LineCountFormat", "Tube {0}"),
			FText::AsNumber(Container->TubeLines.Num()));
	}

	return FText::GetEmpty();
}

FReply SVineContainerViewportOverlay::OnFetchFoliageClicked()
{
	if (AVineContainer* Container = VineContainer.Get())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineOverlay] FetchFoliage clicked on %s"), *Container->GetActorLabel());
		Container->FetchFoliage();
	}

	return FReply::Handled();
}

FReply SVineContainerViewportOverlay::OnRevertFoliageClicked()
{
	if (AVineContainer* Container = VineContainer.Get())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineOverlay] RevertFoliage clicked on %s"), *Container->GetActorLabel());
		Container->RevertFoliage();
	}

	return FReply::Handled();
}

FReply SVineContainerViewportOverlay::OnCleanAllClicked()
{
	if (AVineContainer* Container = VineContainer.Get())
	{
		Container->Clean();
	}

	return FReply::Handled();
}

FReply SVineContainerViewportOverlay::OnGenerateVineActionClicked()
{
	if (AVineContainer* Container = VineContainer.Get())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineOverlay] GenerateVineAction clicked on %s"), *Container->GetActorLabel());
		Container->GenerateVineAction();
	}

	return FReply::Handled();
}

FReply SVineContainerViewportOverlay::OnSaveStaticmeshClicked()
{
	if (AVineContainer* Container = VineContainer.Get())
	{
		Container->SaveStaticmesh();
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
