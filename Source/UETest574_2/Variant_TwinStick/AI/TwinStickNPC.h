// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "TwinStickNPC.generated.h"

class ATwinStickPickup;
class ATwinStickNPCDestruction;

/**
 *  A simple enemy NPC for a Twin Stick Shooter game
 *  It's driven by an AI Controller running a behavior tree
 *  Awards points and randomly spawns pickups on death
 */
UCLASS(abstract)
class ATwinStickNPC : public ACharacter
{
	GENERATED_BODY()

protected:

	/** Score to award when this NPC is destroyed */
	UPROPERTY(EditAnywhere, Category="Score", meta=(ClampMin = 0, ClampMax = 100))
	int32 Score = 1;

	/** Percentage chance of spawning a pickup */
	UPROPERTY(EditAnywhere, Category="Pickup", meta=(ClampMin = 0, ClampMax = 100))
	int32 PickupSpawnChance = 10;

	/** Type of pickup to spawn on death */
	UPROPERTY(EditAnywhere, Category="Pickup")
	TSubclassOf<ATwinStickPickup> PickupClass;

	/** Type of destruction proxy to spawn on death */
	UPROPERTY(EditAnywhere, Category="Destruction")
	TSubclassOf<ATwinStickNPCDestruction> DestructionProxyClass;

	/** Time to wait after this NPC is hit before destroying it */
	UPROPERTY(EditAnywhere, Category="Pickup", meta=(ClampMin = 0, ClampMax = 5, Units = "s"))
	float DeferredDestructionTime = 0.1f;

	/** Deferred destruction timer */
	FTimerHandle DestructionTimer;

public:

	/** If true, this NPC has already been hit by a projectile and is being destroyed. Exposed to BP so it can be read by StateTree */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="NPC")
	bool bHit = false;

public:

	/** Constructor */
	ATwinStickNPC();

protected:

	/** Gameplay Initialization */
	virtual void BeginPlay() override;

	/** Gameplay cleanup */
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

	/** Handle destruction */
	virtual void Destroyed() override;

	/** Collision handling */
	virtual void NotifyHit(class UPrimitiveComponent* MyComp, AActor* Other, class UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

public:

	/** Tells the NPC to process a projectile impact */
	void ProjectileImpact(const FVector& ForwardVector);

protected:

	/** Called from timer to complete the destruction process for this NPC */
	void DeferredDestroy();
};
