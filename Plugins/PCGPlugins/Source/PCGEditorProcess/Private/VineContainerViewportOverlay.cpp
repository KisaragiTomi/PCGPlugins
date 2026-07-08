#include "VineContainerViewportOverlay.h"

#include "GeometryEditorActor.h"
#include "SVineContainerViewportOverlay.h"

bool FVineContainerViewportOverlay::SupportsActor(const AActor* Actor) const
{
	return Actor != nullptr;
}

TSharedRef<SWidget> FVineContainerViewportOverlay::CreateOverlayWidget(TWeakObjectPtr<AActor> Actor)
{
	if (AVineContainer* VineContainer = Cast<AVineContainer>(Actor.Get()))
	{
		return SNew(SVineContainerViewportOverlay)
			.VineContainer(VineContainer)
			.DetailsCategoryWidget(CreateDetailsCategoryOverlayWidget(Actor));
	}

	return FSelectedActorViewportOverlayBase::CreateOverlayWidget(Actor);
}

FText FVineContainerViewportOverlay::GetDetailsCategoryTitle(TWeakObjectPtr<AActor> Actor) const
{
	return FText::FromString(TEXT("ViewEdit"));
}
