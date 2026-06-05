#include "EnhancedHairCardsAsset.h"

int32 UEnhancedHairCardsAsset::GetTotalGuideCurveCount() const
{
	int32 TotalGuideCurveCount = 0;
	for (const FEnhancedHairCardsPart& Part : Parts)
	{
		TotalGuideCurveCount += Part.GuideCurves.Num();
	}
	return TotalGuideCurveCount;
}

int32 UEnhancedHairCardsAsset::GetNumGuideSimulationParts() const
{
	int32 NumGuideSimulationParts = 0;
	for (const FEnhancedHairCardsPart& Part : Parts)
	{
		if (Part.CardSettings.Dynamics.bGuideSimulationEnabled && Part.GuideCurves.Num() > 0)
		{
			++NumGuideSimulationParts;
		}
	}
	return NumGuideSimulationParts;
}

bool UEnhancedHairCardsAsset::HasGuideSimulationEnabled() const
{
	return GetNumGuideSimulationParts() > 0;
}

void UEnhancedHairCardsAsset::SetAllGuideSimulationEnabled(bool bEnabled)
{
	Modify();
	for (FEnhancedHairCardsPart& Part : Parts)
	{
		Part.CardSettings.Dynamics.bGuideSimulationEnabled = bEnabled;
	}
	MarkPackageDirty();
}
