#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "General.generated.h"

UCLASS()
class PCGEDITORPROCESS_API UGeneralBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "General|Timer", meta = (WorldContext = "WorldContextObject"))
	static FTimerHandle StartSolverTimer(UObject* WorldContextObject, FTimerDynamicDelegate Event, float Time = 0.01f, bool bLooping = true);

	UFUNCTION(BlueprintCallable, Category = "General|Timer", meta = (WorldContext = "WorldContextObject"))
	static void StopSolverTimer(UObject* WorldContextObject, UPARAM(ref) FTimerHandle& Handle);

	/** Move the editor viewport camera to Location and load nearby Data Layers within RadiusCm.
	 *  Returns the number of Data Layers made visible. Editor-only. */
	UFUNCTION(BlueprintCallable, Category = "General|Editor", meta = (CallInEditor = "true"))
	static int32 GoToLocationAndLoadDataLayers(FVector Location, float RadiusCm = 50000.0f);
};
