#include "EnhancedHairCardsConvertGroomCommandlet.h"

#include "EnhancedHairCardsConverterLibrary.h"
#include "EnhancedHairCardsAsset.h"
#include "GroomAsset.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "UObject/SoftObjectPath.h"

UEnhancedHairCardsConvertGroomCommandlet::UEnhancedHairCardsConvertGroomCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UEnhancedHairCardsConvertGroomCommandlet::Main(const FString& Params)
{
	FString GroomPath;
	FString OutputPath;
	int32 GroupIndex = -1;
	int32 LODIndex = -1;

	FParse::Value(*Params, TEXT("Groom="), GroomPath);
	FParse::Value(*Params, TEXT("Output="), OutputPath);
	FParse::Value(*Params, TEXT("Group="), GroupIndex);
	FParse::Value(*Params, TEXT("LOD="), LODIndex);
	const bool bOverwrite = FParse::Param(*Params, TEXT("Overwrite"));
	const bool bBlueprint = FParse::Param(*Params, TEXT("Blueprint"));

	if (GroomPath.IsEmpty() || OutputPath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Usage: -run=EnhancedHairCardsConvertGroom -Groom=/Game/Path/Groom.Groom -Output=/EnhancedHairCards/Path/EHC_Asset [-Group=N] [-LOD=N] [-Overwrite] [-Blueprint]"));
		return 1;
	}

	if (!GroomPath.Contains(TEXT(".")))
	{
		const FString GroomAssetName = FPackageName::GetLongPackageAssetName(GroomPath);
		GroomPath = GroomPath + TEXT(".") + GroomAssetName;
	}

	UGroomAsset* GroomAsset = Cast<UGroomAsset>(FSoftObjectPath(GroomPath).TryLoad());
	if (!GroomAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("Could not load Groom asset: %s"), *GroomPath);
		return 2;
	}

	if (bBlueprint)
	{
		UBlueprint* Blueprint = UEnhancedHairCardsConverterLibrary::ConvertGroomToEnhancedHairCardsBlueprint(
			GroomAsset,
			OutputPath,
			GroupIndex,
			LODIndex,
			bOverwrite);
		return Blueprint ? 0 : 3;
	}

	UEnhancedHairCardsAsset* HairCardsAsset = UEnhancedHairCardsConverterLibrary::ConvertGroomToEnhancedHairCardsAsset(
		GroomAsset,
		OutputPath,
		GroupIndex,
		LODIndex,
		bOverwrite);

	return HairCardsAsset ? 0 : 3;
}
