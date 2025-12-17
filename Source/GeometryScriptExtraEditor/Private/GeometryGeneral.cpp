// Fill out your copyright notice in the Description page of Project Settings.


#include "GeometryGeneral.h"

#include "DynamicMeshEditor.h"
#include "GeometryAttribute.h"
#include "MeshBoundaryLoops.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/MeshTransforms.h"
#include "GeometryScript/MeshGeodesicFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshSpatialFunctions.h"
#include "Operations/SmoothDynamicMeshAttributes.h"
#include "Parameterization/DynamicMeshUVEditor.h"
#include "Selections/MeshConnectedComponents.h"
#include "tbb/parallel_reduce.h"
#include "GeometryAttribute.h"
#include "MinVolumeBox3.h"
#include "OrientedBoxTypes.h"
#include "boost/test/data/monomorphic/collection.hpp"
#include "Engine/TextureRenderTarget2D.h"
#include "openvdb/math/BBox.h"
#include "Spatial/FastWinding.h"
#include "Spatial/MeshAABBTree3.h"


using namespace UE::Geometry;

UDynamicMesh* UGeometryGeneral::BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration, bool RecomputeNormals)
{
	FGeometryScriptCalculateNormalsOptions CalculateOptions;
	if (RecomputeNormals)
	{
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(TargetMesh, CalculateOptions);
	}
	
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
	
		if (Normals->ElementCount() > 0)
		{
			FSmoothDynamicMeshAttributes BlurOp(EditMesh);
			BlurOp.bUseParallel = true;
			BlurOp.NumIterations = Iteration;
			BlurOp.Strength = .5;
			BlurOp.EdgeWeightMethod = static_cast<FSmoothDynamicMeshAttributes::EEdgeWeights>(0);
		
			TArray<bool> NormalsToSmooth;
			NormalsToSmooth.Reserve(3);
			NormalsToSmooth.Add(true);
			NormalsToSmooth.Add(true);
			NormalsToSmooth.Add(true);
		
			FGeometryScriptMeshSelection Selection;
			if (!Selection.IsEmpty())
			{
				Selection.ProcessByVertexID(EditMesh, [&](int32 VertexID)
				{
					BlurOp.Selection.Add(VertexID);
				});
			}
			
			BlurOp.SmoothOverlay(Normals, NormalsToSmooth);
		}
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	CreateVertexNormalFromOverlay(TargetMesh);
	
	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::CreateVertexNormalFromOverlay(UDynamicMesh* TargetMesh)
{
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		if (EditMesh.VertexCount() > 0)
		{
			if (EditMesh.HasVertexNormals() == false)
			{
				EditMesh.EnableVertexNormals(FVector3f::UpVector);
			}
			FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
			int32 TriCount = EditMesh.TriangleCount();
			for (int32 TriIndex = 0; TriIndex < TriCount; ++TriIndex)
			{
				FIndex3i TVidNormal =  Normals->GetTriangle(TriIndex);
				FIndex3i TVid = EditMesh.GetTriangle(TriIndex);
				for (int32 i = 0; i < 3; ++i)
				{
					if (TVidNormal[i] < 0 || !EditMesh.IsVertex(TVid[i]))
						continue;
					FVector3f PreNormal = EditMesh.GetVertexNormal(TVid[i]);
					FVector3f Normal = Normals->GetElement(TVidNormal[i]);
					EditMesh.SetVertexNormal(TVid[i], Normal);
				}
			}
		}

	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::CreateVertexNormals(UDynamicMesh* TargetMesh)
{
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		if (EditMesh.VertexCount() > 0)
		{
			if (EditMesh.HasVertexNormals() == false)
			{
				EditMesh.EnableVertexNormals(FVector3f::UpVector);
			}
			FMeshNormals Normals(&EditMesh);
			Normals.ComputeVertexNormals();
			Normals.CopyToVertexNormals(&EditMesh, false);
		}

	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
}



UDynamicMesh* UGeometryGeneral::FillLine(UDynamicMesh* TargetMesh, TArray<FVector> VertexLoop)
{
	bool Success = true;
	UDynamicMesh* BoundaryMeshCombineOut = NewObject<UDynamicMesh>();
	BoundaryMeshCombineOut->Reset();
	FDynamicMesh3 CopyMesh;
	BoundaryMeshCombineOut->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		int32 VertexCount = VertexLoop.Num();
		for (int32 i = 0; i < VertexCount; i++)
		{
			EditMesh.AppendVertex(VertexLoop[i]);
		}
		FVector3d c = FVector3d::Zero();
		for (int i = 0; i < VertexLoop.Num(); ++i)
		{
			c += VertexLoop[i];
		}
		c *= 1.0 / VertexLoop.Num();
		// add centroid vtx
		int32 NewVertex = EditMesh.AppendVertex(c);
		
		FDynamicMeshEditResult AddFanResult;
		int N = VertexLoop.Num();
		AddFanResult.NewTriangles.Reserve(N);

		int i = 0;
		for (i = 0; i < N; ++i)
		{
			int A = i;
			int B = (i + 1) % N;

			FIndex3i NewT(NewVertex, B, A);
			int NewTID = EditMesh.AppendTriangle(NewT, 0);
			if (NewTID < 0)
			{
				Success = false;
			}

			AddFanResult.NewTriangles.Add(NewTID);
		}
		CopyMesh = EditMesh;
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	if (Success)
	{
		TargetMesh->SetMesh(MoveTemp(CopyMesh));
		return TargetMesh;
	}
	return nullptr;
}

UDynamicMesh* UGeometryGeneral::PrimNormal(UDynamicMesh* TargetMesh, FVector TestPos, FVector& OutVector)
{
	FGeometryScriptDynamicMeshBVH BVH;
	UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(TargetMesh, BVH, nullptr);

	
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		FGeometryScriptSpatialQueryOptions Options;
		FGeometryScriptTrianglePoint NearestPoint;
		EGeometryScriptSearchOutcomePins Outcome;
		UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
		TargetMesh, BVH, TestPos, Options, NearestPoint, Outcome, nullptr);
		//VertexLocation = NearestPoint.Position;

		OutVector = GetNearestLocationNormal(EditMesh, NearestPoint);
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	return TargetMesh;
}

void UGeometryGeneral::WindDataForTree(UDynamicMesh* TargetMesh,
                                       TArray<FLinearColor>& PivotIndexData,
                                       
                                       TArray<FLinearColor>& DirExtentData,
                                       int32& tXx,
                                       int32& tXy,
                                       TArray<FVector>& OutHolePositions,
                                       TMap<int, FVector>& DebugClassNum,
										int LeafMaterialIndex,
                                       float CombineDistThreashould,
                                       float FindParentThreashold
)
{
	TSharedPtr<FDynamicMesh3> MeshCopy = MakeShared<FDynamicMesh3>();
	TargetMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh) { MeshCopy->Copy(EditMesh); });

	FMeshBoundaryLoops BLoops(MeshCopy.Get(), true);
	// float CombineDistThreashould = 30;
	TArray<int> UVIndexTest;
	TArray<FVector2D> UVTestIndex;
	TArray<FVector2D> UVFloat;
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		EditMesh.EnableAttributes();
		if (EditMesh.Attributes()->NumUVLayers() < 2)
		{
			EditMesh.Attributes()->SetNumUVLayers(2);
		}
		
		
		VI_ATTR(Class, EditMesh, -1);
		VI_ATTR(PreClass, EditMesh, 0);
		VI_ATTR(DiscardPoint, EditMesh, 0);
		VI_ATTR(HolePoint, EditMesh, 0);
		TI_ATTR(Class, EditMesh, -1);


		

		FMeshConnectedComponents Components(&EditMesh);
		Components.FindConnectedTriangles();
		

		struct FWindTreeComponentData : public FDynamicMeshComponentData
		{
			FVector RootCenter = FVector(0, 0, 0);
			FVector Center = FVector::ZeroVector;
			FVector RootNormal = FVector::UpVector;
			FVector RootUp = FVector::ForwardVector;
			FLinearColor DebugColor = FLinearColor::Black;
			FString Hierarchy = "";
			FString ParentHierarchy = "";
			int AddToClass = -1;
			int UVIndex = 0;
			int StepCount = 0;

			float MaxDistToParent = TNumericLimits<int>::Max();;
			float OriginExtent = 0;

			bool Checked = false;
			bool CheckedTemp = false;
			bool IsLeaf = false;

			TArray<TArray<int>> Holes;
			int VaildHole = 0;
		};


		
		int MaxStepCount = 0;
		FGeometryScriptDynamicMeshBVH BVH;
		UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(TargetMesh, BVH, nullptr);
		EditMesh.Attributes()->EnablePrimaryColors();

		TMap<int, FWindTreeComponentData> LeafComponentDatas;
		TMap<int, FWindTreeComponentData> ComponentDatas;
		for ( int c = 0; c < Components.Num(); c++)
		{

			
			FWindTreeComponentData ComponentData;
			ComponentData.TIDs = Components[c].Indices;
			ComponentData.Class = c;
			FDynamicMeshMaterialAttribute* ATI_MaterialID = EditMesh.Attributes()->GetMaterialID();
			int MaterialID = ATI_MaterialID->GetValue(ComponentData.TIDs[0]);
			bool IsLeaf = false;
			if (MaterialID == LeafMaterialIndex) IsLeaf = true;

			FVector3d CenterPos = FVector3d::Zero();
			for (int TID : ComponentData.TIDs)
			{
				FIndex3i TVIDs = EditMesh.GetTriangle(TID);
				for (int i = 0; i < 3; i++)
				{
					AVI_PreClass->SetValue(TVIDs[i], &c);
					CenterPos = CenterPos + EditMesh.GetVertex(TVIDs[i]);
				}
				ATI_Class->SetValue(TID, &c);

			}
			CenterPos /= ComponentData.TIDs.Num() * 3.0;
			ComponentData.Center = (FVector)CenterPos;
			
			if (IsLeaf)
			{
				ComponentData.IsLeaf = true;
				LeafComponentDatas.Add(c, ComponentData);
			}
			else
			{
				ComponentDatas.Add(c, ComponentData);
			}
			
		}

		


			
		// ReduceComponent
		//=================================================================================

		// TMap<int, FWindTreeReduceData> ReduceComponentMap = FindNearestComponents<FWindTreeReduceData>(EditMesh, BVH, ComponentDatas);
		//
		// for (TPair<int, FWindTreeReduceData>& ComponentReduceData: ReduceComponentMap)
		// {
		// 	int Class = ComponentReduceData.Key;
		// 	FWindTreeReduceData& ReduceDatatype = ComponentReduceData.Value;
		// 	
		// 	int ParentClass = ReduceDatatype.ParentClass;
		// 	float MaxDist = ReduceDatatype.MaxDist;
		// 	if (MaxDist > CombineDistThreashould  ) continue;
		//
		// 	if (ReduceComponentMap.Find(ParentClass))
		// 	{
		// 		FWindTreeReduceData& ParentReduceData = ReduceComponentMap[ParentClass];
		// 		float ParentMaxDist = ParentReduceData.MaxDist;
		// 		if (ParentMaxDist < CombineDistThreashould) continue;
		// 	}
		//
		// 	for (int TID : ComponentDatas[Class].TIDs)
		// 	{
		// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 		for (int i = 0; i < 3; i++)
		// 		{
		// 			int DiscardPoint = 1;
		// 			AVI_DiscardPoint->SetValue(TVIDs[i], &DiscardPoint);
		// 		}
		// 	}	
		// 	ComponentDatas[ParentClass].TIDs.Append(ComponentDatas[Class].TIDs);
		// 	ComponentDatas[Class].TIDs.Empty();
		// 	ComponentDatas[Class].IsValid = false;
		//
		//
		// }
		//
		// for (auto It = ComponentDatas.CreateIterator(); It; ++It)
		// {
		// 	if (It.Value().IsValid) continue;
		// 	It.RemoveCurrent();
		// }
		//
		// for (TPair<int, FWindTreeComponentData>& ComponentData :  ComponentDatas)
		// {
		// 	int ParentClass = TNumericLimits<int>::Max();      
		// 	int Class = ComponentData.Key;
		// 	FWindTreeComponentData& Data = ComponentData.Value;
		// 	TMap<int, FWindTreeReduceData> TempComponentMap;
		// 	IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
		// 	{
		// 		if (TID == TNumericLimits<int>::Max()) return false;
		// 		int FindClass = -1;
		// 		ATI_Class->GetValue(TID, &FindClass);
		// 		
		// 		return FindClass != Class;
		// 	});
		// 	for (int TID : Data.TIDs)
		// 	{
		// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 		FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
		// 					
		// 		int HitTID = TNumericLimits<int>::Max();
		//
		// 		double Dist;
		// 		HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
		// 		
		// 		if (HitTID < 0) continue;
		// 		
		// 		int ParentClassNum = -1;
		// 		ATI_Class->GetValue(HitTID, &ParentClassNum);
		// 		float ParentClassDist = 0;
		// 		FWindTreeReduceData ComponentReduceData;
		// 		
		// 		if (TempComponentMap.Find(ParentClassNum) != nullptr) ComponentReduceData = *TempComponentMap.Find(ParentClassNum);
		//
		// 		ComponentReduceData.CollectedData(EditMesh, TID, Dist);
		// 		// ComponentReduceData.Dist = fmax(ComponentReduceData.Dist, Dist);
		// 		// ComponentReduceData.Count += 1;
		//
		// 		TempComponentMap.Add(ParentClassNum, ComponentReduceData);
		// 		
		// 	}
		// 	TPair<int, FWindTreeReduceData> MinDistComponentReduceData;
		// 	for (TPair<int, FWindTreeReduceData> ComponentReduceData: TempComponentMap)
		// 	{
		// 		if ( MinDistComponentReduceData.Value.Count <  ComponentReduceData.Value.Count) MinDistComponentReduceData = ComponentReduceData;
		// 	}
		// 	// float MaxDist = MinDistComponentReduceData.Value.Dist;
		// 	// if (MaxDist > CombineDistThreashould || MaxDist == TNumericLimits<int>::Max() ) continue;
		// 	ReduceComponentMap.Add(MinDistComponentReduceData);
		// 	// ComponentData.Value.AddToClass = MinDistComponentReduceData.Key;
		// 	
		//
		// }
		// for (TPair<int, FWindTreeReduceData>& ComponentReduceData: ReduceComponentMap)
		// {
		// 	int Class = ComponentReduceData.Key;
		// 	FWindTreeReduceData& ReduceDatatype = ComponentReduceData.Value;
		// 	FWindTreeComponentData& Data = ComponentDatas[Class];
		// 	int ParentClass = Data.ParentClass;
		// 	float MaxDist = ReduceDatatype.MaxDist;
		// 	if (MaxDist > CombineDistThreashould ||  MaxDist == TNumericLimits<int>::Max() ) continue;
		//
		// 	if (ReduceComponentMap.Find(ParentClass))
		// 	{
		// 		FWindTreeReduceData& ParentReduceData = ReduceComponentMap[ParentClass];
		// 		MaxDist = ParentReduceData.MaxDist;
		// 		if (MaxDist > CombineDistThreashould ||  MaxDist == TNumericLimits<int>::Max() ) continue;
		// 	}
		// 	ComponentDatas[ParentClass].TIDs.Append(Data.TIDs);
		// 	ComponentDatas[Class].TIDs.Empty();
		// 	ComponentDatas[Class].IsValid = false;
		// 	for (int TID : ComponentDatas[Class].TIDs)
		// 	{
		// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 		for (int i = 0; i < 3; i++)
		// 		{
		// 			int DiscardPoint = 1;
		// 			AVI_DiscardPoint->SetValue(TVIDs[i], &DiscardPoint);
		// 		}
		// 	}
		// }
		// for (auto It = ComponentDatas.CreateIterator(); It; ++It)
		// {
		// 	if (It.Value().IsValid) continue;
		// 	It.RemoveCurrent();
		// }
		// for (TPair<int, FWindTreeComponentData>& ComponentData :  ComponentDatas)
		// {
		// 	FWindTreeComponentData& Data = ComponentData.Value;
		// 	int AddtoClass = Data.AddToClass;
		// 	if (AddtoClass < 0) continue;
		// 	FWindTreeComponentData& ParentData = ComponentDatas[Data.AddToClass];
		// 	
		// 	if (ParentData.AddToClass >= 0) continue;
		// 	ComponentDatas[AddtoClass].TIDs.Append(Data.TIDs);
		// 	Data.TIDs.Empty();
		// 	
		// }
		//
		// TArray<int> KeyToRemove;
		// for (TPair<int, FWindTreeComponentData>& ComponentData :  ComponentDatas)
		// {
		// 	if (ComponentData.Value.TIDs.Num() > 0) continue;
		// 	KeyToRemove.Add(ComponentData.Key);
		// }
		// for (int i = 0; i < KeyToRemove.Num(); i++)
		// {
		// 	FWindTreeComponentData& Data = ComponentDatas[KeyToRemove[i]];
		// 	for (int TID :Data.TIDs)
		// 	{
		// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 		for (int i = 0; i < 3; i++)
		// 		{
		// 			int DiscardPoint = 1;
		// 			AVI_DiscardPoint->SetValue(TVIDs[i], &DiscardPoint);
		// 		}
		// 	}
		// 	ComponentDatas.Remove(KeyToRemove[i]);
		// }
		for (TPair<int, FWindTreeComponentData>& ComponentData :  ComponentDatas)
		{
			DebugClassNum.Add(ComponentData.Key, ComponentData.Value.Center);
		}

		//End reduce components
		//=================================================================================


		// Rebuild TID Class
		//=================================================================================
		
		for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		{
			TSet<int> VertexsSet;
			int Class = ComponentData.Key;
			for (int TID : ComponentData.Value.TIDs)
			{
				FIndex3i TVIDs = EditMesh.GetTriangle(TID);
				ATI_Class->SetValue(TID, &Class);
				for (int i = 0 ; i < 3 ; i++)
				{
					AVI_Class->SetValue(TVIDs[i], &Class);
					VertexsSet.Add(TVIDs[i]);
				}
			}
			TArray<int> VertexsArray = VertexsSet.Array();
			TArray<FVector> VertexPoss;
			VertexPoss.Reserve(VertexsArray.Num());
			for (int i = 0 ; i < VertexsArray.Num(); i++)
			{
				FVector VertexPos = EditMesh.GetVertex(VertexsArray[i]);
				VertexPoss.Add(VertexPos);
			}
			ComponentData.Value.VPoss = VertexPoss;
		}
		//End Rebuild TID Class
		//=================================================================================

		

		// Find Root
		//=================================================================================
		for (int32 TID : EditMesh.TriangleIndicesItr())
		{
			FIndex3i TVIDs = EditMesh.GetTriangle(TID);
			FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
			if ( VertexPos.Z > 0) continue;
			
			int ClassNum = -1;
			ATI_Class->GetValue(TID, &ClassNum);
			if (ClassNum == -1) continue;
			
			ComponentDatas[ClassNum].Hierarchy = "Root";
			ComponentDatas[ClassNum].Checked = true;
		}
		//End Find Root
		//=================================================================================


		//CollectHoles
		//=================================================================================
		for (int i = 0; i < BLoops.GetLoopCount(); i++)
		{
			TArray<int> LoopVertices = BLoops.Loops[i].Vertices;
			int DiscardPoint = 0;
			int LoopClassNum = -1;
			int PreClassNum = -1;

			AVI_PreClass->GetValue(BLoops.Loops[i].Vertices[0], &PreClassNum);
			AVI_Class->GetValue(BLoops.Loops[i].Vertices[0], &LoopClassNum);
			AVI_DiscardPoint->GetValue(BLoops.Loops[i].Vertices[0], &DiscardPoint);
			
			if (ComponentDatas.Find(LoopClassNum) == nullptr) continue;
			if (DiscardPoint) continue;

			for (int VID : LoopVertices)
			{
				if (!EditMesh.IsVertex(VID)) continue;
				int HolePoint = 1;
				AVI_HolePoint->SetValue(VID, &HolePoint);
			}
			ComponentDatas[LoopClassNum].Holes.Add(LoopVertices);
		}

		//EndCollectHoles
		//=================================================================================
		
		for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		{
			
			FWindTreeComponentData Data = ComponentData.Value;
			if (Data.Holes.Num() <= 1) continue;
			TArray<float> Sizes;
			float MaxSize = -9999999999;
			int VaildHole;
			
			for (int i = 0; i < Data.Holes.Num(); i++)
			{
				TArray<int> Hole = Data.Holes[i];
				TArray<FVector> VertexPoss;
				VertexPoss.Reserve(Hole.Num());
				for (int VID : Hole)
				{
					FVector VertexPos = EditMesh.GetVertex(VID);
					VertexPoss.Add(VertexPos);
				}

				FBox Box = FBox(VertexPoss);
				
				float Size = Box.GetSize().Length();
				MaxSize	= fmax(Size, MaxSize);			
				Sizes.Add(Size);
				if (Size > MaxSize) VaildHole = i;
			}
			// for (int i = 0; i < Data.Holes.Num(); i++)
			// {
			// 	if (Sizes[i] == MaxSize) continue;
			//
			// 	for (int VID : Data.Holes[i])
			// 	{
			// 		int discardPoint = 1;
			// 		AVI_DiscardPoint->SetValue(VID, &discardPoint);
			// 	}
			// }
			
		}
		for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		{
			
		}
		TArray<FVector> AllLoopVertices;
		for (int i = 0; i < BLoops.GetLoopCount(); i++)
		{
			
			FVector Normal = FVector::ZeroVector;
			FVector Center = FVector::ZeroVector;
			
			TArray<int> LoopVertices = BLoops.Loops[i].Vertices;
			TArray<FVector> LoopVertexPos;
			LoopVertexPos.Reserve(LoopVertices.Num());
			float NumLoopVertices = LoopVertices.Num();
			if (NumLoopVertices == 0) continue;
			int DiscardPoint = 0;
			int LoopClassNum = -1;
			int PreClassNum = -1;

			AVI_PreClass->GetValue(BLoops.Loops[i].Vertices[0], &PreClassNum);
			AVI_Class->GetValue(BLoops.Loops[i].Vertices[0], &LoopClassNum);
			AVI_DiscardPoint->GetValue(BLoops.Loops[i].Vertices[0], &DiscardPoint);

			if (ComponentDatas.Find(LoopClassNum) == nullptr) continue;
			if (DiscardPoint) continue;

			bool LoopChecked = ComponentDatas[LoopClassNum].Checked;

			
			for (int j = 0; j < NumLoopVertices; j++)
			{
				FVector PrePos = BLoops.Loops[i].GetPrevVertex(j);
				FVector Pos = BLoops.Loops[i].GetVertex(j);
				FVector NextPos = BLoops.Loops[i].GetNextVertex(j);
				
				Center += Pos;
				
				FVector Dir1 = (PrePos - Pos).GetSafeNormal();
				FVector Dir2 = (NextPos - Pos).GetSafeNormal();

				Normal += FVector::CrossProduct(Dir1, Dir2);
				
			}
			FVector3d ResultN = Normal.GetSafeNormal();
			Center /= NumLoopVertices;




			// Find ParentClass
			//=================================================================================
			int ParentClass = TNumericLimits<int>::Max();
			bool TarChecked = false;
			for (int j = 0; j < NumLoopVertices; j++)
			{
				FVector3d VertexPos = EditMesh.GetVertex(LoopVertices[j]);
				LoopVertexPos.Add(VertexPos);
				
				int HitTID = TNumericLimits<int>::Max();
				IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
				{
					if (TID == TNumericLimits<int>::Max()) return false;
					int FindClass = -1;
					ATI_Class->GetValue(TID, &FindClass);
					if (!ComponentDatas.Find(FindClass)) return false;
					return FindClass != LoopClassNum;
				});
				double Dist;
				HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
				if (HitTID < 0) continue;
				
				if (Dist > FindParentThreashold ) continue;
				
				int ParentClassNum = -1;
				ATI_Class->GetValue(HitTID, &ParentClassNum);
				
				bool Check = ComponentDatas[ParentClassNum].Checked;
				ParentClass = fmin(ParentClass, ParentClassNum);
				TarChecked = TarChecked || Check;
				
			}
			AllLoopVertices.Append(LoopVertexPos);
			
			// End Parent Class
			//=================================================================================
			
			if (LoopChecked == 1 ) continue;
			
			ComponentDatas[LoopClassNum].RootCenter = Center;
			ComponentDatas[LoopClassNum].RootNormal = ResultN;
			ComponentDatas[LoopClassNum].ParentClass = ParentClass;

			if  (ParentClass != TNumericLimits<int>::Max()) continue;
			
			ComponentDatas[LoopClassNum].RootCenter = FVector::ZeroVector;
			ComponentDatas[LoopClassNum].RootNormal = FVector::UpVector;
			ComponentDatas[LoopClassNum].ParentClass = -1;
			ComponentDatas[LoopClassNum].Hierarchy = "Root";
			ComponentDatas[LoopClassNum].Checked = 1;
			
		}
		OutHolePositions = AllLoopVertices;

		// //ReduceComponent  V2
		// //=================================================================================
		// for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		// {
		// 	int Class = ComponentData.Key;
		// 	FWindTreeComponentData& Data = ComponentData.Value;
		// 	
		// 	int ParentClass = Data.ParentClass;
		// 	if (!ComponentDatas.Find(ParentClass)) continue;
		//
		// 	float MaxDist = -99999999;
		// 	for (int TID : Data.TIDs)
		// 	{
		// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 		FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
		// 		
		// 		int HitTID = TNumericLimits<int>::Max();
		// 		IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
		// 		{
		// 			if (TID == TNumericLimits<int>::Max()) return false;
		// 			int FindClass = -1;
		// 			ATI_Class->GetValue(TID, &FindClass);
		// 			return FindClass == ParentClass;
		// 		});
		// 		double Dist;
		// 		HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
		// 		if (HitTID < 0) continue;
		// 		
		// 		MaxDist = fmax(MaxDist, Dist);
		// 	}
		// 	if (MaxDist > CombineDistThreashould  ) continue;
		// 	if (Data.Hierarchy == "Root") continue;
		//
		// 	Data.IsValid = false;
		// }
		// for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		// {
		//
		// 	FWindTreeComponentData& Data = ComponentData.Value;
		// 	int Class = ComponentData.Key;
		// 	int ParentClass = Data.ParentClass;
		//
		// 	if (ParentClass < 0) continue;
		// 	if (!ComponentDatas.Find(ParentClass)) continue;
		// 	if (ComponentDatas[ParentClass].IsValid) continue;
		// 	
		// 	Data.IsValid = true;
		// 	Data.ParentClass = ComponentDatas[ParentClass].ParentClass;
		// }
		//
		// for (auto It = ComponentDatas.CreateIterator(); It; ++It)
		// {
		// 	if (It.Value().IsValid) continue;
		// 	ComponentDatas[It.Value().ParentClass].TIDs.Append(It.Value().TIDs);
		// 	for (int TID : It.Value().TIDs)
		// 	{
		// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 		for (int i = 0; i < 3; i++)
		// 		{
		// 			int DiscardPoint = 1;
		// 			AVI_DiscardPoint->SetValue(TVIDs[i], &DiscardPoint);
		// 		}
		// 	}	
		// 	It.RemoveCurrent();
		// }
		// // ReduceComponent V2
		// //=================================================================================
		
		//
		// //Rebuild TID 
		// for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		// {
		// 	TSet<int> VertexsSet;
		// 	int Class = ComponentData.Key;
		// 	for (int TID : ComponentData.Value.TIDs)
		// 	{
		// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 		ATI_Class->SetValue(TID, &Class);
		// 		for (int i = 0 ; i < 3 ; i++)
		// 		{
		// 			AVI_Class->SetValue(TVIDs[i], &Class);
		// 			VertexsSet.Add(TVIDs[i]);
		// 		}
		// 	}
		// 	TArray<int> VertexsArray = VertexsSet.Array();
		// 	TArray<FVector> VertexPoss;
		// 	VertexPoss.Reserve(VertexsArray.Num());
		// 	for (int i = 0 ; i < VertexsArray.Num(); i++)
		// 	{
		// 		FVector VertexPos = EditMesh.GetVertex(VertexsArray[i]);
		// 		VertexPoss.Add(VertexPos);
		// 	}
		// 	ComponentData.Value.VPoss = VertexPoss;
		// }
		// for (TPair<int, FDynamicMeshComponentData> ComponentData : ComponentDatas)
		// {
		// 	DebugClassNum.Add(ComponentData.Key, ComponentData.Value.RootCenter);
		// }
		//
		// // Find Root
		// //=================================================================================
		// for (int32 TID : EditMesh.TriangleIndicesItr())
		// {
		// 	FIndex3i TVIDs = EditMesh.GetTriangle(TID);
		// 	FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
		// 	if ( VertexPos.Z > 0) continue;
		// 			
		// 	int ClassNum = -1;
		// 	ATI_Class->GetValue(TID, &ClassNum);
		// 	if (ClassNum == -1) continue;
		// 			
		// 	ComponentDatas[ClassNum].Hierarchy = "Root";
		// 	ComponentDatas[ClassNum].Checked = true;
		// }
		// //End Find Root
		// //=================================================================================
		//
		//
		// for (int i = 0; i < BLoops.GetLoopCount(); i++)
		// {
		// 	
		// 	FVector Normal = FVector::ZeroVector;
		// 	FVector Center = FVector::ZeroVector;
		// 	
		// 	TArray<int> LoopVertices = BLoops.Loops[i].Vertices;
		// 	TArray<FVector> LoopVertexPos;
		// 	LoopVertexPos.Reserve(LoopVertices.Num());
		// 	float NumLoopVertices = LoopVertices.Num();
		// 	if (NumLoopVertices == 0) continue;
		// 	int DiscardPoint = 0;
		// 	int LoopClassNum = -1;
		// 	int PreClassNum = -1;
		//
		// 	AVI_PreClass->GetValue(BLoops.Loops[i].Vertices[0], &PreClassNum);
		// 	AVI_Class->GetValue(BLoops.Loops[i].Vertices[0], &LoopClassNum);
		// 	AVI_DiscardPoint->GetValue(BLoops.Loops[i].Vertices[0], &DiscardPoint);
		// 	
		// 	bool LoopChecked = ComponentDatas[LoopClassNum].Checked;
		// 	if (DiscardPoint) continue;
		//
		// 	
		// 	for (int j = 0; j < NumLoopVertices; j++)
		// 	{
		// 		FVector PrePos = BLoops.Loops[i].GetPrevVertex(j);
		// 		FVector Pos = BLoops.Loops[i].GetVertex(j);
		// 		FVector NextPos = BLoops.Loops[i].GetNextVertex(j);
		// 		
		// 		Center += Pos;
		// 		
		// 		FVector Dir1 = (PrePos - Pos).GetSafeNormal();
		// 		FVector Dir2 = (NextPos - Pos).GetSafeNormal();
		//
		// 		Normal += FVector::CrossProduct(Dir1, Dir2);
		// 		
		// 	}
		// 	FVector3d ResultN = Normal.GetSafeNormal();
		// 	Center /= NumLoopVertices;
		//
		//
		//
		//
		// 	// Find ParentClass
		// 	//=================================================================================
		// 	int ParentClass = TNumericLimits<int>::Max();
		// 	bool TarChecked = false;
		// 	for (int j = 0; j < NumLoopVertices; j++)
		// 	{
		// 		FVector3d VertexPos = EditMesh.GetVertex(LoopVertices[j]);
		// 		LoopVertexPos.Add(VertexPos);
		// 		
		// 		int HitTID = TNumericLimits<int>::Max();
		// 		IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
		// 		{
		// 			if (TID == TNumericLimits<int>::Max()) return false;
		// 			int FindClass = -1;
		// 			ATI_Class->GetValue(TID, &FindClass);
		// 			return FindClass != LoopClassNum;
		// 		});
		// 		double Dist;
		// 		HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
		// 		if (HitTID >= 0)
		// 		{
		// 			int ParentClassNum = -1;
		// 			ATI_Class->GetValue(HitTID, &ParentClassNum);
		// 			
		// 			bool Check = ComponentDatas[ParentClassNum].Checked;
		// 			ParentClass = fmin(ParentClass, ParentClassNum);
		// 			TarChecked = TarChecked || Check;
		// 		}
		// 	}
		// 	AllLoopVertices.Append(LoopVertexPos);
		// 	
		// 	// End Parent Class
		// 	//=================================================================================
		// 	
		// 	if (LoopChecked == 1 ) continue;
		// 	
		// 	ComponentDatas[LoopClassNum].RootCenter = Center;
		// 	ComponentDatas[LoopClassNum].RootNormal = ResultN;
		// 	ComponentDatas[LoopClassNum].ParentClass = ParentClass;
		//
		// 	if  (ParentClass != TNumericLimits<int>::Max()) continue;
		// 	
		// 	ComponentDatas[LoopClassNum].RootCenter = FVector::ZeroVector;
		// 	ComponentDatas[LoopClassNum].RootNormal = FVector::UpVector;
		// 	ComponentDatas[LoopClassNum].ParentClass = -1;
		// 	ComponentDatas[LoopClassNum].Hierarchy = "Root";
		// 	ComponentDatas[LoopClassNum].Checked = 1;
		// 	
		// }
		//
		//

		
		//Build hierarchy
		for (int i = 0 ; i < 10; i++)
		{
			for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
			{
				int Class = ComponentData.Key;
				FWindTreeComponentData& Data = ComponentData.Value;
				int ParentClass = Data.ParentClass;
				bool Checked = ComponentData.Value.Checked;

				if (Checked || ParentClass < 0) continue;
				if (!ComponentDatas[ParentClass].Checked) continue;
				
				if (i < 3)
				{
					FString& Hierarchy = ComponentData.Value.Hierarchy;
					FString TartHierarchy = ComponentDatas[ParentClass].Hierarchy;
					Hierarchy += TartHierarchy + "/" + FString::FromInt(Class);
					ComponentData.Value.StepCount = ComponentDatas[ParentClass].StepCount + 1;
					MaxStepCount = FMath::Max(ComponentData.Value.StepCount, MaxStepCount);
				}
				else
				{
					Data.Hierarchy = ComponentDatas[ParentClass].Hierarchy;
					Data.RootCenter = ComponentDatas[ParentClass].RootCenter;
					Data.RootNormal = ComponentDatas[ParentClass].RootNormal;
					Data.ParentClass = ComponentDatas[ParentClass].ParentClass;
					Data.StepCount = ComponentDatas[ParentClass].StepCount;
					Data.DebugColor = FLinearColor::Red;
				}
				Data.CheckedTemp = true;
			}
			for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
			{
				ComponentData.Value.Checked = ComponentData.Value.Checked || ComponentData.Value.CheckedTemp;
			}
		}

		//End build hierarchy
		//=================================================================================

		//CombineLeafComponent
		//=================================================================================
		if (LeafMaterialIndex >= 0)
		{
			// TMap<int, FWindTreeReduceData> ReduceComponentMap = FindNearestComponents<FWindTreeReduceData>(EditMesh, BVH, ComponentDatas);

			TMap<int, FWindTreeCombineLeafData> ReduceComponentMap;
				
			for (TPair<int, FWindTreeComponentData> LeafComponentData :  LeafComponentDatas)
			{
				int ParentClass = TNumericLimits<int>::Max();      
				int Class = LeafComponentData.Key;
				FWindTreeComponentData& Data = LeafComponentData.Value;
				TMap<int, FWindTreeCombineLeafData> TempComponentMap;
				TDynamicMeshTriangleAttribute<int, 1>* AttribClass = static_cast<TDynamicMeshTriangleAttribute<int, 1>*>(EditMesh.Attributes()->GetAttachedAttribute(FName("TI_Class")));
				IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
				{
					int FindClass = -1;
					AttribClass->GetValue(TID, &FindClass);
					if (!ComponentDatas.Find(FindClass)) return false;
					
					return true;
				});
				bool Find = false;
				float MinDist = 9999999999999;
				for (int TID : Data.TIDs)
				{
					FIndex3i TVIDs = EditMesh.GetTriangle(TID);
					FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
					int HitTID = TNumericLimits<int>::Max();

					double Dist;
					HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
					
					if (HitTID < 0) continue;
					if (Dist > MinDist) continue;

					MinDist = Dist;
					int ParentClassNum = -1;
					AttribClass->GetValue(HitTID, &ParentClassNum);
					float ParentClassDist = 0;
					FWindTreeCombineLeafData ComponentReduceData;
					
					if (TempComponentMap.Find(ParentClassNum) != nullptr) ComponentReduceData = *TempComponentMap.Find(ParentClassNum);

					bool LeafCheck = ComponentReduceData.CollectedData(EditMesh, TID, Dist);
					
					TempComponentMap.Add(ParentClassNum, ComponentReduceData);
					Find = true;
				}
				//Find Main Nearest Component
				//=================================================================================
				if (!Find) continue;
				TPair<int, FWindTreeCombineLeafData> MinDistComponentReduceData;
				for (TPair<int, FWindTreeCombineLeafData> ComponentReduceData: TempComponentMap)
				{
					if ( MinDistComponentReduceData.Value.MinDist >  ComponentReduceData.Value.MinDist)
					{
						MinDistComponentReduceData = ComponentReduceData;
						MinDistComponentReduceData.Value.ParentClass = ComponentReduceData.Key;
					}
				}

				ReduceComponentMap.Add(Class, MinDistComponentReduceData.Value);
				
			}
			for (TPair<int, FWindTreeCombineLeafData> LeafComponent : ReduceComponentMap)
			{
				int Class = LeafComponent.Key;
				FWindTreeCombineLeafData Data = LeafComponent.Value;
				int ParentClass = Data.ParentClass;

				ComponentDatas[ParentClass].TIDs.Append(LeafComponentDatas[Class].TIDs);
			}
		}

		TMap<int, FString> HierarchyTest;
		for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
		{
			HierarchyTest.Add(ComponentData.Key, ComponentData.Value.Hierarchy);
		}
		
		TMap<FString, FWindTreeComponentData> HierarchyCompMap;
		int CompDataIndex = 0;
		for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
		{
			int Class = ComponentData.Key;
			FWindTreeComponentData& Data = ComponentData.Value;
			FString ParentHierarchy = "";
			if (Data.ParentClass >= 0) ParentHierarchy = ComponentDatas[Data.ParentClass].Hierarchy;
		
			if (Data.Hierarchy.Len() == 0)
			{
				Data.Hierarchy = "Root";
				Data.ParentClass = -1;
				Data.RootCenter = FVector::ZeroVector;
				Data.RootNormal = FVector(0, 0, 1);
			}
			if (HierarchyCompMap.Find(Data.Hierarchy) == nullptr)
			{
				Data.ParentHierarchy = ParentHierarchy;
				Data.UVIndex = CompDataIndex;
				HierarchyCompMap.Add(Data.Hierarchy, Data);
				CompDataIndex += 1;
			}
			else
			{
				FWindTreeComponentData& RootComponentData = *HierarchyCompMap.Find(Data.Hierarchy);
				RootComponentData.TIDs.Append(Data.TIDs);
				RootComponentData.VPoss.Append(Data.VPoss);
			}
		}
		for (TPair<FString, FWindTreeComponentData>& ComponentData : HierarchyCompMap)
		{

		}

		
		for (TPair<FString, FWindTreeComponentData>& ComponentData : HierarchyCompMap)
		{
			FVector OBBUpDir = FVector::ZeroVector;
			UGeometryGeneral::CalculateOBBUpDir(ComponentData.Value.VPoss, OBBUpDir);
			
			FWindTreeComponentData& Data = ComponentData.Value;
			FVector ObjectBasisVectorX = -OBBUpDir.GetSafeNormal();
			FVector ObjectBasisVectorY = Data.RootNormal.GetSafeNormal();
			FVector ObjectBasisVectorZ = FVector::CrossProduct(ObjectBasisVectorX, -ObjectBasisVectorY).GetSafeNormal();
			FQuat yDhiedral = FQuat::FindBetweenNormals(ObjectBasisVectorY, FVector(0, 1, 0));
			yDhiedral.RotateVector(ObjectBasisVectorZ);
			FQuat zDhiedral = FQuat::FindBetweenNormals(ObjectBasisVectorZ, FVector(0, 0, 1));


			for (int v = 0; v < Data.VPoss.Num(); v++)
			{
				FVector& VertexPos = Data.VPoss[v];
	
				VertexPos -= Data.RootCenter;
				yDhiedral.RotateVector(VertexPos);
				zDhiedral.RotateVector(VertexPos);
			}
			FBox BBox = FBox(Data.VPoss);
			FVector BBoxSize = BBox.GetSize();
			FVector OriginExtent = BBoxSize.GetAbs();
			Data.OriginExtent = FMath::Clamp(OriginExtent.X / 2048.0, 1 / 2048.0, 1);
			if (Data.ParentHierarchy.Len() == 0) Data.ParentHierarchy = Data.Hierarchy;
		}
		

		//Calculate TextureXY
		int32 objectToProcessCount, evenNumber, halfEvenNumber, halfNumber, decrementerTotal, modResult;
		int32 rowCounter, newDecrementerTotal, decrementAmount, complete, maxAttempts, attempt;
		
		
		objectToProcessCount = HierarchyCompMap.Num();
		
		decrementerTotal = 256;
		modResult = 1;
		rowCounter = 1;
		evenNumber = (objectToProcessCount % 2)==0;
		halfEvenNumber = (int32(objectToProcessCount/2.0) % 2)==0;
		halfNumber = ceil(objectToProcessCount/2.0);

		if (halfNumber < decrementerTotal)
		{
			newDecrementerTotal = halfNumber;
		}
		else
		{
			newDecrementerTotal = decrementerTotal;
		}

		if (halfEvenNumber==1)
		{
			decrementAmount = 2;
		}
		else
		{
			decrementAmount = 1;
		}
		
		complete = 0;
		attempt = 0;
		maxAttempts = 100;

		while ( complete == 0 || attempt < maxAttempts )
		{
			attempt += 1;
			modResult = objectToProcessCount % newDecrementerTotal;
			if ( modResult == 0 || newDecrementerTotal < 1 )
			{
				complete = 1;
			}
			if ( complete == 0 )
			{
				newDecrementerTotal -= decrementAmount;
			}
			if (newDecrementerTotal < 1)
			{
				newDecrementerTotal = 1;
			}
		}

		// Get Texture Size
		if ( newDecrementerTotal==1 || ( objectToProcessCount / newDecrementerTotal ) > decrementerTotal )
		{
			tXy = int(floor(sqrt(objectToProcessCount)));
			tXx = int(ceil(float(objectToProcessCount)/(floor(tXy))));
		}
		else
		{
			tXx = newDecrementerTotal;
			tXy = objectToProcessCount / newDecrementerTotal;
		}
		FDynamicMeshUVOverlay* UVOverlay2 = EditMesh.Attributes()->GetUVLayer(1);
		if (UVOverlay2 == nullptr) return;


		TMap<FString, FWindTreeComponentData> HierarchyPurMap;
		
		int MapIndex = 0;
		// for (int i = 0; i < Hierarchys.Num(); i++)
		// {
		// 	if (Hierarchys[i].Len() == 0)
		// 	{
		// 		Hierarchys[i] = "Root";
		// 		ParentClasses[i] = -1;
		// 		RootCenter[i] = FVector::ZeroVector;
		// 		RootNormal[i] = FVector(0, 0, 1);
		// 	}
		// 	float OriginExtent = OriginExtents[i];
		// 	if (Hierarchys[i] == "Root" && HierarchyPurMap.Find("Root") != nullptr)
		// 	{
		// 		FDynamicMeshComponentData RootComponentData = *HierarchyPurMap.Find("Root");
		// 		OriginExtent = RootComponentData.OriginExtent;
		// 	}
		//
		// 	FDynamicMeshComponentData ComponentData;
		// 	ComponentData.RootCenter = RootCenter[i];
		// 	ComponentData.RootNormal = RootNormal[i];
		// 	ComponentData.OriginExtent = OriginExtent;
		// 	ComponentData.ParentClass = ParentClasses[i];
		// 	// ComponentData.Index = MapIndex;
		// 	HierarchyPurMap.Add(Hierarchys[i], ComponentData);
		// }
		// int ComponentDataIndex = 0;
		// for (TPair<FString, FDynamicMeshComponentData>& Data : HierarchyPurMap)
		// {
		// 	Data.Value.UVIndex = ComponentDataIndex;
		// 	ComponentDataIndex += 1;
		// }
		PivotIndexData.SetNumZeroed(HierarchyCompMap.Num());
		DirExtentData.SetNumZeroed(HierarchyCompMap.Num());

		
		float TextureMax = fmax(tXx, tXy);

		UVIndexTest.Reserve(HierarchyCompMap.Num());
		UVTestIndex.Reserve(HierarchyCompMap.Num());
		UVFloat.Reserve(HierarchyCompMap.Num());
		for (TPair<FString, FWindTreeComponentData>& Component : HierarchyCompMap)
		{

			FString HierarchyN = Component.Key;
			FWindTreeComponentData Data = Component.Value;
			UVIndexTest.Add(Data.UVIndex);
			int UVIndex = Data.UVIndex;
			float PosX = rint(fmodf(UVIndex, TextureMax));
			float PosY = floor(UVIndex / TextureMax);
			FVector2f UV2 = FVector2f(PosX / tXx + .5 / tXx, PosY / tXy + 0.5 / tXy);
			for (int TID : Data.TIDs)
			{
				FIndex3i TVIDs = UVOverlay2->GetTriangle(TID);
				for (int v = 0; v < 3; v++)
				{
					// if (!EditMesh.IsVertex(TVIDs[v]))continue;
					UVOverlay2->SetElement(TVIDs[v], UV2);
				}
			}
			UVTestIndex.Add(FVector2D(UV2.X * 103, UV2.Y * 9));
			UVFloat.Add(FVector2D(UV2.X, UV2.Y));

			int ParentIndex = HierarchyCompMap[Data.ParentHierarchy].UVIndex;
			
			int16 ParentIndex16 = static_cast<int16>(ParentIndex); 
			ParentIndex16 += 1024;
			FFloat16 Parent16Value = *reinterpret_cast<FFloat16*>(&ParentIndex16);
			// *reinterpret_cast<FFloat16*>(static_cast<int16>(TestInt + 1024)));

			FVector Center = Data.RootCenter;
			FVector Normal = Data.RootNormal;
			Normal = Normal / 2 + FVector::OneVector * .5;
			PivotIndexData[UVIndex] = (FLinearColor(Center.X, Center.Y, Center.Z, Parent16Value));
			DirExtentData[UVIndex] = (FLinearColor(Normal.X, Normal.Y, Normal.Z, fmax(Data.OriginExtent, 0.01)));
			
			//Debug Color
			FLinearColor TestColor = FLinearColor::MakeRandomColor();
			FDynamicMeshColorOverlay* ColorOverlay = EditMesh.Attributes()->PrimaryColors();
						
			for (int TID : Data.TIDs)
			{
				FIndex3i TVID = ColorOverlay->GetTriangle(TID);
				for (int v = 0; v < 3; v++)
				{
					float ColorB = 0;
					if (Data.Hierarchy == "Root")
					{
						ColorB = 1;
					}
					ColorOverlay->SetElement(TVID[v], FLinearColor(Data.StepCount / 5.0, Data.Class / 255.0, ColorB, 0));
					// ColorOverlay->SetElement(TVID[v], FLinearColor(OriginExtents[c], 0, 0, 0));
					// ColorOverlay->SetElement(TVID[v], TestColor);
					// ColorOverlay->SetElement(TVID[v], DebugColors[c]);
				}
			}
			//End Debug Color
		}
		TArray<TArray<FVector2f>> UVSet;
		for (TPair<int, FWindTreeComponentData>& Component : LeafComponentDatas)
		{
			TArray<FVector2f> UVPerLeaf;
			for (int TID : Component.Value.TIDs)
			{
				FIndex3i TVIDs = UVOverlay2->GetTriangle(TID);
				FVector2f UVf = FVector2f(0, 0);

				UVOverlay2->GetElement(TVIDs[0], UVf);
				
				UVPerLeaf.Add(UVf);
			}
			UVSet.Add(UVPerLeaf);
		}
	});
	

	// FTextureRenderTargetResource* R_PivotIndex = InWindTexture_PivotIndex->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_DirExtent = InWindTexture_DirExtent->GameThread_GetRenderTargetResource();
	//
	// ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	// [=](FRHICommandListImmediate& RHICmdList)
	// {
	// 	FTextureRHIRef RHI_PivotIndex = R_PivotIndex->GetRenderTargetTexture();
	// 	FTextureRHIRef RHI_DirExtent = R_DirExtent->GetRenderTargetTexture();
	// 	uint32 DestStride;
	// 	void* DestData = RHILockTexture2D(RHI_PivotIndex, 0, RLM_WriteOnly, DestStride, false);
	// 	 if (!DestStride)	return;
	// 	FMemory::Memcpy(DestData, Colors16.GetData(), TextureTarget->GetSizeXY().X * TextureTarget->GetSizeXY().Y * sizeof(FFloat16));
	// 	RHIUnlockTexture2D(TextureRHI, 0 ,false);
	// });
	// FlushRenderingCommands();
	
}

void UGeometryGeneral::TreeWindMergeComponents(FDynamicMesh3& EditMesh, FGeometryScriptDynamicMeshBVH BVH,
	TMap<int, FDynamicMeshComponentData> ComponentDatas)
{

}

template<typename ReduceDatatype, typename  ComponentType>
TMap<int, ReduceDatatype> UGeometryGeneral::FindNearestComponents(FDynamicMesh3& EditMesh, FGeometryScriptDynamicMeshBVH BVH, TMap<int, ComponentType> ComponentDatas)
{
	TMap<int, ReduceDatatype> ReduceComponentMap;
	if(!EditMesh.Attributes()->HasAttachedAttribute(FName("TI_Class"))) return ReduceComponentMap;
		
	for (TPair<int, ComponentType> ComponentData :  ComponentDatas)
	{
		int ParentClass = TNumericLimits<int>::Max();      
		int Class = ComponentData.Key;
		ComponentType& Data = ComponentData.Value;
		TMap<int, ReduceDatatype> TempComponentMap;
		TDynamicMeshTriangleAttribute<int, 1>* AttribClass = static_cast<TDynamicMeshTriangleAttribute<int, 1>*>(EditMesh.Attributes()->GetAttachedAttribute(FName("TI_Class")));
		IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
		{
			if (TID == TNumericLimits<int>::Max()) return false;
			int FindClass = -1;
			AttribClass->GetValue(TID, &FindClass);
			
			return FindClass != Class;
		});
		for (int TID : Data.TIDs)
		{
			FIndex3i TVIDs = EditMesh.GetTriangle(TID);
			FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
			int HitTID = TNumericLimits<int>::Max();

			double Dist;
			HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
			
			if (HitTID < 0) continue;
			
			int ParentClassNum = -1;
			AttribClass->GetValue(HitTID, &ParentClassNum);
			float ParentClassDist = 0;
			ReduceDatatype ComponentReduceData;
			
			if (TempComponentMap.Find(ParentClassNum) != nullptr) ComponentReduceData = *TempComponentMap.Find(ParentClassNum);

			ComponentReduceData.CollectedData(EditMesh, TID, Dist);
			TempComponentMap.Add(ParentClassNum, ComponentReduceData);
			
		}
		//Find Main Nearest Component
		//=================================================================================
		TPair<int, ReduceDatatype> MinDistComponentReduceData;
		for (TPair<int, ReduceDatatype> ComponentReduceData: TempComponentMap)
		{
			if ( MinDistComponentReduceData.Value.Count <  ComponentReduceData.Value.Count)
			{
				MinDistComponentReduceData = ComponentReduceData;
				MinDistComponentReduceData.Value.ParentClass = ComponentReduceData.Key;
			}
		}

		ReduceComponentMap.Add(Class, MinDistComponentReduceData.Value);
		
	}
	return ReduceComponentMap;
	
	// for (TPair<int, ReduceDatatype>& ComponentReduceData: ReduceComponentMap)
	// {
	// 	int Class = ComponentReduceData.Key;
	// 	ReduceDatatype& ReduceDatatype = ComponentReduceData.Value;
	// 	FDynamicMeshComponentData& Data = ComponentDatas[Class];
	// 	int ParentClass = Data.ParentClass;
	// 	float MaxDist = ReduceDatatype.MaxDist;
	// 	if (MaxDist > CombineDistThreashould ||  MaxDist == TNumericLimits<int>::Max() ) continue;
	//
	// 	if (ReduceComponentMap.Find(ParentClass))
	// 	{
	// 		FWindTreeReduceData& ParentReduceData = ReduceComponentMap[ParentClass];
	// 		MaxDist = ParentReduceData.MaxDist;
	// 		if (MaxDist > CombineDistThreashould ||  MaxDist == TNumericLimits<int>::Max() ) continue;
	// 	}
	// 	ComponentDatas[ParentClass].TIDs.Append(Data.TIDs);
	// 	ComponentDatas[Class].TIDs.Empty();
	// 	ComponentDatas[Class].IsValid = false;
	// 	for (int TID : ComponentDatas[Class].TIDs)
	// 	{
	// 		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
	// 		for (int i = 0; i < 3; i++)
	// 		{
	// 			int DiscardPoint = 1;
	// 			AVI_DiscardPoint->SetValue(TVIDs[i], &DiscardPoint);
	// 		}
	// 	}
	// }
	// for (auto It = ComponentDatas.CreateIterator(); It; ++It)
	// {
	// 	if (It.Value().IsValid) continue;
	// 	It.RemoveCurrent();
	// }


	
	// if (!MergeOptions.FindConditionLambda) return;
	// struct FComponentReduceData
	// {
	//     double Dist = TNumericLimits<double>::Max();
	//     int Count = 0;
	// };
	//

	
 //    for (TPair<int, FDynamicMeshComponentData>& ComponentDataPair : ComponentDatas)
 //    {
 //        int Class = ComponentDataPair.Key;
 //        FDynamicMeshComponentData& Data = ComponentDataPair.Value;
 //
 //
 //        TMap<int, int> ComponentMap;
 //    	
 //    	TDynamicMeshTriangleAttribute<int, 1>* AttribClass = static_cast<TDynamicMeshTriangleAttribute<int, 1>*>(EditMesh->Attributes()->GetAttachedAttribute(FName("ATI_Class")));
 //        IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
 //        {
	//        int FindClass = -1;
	//         EditMesh->IsTriangle(TID);
	//        AttribClass->GetValue(TID, &FindClass);
	//         
	//        return FindClass != Class;
 //        });
 //    	
 //        for (int TID : Data.TIDs)
 //        {
	// 		FIndex3i TVIDs = EditMesh->GetTriangle(TID);
	// 		FVector3d VertexPos = EditMesh->GetVertex(TVIDs[0]); 
	// 		        
	// 		double Dist = TNumericLimits<double>::Max();
	// 		int HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
 //
	// 		if (HitTID >= 0 && Dist < TNumericLimits<double>::Max())
	// 		{
	// 		    if (!MergeOptions.FindConditionLambda(TID)) continue;
 //
	// 		        
	// 			ComponentMap.Add(Class, ParentClassNum);
	// 		}
 //        	TPair<int, FComponentReduceData> MinDistComponentReduceData;
 //        	for (TPair<int, FComponentReduceData> ComponentReduceData: ComponentMap)
 //        	{
 //        		if ( MinDistComponentReduceData.Value.Count <  ComponentReduceData.Value.Count) MinDistComponentReduceData = ComponentReduceData;
 //        	}
 //        	float MaxDist = MinDistComponentReduceData.Value.Dist;
 //        	if (MaxDist > CombineDistThreashould || MaxDist == TNumericLimits<int>::Max() ) continue;
	// 		
 //        	ComponentData.Value.AddToClass = MinDistComponentReduceData.Key;
	// 		
 //        	for (int TID :Data.TIDs)
 //        	{
 //        		FIndex3i TVIDs = EditMesh.GetTriangle(TID);
 //        		for (int i = 0; i < 3; i++)
 //        		{
 //        			int DiscardPoint = 1;
 //        			AVI_DiscardPoint->SetValue(TVIDs[i], &DiscardPoint);
 //        		}
 //        	}
 //        }
 //    	
 //        TPair<int, FComponentReduceData> MinDistComponentReduceData(
 //            -1, FComponentReduceData()
 //        ); 
 //
 //        for (const TPair<int, FComponentReduceData>& ComponentReduceDataPair: ComponentMap)
 //        {
 //           if (MinDistComponentReduceData.Value.Count < ComponentReduceDataPair.Value.Count)
 //           {
 //               MinDistComponentReduceData = ComponentReduceDataPair;
 //           }
 //        }
 //        
 //        double MaxDist = MinDistComponentReduceData.Value.Dist;
 //        int TargetClass = MinDistComponentReduceData.Key;
 //    	
 //        if (TargetClass < 0 || MaxDist > CombineDistThreashould || MaxDist == TNumericLimits<double>::Max())
 //        {
 //            Data.AddToClass = -1;
 //            Data.MaxDistToParent = TNumericLimits<double>::Max();
 //            MergeMapping.Add(Class, -1);
 //            continue;
 //        }
 //    	
 //        Data.AddToClass = TargetClass;
 //        Data.MaxDistToParent = MaxDist;
 //        MergeMapping.Add(Class, TargetClass);
 //    	
 //        for (int TID :Data.TIDs)
 //        {
 //           FIndex3i TVIDs = EditMesh->GetTriangle(TID);
 //           for (int i = 0; i < 3; i++)
 //           {
 //              int DiscardPoint = 1;
 //              AVI_DiscardPoint->SetValue(TVIDs[i], &DiscardPoint);
 //           }
 //        }
 //    }
	//
	// for (TPair<int, FDynamicMeshComponentData>& ComponentData :  ComponentDatas)
	// {
	// 	FDynamicMeshComponentData& Data = ComponentData.Value;
	// 	int ParentClass = Data.ParentClass;
	// 	if (ParentClass < 0) continue;
	// 	FDynamicMeshComponentData& ParentData = ComponentDatas[Data.ParentClass];
	// 		
	// 	if (ParentData.ParentClass >= 0) continue;
	// 	ComponentDatas[ParentClass].TIDs.Append(Data.TIDs);
	// 	Data.TIDs.Empty();
	// 		
	// }
	// TArray<int> KeyToRemove;
	// for (TPair<int, FDynamicMeshComponentData>& ComponentData :  ComponentDatas)
	// {
	// 	if (ComponentData.Value.TIDs.Num() > 0) continue;
	// 	KeyToRemove.Add(ComponentData.Key);
	// }
	// for (int i = 0; i < KeyToRemove.Num(); i++)
	// {
	// 	ComponentDatas.Remove(KeyToRemove[i]);
	// }

}

UDynamicMesh* UGeometryGeneral::FillUVData(UDynamicMesh* TargetMesh, int32 UVLayerNum)
{
	FName TestAttribute = "TestAttribute";
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		if (EditMesh.HasAttributes() == false)
		{
			EditMesh.EnableAttributes();
		}
		if (UVLayerNum != EditMesh.Attributes()->NumUVLayers())
		{
			EditMesh.Attributes()->SetNumUVLayers(UVLayerNum);
		}
		bool a = EditMesh.HasAttributes();
		int32 b =  EditMesh.Attributes()->NumUVLayers();
		FDynamicMeshUVOverlay* UVLayer = EditMesh.Attributes()->GetUVLayer(2);
		FDynamicMeshColorOverlay* ColorOverlay = EditMesh.Attributes()->PrimaryColors();
		UVLayer->InitializeTriangles(EditMesh.MaxTriangleID());
		// UVLayer.
		FDynamicMeshUVOverlay* UVLayer0 = EditMesh.Attributes()->GetUVLayer(0);
		UVLayer->ElementIndicesItr();

		for (int ElementID : UVLayer0->ElementIndicesItr())
		{
			FVector2f Test = UVLayer0->GetElement(ElementID);
		}
		FDynamicMeshUVEditor UVEditor(&EditMesh, UVLayer);
		UVEditor.CopyUVLayer(UVLayer0);
		for (int ElementID : UVLayer->ElementIndicesItr())
		{
			UVLayer->SetElement(ElementID, FVector2f(0, 0));
			FVector2f Test = UVLayer->GetElement(ElementID);
		}

		for (int32 TriangleID : EditMesh.TriangleIndicesItr())
		{
			if (UVLayer0->IsSetTriangle(TriangleID))
			{

				FIndex3i TriangleVertex = EditMesh.GetTriangle(TriangleID);


				FIndex3i TriangleUV = UVLayer->GetTriangle(TriangleID);
				float VertexNum = UVLayer->ElementCount();
				for (int32 i = 0; i < 3; i++)
				{
					FGeometryScriptIndexList VertexIDList;
					bool Errors = 0;
					UGeometryScriptLibrary_MeshGeodesicFunctions::GetShortestVertexPath(TargetMesh,TriangleVertex[i],int32(EditMesh.VertexCount() / 2),VertexIDList,Errors,nullptr);

					if (VertexIDList.List.IsValid())
					{
						TArray<FVector> PathPositions;
						PathPositions.Reset(VertexIDList.List->Num());

					
						for (int32 Idx : *VertexIDList.List)
						{
							PathPositions.Add((FVector)EditMesh.GetVertex(TriangleVertex[i]));
						}
						float PathLength = 0;
						for (int32 k = 0; k < PathPositions.Num() - 1; k++)
						{
							PathLength += FVector::Distance(PathPositions[k], PathPositions[k + 1]);
						}
					}



					FVector3d VertexPos = EditMesh.GetVertex(TriangleVertex[i]);
					FVector3d PreVertex = VertexPos;
					VertexPos *= .02;
					FVector2f UVData = FVector2f( FMath::Abs(VertexPos.X) / VertexNum, FMath::Abs(VertexPos.Y) / VertexNum);
					UVLayer->SetElement(TriangleUV[i], UVData);
					
				}
				
				FIndex3i TriangleColor = ColorOverlay->GetTriangle(TriangleID);
				for (int32 i = 0; i < 3; i++)
				{
					
					FVector3d VertexPos = EditMesh.GetVertex(TriangleVertex[i]);
					FVector3d PreVertex = VertexPos;
					ColorOverlay->SetElement(TriangleUV[i], FVector4f(VertexPos.X, VertexPos.Y, VertexPos.Z, 0));
					
				}
			}
		}
		// for (int32 TriangleID : EditMesh.TriangleIndicesItr())
		// {
		// 	if (UVLayer0->IsSetTriangle(TriangleID))
		// 	{
		// 		FIndex3i UVTriangle = UVLayer0->GetTriangle(TriangleID);
		// 		UVTriangle.A = ElementIDMap[UVTriangle.A];
		// 		UVTriangle.B = ElementIDMap[UVTriangle.B];
		// 		UVTriangle.C = ElementIDMap[UVTriangle.C];
		// 		UVOverlay->SetTriangle(TriangleID, UVTriangle);
		// 	}
		// }
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::AddCustomAttribute(UDynamicMesh* TargetMesh, FName AttributeName)
{

	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		EditMesh.EnableAttributes();
		VI_ATTR(Class, EditMesh, 0);
		TI_ATTR(Class, EditMesh, 0);

		for (int32 VID : EditMesh.VertexIndicesItr())
		{
			int TestData = 1;
			AVI_Class->SetValue(VID, &TestData);
		}
		
		for (int32 TID : EditMesh.TriangleIndicesItr())
		{
			int TestData = 1;
			FIndex3i TVIDs = EditMesh.GetTriangle(TID);
			for (int32 i = 0; i < 3; i++)
			{
				ATI_Class->SetValue(TVIDs[i], &TestData);
			}
		}
		
		for (int32 TID : EditMesh.TriangleIndicesItr())
		{
			int TestData = 0;
			FIndex3i TVIDs = EditMesh.GetTriangle(TID);
			for (int32 i = 0; i < 3; i++)
			{
				ATI_Class->GetValue(TVIDs[i], &TestData);
				TestData = 0;
			}
		}

		for (int32 VID : EditMesh.VertexIndicesItr())
		{
			int TestData = 0;
			AVI_Class->GetValue(VID, &TestData);
			TestData = 0;
		}

		//
		//
		// FVector Sum = FVector::ZeroVector;
		// FVector SumNormal = FVector::ZeroVector;
		// FIndex3i T0 = EditMesh.GetTriangle(0);
		// FDynamicMeshNormalOverlay* NormalOverlay = EditMesh.Attributes()->PrimaryNormals();
		// FIndex3i TN0 = NormalOverlay->GetTriangle(0);
		// for (int32 i = 0; i < 3; i++)
		// {
		// 	FVector Pos = EditMesh.GetVertex(T0[i]);
		// 	Sum += Pos;
		// 	SumNormal = (FVector)NormalOverlay->GetElement(TN0[i]) ;
		// }
		// Sum /= 3.0;
		// SumNormal *= 100;
		//
		//
		// FGeometryScriptDynamicMeshBVH BVH;
		// UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(TargetMesh, BVH, nullptr);
		//
		// FGeometryScriptRayHitResult HitResult;
		//
		//
		// IMeshSpatial::FQueryOptions QueryOptions;
		// QueryOptions.MaxDistance = TNumericLimits<float>::Max();
		// FRay3d Ray((FVector3d)Sum, Normalized((FVector3d)SumNormal));
		// int HitTID = BVH.Spatial->FindNearestHitTriangle(Ray, QueryOptions);
		// if (HitTID >= 0)
		// {
		// 	FIntrRay3Triangle3d Intersection = TMeshQueries<FDynamicMesh3>::TriangleIntersection(EditMesh, HitTID, Ray);
		// 	HitResult.RayParameter = Intersection.RayParameter;
		// 	HitResult.bHit = true;
		// 	HitResult.HitTriangleID = HitTID;
		// 	HitResult.HitPosition = Ray.PointAt(Intersection.RayParameter);
		// 	HitResult.HitBaryCoords = (FVector)Intersection.TriangleBaryCoords;
		// }
		//
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return nullptr;
}

FVector UGeometryGeneral::GetNearestLocationNormal(FDynamicMesh3& EditMesh, FGeometryScriptTrianglePoint NearestPoint)
{
	if (EditMesh.HasVertexNormals())
	{
		FIndex3i TriVertexIDs = EditMesh.GetTriangle(NearestPoint.TriangleID);
		FVector N = FVector(NearestPoint.BaryCoords[0] * EditMesh.GetVertexNormal(TriVertexIDs[0]) + NearestPoint.BaryCoords[1] * EditMesh.GetVertexNormal(TriVertexIDs[1]) + NearestPoint.BaryCoords[2] * EditMesh.GetVertexNormal(TriVertexIDs[2]));
		N.Normalize();
		return N;
	}
	else
	{
		FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();

		FIndex3i TVidNormal =  Normals->GetTriangle(NearestPoint.TriangleID);
		FIndex3i TVid = EditMesh.GetTriangle(NearestPoint.TriangleID);
		FVector VertexNormals[3];
		for (int32 i = 0; i < 3; ++i)
		{
			if (TVidNormal[i] < 0 || !EditMesh.IsVertex(TVid[i]))
				continue;
			FVector3f PreNormal = EditMesh.GetVertexNormal(TVid[i]);
			FVector Normal = (FVector)Normals->GetElement(TVidNormal[i]);
			VertexNormals[i] = Normal;
		}
	
		FVector N = FVector(NearestPoint.BaryCoords[0] * VertexNormals[0] + NearestPoint.BaryCoords[1] * VertexNormals[1] + NearestPoint.BaryCoords[2] * VertexNormals[2]);
		N.Normalize();
	
		return N;
	}
}


static void ApplyPrimitiveOptionsToMesh(
	FDynamicMesh3& Mesh, const FTransform& Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions, 
	FVector3d PreTranslate = FVector3d::Zero(),
	TOptional<FQuaterniond> PreRotate = TOptional<FQuaterniond>())
{
	bool bHasTranslate = PreTranslate.SquaredLength() > 0;
	if (PreRotate.IsSet())
	{
		FFrame3d Frame(PreTranslate, *PreRotate);
		MeshTransforms::FrameCoordsToWorld(Mesh, Frame);
	}
	else if (bHasTranslate)
	{
		MeshTransforms::Translate(Mesh, PreTranslate);
	}

	MeshTransforms::ApplyTransform(Mesh, (FTransformSRT3d)Transform, true);
	if (PrimitiveOptions.PolygroupMode == EGeometryScriptPrimitivePolygroupMode::SingleGroup)
	{
		for (int32 tid : Mesh.TriangleIndicesItr())
		{
			Mesh.SetTriangleGroup(tid, 0);
		}
	}
	if (PrimitiveOptions.bFlipOrientation)
	{
		Mesh.ReverseOrientation(true);
		if (Mesh.HasAttributes())
		{
			FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
			for (int elemid : Normals->ElementIndicesItr())
			{
				Normals->SetElement(elemid, -Normals->GetElement(elemid));
			}
		}
	}
}


void UGeometryGeneral::AppendPrimitive(
	UDynamicMesh* TargetMesh,
	FMeshShapeGenerator* Generator, 
	FTransform Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions,
	FVector3d PreTranslate,
	TOptional<FQuaterniond> PreRotate)
{
	if (TargetMesh->IsEmpty())
	{
		TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			EditMesh.Copy(Generator);
			ApplyPrimitiveOptionsToMesh(EditMesh, Transform, PrimitiveOptions, PreTranslate, PreRotate);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
	else
	{
		FDynamicMesh3 TempMesh(Generator);
		ApplyPrimitiveOptionsToMesh(TempMesh, Transform, PrimitiveOptions, PreTranslate, PreRotate);
		TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			FMeshIndexMappings TmpMappings;
			FDynamicMeshEditor Editor(&EditMesh);
			Editor.AppendMesh(&TempMesh, TmpMappings);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
}


void UGeometryGeneral::CalculateOBBUpDir(TArray<FVector> OrientSpaceVertices, FVector& BoxUpDir)
{
	FVector ObjectUpVector = FVector(0, 0, 1);
	FOrientedBox3d Box;
	FMinVolumeBox3d MinBoxCalc;
	bool bMinBoxOK = MinBoxCalc.Solve(OrientSpaceVertices.Num(),[&](int32 Index) { return OrientSpaceVertices[Index]; });
	
	if (!bMinBoxOK || !MinBoxCalc.IsSolutionAvailable()) return;
	MinBoxCalc.GetResult(Box);
	
	FVector3d Extents = Box.Extents;
	FVector3d Center = Box.Frame.Origin;
	FQuaterniond Quat = Box.Frame.Rotation;
	// OutCenter = FVector(Center.X, Center.Y, Center.Z);
	// OutExtent = FVector(Extents.X, Extents.Y, Extents.Z);
	// OutRotator = FQuat(Quat.X, Quat.Y, Quat.Z, Quat.W).Rotator();

	float DotObjectCheck = -1;
	float DotUpCheck = -1;
	float FAxisDot = 0;
	float UpDot = 0;
	// FVector ObjectUpDir;
	// FVector UpVectorUpDir;

	FVector3d Corner0 = Box.GetCorner(0);
	FVector3d Corner1 = Box.GetCorner(1);
	FVector3d Corner3 = Box.GetCorner(3);
	FVector3d Corner4 = Box.GetCorner(4);
	FVector3d Axis0 = Corner1 - Corner0;
	FVector3d Axis1 = Corner3 - Corner0;
	FVector3d Axis2 = Corner4 - Corner0;
	// FVector3d BoxUpDir;
	
	if (Axis0.Length() < Axis1.Length())
	{
		BoxUpDir = Axis0;
	}
	else
	{
		BoxUpDir = Axis1;
	}
	if (Axis2.Length() < BoxUpDir.Length())
	{
		BoxUpDir = Axis2;
	}
	
	BoxUpDir = BoxUpDir.GetSafeNormal();
	float Dotd = BoxUpDir.Dot(ObjectUpVector);
	if (Dotd < 0)
	{
		BoxUpDir *= -1;
	}
	BoxUpDir = (FVector)BoxUpDir;
	//
	// for (int32 i = 0; i < 3; i++)
	// {
	// 	FVector3d Axis = Box.GetAxis(i);
	// 	FVector FAxis = FVector(Axis.X, Axis.Y, Axis.Z);
	//
	// 	FAxisDot = FVector::DotProduct(ObjectUpVector, FAxis);
	// 	UpDot = FVector::DotProduct(UpVector, FAxis);
	// 	if (FAxisDot > DotObjectCheck)
	// 	{
	// 		ObjectUpDir = FAxis;
	// 		DotObjectCheck = FAxisDot;
	// 	}
	// 	if (UpDot > DotUpCheck)
	// 	{
	// 		UpVectorUpDir = FAxis;
	// 		DotUpCheck = UpDot;
	// 	}
	// 	FAxisDot = FVector::DotProduct(ObjectUpVector, -FAxis);
	// 	UpDot = FVector::DotProduct(UpVector, -FAxis);
	// 	if (FAxisDot > DotObjectCheck)
	// 	{
	// 		ObjectUpDir = -FAxis;
	// 		DotObjectCheck = FAxisDot;
	// 	}
	// 	if (UpDot > DotUpCheck)
	// 	{
	// 		UpVectorUpDir = FAxis;
	// 		DotUpCheck = UpDot;
	// 	}
	// }
	// float QuadrateThreshold = 11;
	// if (FMath::Abs(Axis0.Length() - Axis1.Length()) < QuadrateThreshold || FMath::Abs(Axis0.Length() - Axis2.Length()) < QuadrateThreshold || FMath::A
}