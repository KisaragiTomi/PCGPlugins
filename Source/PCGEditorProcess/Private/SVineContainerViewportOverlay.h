#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class AVineContainer;

class SVineContainerViewportOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVineContainerViewportOverlay) {}
		SLATE_ARGUMENT(TWeakObjectPtr<AVineContainer>, VineContainer)
		SLATE_ARGUMENT(TSharedPtr<SWidget>, DetailsCategoryWidget)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FText GetActorLabelText() const;
	FText GetLineCountText() const;
	FReply OnFetchFoliageClicked();
	FReply OnRevertFoliageClicked();
	FReply OnCleanAllClicked();
	FReply OnGenerateVineActionClicked();
	FReply OnSaveStaticmeshClicked();

	TWeakObjectPtr<AVineContainer> VineContainer;
	TSharedPtr<SWidget> DetailsCategoryWidget;
};
