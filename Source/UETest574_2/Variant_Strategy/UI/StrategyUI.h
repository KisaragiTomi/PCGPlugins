// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StrategyUI.generated.h"

/**
 *  Simple UI widget for the strategy game
 *	Keeps track of the number of units currently selected
 */
UCLASS(abstract)
class UStrategyUI : public UUserWidget
{
	GENERATED_BODY()
	
protected:

	/** Number of units currently selected */
	int32 SelectedUnitCount = 0;

public:

	/** Sets the number of units selected */
	void SetSelectedUnitsCount(int32 Count);

	/** Blueprint handler to update unit count sub-widgets */
	UFUNCTION(BlueprintImplementableEvent, Category="UI", meta = (DisplayName="Update Units Count"))
	void BP_UpdateUnitsCount();

protected:

	/** Returns the number of units selected */
	UFUNCTION(BlueprintPure, Category="UI")
	int32 GetSelectedUnitsCount() { return SelectedUnitCount; }
};
