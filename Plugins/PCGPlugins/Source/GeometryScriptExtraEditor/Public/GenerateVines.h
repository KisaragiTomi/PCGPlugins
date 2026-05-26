// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryGenerate.h"
#include "GenerateVines.generated.h"

USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTEXTRAEDITOR_API FSpaceColonizationAttribute
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	bool Attractor = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	bool End = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	bool Startpt = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	float CurveU = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 SpawnCount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 Startid = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 PrePt = -1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 NextPt = -1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 Infaction = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 BranchCount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	int32 BackCount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	FVector N = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Colonization")
	TArray<int32> Associates;

	

	// FORCEINLINE bool operator==(const FMeshData& Other) const
	// {
	// 	return (Mesh == Other.Mesh) && (UKismetMathLibrary::EqualEqual_TransformTransform(Transform, Other.Transform)) && (Count == Other.Count);
	// }
};

/**
 *
 */
UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UGenerateVines : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Generate)
	static void GenerateVines_L(AVineContainer* Container, FSpaceColonizationOptions SC, float ExtrudeScale = 50, bool Result = true, bool OutDebugMesh = false, bool MultThread
		                          = false);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static void BuildSpaceColonizationQueue(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms,
		int32 Iterations, int32 Activetime, float RandGrow, float Seed, bool MultThread,
		TArray<FVector>& OutTargetLocations, TArray<FSpaceColonizationAttribute>& OutSCAttributes);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static bool BuildSpaceColonizationQueueCS(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms,
		int32 Iterations, int32 Activetime, float RandGrow, float Seed, float InfluenceRadius,
		TArray<FVector>& OutTargetLocations, TArray<FSpaceColonizationAttribute>& OutSCAttributes);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static TArray<FGeometryScriptPolyPath> SpaceColonization(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations =
											   50, int32 Activetime = 20,  int32 BackGrowCount = 8, float Ranggrow = 0.5, float Seed = 0.2, float BackGrowRange = 0.8, bool MultThread = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static TArray<FGeometryScriptPolyPath> SpaceColonizationCS(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations =
											   50, int32 Activetime = 20, int32 BackGrowCount = 8, float Ranggrow = 0.5, float Seed = 0.2, float BackGrowRange = 0.8, float InfluenceRadius = 200.0f);
};
