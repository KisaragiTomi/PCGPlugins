#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EnhancedHairCardsConverterLibrary.generated.h"

class UBlueprint;
class UEnhancedHairCardsAsset;
class UGroomAsset;

UCLASS()
class UEnhancedHairCardsConverterLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Conversion", meta=(DevelopmentOnly))
	static UEnhancedHairCardsAsset* ConvertGroomToEnhancedHairCardsAsset(
		UGroomAsset* GroomAsset,
		const FString& OutputPackagePath,
		int32 GroupIndex = -1,
		int32 LODIndex = -1,
		bool bOverwriteExisting = false);

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Conversion", meta=(DevelopmentOnly))
	static UBlueprint* ConvertGroomToEnhancedHairCardsBlueprint(
		UGroomAsset* GroomAsset,
		const FString& OutputPackagePath,
		int32 GroupIndex = -1,
		int32 LODIndex = -1,
		bool bOverwriteExisting = false);

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Conversion", meta=(DevelopmentOnly))
	static UBlueprint* ExportEnhancedHairCardsAssetToBlueprint(
		UEnhancedHairCardsAsset* HairCardsAsset,
		const FString& OutputPackagePath,
		bool bOverwriteExisting = false);

	UFUNCTION(BlueprintCallable, Category="Enhanced Hair Cards|Conversion", meta=(DevelopmentOnly))
	static bool RebuildEnhancedHairCardsAssetFromGroom(
		UEnhancedHairCardsAsset* HairCardsAsset,
		int32 GroupIndex = -1,
		int32 LODIndex = -1);
};
