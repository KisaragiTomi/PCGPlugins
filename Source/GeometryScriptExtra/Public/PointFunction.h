// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "PointFunction.generated.h"



/**
 * 
 */
UCLASS()
class GEOMETRYSCRIPTEXTRA_API UPointFunction : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	
	static int32 FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation);

	static int32 FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation, TFunction<bool(int32)> Func);

};
