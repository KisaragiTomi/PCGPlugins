// Fill out your copyright notice in the Description page of Project Settings.


#include "FlexibleWaveFunctionCollapseSubsystem.h"
#include "WaveFunctionCollapseBPLibrary.h"
#include "Math/UnrealMathUtility.h"
#include "Math/NumericLimits.h"
#include "Math/Vector.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Editor/EditorEngine.h"

AActor* UFiexibleWaveFunctionCollapseSubsystem::FiexibleCollapse(int32 TryCount, int32 RandomSeed)
{
		if (!WFCModel)
	{
		UE_LOG(LogWFC, Error, TEXT("Invalid WFC Model"));
		return nullptr;
	}
	
	UE_LOG(LogWFC, Display, TEXT("Starting WFC - Model: %s, Resolution %dx%dx%d"), *WFCModel->GetFName().ToString(), Resolution.X, Resolution.Y, Resolution.Z);

	// Determinism settings
	int32 ChosenRandomSeed = (RandomSeed != 0 ? RandomSeed : FMath::RandRange(1, TNumericLimits<int32>::Max()));

	int32 ArrayReserveValue = Resolution.X * Resolution.Y * Resolution.Z;
	TArray<FWaveFunctionCollapseTile> Tiles;
	TArray<int32> RemainingTiles;
	TMap<int32, FWaveFunctionCollapseQueueElement> ObservationQueue;
	Tiles.Reserve(ArrayReserveValue);
	RemainingTiles.Reserve(ArrayReserveValue);

	FiexibleInitializeWFC(Tiles, RemainingTiles);
	
	bool bSuccessfulSolve = false;
	
	
	if (TryCount > 1)
	{
		//Copy Original Initialized tiles
		TArray<FWaveFunctionCollapseTile> TilesCopy = Tiles;
		TArray<int32> RemainingTilesCopy = RemainingTiles;

		int32 CurrentTry = 1;
		bSuccessfulSolve = FiexibleObservationPropagation(Tiles, RemainingTiles, ObservationQueue, ChosenRandomSeed);
		FRandomStream RandomStream(ChosenRandomSeed);
		 while (!bSuccessfulSolve && CurrentTry<TryCount)
		 {
		 	CurrentTry += 1;
		 	UE_LOG(LogWFC, Warning, TEXT("Failed with Seed Value: %d. Trying again.  Attempt number: %d"), ChosenRandomSeed, CurrentTry);
		 	ChosenRandomSeed = RandomStream.RandRange(1, TNumericLimits<int32>::Max());
		 	
		 	// Start from Original Initialized tiles
		 	Tiles = TilesCopy;
		 	RemainingTiles = RemainingTilesCopy;
		 	ObservationQueue.Empty();
		 	bSuccessfulSolve = FiexibleObservationPropagation(Tiles, RemainingTiles, ObservationQueue, ChosenRandomSeed);
		 }
	}
	else if (TryCount == 1)
	{
		bSuccessfulSolve = FiexibleObservationPropagationSingle(Tiles, RemainingTiles, ObservationQueue, ChosenRandomSeed);
	}
	else
	{
		UE_LOG(LogWFC, Error, TEXT("Invalid TryCount on Collapse: %d"), TryCount);
		return nullptr;
	}

	// if Successful, Spawn Actor
	if (bSuccessfulSolve)
	{
		AFiexibleWaveFunctionCollapseContainer* SpawnedActor = FiexibleSpawnActorFromTiles(Tiles);
		SpawnedActor->RemainingTiles = RemainingTiles;
		UE_LOG(LogWFC, Display, TEXT("Success! Seed Value: %d. Spawned Actor: %s"), ChosenRandomSeed, *SpawnedActor->GetActorLabel());
		return SpawnedActor;
	}
	else
	{
		UE_LOG(LogWFC, Error, TEXT("Failed after %d tries."), TryCount);
		return nullptr;
	}
}


void UFiexibleWaveFunctionCollapseSubsystem::FiexibleCollapseSingle(int32 TryCount, int32 RandomSeed)
{
	if (!WFCModel)
	{
		UE_LOG(LogWFC, Error, TEXT("Invalid WFC Model"));
		return;
	}
	
	UE_LOG(LogWFC, Display, TEXT("Starting WFC - Model: %s, Resolution %dx%dx%d"), *WFCModel->GetFName().ToString(), Resolution.X, Resolution.Y, Resolution.Z);

	// Determinism settings
	int32 ChosenRandomSeed = (RandomSeed != 0 ? RandomSeed : FMath::RandRange(1, TNumericLimits<int32>::Max()));

	int32 ArrayReserveValue = Resolution.X * Resolution.Y * Resolution.Z;
	TArray<FWaveFunctionCollapseTile> Tiles;
	TArray<int32> RemainingTiles;
	TMap<int32, FWaveFunctionCollapseQueueElement> ObservationQueue;
	Tiles.Reserve(ArrayReserveValue);
	RemainingTiles.Reserve(ArrayReserveValue);

	bool FindContainer = false;
	for (TActorIterator<AFiexibleWaveFunctionCollapseContainer> It(GWorld, AFiexibleWaveFunctionCollapseContainer::StaticClass()); It; ++It)
	{
		AFiexibleWaveFunctionCollapseContainer* Actor = *It;
		if(Actor->RemainingTiles.Num() > 0)
		{
			
			Container = Actor;
			FindContainer = true;
			break; 
		}
	}
	
	
	if(FindContainer)
	{
		RemainingTiles = Container->RemainingTiles;
		Tiles = Container->Tiles;
	}
	else
	{
		FiexibleInitializeWFC(Tiles, RemainingTiles);
		//Tiles.Sort([](FWaveFunctionCollapseTile A, FWaveFunctionCollapseTile B) { return A.ShannonEntropy > B.ShannonEntropy; });
		RemainingTiles.Empty();
		//收集到一起排个序
		TMap<float, TArray<int32>> SeqCollection;
		for(int32 i = 0; i < Tiles.Num(); i++)
		{
			if(Tiles[i].ShannonEntropy > 999)
			{
				continue;
			}
			bool find = false;
			if(SeqCollection.Find(Tiles[i].ShannonEntropy) != nullptr)
			{
				TArray<int32>& PreArray = *SeqCollection.Find(Tiles[i].ShannonEntropy);
				PreArray.Add(i);
			}
			else
			{
				TArray<int32> TempArray;
				TempArray.Add(i);
				SeqCollection.Add(Tiles[i].ShannonEntropy, TempArray);
			}
		}
		SeqCollection.KeySort([](float A, float B) { return A < B; });
		for(TTuple<float, TArray<int32>>& Pair : SeqCollection)
		{
			for(int32 i : Pair.Value)
			{
				RemainingTiles.Add(i);
			}
		}
		RemainingTilesStore = RemainingTiles;
	}
	
	//sort简单排序一下等下按照接触数量多的顺序加组件
	
	bool bSuccessfulSolve = false;

	if (TryCount > 1)
	{
		//Copy Original Initialized tiles
		TArray<FWaveFunctionCollapseTile> TilesCopy = Tiles;
		TArray<int32> RemainingTilesCopy = RemainingTiles;

		int32 CurrentTry = 1;
		bSuccessfulSolve = FiexibleObservationPropagationSingle(Tiles, RemainingTiles, ObservationQueue, ChosenRandomSeed);
	}
	else if (TryCount == 1)
	{
		bSuccessfulSolve = FiexibleObservationPropagationSingle(Tiles, RemainingTiles, ObservationQueue, ChosenRandomSeed);
	}
	else
	{
		UE_LOG(LogWFC, Error, TEXT("Invalid TryCount on Collapse: %d"), TryCount);
		return;
	}

	// if Successful, Spawn Actor
	// if (bSuccessfulSolve)
	// {
	//Container->RemainingTiles.Empty();

	if(!FindContainer)
	{
		UObject* loadObj = StaticLoadObject(UBlueprint::StaticClass(), NULL, TEXT("Blueprint'/TAToolsPlugin/WFC/FiexibleWaveFunctionCollapseContainerVisualization.FiexibleWaveFunctionCollapseContainerVisualization'"));  
		if (loadObj != nullptr)  
		{
			UBlueprint* ubp = Cast<UBlueprint>(loadObj);  
			UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
			// Spawn Actor
			Container = Cast<AFiexibleWaveFunctionCollapseContainer>(GWorld->SpawnActor<AActor>(ubp->GeneratedClass));
			Container->SetActorLocationAndRotation(OriginLocation, Orientation);

		}
		else
		{
			return;
		}
	}
	//每次更新都改变属性
	Container->RemainingTiles = RemainingTilesStore;
	Container->Tiles = Tiles;
	Container->Resolution = Resolution;
	Container->TileSize = WFCModel->TileSize;
	//FActorLabelUtilities::SetActorLabelUnique(SpawnedActor, WFCModel->GetFName().ToString());
	FiexibleSpawnActorFromTile(Tiles);
		//UE_LOG(LogWFC, Display, TEXT("Success! Seed Value: %d. Spawned Actor: %s"), ChosenRandomSeed, *SpawnedActor->GetActorLabel());
	

	//UE_LOG(LogWFC, Error, TEXT("Failed after %d tries."), TryCount);
	return;
	
}

AFiexibleWaveFunctionCollapseContainer* UFiexibleWaveFunctionCollapseSubsystem::FiexibleSpawnActorFromTiles(const TArray<FWaveFunctionCollapseTile>& Tiles)
{
	UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	// Spawn Actor
	AFiexibleWaveFunctionCollapseContainer* SpawnedActor = Cast<AFiexibleWaveFunctionCollapseContainer>(EditorActorSubsystem->SpawnActorFromClass(AFiexibleWaveFunctionCollapseContainer::StaticClass(), FVector::ZeroVector));
	SpawnedActor->GetRootComponent()->SetMobility(EComponentMobility::Static);
	SpawnedActor->SetActorLocationAndRotation(OriginLocation, Orientation);
	FActorLabelUtilities::SetActorLabelUnique(SpawnedActor, WFCModel->GetFName().ToString());

	// Create Components
	TMap<FSoftObjectPath, UInstancedStaticMeshComponent*> BaseObjectToISM;
	for (int32 index = 0; index < Tiles.Num(); index++)
	{
		if (Tiles[index].RemainingOptions.Num() != 1)
		{
			continue;
		}

		// check for empty or void options
		FSoftObjectPath BaseObject = Tiles[index].RemainingOptions[0].BaseObject;
		if (BaseObject == FWaveFunctionCollapseOption::EmptyOption.BaseObject
			|| BaseObject == FWaveFunctionCollapseOption::VoidOption.BaseObject)
		{
			continue;
		}

		// Check the SpawnExclusion array
		if (WFCModel->SpawnExclusion.Contains(BaseObject))
		{
			continue;
		}

		UObject* LoadedObject = BaseObject.TryLoad();
		if (LoadedObject)
		{
			FRotator BaseRotator = Tiles[index].RemainingOptions[0].BaseRotator;
			FVector BaseScale3D = Tiles[index].RemainingOptions[0].BaseScale3D;
			FVector PositionOffset = FVector(WFCModel->TileSize * 0.5f);
			FVector TilePosition = (FVector(UWaveFunctionCollapseBPLibrary::IndexAsPosition(index, Resolution)) * WFCModel->TileSize) + PositionOffset;

			// Static meshes are handled with ISM Components
			if (UStaticMesh* LoadedStaticMesh = Cast<UStaticMesh>(LoadedObject))
			{
				UInstancedStaticMeshComponent* ISMComponent;
				if (UInstancedStaticMeshComponent** FoundISMComponentPtr = BaseObjectToISM.Find(BaseObject))
				{
					ISMComponent = *FoundISMComponentPtr;
				}
				else
				{
					ISMComponent = Cast<UInstancedStaticMeshComponent>(UFiexibleWaveFunctionCollapseSubsystem::FiexibleAddNamedInstanceComponent(SpawnedActor, UInstancedStaticMeshComponent::StaticClass(), LoadedObject->GetFName()));
					BaseObjectToISM.Add(BaseObject, ISMComponent);
				}
				ISMComponent->SetStaticMesh(LoadedStaticMesh);
				ISMComponent->SetMobility(EComponentMobility::Static);
				ISMComponent->AddInstance(FTransform(BaseRotator, TilePosition, BaseScale3D));
			}
			// Blueprints are handled with ChildActorComponents
			else if (UBlueprint* LoadedBlueprint = Cast<UBlueprint>(LoadedObject))
			{
				// if BP is placeable
				if (!(LoadedBlueprint->GetClass()->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Deprecated | CLASS_Abstract))
					&& LoadedBlueprint->GetClass()->IsChildOf(AActor::StaticClass()))
				{
					UChildActorComponent* ChildActorComponent = Cast<UChildActorComponent>(UFiexibleWaveFunctionCollapseSubsystem::FiexibleAddNamedInstanceComponent(SpawnedActor, UChildActorComponent::StaticClass(), LoadedObject->GetFName()));
					ChildActorComponent->SetChildActorClass(LoadedBlueprint->GetClass());
					ChildActorComponent->SetRelativeLocation(TilePosition);
					ChildActorComponent->SetRelativeRotation(BaseRotator);
					ChildActorComponent->SetRelativeScale3D(BaseScale3D);
				}
			}
			else
			{
				UE_LOG(LogWFC, Warning, TEXT("Invalid Type, skipping: %s"), *BaseObject.ToString());
			}
		}
		else
		{
			UE_LOG(LogWFC, Warning, TEXT("Unable to load object, skipping: %s"), *BaseObject.ToString());
		}
	}

	return SpawnedActor;
}

bool UFiexibleWaveFunctionCollapseSubsystem::FiexibleSpawnActorFromTile(const TArray<FWaveFunctionCollapseTile>& Tiles)
{
	// Create Components
	TMap<FSoftObjectPath, UInstancedStaticMeshComponent*> BaseObjectToISM;

	if (Tiles[CurrentSpawnIndex].RemainingOptions.Num() != 1)
	{
		return false;
	}

	// check for empty or void options
	FSoftObjectPath BaseObject = Tiles[CurrentSpawnIndex].RemainingOptions[0].BaseObject;
	if (BaseObject == FWaveFunctionCollapseOption::EmptyOption.BaseObject
		|| BaseObject == FWaveFunctionCollapseOption::VoidOption.BaseObject)
	{
		return false;
	}

	// Check the SpawnExclusion array
	if (WFCModel->SpawnExclusion.Contains(BaseObject))
	{
		return false;
	}

	UObject* LoadedObject = BaseObject.TryLoad();
	if (LoadedObject)
	{
		FRotator BaseRotator = Tiles[CurrentSpawnIndex].RemainingOptions[0].BaseRotator;
		FVector BaseScale3D = Tiles[CurrentSpawnIndex].RemainingOptions[0].BaseScale3D;
		FVector PositionOffset = FVector(WFCModel->TileSize * 0.5f);
		FVector TilePosition = (FVector(UWaveFunctionCollapseBPLibrary::IndexAsPosition(CurrentSpawnIndex, Resolution)) * WFCModel->TileSize) + PositionOffset;

		// Static meshes are handled with ISM Components
		if (UStaticMesh* LoadedStaticMesh = Cast<UStaticMesh>(LoadedObject))
		{
			UInstancedStaticMeshComponent* ISMComponent;
			if (UInstancedStaticMeshComponent** FoundISMComponentPtr = BaseObjectToISM.Find(BaseObject))
			{
				ISMComponent = *FoundISMComponentPtr;
			}
			else
			{
				ISMComponent = Cast<UInstancedStaticMeshComponent>(UFiexibleWaveFunctionCollapseSubsystem::FiexibleAddNamedInstanceComponent(Container, UInstancedStaticMeshComponent::StaticClass(), LoadedObject->GetFName()));
				BaseObjectToISM.Add(BaseObject, ISMComponent);
			}
			ISMComponent->SetStaticMesh(LoadedStaticMesh);
			ISMComponent->SetMobility(EComponentMobility::Movable);
			ISMComponent->AddInstance(FTransform(BaseRotator, TilePosition, BaseScale3D));
		}
		// Blueprints are handled with ChildActorComponents
		else if (UBlueprint* LoadedBlueprint = Cast<UBlueprint>(LoadedObject))
		{
			// if BP is placeable
			if (!(LoadedBlueprint->GetClass()->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Deprecated | CLASS_Abstract))
				&& LoadedBlueprint->GetClass()->IsChildOf(AActor::StaticClass()))
			{
				UChildActorComponent* ChildActorComponent = Cast<UChildActorComponent>(UFiexibleWaveFunctionCollapseSubsystem::FiexibleAddNamedInstanceComponent(Container, UChildActorComponent::StaticClass(), LoadedObject->GetFName()));
				ChildActorComponent->SetChildActorClass(LoadedBlueprint->GetClass());
				ChildActorComponent->SetRelativeLocation(TilePosition);
				ChildActorComponent->SetRelativeRotation(BaseRotator);
				ChildActorComponent->SetRelativeScale3D(BaseScale3D);
			}
		}
		else
		{
			UE_LOG(LogWFC, Warning, TEXT("Invalid Type, skipping: %s"), *BaseObject.ToString());
		}
	}
	else
	{
		UE_LOG(LogWFC, Warning, TEXT("Unable to load object, skipping: %s"), *BaseObject.ToString());
	}
	
	return true;
}

UActorComponent* UFiexibleWaveFunctionCollapseSubsystem::FiexibleAddNamedInstanceComponent(AActor* Actor, TSubclassOf<UActorComponent> ComponentClass, FName ComponentName)
{
	Actor->Modify();
	// Assign Unique Name
	int32 Counter = 1;
	FName ComponentInstanceName = ComponentName;
	while (!FComponentEditorUtils::IsComponentNameAvailable(ComponentInstanceName.ToString(), Actor))
	{
		ComponentInstanceName = FName(*FString::Printf(TEXT("%s_%d"), *ComponentName.ToString(), Counter++));
	}
	UActorComponent* InstanceComponent = NewObject<UActorComponent>(Actor, ComponentClass, ComponentInstanceName, RF_Transactional);
	if (InstanceComponent)
	{
		Actor->AddInstanceComponent(InstanceComponent);
		Actor->FinishAddComponent(InstanceComponent, false, FTransform::Identity);
		Actor->RerunConstructionScripts();
	}
	return InstanceComponent;
}


TArray<FWaveFunctionCollapseOption> UFiexibleWaveFunctionCollapseSubsystem::FiexibleGetInnerBorderOptions(FIntVector Position, const TArray<FWaveFunctionCollapseOption>& InitialOptions)
{
	TArray<FWaveFunctionCollapseOption> InnerBorderOptions;
	TArray<FWaveFunctionCollapseOption> InnerBorderOptionsToRemove;
	InnerBorderOptions = InitialOptions;
	int32 Neighbour;

	// gather options to remove
	Neighbour = UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(int32(FMath::Max(Position.X - 1, 0)), Position.Y, Position.Z), Resolution);
	if (Position.X == 0 || BlockNumbers.Find(Neighbour) > -1)
	{
		FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency::Front, InnerBorderOptions, InnerBorderOptionsToRemove);
	}
	Neighbour = UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(int32(FMath::Min(Position.X + 1, Resolution.X - 1)), Position.Y, Position.Z), Resolution);
	if (Position.X == Resolution.X - 1 || BlockNumbers.Find(Neighbour) > -1)
	{
		FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency::Back, InnerBorderOptions, InnerBorderOptionsToRemove);
	}
	Neighbour = UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(Position.X, int32(FMath::Max(Position.Y - 1, 0)), Position.Z), Resolution);
	if (Position.Y == 0 || BlockNumbers.Find(Neighbour) > -1)
	{
		FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency::Right, InnerBorderOptions, InnerBorderOptionsToRemove);
	}
	Neighbour = UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(Position.X, int32(FMath::Min(Position.Y + 1, Resolution.Y - 1)), Position.Z), Resolution);
	if (Position.Y == Resolution.Y - 1 || BlockNumbers.Find(Neighbour) > -1)
	{
		FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency::Left, InnerBorderOptions, InnerBorderOptionsToRemove);
	}
	Neighbour = UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(Position.X, Position.Y, int32(FMath::Max(Position.Z - 1, 0))), Resolution);
	if (Position.Z == 0 || BlockNumbers.Find(Neighbour) > -1)
	{
		FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency::Up, InnerBorderOptions, InnerBorderOptionsToRemove);
	}
	Neighbour = UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(Position.X, Position.Y, int32(FMath::Min(Position.Z + 1, Resolution.Z - 1))), Resolution);
	if (Position.Z == Resolution.Z - 1 || BlockNumbers.Find(Neighbour) > -1)
	{
		FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency::Down, InnerBorderOptions, InnerBorderOptionsToRemove);
	}
	
	//remove options
	if (!InnerBorderOptionsToRemove.IsEmpty())
	{
		for (FWaveFunctionCollapseOption& RemoveThisOption : InnerBorderOptionsToRemove)
		{
			InnerBorderOptions.RemoveSingleSwap(RemoveThisOption, false);
		}
		InnerBorderOptions.Shrink();
	}
	
	return InnerBorderOptions;
}

void UFiexibleWaveFunctionCollapseSubsystem::FiexibleInitializeWFC(TArray<FWaveFunctionCollapseTile>& Tiles, TArray<int32>& RemainingTiles)
{
	FWaveFunctionCollapseTile InitialTile;
	int32 SwapIndex = 0;
	
	if (FiexibleBuildInitialTile(InitialTile))
	{
		//检测是否有碰撞
		BlockNumbers.Empty();
		//BlockNumber.Reserve(Tiles.Num());
		int32 NumTiles = Resolution.X * Resolution.Y * Resolution.Z;
		for(int32 index = 0; index < NumTiles; index++)
		{
			
			//FRotator BaseRotator = Tiles[index].RemainingOptions[0].BaseRotator;
			//FVector BaseScale3D = Tiles[index].RemainingOptions[0].BaseScale3D;
			FVector PositionOffset = FVector(WFCModel->TileSize * 0.5f);
			FVector TilePosition = (FVector(UWaveFunctionCollapseBPLibrary::IndexAsPosition(index, Resolution)) * WFCModel->TileSize) + PositionOffset;
			FActorSpawnParameters Params;
			//AStaticMeshActor* StaticActor = GWorld->SpawnActor<AStaticMeshActor>(TilePosition, BaseRotator, Params);
			
			TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes{ UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldDynamic), UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic) };
			TArray<AActor*> ActorsToIgnore;
			TArray<FHitResult> OutHits;

			if (UKismetSystemLibrary::BoxTraceMultiForObjects(GWorld, TilePosition, TilePosition, FVector::OneVector * WFCModel->TileSize / 2,FRotator::ZeroRotator, ObjectTypes, true, ActorsToIgnore, EDrawDebugTrace::None, OutHits, true))
			{
				for(FHitResult OutHit : OutHits)
				{
					if(Cast<AFiexibleWaveFunctionCollapseBlocker>(OutHit.GetActor()))
					{
						BlockNumbers.Add(index);
						break;
					}
				}
			}
		}
		
		float MinEntropy = InitialTile.ShannonEntropy;
		for (int32 Z = 0;Z < Resolution.Z; Z++)
		{
			for (int32 Y = 0;Y < Resolution.Y; Y++)
			{
				for (int32 X = 0;X < Resolution.X; X++)
				{
					// Pre-populate with border tiles
					int32 Index = UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(X, Y, Z), Resolution);
					if(BlockNumbers.Contains(Index))
					{
						FWaveFunctionCollapseTile EmptyTile = FWaveFunctionCollapseTile(FWaveFunctionCollapseOption::EmptyOption, TNumericLimits<float>::Max());
						Tiles.Add(EmptyTile);
						//RemainingTiles.Add(Index);
						continue;
					}
					if (FiexibleIsPositionInnerBorder(FIntVector(X, Y, Z))
						&& (bUseEmptyBorder || WFCModel->Constraints.Contains(FWaveFunctionCollapseOption::BorderOption)))
					{
						FWaveFunctionCollapseTile BorderTile;
						BorderTile.RemainingOptions = FiexibleGetInnerBorderOptions(FIntVector(X, Y, Z), InitialTile.RemainingOptions);
						BorderTile.ShannonEntropy = CalculateShannonEntropy(BorderTile.RemainingOptions);
						Tiles.Add(BorderTile);
						RemainingTiles.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(X, Y, Z), Resolution));

						// swap lower entropy tile to the beginning of RemainingTiles
						if (BorderTile.ShannonEntropy < MinEntropy)
						{
							RemainingTiles.Swap(0, RemainingTiles.Num() - 1);
							MinEntropy = BorderTile.ShannonEntropy;
							SwapIndex = 0;
						}
						
						// else, swap min entropy tile with the previous min entropy index+1
						else if (BorderTile.ShannonEntropy ==  MinEntropy && BorderTile.ShannonEntropy != InitialTile.ShannonEntropy)
						{
							SwapIndex += 1;
							RemainingTiles.Swap(SwapIndex, RemainingTiles.Num() - 1);
						}
					}
					
					// Fill the rest with initial tiles
					else
					{
						Tiles.Add(InitialTile);
						RemainingTiles.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(X, Y, Z), Resolution));
					}
					//RemainingTiles.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(FIntVector(X, Y, Z), Resolution));
				}
			}
		}
		StarterOptions.Empty();
	}
	else
	{
		UE_LOG(LogWFC, Error, TEXT("Could not create Initial Tile from Model"));
	}

}

void UFiexibleWaveFunctionCollapseSubsystem::FiexibleGatherInnerBorderOptionsToRemove(EWaveFunctionCollapseAdjacency Adjacency, const TArray<FWaveFunctionCollapseOption>& InitialOptions, TArray<FWaveFunctionCollapseOption>& OutBorderOptionsToRemove)
{
	bool bFoundBorderOptions = false;
	for (const FWaveFunctionCollapseOption& InitialOption : InitialOptions)
	{
		// if border option exists in the model, use it
		if (FWaveFunctionCollapseAdjacencyToOptionsMap* FoundBorderAdjacencyToOptionsMap = WFCModel->Constraints.Find(FWaveFunctionCollapseOption::BorderOption))
		{
			if (FWaveFunctionCollapseOptions* FoundBorderOptions = FoundBorderAdjacencyToOptionsMap->AdjacencyToOptionsMap.Find(Adjacency))
			{
				if (!FoundBorderOptions->Options.Contains(InitialOption))
				{
					OutBorderOptionsToRemove.AddUnique(InitialOption);
					bFoundBorderOptions = true;
				}
			}
		}
		
		// else, if useEmptyBorder, use empty option
		if (bUseEmptyBorder && !bFoundBorderOptions)
		{
			if (FWaveFunctionCollapseAdjacencyToOptionsMap* FoundEmptyAdjacencyToOptionsMap = WFCModel->Constraints.Find(FWaveFunctionCollapseOption::EmptyOption))
			{
				if (FWaveFunctionCollapseOptions* FoundEmptyOptions = FoundEmptyAdjacencyToOptionsMap->AdjacencyToOptionsMap.Find(Adjacency))
				{
					if (!FoundEmptyOptions->Options.Contains(InitialOption))
					{
						OutBorderOptionsToRemove.AddUnique(InitialOption);
					}
				}
			}
		}
	}
}

bool UFiexibleWaveFunctionCollapseSubsystem::FiexibleBuildInitialTile(FWaveFunctionCollapseTile& InitialTile)
{
	TArray<FWaveFunctionCollapseOption> InitialOptions;
	for (const TPair< FWaveFunctionCollapseOption, FWaveFunctionCollapseAdjacencyToOptionsMap>& Constraint : WFCModel->Constraints)
	{
		if (Constraint.Key.BaseObject != FWaveFunctionCollapseOption::BorderOption.BaseObject)
		{
			InitialOptions.Add(Constraint.Key);
		}
	}

	if (!InitialOptions.IsEmpty())
	{
		InitialTile.RemainingOptions = InitialOptions;
		InitialTile.ShannonEntropy = UWaveFunctionCollapseBPLibrary::CalculateShannonEntropy(InitialOptions, WFCModel);
		return true;
	}
	else
	{
		return false;
	}
}

bool UFiexibleWaveFunctionCollapseSubsystem::FiexibleIsPositionInnerBorder(FIntVector Position)
{
	return (Position.X == 0
		|| Position.Y == 0
		|| Position.Z == 0
		|| Position.X == Resolution.X - 1
		|| Position.Y == Resolution.Y - 1
		|| Position.Z == Resolution.Z - 1);
}

bool UFiexibleWaveFunctionCollapseSubsystem::FiexibleObserve(TArray<FWaveFunctionCollapseTile>& Tiles, 
	TArray<int32>& RemainingTiles, 
	TMap<int32, FWaveFunctionCollapseQueueElement>& ObservationQueue,
	int32 RandomSeed)
{
	float MinEntropy = 0;
	float MaxEntropy = 0;
	int32 SelectIndex = 0;
	int32 LastSameMaxEntropyIndex = 0;
	int32 SelectedMaxEntropyIndex = 0;
	int32 MaxEntropyIndex = 0;
	FRandomStream RandomStream(RandomSeed);

	// Find MinEntropy Tile Indices
	// if (RemainingTiles.Num() > 1)
	// {
	// 	for (int32 index = 0; index<RemainingTiles.Num(); index++)
	// 	{
	//
	// 		if (Tiles[RemainingTiles[index]].ShannonEntropy > MaxEntropy)
	// 		{
	// 			MaxEntropy = Tiles[RemainingTiles[index]].ShannonEntropy;
	// 			SelectIndex = index;
	// 		}
	// 		else
	// 		{
	// 			LastSameMaxEntropyIndex += 1;
	// 		}
	// 	}
	// 	SelectedMaxEntropyIndex = RandomStream.RandRange(0, LastSameMaxEntropyIndex);
	// 	MaxEntropyIndex = RemainingTiles[SelectIndex];
	// }
	// else
	// {
	// 	MaxEntropyIndex = RemainingTiles[0];
	// }
	MaxEntropyIndex = RemainingTilesStore.Pop();

	// Rand Selection of Weighted Options using Cumulative Density
	TArray<float> CumulativeDensity;
	//TArray<int32> AdjacencyCounts;
	//TMap<int32, int32> AdjacencyMap;
	CumulativeDensity.Reserve(Tiles[MaxEntropyIndex].RemainingOptions.Num());
	float CumulativeWeight = 0;
	float MaxOptionEntropy = 0;
	int32 SelectOption = 0;
	int32 Count = 0;
	int32 AdjacencyCountStore = 0;
	for (FWaveFunctionCollapseOption& Option : Tiles[MaxEntropyIndex].RemainingOptions)
	{
		if(Option.BaseObject == FWaveFunctionCollapseOption::EmptyOption.BaseObject
				|| Option.BaseObject == FWaveFunctionCollapseOption::VoidOption.BaseObject)
		{
			Count += 1;
			continue;
		}
		TArray<EWaveFunctionCollapseAdjacency> InitialAdjacencies;
		InitialAdjacencies.Add(EWaveFunctionCollapseAdjacency::Back);
		InitialAdjacencies.Add(EWaveFunctionCollapseAdjacency::Front);
		InitialAdjacencies.Add(EWaveFunctionCollapseAdjacency::Down);
		InitialAdjacencies.Add(EWaveFunctionCollapseAdjacency::Up);
		InitialAdjacencies.Add(EWaveFunctionCollapseAdjacency::Left);
		InitialAdjacencies.Add(EWaveFunctionCollapseAdjacency::Right);
		int32 AdjacencyCount = 0;
		for(const TPair<EWaveFunctionCollapseAdjacency, FWaveFunctionCollapseOptions> AdjacencyPair :WFCModel->Constraints.Find(Option)->AdjacencyToOptionsMap)
		{
			if(InitialAdjacencies.Contains(AdjacencyPair.Key))
			{
				InitialAdjacencies.Remove( AdjacencyPair.Key);
				AdjacencyCount += 1;
			}
		}
		if(AdjacencyCount == 6 )
		{
			SelectOption = Count;
			break;
		}
		if(AdjacencyCount > AdjacencyCountStore)
		{
			SelectOption = Count;
			AdjacencyCountStore = AdjacencyCount;
		}
		//AdjacencyCounts.Add(Count);
		CumulativeWeight += WFCModel->Constraints.Find(Option)->Weight;
		CumulativeDensity.Add(CumulativeWeight);
		Count += 1;
	}

	// CumulativeDensity.Reserve(Tiles[MaxEntropyIndex].RemainingOptions.Num());
	// CumulativeWeight = 0;
	// for (FWaveFunctionCollapseOption& Option : Tiles[MaxEntropyIndex].RemainingOptions)
	// {
	// 	CumulativeWeight += WFCModel->Constraints.Find(Option)->Weight;
	// 	CumulativeDensity.Add(CumulativeWeight);
	// }
	//
	// int32 SelectedOptionIndex = 0;
	// float RandomDensity = RandomStream.FRandRange(0.0f, CumulativeDensity.Last());
	// for (int32 Index = 0; Index < CumulativeDensity.Num(); Index++)
	// {
	// 	if (CumulativeDensity[Index] > RandomDensity)
	// 	{
	// 		SelectedOptionIndex = Index;
	// 		break;
	// 	}
	// }
	float MaxWeight = 0;
	int32 SelectedOptionIndex = 0;
	TMap<float, TArray<int32>> SeqCollection;
	for (int32 i = 0; i < Tiles[MaxEntropyIndex].RemainingOptions.Num(); i++)
	{
		float CurrentWeight = WFCModel->Constraints.Find(Tiles[MaxEntropyIndex].RemainingOptions[i])->Weight;
		if(CurrentWeight >= MaxWeight)
		{
			SelectedOptionIndex = i;
			MaxWeight = CurrentWeight;
		}
	}
	// Make Selection
	CurrentSpawnIndex = MaxEntropyIndex;
	if(Tiles[MaxEntropyIndex].RemainingOptions.Num() == 1)
	{
		Tiles[MaxEntropyIndex] = FWaveFunctionCollapseTile(Tiles[MaxEntropyIndex].RemainingOptions[0], TNumericLimits<float>::Max());
	}
	else
	{
		Tiles[MaxEntropyIndex] = FWaveFunctionCollapseTile(Tiles[MaxEntropyIndex].RemainingOptions[SelectedOptionIndex], TNumericLimits<float>::Max());

	}
	//Tiles[MinEntropyIndex] = FWaveFunctionCollapseTile(Tiles[MinEntropyIndex].RemainingOptions[SelectedOptionIndex], TNumericLimits<float>::Max());


	// if (SelectIndex != LastSameMaxEntropyIndex)
	// {
	// 	RemainingTiles.Swap(SelectIndex, LastSameMaxEntropyIndex);
	// }
	//RemainingTilesStore.RemoveAtSwap(0);

	if (!RemainingTiles.IsEmpty())
	{
		// if MinEntropy has changed after removal, find new MinEntropy and swap to front of array
		if (Tiles[RemainingTiles[0]].ShannonEntropy != MinEntropy)
		{
			int32 SwapToIndex = 0;
			for (int32 index = 0; index < RemainingTiles.Num(); index++)
			{
				if (index == 0)
				{
					MinEntropy = Tiles[RemainingTiles[index]].ShannonEntropy;
				}
				else
				{
					if (Tiles[RemainingTiles[index]].ShannonEntropy < MinEntropy)
					{
						SwapToIndex = 0;
						MinEntropy = Tiles[RemainingTiles[index]].ShannonEntropy;
						RemainingTiles.Swap(SwapToIndex, index);
					}
					else if (Tiles[RemainingTiles[index]].ShannonEntropy == MinEntropy)
					{
						SwapToIndex += 1;
						RemainingTiles.Swap(SwapToIndex, index);
					}
				}
			}
		}

		// Add Adjacent Tile Indices to Queue
		FiexibleAddAdjacentIndicesToQueue(MaxEntropyIndex, RemainingTiles, ObservationQueue);

		// Continue To Propagation
		return true;
	}
	else
	{
		// Do Not Continue to Propagation
		return false;
	}
}

bool UFiexibleWaveFunctionCollapseSubsystem::FiexibleObservationPropagation(TArray<FWaveFunctionCollapseTile>& Tiles, 
	TArray<int32>& RemainingTiles,
	TMap<int32, FWaveFunctionCollapseQueueElement>& ObservationQueue,
	int32 RandomSeed)
{
	int32 PropagationCount = 1;
	int32 MutatedRandomSeed = RandomSeed;
	
	while (FiexibleObserve(Tiles, RemainingTiles, ObservationQueue, MutatedRandomSeed))
	{
		if (!Propagate(Tiles, RemainingTiles, ObservationQueue, PropagationCount))
		{
			return false;
		}

		// Mutate Seed
		MutatedRandomSeed--;
	}

	// Check if all tiles in the solve are non-spawnable
	return !FiexibleAreAllTilesNonSpawnable(Tiles);
}

bool UFiexibleWaveFunctionCollapseSubsystem::FiexibleObservationPropagationSingle(TArray<FWaveFunctionCollapseTile>& Tiles, 
	TArray<int32>& RemainingTiles,
	TMap<int32, FWaveFunctionCollapseQueueElement>& ObservationQueue,
	int32 RandomSeed)
{
	int32 PropagationCount = 1;
	int32 MutatedRandomSeed = RandomSeed;
	
	FiexibleObserve(Tiles, RemainingTiles, ObservationQueue, MutatedRandomSeed);
	
	if (!Propagate(Tiles, RemainingTiles, ObservationQueue, PropagationCount))
	{
		return false;
	}
	// Check if all tiles in the solve are non-spawnable
	return !FiexibleAreAllTilesNonSpawnable(Tiles);
}

void UFiexibleWaveFunctionCollapseSubsystem::FiexibleAddAdjacentIndicesToQueue(int32 CenterIndex, const TArray<int32>& RemainingTiles, TMap<int32,FWaveFunctionCollapseQueueElement>& OutQueue)
{
	FIntVector Position = UWaveFunctionCollapseBPLibrary::IndexAsPosition(CenterIndex, Resolution);
	if (Position.X + 1 < Resolution.X && RemainingTiles.Contains(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(1, 0, 0), Resolution)))
	{ 
		OutQueue.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(1, 0, 0), Resolution),
			FWaveFunctionCollapseQueueElement(CenterIndex, EWaveFunctionCollapseAdjacency::Front));
	}
	if (Position.X - 1 >= 0 && RemainingTiles.Contains(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(-1, 0, 0), Resolution)))
	{ 
		OutQueue.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(-1, 0, 0), Resolution),
			FWaveFunctionCollapseQueueElement(CenterIndex, EWaveFunctionCollapseAdjacency::Back));
	}
	if (Position.Y + 1 < Resolution.Y && RemainingTiles.Contains(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, 1, 0), Resolution)))
	{ 
		OutQueue.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, 1, 0), Resolution),
			FWaveFunctionCollapseQueueElement(CenterIndex, EWaveFunctionCollapseAdjacency::Right));
	}
	if (Position.Y - 1 >= 0 && RemainingTiles.Contains(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, -1, 0), Resolution)))
	{ 
		OutQueue.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, -1, 0), Resolution),
			FWaveFunctionCollapseQueueElement(CenterIndex, EWaveFunctionCollapseAdjacency::Left));
	}
	if (Position.Z + 1 < Resolution.Z && RemainingTiles.Contains(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, 0, 1), Resolution)))
	{ 
		OutQueue.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, 0, 1), Resolution),
			FWaveFunctionCollapseQueueElement(CenterIndex, EWaveFunctionCollapseAdjacency::Up));
	}
	if (Position.Z - 1 >= 0 && RemainingTiles.Contains(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, 0, -1), Resolution)))
	{ 
		OutQueue.Add(UWaveFunctionCollapseBPLibrary::PositionAsIndex(Position + FIntVector(0, 0, -1), Resolution),
			FWaveFunctionCollapseQueueElement(CenterIndex, EWaveFunctionCollapseAdjacency::Down));
	}
}

bool UFiexibleWaveFunctionCollapseSubsystem::FiexibleAreAllTilesNonSpawnable(const TArray<FWaveFunctionCollapseTile>& Tiles)
{
	bool bAllTilesAreNonSpawnable = true;
	for (int32 index = 0; index < Tiles.Num(); index++)
	{
		if (Tiles[index].RemainingOptions.Num() == 1)
		{
			FSoftObjectPath BaseObject = Tiles[index].RemainingOptions[0].BaseObject;
			if (!(BaseObject == FWaveFunctionCollapseOption::EmptyOption.BaseObject
				|| BaseObject == FWaveFunctionCollapseOption::VoidOption.BaseObject
				|| WFCModel->SpawnExclusion.Contains(BaseObject)))
			{
				bAllTilesAreNonSpawnable = false;
				break;
			}
		}
	}
	return bAllTilesAreNonSpawnable;
}

float UFiexibleWaveFunctionCollapseSubsystem::CalculateShannonEntropy(const TArray<FWaveFunctionCollapseOption>& Options)
{
	if (Options.IsEmpty())
	{
		UE_LOG(LogWFC, Display, TEXT("Cannot calculate shannon entropy because the options are empty."));
		return -1;
	}

	float SumWeights = 0;
	float SumWeightXLogWeight = 0;
	for (const FWaveFunctionCollapseOption& Option : Options)
	{
		if (WFCModel->Constraints.Contains(Option))
		{
			//const float& Weight = WFCModel->Constraints.FindRef(Option).Weight;
			float Weight = 1;
			SumWeights += Weight;
			SumWeightXLogWeight += (Weight * log(Weight));
		}
	}

	if (SumWeights == 0)
	{
		UE_LOG(LogWFC, Display, TEXT("Cannot calculate shannon entropy because the sum of weights equals zero."));
		return -1;
	}

	return log(SumWeights) - (SumWeightXLogWeight / SumWeights);
}