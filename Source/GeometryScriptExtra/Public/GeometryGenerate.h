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
namespace ProcessAsync
{
	template<typename T>
	TArray<T> ProcessAsync(int32 NumPt, int32 ThreadPointNum, TFunction<T(int32)> Func)
	{
		int32 NumThreads = FMath::Min(NumPt / ThreadPointNum, FPlatformMisc::NumberOfCoresIncludingHyperthreads() - 1LL);
		TArray<TFuture<TArray<T>>> Threads;
		Threads.Reserve(NumThreads);
		const int32 Batch = NumPt / NumThreads;
		for (int32 t = 0; t < NumThreads; ++t)
		{
			const int64 StartIdx = Batch * t;
			const int64 EndIdx = t == NumThreads - 1 ? NumPt : StartIdx + Batch;
			Threads.Emplace(Async(EAsyncExecution::TaskGraph, [StartIdx, EndIdx, Func]
			{
				TArray<T> ResultsPerTask;
				ResultsPerTask.Reserve(EndIdx - StartIdx);
				for (int64 p = StartIdx; p < EndIdx; ++p)
				{
					T Results = Func(p);
					ResultsPerTask.Add(Results);
				}
				return ResultsPerTask;
			}));
			
			float Time = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Log, TEXT("The float value is: %f"), Time);
		}
		TArray<T> Results;
		Results.Reserve(NumThreads);
		for (const TFuture<TArray<T>>& ThreadResult : Threads)
		{
			ThreadResult.Wait();
			TArray<T> ResultsPerTask =  ThreadResult.Get();
			Results.Append(ResultsPerTask);
		}
		return Results;
	}
}

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
	static void GenerateVines(FSpaceColonizationOptions SC, bool Result = true, bool OutDebugMesh = false, bool MultThread = false);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VDBMeshFromActors(TArray<AActor*> In_Actors, FBox Bounds, bool Result, int32 ExtentPlus = 3, float VoxelSize = 10, bool MultThread = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FixUnclosedBoundary(UDynamicMesh* FixMesh, float ProjectOffset = 100, bool ProjectToLandscape = true, bool AppendMesh = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VoxelMergeMeshs(UDynamicMesh* TargetMesh, float VoxelSize = 10);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration = 5, bool RecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormals(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormalFromOverlay(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UStaticMesh* CreateStaticMeshAsset(UDynamicMesh* TargetMesh, FString AssetPathAndName, TArray<UMaterialInterface*> Materials);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* ExtrudeUnclosedBoundary(UDynamicMesh* FixMesh, float Offset = 100, bool AppendMesh = true);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FillLine(UDynamicMesh* TargetMesh, TArray<FVector> VertexLoop);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static FVector TestViewPosition();

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
