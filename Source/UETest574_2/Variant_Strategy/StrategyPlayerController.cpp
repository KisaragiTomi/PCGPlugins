// Copyright Epic Games, Inc. All Rights Reserved.


#include "StrategyPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "InputMappingContext.h"
#include "Camera/CameraComponent.h"
#include "StrategyPawn.h"
#include "Camera/CameraComponent.h"
#include "InputActionValue.h"
#include "StrategyHUD.h"
#include "Engine/CollisionProfile.h"
#include "Kismet/GameplayStatics.h"
#include "StrategyUnit.h"
#include "NavigationSystem.h"
#include "Engine/OverlapResult.h"

AStrategyPlayerController::AStrategyPlayerController()
{
	// mouse cursor should always be shown
	bShowMouseCursor = true;
}

void AStrategyPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// only set up input on local player controllers
	if (IsLocalPlayerController())
	{
		// add the input mapping context
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			// choose the context based on the input mode
			UInputMappingContext* ChosenContext = nullptr;

			switch (InputMode)
			{
			case SIM_Mouse:
				ChosenContext = MouseMappingContext;
				break;
			case SIM_Touch:
				ChosenContext = TouchMappingContext;
				break;
			}

			Subsystem->AddMappingContext(ChosenContext, 0);
		}

		// bind the input mappings
		if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
		{
			// Camera
			EnhancedInputComponent->BindAction(MoveCameraAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::MoveCamera);
			EnhancedInputComponent->BindAction(ZoomCameraAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::ZoomCamera);
			EnhancedInputComponent->BindAction(ResetCameraAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::ResetCamera);

			// Mouse Interaction
			EnhancedInputComponent->BindAction(SelectHoldAction, ETriggerEvent::Started, this, &AStrategyPlayerController::SelectHoldStarted);
			EnhancedInputComponent->BindAction(SelectHoldAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::SelectHoldTriggered);
			EnhancedInputComponent->BindAction(SelectHoldAction, ETriggerEvent::Completed, this, &AStrategyPlayerController::SelectHoldCompleted);
			EnhancedInputComponent->BindAction(SelectHoldAction, ETriggerEvent::Canceled, this, &AStrategyPlayerController::SelectHoldCompleted);

			EnhancedInputComponent->BindAction(SelectClickAction, ETriggerEvent::Completed, this, &AStrategyPlayerController::SelectClick);

			EnhancedInputComponent->BindAction(SelectionModifierAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::SelectionModifier);
			EnhancedInputComponent->BindAction(SelectionModifierAction, ETriggerEvent::Completed, this, &AStrategyPlayerController::SelectionModifier);
			EnhancedInputComponent->BindAction(SelectionModifierAction, ETriggerEvent::Canceled, this, &AStrategyPlayerController::SelectionModifier);

			EnhancedInputComponent->BindAction(InteractHoldAction, ETriggerEvent::Started, this, &AStrategyPlayerController::InteractHoldStarted);
			EnhancedInputComponent->BindAction(InteractHoldAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::InteractHoldTriggered);

			EnhancedInputComponent->BindAction(InteractClickAction, ETriggerEvent::Started, this, &AStrategyPlayerController::InteractClickStarted);
			EnhancedInputComponent->BindAction(InteractClickAction, ETriggerEvent::Completed, this, &AStrategyPlayerController::InteractClickCompleted);

			// Touch Interaction
			EnhancedInputComponent->BindAction(TouchPrimaryHoldAction, ETriggerEvent::Started, this, &AStrategyPlayerController::TouchPrimaryHoldStarted);
			EnhancedInputComponent->BindAction(TouchPrimaryHoldAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::TouchPrimaryHoldTriggered);
			EnhancedInputComponent->BindAction(TouchPrimaryHoldAction, ETriggerEvent::Completed, this, &AStrategyPlayerController::TouchPrimaryHoldCompleted);

			EnhancedInputComponent->BindAction(TouchSecondaryAction, ETriggerEvent::Started, this, &AStrategyPlayerController::TouchSecondaryStarted);
			EnhancedInputComponent->BindAction(TouchSecondaryAction, ETriggerEvent::Triggered, this, &AStrategyPlayerController::TouchSecondaryTriggered);
			EnhancedInputComponent->BindAction(TouchSecondaryAction, ETriggerEvent::Completed, this, &AStrategyPlayerController::TouchSecondaryCompleted);
			EnhancedInputComponent->BindAction(TouchSecondaryAction, ETriggerEvent::Canceled, this, &AStrategyPlayerController::TouchSecondaryCompleted);

		}
	}
}

void AStrategyPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// ensure we have the right pawn type
	ControlledPawn = Cast<AStrategyPawn>(InPawn);
	check(ControlledPawn);

	// set the zoom level from the pawn's camera
	DefaultZoom = CameraZoom = ControlledPawn->GetCamera()->OrthoWidth;

	// cast the HUD pointer
	StrategyHUD = Cast<AStrategyHUD>(GetHUD());
	check(StrategyHUD);
}

void AStrategyPlayerController::DragSelectUnits(const TArray<AStrategyUnit*>& Units)
{
	// do we have units in the list?
	if (Units.Num() > 0)
	{
		// ensure any previous units are deselected
		DoDeselectAllCommand();

		// select each new unit
		for (AStrategyUnit* CurrentUnit : Units)
		{
			// add the unit to the selection list
			ControlledUnits.Add(CurrentUnit);

			// select the unit
			CurrentUnit->UnitSelected();
		}

	}
	else
	{

		// release any currently selected units since nothing is on the box
		if (ControlledUnits.Num() > 0)
		{
			DoDeselectAllCommand();
		}

	}
}

const TArray<AStrategyUnit*>& AStrategyPlayerController::GetSelectedUnits()
{
	return ControlledUnits;
}

void AStrategyPlayerController::MoveCamera(const FInputActionValue& Value)
{
	FVector2D InputVector = Value.Get<FVector2D>();

	// get the forward input component vector
	FRotator ForwardRot = GetControlRotation();
	ForwardRot.Pitch = 0.0f;

	// get the right input component vector
	FRotator RightRot = GetControlRotation();
	ForwardRot.Pitch = 0.0f;
	ForwardRot.Roll = 0.0f;

	// add the forward input
	ControlledPawn->AddMovementInput(ForwardRot.RotateVector(FVector::ForwardVector), InputVector.X + InputVector.Y);

	// add the right input
	ControlledPawn->AddMovementInput(RightRot.RotateVector(FVector::RightVector), InputVector.X - InputVector.Y);

}

void AStrategyPlayerController::ZoomCamera(const FInputActionValue& Value)
{
	// scale the input and subtract from the current zoom level
	float ZoomLevel = CameraZoom - (Value.Get<float>() * ZoomScaling);

	// clamp to min/max zoom levels
	CameraZoom = FMath::Clamp(ZoomLevel, MinZoomLevel, MaxZoomLevel);

	// update the pawn's camera
	ControlledPawn->SetZoomModifier(CameraZoom);

}

void AStrategyPlayerController::ResetCamera(const FInputActionValue& Value)
{
	// reset zoom level to its initial value
	CameraZoom = DefaultZoom;

	// update the pawn's camera
	ControlledPawn->SetZoomModifier(DefaultZoom);

}

void AStrategyPlayerController::SelectHoldStarted(const FInputActionValue& Value)
{
	// save the selection start position
	StartingSelectionPosition = GetMouseLocation();

}

void AStrategyPlayerController::SelectHoldTriggered(const FInputActionValue& Value)
{

	// get the current mouse position
	FVector2D SelectionPosition = GetMouseLocation();

	// calculate the size of the selection box
	FVector2D SelectionSize = SelectionPosition - StartingSelectionPosition;

	// update the selection box on the HUD
	if (StrategyHUD)
	{
		StrategyHUD->DragSelectUpdate(StartingSelectionPosition, SelectionSize, SelectionPosition, true);
	}
	
}

void AStrategyPlayerController::SelectHoldCompleted(const FInputActionValue& Value)
{
	// reset the drag box on the HUD
	if (StrategyHUD)
	{
		StrategyHUD->DragSelectUpdate(FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, false);
	}
}

void AStrategyPlayerController::SelectClick(const FInputActionValue& Value)
{

	if (GetLocationUnderCursor(CachedSelection))
	{
		DoSelectionCommand();
	}
}

void AStrategyPlayerController::SelectionModifier(const FInputActionValue& Value)
{

	// update the selection modifier flag
	bSelectionModifier = Value.Get<bool>();
}

void AStrategyPlayerController::InteractHoldStarted(const FInputActionValue& Value)
{

	// save the starting interaction position
	StartingInteractionPosition = GetMouseLocation();
}

void AStrategyPlayerController::InteractHoldTriggered(const FInputActionValue& Value)
{

	// do a drag scroll 
	DoDragScrollCommand();
}

void AStrategyPlayerController::InteractClickStarted(const FInputActionValue& Value)
{

	// reset the interaction flag
	ResetInteraction();
}

void AStrategyPlayerController::InteractClickCompleted(const FInputActionValue& Value)
{

	// do we have any units in the control list and a valid interaction location under the cursor?
	if (ControlledUnits.Num() > 0 && GetLocationUnderCursor(CachedInteraction))
	{
		// is double tap select all active?
		if (bDoubleTapActive)
		{
			// release double tap select all
			bDoubleTapActive = false;

		}
		else
		{

			// move the selected units to the target location
			DoMoveUnitsCommand();

		}
	}
}

void AStrategyPlayerController::TouchPrimaryHoldStarted(const FInputActionValue& Value)
{
	// save the tap press time
	LastTapPressTime = GetWorld()->GetRealTimeSeconds();

	// save the starting interaction position
	StartingInteractionPosition = Value.Get<FVector2D>();
}

void AStrategyPlayerController::TouchPrimaryHoldTriggered(const FInputActionValue& Value)
{
	// is this touch longer than a tap?
	if ((GetWorld()->GetRealTimeSeconds() - LastTapPressTime) > TouchTapMaxAllowedTime)
	{
		// if we're not doing a box select, do a drag scroll
		if (!bSelectionModifier)
		{
			DoDragScrollCommand();
		}
	}
}

void AStrategyPlayerController::TouchPrimaryHoldCompleted(const FInputActionValue& Value)
{
	// check if we're doing a tap or double tap.
	// we have to do this manually because EnhancedInput tap triggers work differently on touch inputs
	bool bTapped = false;
	bool bDoubleTapped = false;

	CheckTouchTap(bTapped, bDoubleTapped);

	// do we have a double tap?
	if (bTapped)
	{
		if (bDoubleTapped)
		{
			// ensure are we not doing a box select
			if (!bSelectionModifier)
			{
				// depending on the double tap toggle, select or deselect all units
				if (bDoubleTapActive)
				{
					DoDeselectAllCommand();
				}
				else
				{
					DoSelectAllOnScreenCommand();
				}

				// toggle the double tap flag
				bDoubleTapActive = !bDoubleTapActive;
			}
		}

	// no double tap, handle this touch input normally
	}
	else
	{

		// ensure we're not already box selecting, or were just box selecting
		if (!(bSelectionModifier || (GetWorld()->GetRealTimeSeconds() - LastBoxSelectTime) < TouchTapMaxAllowedTime))
		{
			// project the touch location and cache the selection point
			CachedInteraction = CachedSelection = ProjectTouchPointToWorldSpace();

			// do a selection action with the cached location
			DoSelectionCommand();
		}

	}

}

void AStrategyPlayerController::TouchSecondaryStarted(const FInputActionValue& Value)
{

	// raise the selection modifier flag
	bSelectionModifier = true;

	// save the starting position for the second finger
	StartingSecondFingerPosition = Value.Get<FVector2D>();
}

void AStrategyPlayerController::TouchSecondaryTriggered(const FInputActionValue& Value)
{
	// update the current position for the second finger
	CurrentSecondFingerPosition = Value.Get<FVector2D>();

	// are we box selecting, and the finger has moved enough on the touchscreen?
	if (bSelectionModifier && !StartingSecondFingerPosition.Equals(CurrentSecondFingerPosition, 10.0f))
	{
		// update the current interaction position
		CurrentInteractionPosition = CurrentSecondFingerPosition;

		// update the selection box on the HUD
		if (StrategyHUD)
		{
			const FVector2D DragSize = CurrentSecondFingerPosition - StartingSecondFingerPosition;

			StrategyHUD->DragSelectUpdate(StartingInteractionPosition, DragSize, CurrentSecondFingerPosition, true);
		}
	}
}

void AStrategyPlayerController::TouchSecondaryCompleted(const FInputActionValue& Value)
{

	// lower the selection modifier flag
	bSelectionModifier = false;

	// save the last box selection time
	LastBoxSelectTime = GetWorld()->GetRealTimeSeconds();

	// hide the selection box on the HUD
	if (StrategyHUD)
	{
		StrategyHUD->DragSelectUpdate(FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, false);
	}
}

void AStrategyPlayerController::DoSelectionCommand()
{

	// do a sphere sweep to look for actors to select
	FHitResult OutHit;

	const FVector Start = CachedSelection;
	const FVector End = Start + FVector::UpVector * 350.0f;

	FCollisionShape InteractionSphere;
	InteractionSphere.SetSphere(InteractionRadius);

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);

	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(this);
	QueryParams.AddIgnoredActor(GetPawn());
	QueryParams.bTraceComplex = true;

	GetWorld()->SweepSingleByObjectType(OutHit, Start, End, FQuat::Identity, ObjectParams, InteractionSphere, QueryParams);

	// if we're using the mouse and are not holding the selection modifier key, deselect any units first
	if (InputMode == SIM_Mouse && !bSelectionModifier)
	{

		DoDeselectAllCommand();
	}

	// did we hit a unit?
	if (OutHit.bBlockingHit)
	{

		// update the target unit
		TargetUnit = Cast<AStrategyUnit>(OutHit.GetActor());

		if (TargetUnit)
		{

			// is the unit already in the controlled list?
			if (ControlledUnits.Contains(TargetUnit))
			{

				// remove the units from the controlled list
				ControlledUnits.Remove(TargetUnit);

				// tell the unit it's been deselected
				TargetUnit->UnitDeselected();

			}
			else
			{

				// add the unit to the controlled list
				ControlledUnits.Add(TargetUnit);

				// tell the unit it's been selected
				TargetUnit->UnitSelected();

			}
		}

	}
	else
	{

		// are we using touch input?
		if (InputMode == SIM_Touch)
		{
			// move all selected units to the target location
			DoMoveUnitsCommand();
		}

	}
}

void AStrategyPlayerController::DoSelectAllOnScreenCommand()
{

	// find all NPCs currently on screen
	TArray<AActor*> FoundActors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AStrategyUnit::StaticClass(), FoundActors);

	// process each actor found
	for (AActor* CurrentActor : FoundActors)
	{
		// cast back to our unit class
		if (AStrategyUnit* CurrentUnit = Cast<AStrategyUnit>(CurrentActor))
		{
			// has the actor been recently rendered?
			if (CurrentActor->WasRecentlyRendered(0.2f))
			{

				// is the actor not on our controlled units list?
				if (!ControlledUnits.Contains(CurrentUnit))
				{
					// add it to the controlled units list
					ControlledUnits.Add(CurrentUnit);

					// notify it of selection
					CurrentUnit->UnitSelected();
				}
			}
		}		
	}

}

void AStrategyPlayerController::DoDeselectAllCommand()
{

	// tell each controlled unit it's been deselected
	for (AStrategyUnit* CurrentUnit : ControlledUnits)
	{
		// ensure the unit hasn't been destroyed
		if (IsValid(CurrentUnit))
		{

			CurrentUnit->UnitDeselected();
		}
	}

	// clear the controlled units list
	ControlledUnits.Empty();
}

void AStrategyPlayerController::DoDragScrollCommand()
{

	// choose the cursor position based on the input mode
	FVector2D WorkingPosition;
	
	if (InputMode == EStrategyInputMode::SIM_Mouse)
	{

		// read the mouse position
		bool bResult = GetMousePosition(WorkingPosition.X, WorkingPosition.Y);

	}
	else
	{

		// read the touch 1 position
		bool bPressed;
		GetInputTouchState(ETouchIndex::Touch1, WorkingPosition.X, WorkingPosition.Y, bPressed);

	}

	// find the difference between the starting interaction position and current coords
	const FVector2D InteractionDelta = StartingInteractionPosition - WorkingPosition;

	const FRotator CameraRot(0.0f, -45.0f, 0.0f);

	// rotate and scale the interaction delta
	const FVector ScrollDelta = CameraRot.RotateVector(FVector(InteractionDelta.X, InteractionDelta.Y, 0.0f)) * DragMultiplier;

	// apply the world offset to the controlled pawn
	ControlledPawn->AddActorWorldOffset(ScrollDelta);
}

void AStrategyPlayerController::DoMoveUnitsCommand()
{

	// set the movement goal
	FVector CurrentMoveGoal;

	if (InputMode == EStrategyInputMode::SIM_Mouse)
	{

		// set the cached interaction point as our move goal
		CurrentMoveGoal = CachedInteraction;

	}
	else
	{

		// set the cached selection as our move goal
		CurrentMoveGoal = CachedSelection;

	}

	// get the closest selected unit to the move goal. This will be our lead unit
	AStrategyUnit* Closest = GetClosestSelectedUnitToLocation(CurrentMoveGoal);

	// this will be set to true if any of the move requests fail
	bool bInteractionFailed = false;

	// process each unit in the controlled list
	for (AStrategyUnit* CurrentUnit : ControlledUnits)
	{
		if (IsValid(CurrentUnit))
		{

			// stop the unit
			CurrentUnit->StopMoving();

			// move the lead unit to the goal, all other units to random navigable points around it
			FVector MoveGoal = CurrentMoveGoal;

			if (CurrentUnit != Closest)
			{

				UNavigationSystemV1::K2_GetRandomLocationInNavigableRadius(GetWorld(), CurrentMoveGoal, MoveGoal, InteractionRadius * 0.66f);
			}

			// subscribe to the unit's move completed delegate
			CurrentUnit->OnMoveCompleted.AddDynamic(this, &AStrategyPlayerController::OnMoveCompleted);

			// set up movement to the goal location
			if (!CurrentUnit->MoveToLocation(MoveGoal, InteractionRadius * 0.66f))
			{
				// the move request failed, so flag it
				bInteractionFailed = true;
			}
		}

	}

	// play the cursor feedback depending on whether our move succeeded or not
	BP_CursorFeedback(CachedInteraction, !bInteractionFailed);

}

void AStrategyPlayerController::OnMoveCompleted(AStrategyUnit* MovedUnit)
{
	// is the unit valid?
	if (IsValid(MovedUnit))
	{
		// unsubscribe from the delegate
		MovedUnit->OnMoveCompleted.RemoveDynamic(this, &AStrategyPlayerController::OnMoveCompleted);
		
		// skip if interactions are locked
		if (!bAllowInteraction)
		{
			return;
		}

		// disallow additional interactions until we reset
		bAllowInteraction = false;

		// is the unit close enough to the cached interaction location?
		if(FVector::Dist2D(CachedInteraction, MovedUnit->GetActorLocation()) < InteractionRadius)
		{

			// do an overlap test to find nearby interactive objects
			TArray<FOverlapResult> OutOverlaps;

			FCollisionShape CollisionSphere;
			CollisionSphere.SetSphere(InteractionRadius);

			FCollisionObjectQueryParams ObjectParams;
			ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
			
			FCollisionQueryParams QueryParams;

			QueryParams.AddIgnoredActor(MovedUnit);

			for(const AStrategyUnit* CurSelected : ControlledUnits)
			{
				QueryParams.AddIgnoredActor(CurSelected);
			}

			if (GetWorld()->OverlapMultiByObjectType(OutOverlaps, CachedInteraction, FQuat::Identity, ObjectParams, CollisionSphere, QueryParams))
			{
				for (const FOverlapResult& CurrentOverlap : OutOverlaps)
				{
					if (AStrategyUnit* CurrentUnit = Cast<AStrategyUnit>(CurrentOverlap.GetActor()))
					{
						CurrentUnit->Interact(MovedUnit);
					}
				}
			}
		}
	}
}

AStrategyUnit* AStrategyPlayerController::GetClosestSelectedUnitToLocation(FVector TargetLocation)
{
	// closest unit and distance
	AStrategyUnit* OutUnit = nullptr;
	float Closest = 0.0f;

	// process each unit on the list
	for (AStrategyUnit* CurrentUnit : ControlledUnits)
	{
		if (CurrentUnit != nullptr)
		{
			// have we selected a unit already?
			if (OutUnit != nullptr)
			{
				// calculate the squared distance to the target location
				float Dist = FVector::DistSquared2D(TargetLocation, CurrentUnit->GetActorLocation());

				// is this unit closer?
				if (Dist < Closest)
				{
					// update the closest unit and distance
					OutUnit = CurrentUnit;
					Closest = Dist;
				}

			}
			else
			{

				// no previously selected unit, so use this one
				OutUnit = CurrentUnit;

				// initialize the closest distance
				Closest = FVector::DistSquared2D(TargetLocation, CurrentUnit->GetActorLocation());
			}
		}
		
	}

	// return the selected unit
	return OutUnit;
}

FVector2D AStrategyPlayerController::GetMouseLocation()
{
	// attempt to get the mouse position from this PC
	float MouseX, MouseY;

	if (GetMousePosition(MouseX, MouseY))
	{
		return FVector2D(MouseX, MouseY);
	}

	// return an invalid vector
	return FVector2D::ZeroVector;
}

bool AStrategyPlayerController::GetLocationUnderCursor(FVector& Location)
{
	// trace the visibility channel at the cursor location
	FHitResult OutHit;

	GetHitResultUnderCursorByChannel(SelectionTraceChannel, true, OutHit);

	// if there was a blocking hit, return the hit location
	if (OutHit.bBlockingHit)
	{
		Location = OutHit.Location;
		return true;
	}

	return OutHit.bBlockingHit;
}

FVector AStrategyPlayerController::ProjectTouchPointToWorldSpace()
{
	// get the touch coordinates for the first finger
	float TouchX, TouchY = 0.0f;
	bool bPressed = false;

	GetInputTouchState(ETouchIndex::Touch1, TouchX, TouchY, bPressed);

	FVector WorldLocation = FVector::ZeroVector;
	FVector WorldDirection = FVector::ZeroVector;

	// deproject the coords into world space
	if (DeprojectScreenPositionToWorld(TouchX, TouchY, WorldLocation, WorldDirection))
	{
		// intersect with a horizontal plane and return the resulting point
		const FPlane IntersectPlane(FVector::ZeroVector, FVector::UpVector);

		return FMath::LinePlaneIntersection(WorldLocation, WorldLocation + (WorldDirection * 100000.0f), IntersectPlane);
	}

	// failed to deproject, return a zero vector
	return FVector::ZeroVector;
}

void AStrategyPlayerController::ResetInteraction()
{
	bAllowInteraction = true;
}

void AStrategyPlayerController::CheckTouchTap(bool& bTapped, bool& bDoubleTapped)
{
	// get the current game time
	const float GameTime = GetWorld()->GetRealTimeSeconds();

	// if the player released touch before the max allowed time since press, we have a tap
	bTapped = (GameTime - LastTapPressTime) < TouchTapMaxAllowedTime;

	if (bTapped)
	{
		// we have a double tap if another tap happened before the last release time
		if ((GameTime - LastTapReleaseTime) < TouchDoubleTapMaxAllowedTime)
		{
			// increase the tap counter
			++TapCount;

		}
		else
		{

			// reset the tap counter
			TapCount = 0;
		}

	}
	else
	{

		// reset the tap counter
		TapCount = 0;
	}

	// we have a double tap if the tap count is not zero
	bDoubleTapped = TapCount >= 1;

	// save the tap release time
	LastTapReleaseTime = GameTime;
}
