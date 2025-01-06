// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "PolyLine.generated.h"



/**
 * 
 */
UCLASS()
class GEOMETRYSCRIPTEXTRA_API UPolyLine : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	
	UFUNCTION(BlueprintCallable, Category = PolyLine)
	static FGeometryScriptPolyPath SmoothLine(FGeometryScriptPolyPath PolyPath, int NumIterations);

	UFUNCTION(BlueprintPure, Category = PolyLine)
	static FGeometryScriptPolyPath ResamppleByCount(FGeometryScriptPolyPath PolyPath, int32 NumIterations = 50);
	
	UFUNCTION(BlueprintPure, Category = PolyLine)
	static FGeometryScriptPolyPath ResamppleByLength(FGeometryScriptPolyPath PolyPath, float Interval = 50);

	UFUNCTION(BlueprintPure, Category = PolyLine)
	static TArray<FTransform> ConvertPolyPathToTransforms(FGeometryScriptPolyPath PolyPath, bool GenerateRotator);

	UFUNCTION(BlueprintPure, Category = PolyLine)
	static TArray<float> CurveU(FGeometryScriptPolyPath PolyPath, bool Normalize);

	// UFUNCTION(BlueprintCallable, Category = PolyLine , meta=(ScriptMethod))
	// static void ArcLength(FGeometryScriptPolyPath PolyPath, bool bClosed);
	
};
