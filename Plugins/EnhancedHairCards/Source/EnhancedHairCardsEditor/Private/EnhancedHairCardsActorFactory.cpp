#include "EnhancedHairCardsActorFactory.h"

#include "AssetRegistry/AssetData.h"
#include "EnhancedHairCardsActor.h"
#include "EnhancedHairCardsAsset.h"

#define LOCTEXT_NAMESPACE "EnhancedHairCardsActorFactory"

UEnhancedHairCardsActorFactory::UEnhancedHairCardsActorFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = LOCTEXT("DisplayName", "Enhanced Hair Cards");
	NewActorClass = AEnhancedHairCardsActor::StaticClass();
	bUseSurfaceOrientation = false;
	bShowInEditorQuickMenu = true;
}

bool UEnhancedHairCardsActorFactory::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.IsInstanceOf(UEnhancedHairCardsAsset::StaticClass()))
	{
		OutErrorMsg = LOCTEXT("InvalidEnhancedHairCardsAsset", "A valid Enhanced Hair Cards asset must be specified.");
		return false;
	}

	return true;
}

void UEnhancedHairCardsActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);

	UEnhancedHairCardsAsset* HairCardsAsset = CastChecked<UEnhancedHairCardsAsset>(Asset);
	AEnhancedHairCardsActor* HairCardsActor = CastChecked<AEnhancedHairCardsActor>(NewActor);
	HairCardsActor->SetHairCardsAsset(HairCardsAsset);
}

UObject* UEnhancedHairCardsActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
	check(ActorInstance->IsA(NewActorClass));

	AEnhancedHairCardsActor* HairCardsActor = CastChecked<AEnhancedHairCardsActor>(ActorInstance);
	return HairCardsActor->HairCardsAsset;
}

FQuat UEnhancedHairCardsActorFactory::AlignObjectToSurfaceNormal(const FVector& InSurfaceNormal, const FQuat& ActorRotation) const
{
	return FindActorAlignmentRotation(ActorRotation, FVector(0.f, 0.f, 1.f), InSurfaceNormal);
}

#undef LOCTEXT_NAMESPACE
