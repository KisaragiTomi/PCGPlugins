#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ComputeShaderShallowWater.h"
#include "CSShallowWaterProcess.generated.h"

UCLASS()
class PCGEDITORPROCESS_API UCSShallowWaterProcess : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "ComputeShader|ShallowWater")
	static void SaveSWData(ACSShallowWaterCapture* InCSSWActor);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|ShallowWater")
	static ACSShallowWaterCapture* CSSW_GetSelectActor();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|ShallowWater")
	static void MaterialTest(ACSShallowWaterCapture* InCSSWActor);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|ShallowWater|Debug")
	static void DebugDumpSWPassResults(ACSShallowWaterCapture* InCSSWActor);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|ShallowWater", meta = (ExpandBoolAsExecs = "ReturnValue"))
	static bool StartSWSolver(ACSShallowWaterCapture*& OutCSSWActor,
		int32 Iteration = 1, float TimerRate = 0.0f);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|ShallowWater")
	static void StopSWSolver(ACSShallowWaterCapture* InCSSWActor);
};
