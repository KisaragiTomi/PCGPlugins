// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DynamicMeshEditor.h"
#include "UDynamicMesh.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

class UStaticMesh;
class FSceneView;

#include "GeometryGeneral.generated.h"

/**
 * 
 */

struct FDynamicMeshComponentData
{
public:
	int Class = -1;
	int ParentClass= -1;
			
	TArray<int> TIDs;
	TArray<FVector> VPoss;

	bool IsValid = true;
};

// struct FComponentMergeOptions
// {
// 	TFunction<bool(int32)> FindConditionLambda;
// 	virtual bool MergeQ();
// };

struct FDynamicMeshComponentReduceData
{
public:
	float Count = 0;
	float MaxDist = -TNumericLimits<float>::Max();
	float MinDist = TNumericLimits<float>::Max();
	int ClassNum = -1;
	int ParentClass = -1;

	virtual bool  CollectedData(FDynamicMesh3& EditMesh, int TID, float Dist) = 0;
};

struct FWindTreeReduceData :public FDynamicMeshComponentReduceData
{
public:

	virtual bool  CollectedData(FDynamicMesh3& EditMesh, int TID, float InDist)
	{
		MaxDist = fmaxf(MaxDist, InDist);
		Count += 1;
		return true;
	}
};

struct FWindTreeCombineLeafData :public FDynamicMeshComponentReduceData
{
public:

	virtual bool  CollectedData(FDynamicMesh3& EditMesh, int TID, float InDist)
	{
		if (InDist < MinDist)
		{
			MinDist = fmin(MinDist, InDist);
			Count += 1;
			return true;
		}
		return false;
	}
};

using namespace UE::Geometry;

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UGeometryGeneral : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration = 5, bool RecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormals(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormalFromOverlay(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FillLine(UDynamicMesh* TargetMesh, TArray<FVector> VertexLoop);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* PrimNormal(UDynamicMesh* TargetMesh, FVector TestPos, FVector& OutVector);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static void WindDataForTree(UDynamicMesh* TargetMesh, TArray<FLinearColor>& PivotIndexData, TArray<FLinearColor>& DirExtentData, int32& tXx, int32
	                            & tXy, TArray<FVector>& OutHolePositions, TMap<int, FVector>& DebugClassNum, int LeafMaterialIndex = -1, float
	                            CombineDistThreshold = 50, float FindParentThreshold = 5, bool OutDebugColor = false);
	

	static void TreeWindMergeComponents(FDynamicMesh3& EditMesh, FGeometryScriptDynamicMeshBVH BVH, TMap<int, FDynamicMeshComponentData> ComponentDatas);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* WeldDynamicMesh(UDynamicMesh* TargetMesh, float Tolerance = 0.001);
	
	static void WeldVertices(FDynamicMesh3& EditMesh, float Tolerance);


	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* ColorAttribTransf(UDynamicMesh* SourceMesh, UDynamicMesh* TargetMesh, bool& Success, FLinearColor ChannelMask = FLinearColor(1, 1, 1, 1));
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* UVAttribTransf(UDynamicMesh* SourceMesh, UDynamicMesh* TargetMesh, bool& Success, int UVNum = 1, FLinearColor ChannelMask = FLinearColor(1, 1, 1, 1));
	
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FillUVData(UDynamicMesh* TargetMesh, int32 UVLayerNum);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* AddCustomAttribute(UDynamicMesh* TargetMesh, FName AttributeName);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static void CalculateOBBUpDir(TArray<FVector> OrientSpaceVertices, FVector& BoxUpDir);
	
	static FVector GetNearestLocationNormal(FDynamicMesh3& EditMesh, FGeometryScriptTrianglePoint NearestPoint);

	template<typename ReduceDatatype, typename ComponentType>
	static TMap<int, ReduceDatatype> FindNearestComponents(FDynamicMesh3& EditMesh, FGeometryScriptDynamicMeshBVH BVH, TMap<int, ComponentType> ComponentDatas);

	
	
	
	static void AppendPrimitive(
	UDynamicMesh* TargetMesh,
	FMeshShapeGenerator* Generator, 
	FTransform Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions,
	FVector3d PreTranslate = FVector3d::Zero(),
	TOptional<FQuaterniond> PreRotate = TOptional<FQuaterniond>());

	UFUNCTION(BlueprintCallable, Category = "DistanceField")
	static bool HasMeshDistanceField(UStaticMesh* StaticMesh);

	UFUNCTION(BlueprintCallable, Category = "DistanceField")
	static FBox GetMeshDistanceFieldBounds(UStaticMesh* StaticMesh);
};