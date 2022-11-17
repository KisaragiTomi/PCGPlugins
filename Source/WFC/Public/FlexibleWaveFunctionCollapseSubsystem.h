// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "WaveFunctionCollapseSubsystem.h"
#include "FlexibleWaveFunctionCollapseSubsystem.generated.h"

/**
 * 
 */
class AStaticMeshActor;

UCLASS()
class WFC_API AFiexibleWaveFunctionCollapseBlocker : public AActor
{
	GENERATED_BODY()
public:

};

UCLASS()
class WFC_API AFiexibleWaveFunctionCollapseContainer : public AActor
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WaveFunctionCollapse")
	TArray<int32> RemainingTiles;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WaveFunctionCollapse")
	TArray<FWaveFunctionCollapseTile> Tiles;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WaveFunctionCollapse")
	FIntVector Resolution = FIntVector::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WaveFunctionCollapse")
	float TileSize = 100;
};


UCLASS()
class WFC_API UFiexibleWaveFunctionCollapseSubsystem : public UWaveFunctionCollapseSubsystem
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "WFCFunctions")
	AActor* FiexibleCollapse(int32 TryCount = 1, int32 RandomSeed = 0);

	UFUNCTION(BlueprintCallable, Category = "WFCFunctions")
	void FiexibleCollapseSingle(int32 TryCount, int32 RandomSeed);
	
	AFiexibleWaveFunctionCollapseContainer* Container;
	TArray<int32> BlockNumbers;
	TArray<int32> RemainingTilesStore;
	int32 CurrentSpawnIndex;
private:
	AFiexibleWaveFunctionCollapseContainer* FiexibleSpawnActorFromTiles(const TArray<FWaveFunctionCollapseTile>& Tiles);
	
	bool FiexibleSpawnActorFromTile(const TArray<FWaveFunctionCollapseTile>& Tiles);

	UActorComponent* FiexibleAddNamedInstanceComponent(AActor* Actor, TSubclassOf<UActorComponent> ComponentClass, FName ComponentName);

	void FiexibleInitializeWFC(TArray<FWaveFunctionCollapseTile>& Tiles, TArray<int32>& RemainingTiles);

	TArray<FWaveFunctionCollapseOption> FiexibleGetInnerBorderOptions(FIntVector Position, const TArray<FWaveFunctionCollapseOption>& InitialOptions);

	void FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency Adjacency, const TArray<FWaveFunctionCollapseOption>& InitialOptions, TArray<FWaveFunctionCollapseOption>& OutBorderOptionsToRemove);

	bool FiexibleBuildInitialTile(FWaveFunctionCollapseTile& InitialTile);

	bool FiexibleIsPositionInnerBorder(FIntVector Position);

	bool FiexibleObservationPropagation(TArray<FWaveFunctionCollapseTile>& Tiles, TArray<int32>& RemainingTiles,TMap<int32, FWaveFunctionCollapseQueueElement>& ObservationQueue, int32 RandomSeed);

	bool FiexibleObserve(TArray<FWaveFunctionCollapseTile>& Tiles, TArray<int32>& RemainingTiles, TMap<int32, FWaveFunctionCollapseQueueElement>& ObservationQueue, int32 RandomSeed);
	
	bool FiexibleAreAllTilesNonSpawnable(const TArray<FWaveFunctionCollapseTile>& Tiles);

	void FiexibleAddAdjacentIndicesToQueue(int32 CenterIndex, const TArray<int32>& RemainingTiles, TMap<int32,FWaveFunctionCollapseQueueElement>& OutQueue);
	
	bool FiexibleObservationPropagationSingle(TArray<FWaveFunctionCollapseTile>& Tiles, TArray<int32>& RemainingTiles,TMap<int32, FWaveFunctionCollapseQueueElement>& ObservationQueue,int32 RandomSeed);

	float CalculateShannonEntropy(const TArray<FWaveFunctionCollapseOption>& Options);
};

