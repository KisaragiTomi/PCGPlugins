// Copyright Epic Games, Inc. All Rights Reserved.


#include "TwinStickAIController.h"
#include "Components/StateTreeAIComponent.h"

ATwinStickAIController::ATwinStickAIController()
{
	// create the StateTree AI Component
	StateTreeAI = CreateDefaultSubobject<UStateTreeAIComponent>(TEXT("StateTreeAI"));
	check(StateTreeAI);

	// ensure we start the StateTree when we possess the pawn
	bStartAILogicOnPossess = true;

	// ensure we're attached to the possessed character.
	// this is necessary for EnvQueries to work correctly
	bAttachToPawn = true;
}