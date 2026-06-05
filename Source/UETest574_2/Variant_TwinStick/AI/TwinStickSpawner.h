// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TwinStickNPC.h"
#include "TwinStickSpawner.generated.h"

class ARecastNavMesh;

/**
 *  A simple NPC spawner for a Twin Stick Shooter game
 */
UCLASS(abstract)
class ATwinStickSpawner : public AActor
{
	GENERATED_BODY()
	
protected:

	/** Type of NPC to spawn */
	UPROPERTY(EditAnywhere, Category="NPC Spawner")
	TSubclassOf<ATwinStickNPC> NPCClass;
	
	/** Time delay between enemy group spawns */
	UPROPERTY(EditAnywhere, Category="NPC Spawner", meta = (ClampMin = 0, ClampMax = 20, Units = "s"))
	float SpawnGroupDelay = 5.0f;

	/** Min time delay between individual NPC spawns */
	UPROPERTY(EditAnywhere, Category="NPC Spawner", meta = (ClampMin = 0, ClampMax = 2, Units = "s"))
	float MinSpawnDelay = 0.33f;

	/** Max time delay between individual NPC spawns */
	UPROPERTY(EditAnywhere, Category="NPC Spawner", meta = (ClampMin = 0, ClampMax = 2, Units = "s"))
	float MaxSpawnDelay = 0.66f;

	/** Radius around the spawner where it can spawn NPCs */
	UPROPERTY(EditAnywhere, Category="NPC Spawner", meta = (ClampMin = 0, ClampMax = 20, Units = "cm"))
	float SpawnRadius = 600.0f;

	/** Number of NPCs to spawn per group */
	UPROPERTY(EditAnywhere, Category="NPC Spawner", meta = (ClampMin = 0, ClampMax = 10))
	int32 SpawnGroupSize = 3;
	
	/** Number of NPCs spawned in the current group */
	int32 SpawnCount = 0;

	/** NPC group spawn timer */
	FTimerHandle SpawnGroupTimer;

	/** NPC spawn timer */
	FTimerHandle SpawnNPCTimer;

	/** Pointer to the recast nav mesh actor, used to provide NPC spawn locations */
	TObjectPtr<ARecastNavMesh> NavData;

public:	

	/** Constructor */
	ATwinStickSpawner();

protected:

	/** Gameplay initialization */
	virtual void BeginPlay() override;

	/** Gameplay cleanup */
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

protected:

	/** Spawns a new NPC group */
	void SpawnNPCGroup();

	/** Spawns an individual NPC */
	void SpawnNPC();

};
