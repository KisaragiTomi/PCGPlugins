// Copyright Epic Games, Inc. All Rights Reserved.


#include "StrategyPawn.h"
#include "Components/SceneComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/FloatingPawnMovement.h"

AStrategyPawn::AStrategyPawn()
{
 	PrimaryActorTick.bCanEverTick = true;

	// create the root
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// create the camera
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);

	// create the movement component
	FloatingPawnMovement = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Floating Pawn Movement"));

	// configure the camera
	Camera->ProjectionMode = ECameraProjectionMode::Orthographic;
	Camera->OrthoWidth = 1500.0f;
	Camera->AutoPlaneShift = 1.0f;
	Camera->bUpdateOrthoPlanes = false;

	// configure the movement comp
	FloatingPawnMovement->bConstrainToPlane = true;
	FloatingPawnMovement->SetPlaneConstraintNormal(FVector::UpVector);
	FloatingPawnMovement->SetPlaneConstraintOrigin(FVector::UpVector * 1500.0f);
}

void AStrategyPawn::SetZoomModifier(float Value)
{
	// set the ortho width on the camera
	Camera->SetOrthoWidth(Value);
}
