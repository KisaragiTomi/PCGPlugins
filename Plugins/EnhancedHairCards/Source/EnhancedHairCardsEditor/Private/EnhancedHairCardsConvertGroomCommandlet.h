#pragma once

#include "Commandlets/Commandlet.h"
#include "EnhancedHairCardsConvertGroomCommandlet.generated.h"

UCLASS()
class UEnhancedHairCardsConvertGroomCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UEnhancedHairCardsConvertGroomCommandlet();

	virtual int32 Main(const FString& Params) override;
};
