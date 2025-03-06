// Fill out your copyright notice in the Description page of Project Settings.


#include "GeometryGeneral.h"

#include "DynamicMeshEditor.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/MeshNormals.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshSpatialFunctions.h"
#include "Operations/SmoothDynamicMeshAttributes.h"

using namespace UE::Geometry;

UDynamicMesh* UGeometryGeneral::BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration, bool RecomputeNormals)
{
	FGeometryScriptCalculateNormalsOptions CalculateOptions;
	if (RecomputeNormals)
	{
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(TargetMesh, CalculateOptions);
	}
	
	FVector3f normaltestpre;
	FVector3f normaltestaffter;
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
