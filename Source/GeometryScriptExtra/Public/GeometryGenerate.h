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
	static void GenerateVines(FSpaceColonizationOptions SC, bool Result, bool OutDebugMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VDBMeshFromActors(TArray<AActor*> In_Actors, FBox Bounds, bool Result, int32 ExtentPlus = 3, float VoxelSize = 10);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FixUnClosedBoundary(UDynamicMesh* FixMesh, bool ProjectToLandscape = true, bool AppendMesh = true, float ProjectOffset = 50);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VoxelMergeMeshs(UDynamicMesh* TargetMesh, float VoxelSize = 10);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration = 5, bool RecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UStaticMesh* CreateStaticMeshAsset(UDynamicMesh* TargetMesh, FString AssetPathAndName, TArray<UMaterialInterface*> Materials);
	


	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static FVector Test();
	// UFUNCTION(BlueprintCallable, Category = Generate)
	// static UDynamicMesh* SpaceColonizationMesh(UDynamicMesh* TargetMesh, TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations =
	// 									   50, int32 Activetime = 20, float Ranggrow = 0.5, float Seed = 0.2);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static TArray<FGeometryScriptPolyPath> SpaceColonization(TArray<FTransform> SourceTransforms, TArray<FTransform> TargetTransforms, int32 Iterations =
		                                       50, int32 Activetime = 20,  int32 BackGrowCount = 8, float Ranggrow = 0.5, float Seed = 0.2, float BackGrowRange = 0.8);
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
