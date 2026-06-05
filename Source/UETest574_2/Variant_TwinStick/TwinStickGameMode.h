// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "TwinStickGameMode.generated.h"

class UTwinStickUI;

/**
 *  Simple Game Mode for a Twin Stick Shooter game.
 *  Manages the score and UI
 */
UCLASS(abstract)
class ATwinStickGameMode : public AGameModeBase
{
	GENERATED_BODY()
	
protected:

	/** Type of UI Widget to spawn */
	UPROPERTY(EditAnywhere, Category="Twin Stick")
	TSubclassOf<UTwinStickUI> UIWidgetClass;

	/** Pointer to the spawned UI Widget */
	TObjectPtr<UTwinStickUI> UIWidget;

	/** Current game score */
	int32 Score = 0;

	/** Current combo multiplier */
	int32 Combo = 1;

	/** Current combo increment value */
	int32 ComboIncrement = 0;

	/** Number of combo hits to process before incrementing the combo multiplier */
	UPROPERTY(EditAnywhere, Category="Twin Stick", meta=(ClampMin = 0, ClampMax = 10))
	int32 ComboIncrementMax = 5;

	/** Maximum allowed combo multiplier value */
	UPROPERTY(EditAnywhere, Category="Twin Stick", meta=(ClampMin = 0, ClampMax = 10))
	int32 ComboCap = 4;

	/** Max time between kills before the combo multiplier resets */
	UPROPERTY(EditAnywhere, Category="Twin Stick", meta=(ClampMin = 0, ClampMax = 10, Units = "s"))
	float ComboCooldown = 3.0f;

	/** Game time of the last combo kill */
	float LastComboTime = 0.0f;

	FTimerHandle ComboTimer;

	/** Max number of NPCs to allow in the level at once */
	UPROPERTY(EditAnywhere, Category="Twin Stick", meta=(ClampMin = 0, ClampMax = 100))
	int32 NPCCap = 20;

	/** Current number of NPCs in the level */
	int32 NPCCount = 0;

public:

	/** Gameplay initialization */
	virtual void BeginPlay() override;

	/** Cleanup */
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

public:

	/** Called when an item has been used */
	void ItemUsed(int32 Value);

	/** Increments the score by the given value */
	void ScoreUpdate(int32 Value);

protected:

	/** Creates the UI widget if it hasn't been created already */
	void CreateUI();

	/** Updates the combo multiplier */
	void ComboUpdate();

	/** Resets the combo cooldown timer */
	void ResetComboCooldown();

	/** Resets the combo multiplier after the cooldown time expires */
	void ResetCombo();

public:

	/** Returns true if the number of NPCs is under the cap */
	bool CanSpawnNPCs();

	/** Increases the NPC count */
	void IncreaseNPCs();

	/** Decreases the NPC count */
	void DecreaseNPCs();
};
