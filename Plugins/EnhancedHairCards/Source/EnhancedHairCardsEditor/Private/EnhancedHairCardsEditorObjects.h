#pragma once

#include "CoreMinimal.h"
#include "EnhancedHairCardsAsset.h"
#include "EnhancedHairCardsEditorObjects.generated.h"

UCLASS(Transient)
class UEnhancedHairCardsPartEditorObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category="Part")
	FEnhancedHairCardsPart Part;
};
