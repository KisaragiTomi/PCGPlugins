// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "TATools_Async.h"
#include "GroupTopology.h"
#include "DynamicMeshToMeshDescription.h"
#include "CoreMinimal.h"
//#include "MeshOpPreviewHelpers.h"
#include "MeshRegionBoundaryLoops.h"
#include "PolygonProcess.generated.h"


class AStaticMeshActor;
class ATransformManager;

using namespace UE::Geometry;
/**
 * 
 */
UCLASS()
class TATOOLSPLUGIN_API UPolygonProcess : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "TopologyAPI")
	static void GetHoleVerticesPosition(UStaticMesh* InMesh, int32 LODIndex, int32 SectionIndex, TArray<FVector>& InVertices);

	UFUNCTION(BlueprintCallable, Category = "TopologyAPI")
	static void CalculateRoatation(TArray<UStaticMesh*> StaticMeshs, bool DebugBox);
	
	UFUNCTION(BlueprintCallable, Category = "TopologyAPI")
	static void FixOpenAssetAsync(TArray<UStaticMesh*> StaticMeshs, int32 NumberOfCheck = 1, bool DebugBox = false, bool OpenVertexDir = false,  bool MultiThread = true);
	
	UFUNCTION(BlueprintCallable, Category = "TopologyAPI")
	static void StopFixOpenAssetAsync(TArray<UStaticMesh*> StaticMeshs);



};

class TATOOLSPLUGIN_API OpenAssetProcess : public AsyncAble
{
public:
	ATransformManager* TransformManager;
	AStaticMeshActor* StaticActor;
	UStaticMesh* StaticMesh;
	TArray<AActor*> IgOutActors;
	TArray<FVector> OrientSpaceVertices;
	FVector RotateDir = FVector::ZeroVector;
	FVector UpVector = FVector(0, 0, 1);
	FVector BoxUpDir = FVector::ZeroVector;
	FVector OutCenter = FVector::ZeroVector;
	FVector OutExtent = FVector::ZeroVector;
	FRotator OutRotator = FRotator::ZeroRotator;
	FTransform Transform;
	FTransform FixedTransform;
	int32 NumberOfCheck = 1;
	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes{ UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldDynamic) };
	TArray<FVector> Vertices;
	
	float Zoffset = 10;
	float Angle = 0.05;
	
	bool DebugBox = false;

	virtual void CalculateResult() override;
	virtual void DoCalculate() override;
	void CalculateEigenVector();
	void CalculateOpenAssetTransform();
	void CalculateSafeTransform();
	void CalculateOpenVerticesDir();

};

UCLASS()
class TATOOLSPLUGIN_API ATransformManager : public AActor
{
	GENERATED_BODY()
	ATransformManager() {}

public:
	TMap<AStaticMeshActor*, FTransform> TransformMap;
	TMap<UStaticMesh*, TArray<FVector>> MeshOpenVertex;
	TArray<AStaticMeshActor*> FixActors;
	TArray<FTransform> FixTransfoms;

};


//因为获取边缘点的方法是被保护的.我只能继承FGroupTopology来做事情
class FBasicTopologyFindPosition : public FGroupTopology
{
public:
	FBasicTopologyFindPosition(const FDynamicMesh3* Mesh, bool bAutoBuild) :
		FGroupTopology(Mesh, bAutoBuild)
	{}

	int GetGroupID(int TriangleID) const override
	{
		return Mesh->IsTriangle(TriangleID) ? 1 : 0;
	}

	void GetVerticesPosition(TArray<FVector>& InVertices)
	{
		Groups.Reset();

		int32 MaxGroupID = 0;
		for (int32 tid : Mesh->TriangleIndicesItr())
		{
			MaxGroupID = FMath::Max(GetGroupID(tid), MaxGroupID);
		}
		MaxGroupID++;

		// initialize groups map first to avoid resizes
		GroupIDToGroupIndexMap.Reset();
		GroupIDToGroupIndexMap.Init(-1, MaxGroupID);
		TArray<int> GroupFaceCounts;
		GroupFaceCounts.Init(0, MaxGroupID);
		for (int tid : Mesh->TriangleIndicesItr())
		{
			int GroupID = FMathd::Max(0, GetGroupID(tid));
			if (GroupIDToGroupIndexMap[GroupID] == -1)
			{
				FGroup NewGroup;
				NewGroup.GroupID = GroupID;
				GroupIDToGroupIndexMap[GroupID] = Groups.Add(NewGroup);
			}
			GroupFaceCounts[GroupID]++;
		}
		for (FGroup& Group : Groups)
		{
			Group.Triangles.Reserve(GroupFaceCounts[Group.GroupID]);
		}

		// sort faces into groups
		for (int tid : Mesh->TriangleIndicesItr())
		{
			int GroupID = FMathd::Max(0, GetGroupID(tid));
			Groups[GroupIDToGroupIndexMap[GroupID]].Triangles.Add(tid);
		}

		FMeshRegionBoundaryLoops BdryLoops(Mesh, Groups[0].Triangles, true);

		int NumLoops = BdryLoops.Loops.Num();
		TArray<FVector> vertices;
		for (int li = 0; li < NumLoops; ++li)
		{
			FEdgeLoop& Loop = BdryLoops.Loops[li];
			int NumVertices = Loop.Vertices.Num();

			for (int i = 0; i < NumVertices; i++) {
				FVector3d tempvector;
				tempvector = Mesh->GetVertex(Loop.Vertices[i]);
				FVector outvector = FVector(tempvector);
				InVertices.AddUnique(outvector);
			}
		}
	}
};