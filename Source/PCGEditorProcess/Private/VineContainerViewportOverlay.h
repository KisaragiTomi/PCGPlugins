#pragma once

#include "SelectedActorViewportOverlayBase.h"

class AActor;
class SWidget;

class FVineContainerViewportOverlay : public FSelectedActorViewportOverlayBase
{
protected:
	virtual bool SupportsActor(const AActor* Actor) const override;
	virtual TSharedRef<SWidget> CreateOverlayWidget(TWeakObjectPtr<AActor> Actor) override;
	virtual int32 GetOverlayZOrder() const override { return 250; }
	virtual FName GetRequiredDetailsCategoryName() const override { return TEXT("ViewEdit"); }
	virtual FText GetDetailsCategoryTitle(TWeakObjectPtr<AActor> Actor) const override;
};
