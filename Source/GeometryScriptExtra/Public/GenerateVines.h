// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryGenerate.h"
#include "GenerateVines.generated.h"

/**
 * 
 */
UCLASS()
class GEOMETRYSCRIPTEXTRA_API UGenerateVines : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Generate)
	static void GenerateVines(AVineContainer* Container, FSpaceColonizationOptions SC, float ExtrudeScale = 50, bool Result = true, bool OutDebugMesh = false, bool MultThread
		                          = false);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static TArray<FGeometryScriptPolyPath> SpaceColonization(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations =
											   50, int32 Activetime = 20,  int32 BackGrowCount = 8, float Ranggrow = 0.5, float Seed = 0.2, float BackGrowRange = 0.8, bool MultThread = true);
};

USTRUCT()
struct FSpaceColonizationAttribute
{
	GENERATED_USTRUCT_BODY()
public:
	UPROPERTY()
	bool Attractor = true;
	UPROPERTY()
	bool End = false;
	UPROPERTY()
	bool Startpt = false;
	UPROPERTY()
	float CurveU = 0;
	UPROPERTY()
	int32 SpawnCount = 0;
	UPROPERTY()
	int32 Startid = 0;
	UPROPERTY()
	int32 PrePt = -1;
	UPROPERTY()
	int32 NextPt = -1;
	UPROPERTY()
	int32 Infaction = 0;
	UPROPERTY()
	int32 BranchCount = 0;
	UPROPERTY()
	int32 BackCount = 0;
	UPROPERTY()
	FVector N = FVector::ZeroVector;
	UPROPERTY()
	TArray<int32> Associates;

	

	// FORCEINLINE bool operator==(const FMeshData& Other) const
	// {
	// 	return (Mesh == Other.Mesh) && (UKismetMathLibrary::EqualEqual_TransformTransform(Transform, Other.Transform)) && (Count == Other.Count);
	// }
};
