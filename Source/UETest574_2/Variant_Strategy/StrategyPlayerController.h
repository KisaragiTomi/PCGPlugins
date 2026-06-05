// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "StrategyPlayerController.generated.h"

class AStrategyPawn;
class UInputMappingContext;
class UNiagaraSystem;
struct FInputActionValue;
class AStrategyHUD;
class AStrategyNPC;
class UInputAction;

/** Enum to determine the last used input type */
UENUM(BlueprintType)
enum EStrategyInputMode : uint8
{
	SIM_Mouse	UMETA(DisplayName = "Mouse"),
	SIM_Touch	UMETA(DisplayName = "Touch")
};

/**
 *  Player Controller for a top-down strategy game.
 *  Handles unit selection and commands.
 *  Implements both mouse and touch controls.
 */
UCLASS(abstract)
class AStrategyPlayerController : public APlayerController
{
	GENERATED_BODY()

protected:

	/** Strategy Pawn associated with this controller */
	TObjectPtr<AStrategyPawn> ControlledPawn;

	/** Strategy HUD associated with this controller */
	TObjectPtr<AStrategyHUD> StrategyHUD;

	/** Determines the chosen input type */
	UPROPERTY(EditAnywhere, Category="Input")
	TEnumAsByte<EStrategyInputMode> InputMode = SIM_Mouse;

	/** Input mapping context to use with mouse input */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputMappingContext* MouseMappingContext;

	/** Input mapping context to use with touch input */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputMappingContext* TouchMappingContext;

	/** If true, the player is adding or removing units to the selected units list */
	bool bSelectionModifier = false;

	/** If true, double-tap touch select all mode is active */
	bool bDoubleTapActive = false;

	/** If true, allow the player to interact with game objects */
	bool bAllowInteraction = true;

	/** Input Action for moving the camera */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* MoveCameraAction;

	/** Input Action for zooming the camera */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* ZoomCameraAction;

	/** Input Action for resetting the camera to its default position */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* ResetCameraAction;

	/** Input Action for select and click */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* SelectClickAction;

	/** Input Action for select press and hold */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* SelectHoldAction;

	/** Input Action for click interaction */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* InteractClickAction;

	/** Input Action for interaction press and hold */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* InteractHoldAction;

	/** Input Action for modifying selection mode */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* SelectionModifierAction;

	/** Input Action for primary touch hold */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* TouchPrimaryHoldAction;

	/** Input Action for secondary touch */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* TouchSecondaryAction;

	/** Max distance to look for nearby units when doing a click or touch interaction */
	UPROPERTY(EditAnywhere, Category="Input", meta = (ClampMin = 0, ClampMax = 10000, Units = "cm"))
	float InteractionRadius = 250.0f;

	/** Max distance between the starting and current position of the second touch finger to be considered a box selection */
	UPROPERTY(EditAnywhere, Category="Input", meta = (ClampMin = 0, ClampMax = 10000))
	float MinSecondFingerDistanceForBoxSelect = 10.0f;

	/** Saves the world location of the last initiated interaction */
	FVector CachedInteraction;

	/** Saves the world location of the last initiated unit selection */
	FVector CachedSelection;

	/** Saves the world location where the player started a press and hold interaction */
	FVector2D StartingInteractionPosition;

	/** Saves the current world location of the player's cursor in press and hold interaction */
	FVector2D CurrentInteractionPosition;

	/** Saves the starting world location of a player's cursor in a press and hold selection box */
	FVector2D StartingSelectionPosition;

	/** Saves the starting location of a two-finger touch interaction (pinch) */
	FVector2D StartingSecondFingerPosition;

	/** Saves the current location of a two-finger touch interaction (pinch) */
	FVector2D CurrentSecondFingerPosition;

	/** Current camera zoom level */
	float CameraZoom;

	/** Default camera zoom level */
	float DefaultZoom;

	/** Minimum allowed camera zoom level */
	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = 0, ClampMax = 10000))
	float MinZoomLevel = 1000.0f;

	/** Maximum allowed camera zoom level */
	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = 0, ClampMax = 10000))
	float MaxZoomLevel = 2500.0f;

	/** Scales zoom inputs by this value */
	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = 0, ClampMax = 1000))
	float ZoomScaling = 100.0f;

	/** Affects how fast the camera moves while dragging with the mouse */
	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = 0, ClampMax = 10000))
	float DragMultiplier = 0.1f;

	/** Trace channel to use for selection trace checks */
	UPROPERTY(EditAnywhere, Category = "Selection")
	TEnumAsByte<ETraceTypeQuery> SelectionTraceChannel;

	/** Currently selected unit */
	AStrategyUnit* TargetUnit = nullptr;

	/** Currently selected unit list */
	TArray<AStrategyUnit*> ControlledUnits;

	///////////////////////////////////
	// Touchscreen enhanced input workaround

	/** Game time when the player last started tapping the touchscreen */
	float LastTapPressTime = 0.0f;

	/** Game time when the player last ended tapping the touchscreen */
	float LastTapReleaseTime = 0.0f;

	/** Number of successive times the player has tapped the touchscreen */
	int32 TapCount = 0;

	/** Game time when the player last completed a box select on the touchscreen */
	float LastBoxSelectTime = 0.0f;

	/** Max time between touch press and release to be considered a tap */
	UPROPERTY(EditAnywhere, Category = "Touch Input", meta = (ClampMin = 0, ClampMax = 1, Units="s"))
	float TouchTapMaxAllowedTime = 0.15f;

	/** Max time between touchscreen taps to be considered a double tap */
	UPROPERTY(EditAnywhere, Category = "Touch Input", meta = (ClampMin = 0, ClampMax = 1, Units="s"))
	float TouchDoubleTapMaxAllowedTime = 0.4f;

public:

	/** Constructor */
	AStrategyPlayerController();

	/** Initialize input bindings */
	virtual void SetupInputComponent() override;

	/** Pawn initialization */
	virtual void OnPossess(APawn* InPawn);

public:

	/** Updates selected units from the HUD's drag select box */
	void DragSelectUnits(const TArray<AStrategyUnit*>& Units);

	/** Passes the list of selected units */
	const TArray<AStrategyUnit*>& GetSelectedUnits();

protected:

	/** Moves the camera by the given input */
	void MoveCamera(const FInputActionValue& Value);

	/** Changes the camera zoom level by the given input */
	void ZoomCamera(const FInputActionValue& Value);

	/** Resets the camera to its initial value */
	void ResetCamera(const FInputActionValue& Value);

	/** Start a select and hold input */
	void SelectHoldStarted(const FInputActionValue& Value);
	
	/** Select and hold input triggered */
	void SelectHoldTriggered(const FInputActionValue& Value);

	/** Select and hold input completed */
	void SelectHoldCompleted(const FInputActionValue& Value);

	/** Select click action */
	void SelectClick(const FInputActionValue& Value);

	/** Presses or releases the selection modifier key */
	void SelectionModifier(const FInputActionValue& Value);

	/** Starts an interaction hold input */
	void InteractHoldStarted(const FInputActionValue& Value);

	/** Interaction hold input triggered */
	void InteractHoldTriggered(const FInputActionValue& Value);

	/** Interaction click input started */
	void InteractClickStarted(const FInputActionValue& Value);

	/** Interaction click input completed */
	void InteractClickCompleted(const FInputActionValue& Value);

	/** Touch primary finger hold started */
	void TouchPrimaryHoldStarted(const FInputActionValue& Value);

	/** Touch primary finger hold triggered */
	void TouchPrimaryHoldTriggered(const FInputActionValue& Value);

	/** Touch primary finger hold completed */
	void TouchPrimaryHoldCompleted(const FInputActionValue& Value);

	/** Touch secondary finger started */
	void TouchSecondaryStarted(const FInputActionValue& Value);

	/** Touch secondary finger triggered */
	void TouchSecondaryTriggered(const FInputActionValue& Value);

	/** Touch secondary finger completed */
	void TouchSecondaryCompleted(const FInputActionValue& Value);

	/** Attempt to select or deselect units at the cached location */
	void DoSelectionCommand();

	/** Select all units currently on screen */
	void DoSelectAllOnScreenCommand();

	/** Deselect all controlled units */
	void DoDeselectAllCommand();

	/** Drag scroll the camera */
	void DoDragScrollCommand();

	/** Move all selected units */
	void DoMoveUnitsCommand();

	/** Called when a unit move is completed */
	UFUNCTION()
	void OnMoveCompleted(AStrategyUnit* MovedUnit);

	/** Sorts all controlled units based on their distance to the provided world location */
	AStrategyUnit* GetClosestSelectedUnitToLocation(FVector TargetLocation);

	/** Calculates and returns the current mouse location */
	FVector2D GetMouseLocation();

	/** Attempts to get the world location under the cursor, returns true if successful */
	bool GetLocationUnderCursor(FVector& Location);

	/** Projects the current touch location into world space */
	FVector ProjectTouchPointToWorldSpace();

	/** Spawns the positive cursor effect */
	UFUNCTION(BlueprintImplementableEvent, Category="Cursor", meta = (DisplayName="Cursor Feedback"))
	void BP_CursorFeedback(FVector Location, bool bPositive);

	/** Resets the interaction flag */
	void ResetInteraction();

	/** Detects taps and double taps for mobile platforms. */
	void CheckTouchTap(bool& bTapped, bool& bDoubleTapped);
};
