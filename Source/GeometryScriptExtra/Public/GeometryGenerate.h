// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "LandscapeComponent.h"
#include "UDynamicMesh.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "GeometryEditorActor.h"
#include "Curves/CurveLinearColor.h"

#include "GeometryGenerate.generated.h"


/**
 * 
 */
DECLARE_STATS_GROUP(TEXT("TestTime"), STATGROUP_TestTime, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("SCTime"), STAT_SpaceColonization, STATGROUP_TestTime);
DECLARE_CYCLE_STAT(TEXT("SCTimeMultThread"), STAT_SpaceColonizationMultThread, STATGROUP_TestTime);

DECLARE_CYCLE_STAT(TEXT("SCConverteMesh"), STAT_SCConvertMesh, STATGROUP_TestTime);
DECLARE_CYCLE_STAT(TEXT("SCConverteMeshMultThread"), STAT_SCConvertMeshMultThread, STATGROUP_TestTime);


UENUM(BlueprintType)
enum EOutMeshType : int
{

	CTF_OutResult UMETA(DisplayName="Result"),

	CTF_OutSceneMeshs UMETA(DisplayName = "SceneMeshs"),

	CTF_VDBMeshs UMETA(DisplayName = "VDBMeshs"),
};

USTRUCT(BlueprintType, meta = (DisplayName = "SC Options"))
struct GEOMETRYSCRIPTEXTRA_API FSpaceColonizationOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Iteration = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Activetime = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 ExtentPlus = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float RandGrow = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float Seed = .5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float BackGrowRange = .8;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float VoxelSize = 10;
};

UCLASS()
class GEOMETRYSCRIPTEXTRA_API UGeometryGenerate : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VDBMeshFromActors(TArray<AActor*> In_Actors, TArray<FVector> BBoxVertors, bool Result, int32 ExtentPlus = 3, float VoxelSize = 10, float LandscapeMeshExtrude = 50,  bool
	                                       MultThread = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FixUnclosedBoundary(UDynamicMesh* FixMesh, float ProjectOffset = 100, bool ProjectToLandscape = true, bool AppendMesh = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VoxelMergeMeshs(UDynamicMesh* TargetMesh, float VoxelSize = 10);
	


	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* ExtrudeUnclosedBoundary(UDynamicMesh* FixMesh, float Offset = 100, bool AppendMesh = true);
	

	static UDynamicMesh* CollectMeshsMultThread(UDynamicMesh* TargetMesh, TArray<UStaticMesh*> BoundTransformMapKeyArray, TArray<TArray<FTransform>>
	                                            BoundTransformMapValueArray, FBox Bounds, float MeshExtrude, float VoxelSize = 10);
	

	static UDynamicMesh* CollectMeshs(UDynamicMesh* TargetMesh, TArray<UStaticMesh*> BoundTransformMapKeyArray, TArray<TArray<FTransform>>
	                                  BoundTransformMapValueArray, FBox Bounds, float MeshExtrude);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static FVector TestViewPosition();


};





//
// UCLASS()
// class GEOMETRYSCRIPTEXTRA_API AMyClass : public AActor
// {
// 	GENERATED_BODY()
//
// public:
// 	// Sets default values for this actor's properties
// 	AMyClass();
//
// protected:
// 	// Called when the game starts or when spawned
// 	virtual void BeginPlay() override;
//
// public:
// 	// Called every frame
// 	virtual void Tick(float DeltaTime) override;
// };
