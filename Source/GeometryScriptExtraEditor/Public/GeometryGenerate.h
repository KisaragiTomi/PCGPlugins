// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "UDynamicMesh.h"
#include "ComputeShaderMeshGenerator.h"

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

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UGeometryGenerate : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VDBMeshFromActors(TArray<AActor*> In_Actors, TArray<FVector> BBoxVertors, bool Result, int32 ExtentPlus = 3, float VoxelSize = 10, float LandscapeMeshExtrude = 50,  bool
	                                       MultThread = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VDBMeshFromActorPoints(TArray<AActor*> In_Actors, TArray<FVector> BBoxVertors, bool Result, int32 ExtentPlus = 3,
	                                            float VoxelSize = 10, float LandscapeMeshExtrude = 50, float PointSpacing = 0,
	                                            float PointRadiusMult = 2, int32 MaxPointsPerComponent = 20000);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* VDBMeshFromSurfaceVoxels(TArray<AActor*> In_Actors, TArray<FVector> ValidPositions,
	                                              float VoxelSize = 10, float SurfaceDistance = 0,
	                                              float PointRadiusMult = 2, bool bProjectToSurface = true,
	                                              float InclusionDistance = 50.0f);

	UFUNCTION(BlueprintCallable, Category = "Generate|ComputeShader")
	static UDynamicMesh* CSTriangleDataToDynamicMesh(FCSTriangleMeshData CSTriangleData,
		bool bReverseOrientation = false,
		bool bSkipDegenerateTriangles = true,
		bool bRecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = "Generate|ComputeShader")
	static UDynamicMesh* CSTriangleBuffersToDynamicMesh(TArray<FVector> Vertices,
		TArray<int32> Indices,
		TArray<FVector> VertexNormals,
		int32 VertexCount = -1,
		int32 IndexCount = -1,
		bool bReverseOrientation = false,
		bool bSkipDegenerateTriangles = true,
		bool bRecomputeNormals = true);

	static UDynamicMesh* CSTriangleReadbackToDynamicMesh(const TArray<FVector4f>& CompactVertices,
		const TArray<uint32>& CompactIndices,
		const TArray<FVector4f>& CompactVertexNormals,
		int32 VertexCount = -1,
		int32 IndexCount = -1,
		bool bReverseOrientation = false,
		bool bSkipDegenerateTriangles = true,
		bool bRecomputeNormals = true);

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

	/** 将 ComputeShaderMeshGenerator 的 GPU surface voxels 通过 OpenVDB ParticlesToLevelSet 转为 mesh。
	 *  输出为世界空间坐标。 */
	UFUNCTION(BlueprintCallable, Category = "Generate|ComputeShader")
	static UDynamicMesh* SurfaceVoxelsToVDBMesh(AComputeShaderMeshGenerator* Generator,
		float VoxelSize = 10.0f,
		float RadiusMult = 2.0f,
		bool bRecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = "Generate|ComputeShader")
	static UDynamicMesh* VDBVoxelsToOpenDynamicMesh(FCSSurfaceVoxelData SurfaceVoxels,
		float VoxelSize = 0.0f,
		float RadiusMult = 2.0f,
		bool bRecomputeNormals = true);

};





//
// UCLASS()
// class GEOMETRYSCRIPTEXTRAEDITOR_API AMyClass : public AActor
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
