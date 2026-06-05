// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "StrategyHUD.generated.h"

class UStrategyUI;

/**
 *  Simple strategy game HUD
 *  Draws the selection box and unit selected overlays
 */
UCLASS(abstract)
class AStrategyHUD : public AHUD
{
	GENERATED_BODY()
	
protected:

	/** Pointer to the UI user widget */
	UPROPERTY()
	TObjectPtr<UStrategyUI> UIWidget;

	/** Type of UI Widget to spawn */
	UPROPERTY(EditAnywhere, Category="UI")
	TSubclassOf<UStrategyUI> UIWidgetClass;

	/** If true, the HUD will draw the selection box */
	bool bDrawBox = false;

	/** Starting coords of the selection box */
	FVector2D BoxStart;

	/** Width and height of the selection box */
	FVector2D BoxSize;

	/** Current position of the selection box */
	FVector2D BoxCurrentPosition;

	/** Color of the selection box */
	UPROPERTY(EditAnywhere, Category="UI")
	FLinearColor SelectionBoxColor;

public:

	/** Initialization */
	virtual void BeginPlay() override;

	/** Updates the drag selection box */
	void DragSelectUpdate(FVector2D Start, FVector2D WidthAndHeight, FVector2D CurrentPosition, bool bDraw);

protected:

	/** Draws the HUD */
	virtual void DrawHUD() override;
};
