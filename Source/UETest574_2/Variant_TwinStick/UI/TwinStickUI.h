// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "TwinStickUI.generated.h"

/**
 *  A simple Twin Stick Shooter UI widget
 *  Provides a blueprint interface to expose score values to the UI
 */
UCLASS(abstract)
class UTwinStickUI : public UUserWidget
{
	GENERATED_BODY()
	
public:

	/** Blueprint handler to update the items counter */
	UFUNCTION(BlueprintImplementableEvent, Category="Score")
	void UpdateItems(int32 Score);

	/** Blueprint handler to update the score sub-widgets */
	UFUNCTION(BlueprintImplementableEvent, Category="Score")
	void UpdateScore(int32 Score);

	/** Blueprint handler to update the combo sub-widgets */
	UFUNCTION(BlueprintImplementableEvent, Category="Score")
	void UpdateCombo(int32 Combo);
};
