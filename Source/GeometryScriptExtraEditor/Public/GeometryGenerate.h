// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "UDynamicMesh.h"
#include "ComputeShaderMeshGenerator.h"

#include "GeometryGenerate.generated.h"

class UCSMesh;

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

	// CS 三角形数据的出口一律是 UCSMesh。要 UDynamicMesh 的调用方接一个
	// UCSMeshOps::CopyToDynamicMesh —— GPU 管线只留一条出桥。
	// TargetMesh 传 null 会在 Outer 下新建一个（Outer 也为 null 则挂到 transient package）。
	UFUNCTION(BlueprintCallable, Category = "Generate|ComputeShader", meta = (DefaultToSelf = "Outer"))
	static UPARAM(DisplayName = "Target") UCSMesh* CSTriangleDataToGpuMesh(UCSMesh* TargetMesh,
		UObject* Outer,
		FCSTriangleMeshData CSTriangleData,
		bool bReverseOrientation = false,
		bool bSkipDegenerateTriangles = true,
		bool bRecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = "Generate|ComputeShader", meta = (DefaultToSelf = "Outer"))
	static UPARAM(DisplayName = "Target") UCSMesh* CSTriangleBuffersToGpuMesh(UCSMesh* TargetMesh,
		UObject* Outer,
		TArray<FVector> Vertices,
		TArray<int32> Indices,
		TArray<FVector> VertexNormals,
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

	/** 把一批已经读回 CPU 的 GPU surface voxels 通过 OpenVDB ParticlesToLevelSet 转成 mesh。
	 *  输出为世界空间坐标。
	 *
	 *  实现落在 AComputeShaderMeshGenerator::VDBParticlesToGpuMesh：这里和 generator 自己的
	 *  SurfaceVoxelsToVDBGpuMesh 只差"体素从哪来"，各写一份 OpenVDB 设置正是两份实现当初
	 *  走岔的原因。要 generator 直接跑这条路的调用方直接调它的 SurfaceVoxelsToVDBGpuMesh。 */
	UFUNCTION(BlueprintCallable, Category = "Generate|ComputeShader", meta = (DefaultToSelf = "Outer"))
	static UPARAM(DisplayName = "Target") UCSMesh* VDBVoxelsToOpenGpuMesh(UCSMesh* TargetMesh,
		UObject* Outer,
		FCSSurfaceVoxelData SurfaceVoxels,
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
