// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TwinStickAoEAttack.generated.h"

class UStaticMeshComponent;
class USphereComponent;

/**
 *  A simple persistent AoE attack.
 *  Damages characters that enter for as long as it's active
 */
UCLASS(abstract)
class ATwinStickAoEAttack : public AActor
{
	GENERATED_BODY()
	
	/** Provides the visual representation for the AoE attack */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	UStaticMeshComponent* SphereVisual;

	/** Provides the collision volume for the AoE attack */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	USphereComponent* CollisionSphere;

protected:

	/** Timer to start AoE damage checks */
	FTimerHandle StartAoETimer;

	/** Timer to end AoE damage checks */
	FTimerHandle StopAoETimer;

	/** Time to wait between AoE damage ticks */
	UPROPERTY(EditAnywhere, Category="AoE Attack", meta=(ClampMin = 0, ClampMax = 5, Units = "s"))
	float StartAoETime = 0.033f;

	/** Time to wait before stopping AoE damage checks */
	UPROPERTY(EditAnywhere, Category="AoE Attack", meta=(ClampMin = 0, ClampMax = 5, Units = "s"))
	float StopAoETime = 0.5f;

	/** While true, the AoE will damage anything that overlaps it */
	bool bIsAoEActive = false;

public:	
	
	/** Constructor */
	ATwinStickAoEAttack();

protected:

	/** Initialization */
	virtual void BeginPlay() override;

	/** Cleanup */
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

protected:

	/** Called when the start AoE timer triggers */
	void StartAoE();

	/** Called when the stop AoE timer triggers */
	void StopAoE();

	/** Allows Blueprint handling of AoE fade out effects. NOTE: Call Destroy Actor at the end of this! */
	UFUNCTION(BlueprintImplementableEvent, Category="AoE Attack")
	void BP_AoEFinished();

	/** Handles collision with the AoE while it's active */
	UFUNCTION()
	void OnAoEOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};
