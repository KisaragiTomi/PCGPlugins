#include "General.h"

#if WITH_EDITOR
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "LevelEditorViewport.h"
#endif

FTimerHandle UGeneralBPLibrary::StartSolverTimer(UObject* WorldContextObject, FTimerDynamicDelegate Event, float Time, bool bLooping)
{
	FTimerHandle Handle;
	if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull))
	{
		World->GetTimerManager().SetTimer(Handle, Event, Time, bLooping);
	}
	return Handle;
}

void UGeneralBPLibrary::StopSolverTimer(UObject* WorldContextObject, FTimerHandle& Handle)
{
	if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull))
	{
		World->GetTimerManager().ClearTimer(Handle);
		Handle.Invalidate();
	}
}

int32 UGeneralBPLibrary::GoToLocationAndLoadDataLayers(FVector Location, float RadiusCm)
{
#if WITH_EDITOR
	UUnrealEditorSubsystem* EditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>() : nullptr;
	if (EditorSubsystem)
	{
		EditorSubsystem->SetLevelViewportCameraInfo(Location, FRotator::ZeroRotator);
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return 0;

	UDataLayerEditorSubsystem* DLES = UDataLayerEditorSubsystem::Get();
	if (!DLES) return 0;

	const float RadiusSq = RadiusCm * RadiusCm;
	TArray<UDataLayerInstance*> AllLayers = DLES->GetAllDataLayers();
	TArray<UDataLayerInstance*> LayersToLoad;

	for (UDataLayerInstance* DL : AllLayers)
	{
		if (!DL) continue;

		TArray<AActor*> Actors = DLES->GetActorsFromDataLayer(DL);
		for (AActor* Actor : Actors)
		{
			if (!Actor) continue;
			const float DistSq = FVector::DistSquared(Actor->GetActorLocation(), Location);
			if (DistSq <= RadiusSq)
			{
				LayersToLoad.AddUnique(DL);
				break;
			}
		}
	}

	if (LayersToLoad.Num() == 0) return 0;

	DLES->SetDataLayersIsLoadedInEditor(LayersToLoad, true, true);
	DLES->SetDataLayersVisibility(LayersToLoad, true);
	DLES->UpdateAllActorsVisibility(true, true);

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	return LayersToLoad.Num();
#else
	return 0;
#endif
}
