#include "General.h"

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
