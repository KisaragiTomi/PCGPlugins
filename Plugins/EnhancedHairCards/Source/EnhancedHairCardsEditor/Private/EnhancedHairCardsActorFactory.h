#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "EnhancedHairCardsActorFactory.generated.h"

UCLASS(config=Editor)
class UEnhancedHairCardsActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	UEnhancedHairCardsActorFactory(const FObjectInitializer& ObjectInitializer);

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
	virtual FQuat AlignObjectToSurfaceNormal(const FVector& InSurfaceNormal, const FQuat& ActorRotation) const override;
};
