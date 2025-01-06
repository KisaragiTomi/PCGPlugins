// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Noise.generated.h"




/**
 * 
 */
UCLASS()
class GEOMETRYMATH_API UNoise : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	
	UFUNCTION(BlueprintCallable, Category = Noise)
	static FVector CurlNoise(FVector Pos, FVector& Out_AddedPos, FVector Offset = FVector(0, 0, 0), float Strength = 1, float Frequency = 1);
	
	UFUNCTION(BlueprintCallable, Category = Noise)
	static FVector PerlinNoise3D(FVector Pos, FVector& Out_AddedPos, FVector Offset = FVector(0, 0, 0), float Strength = 1, float Frequency = 1, int32 RandomSeed = 0);


};
