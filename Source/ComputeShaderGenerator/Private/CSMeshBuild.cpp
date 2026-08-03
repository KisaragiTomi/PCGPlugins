#include "CSMeshBuild.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/MeshNormalsFunctions.h"

// NOTE: helpers are given CSMB_-prefixed names on purpose. UE unity/jumbo builds
// concatenate this .cpp with sibling module .cpp files (e.g. ComputeShaderMeshGenerator.cpp)
// into one translation unit, so an anonymous-namespace helper that merely shared a name
// with one there would be a redefinition. Unique names keep this unit unity-safe.
namespace
{
UDynamicMesh* CSMB_CreateEmptyDynamicMesh()
{
	UDynamicMesh* OutMesh = NewObject<UDynamicMesh>();
	if (OutMesh)
	{
		OutMesh->Reset();
	}
	return OutMesh;
}

bool CSMB_IsValidTriangleIndex(int32 Index, int32 VertexCount)
{
	return Index >= 0 && Index < VertexCount;
}

bool CSMB_IsFiniteVertex(const FVector& Vertex)
{
	return !Vertex.ContainsNaN()
		&& FMath::IsFinite(Vertex.X)
		&& FMath::IsFinite(Vertex.Y)
		&& FMath::IsFinite(Vertex.Z);
}

bool CSMB_IsDegenerateTriangle(const FVector& A, const FVector& B, const FVector& C)
{
	const FVector AB = B - A;
	const FVector AC = C - A;
	const double AreaSq4 = FVector::CrossProduct(AB, AC).SizeSquared();
	return AreaSq4 <= 1.0e-8;
}
}

UDynamicMesh* CSMeshBuild::BuildDynamicMeshFromCSTriangleData(const TArray<FVector>& Vertices,
	const TArray<int32>& Indices,
	const TArray<FVector>& VertexNormals,
	int32 VertexCount,
	int32 IndexCount,
	bool bReverseOrientation,
	bool bSkipDegenerateTriangles,
	bool bRecomputeNormals)
{
	UDynamicMesh* OutMesh = CSMB_CreateEmptyDynamicMesh();
	if (!OutMesh)
	{
		return nullptr;
	}

	const int32 EffectiveVertexCount = VertexCount >= 0
		? FMath::Clamp(VertexCount, 0, Vertices.Num())
		: Vertices.Num();
	const int32 EffectiveIndexCount = IndexCount >= 0
		? FMath::Clamp(IndexCount, 0, Indices.Num())
		: Indices.Num();

	if (EffectiveVertexCount < 3)
	{
		return OutMesh;
	}

	const bool bUseTriangleSoup = EffectiveIndexCount == 0;
	const int32 TriangleCount = bUseTriangleSoup ? EffectiveVertexCount / 3 : EffectiveIndexCount / 3;
	if (TriangleCount <= 0)
	{
		return OutMesh;
	}

	const bool bUseInputVertexNormals = !bRecomputeNormals && VertexNormals.Num() >= EffectiveVertexCount;

	UE::Geometry::FDynamicMesh3 Mesh;
	TArray<int32> VertexIDMap;
	VertexIDMap.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector& Vertex = Vertices[VertexIndex];
		if (CSMB_IsFiniteVertex(Vertex))
		{
			VertexIDMap.Add(Mesh.AppendVertex(FVector3d(Vertex)));
		}
		else
		{
			VertexIDMap.Add(INDEX_NONE);
		}
	}

	if (bUseInputVertexNormals)
	{
		Mesh.EnableVertexNormals(FVector3f::UpVector);
		for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
		{
			const int32 MeshVertexID = VertexIDMap[VertexIndex];
			if (MeshVertexID == INDEX_NONE || VertexNormals[VertexIndex].ContainsNaN())
			{
				continue;
			}

			const FVector SafeNormal = VertexNormals[VertexIndex].GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			Mesh.SetVertexNormal(MeshVertexID, FVector3f(SafeNormal));
		}
	}

	int32 AddedTriangles = 0;
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		int32 A = INDEX_NONE;
		int32 B = INDEX_NONE;
		int32 C = INDEX_NONE;

		if (bUseTriangleSoup)
		{
			A = TriangleIndex * 3 + 0;
			B = TriangleIndex * 3 + 1;
			C = TriangleIndex * 3 + 2;
		}
		else
		{
			A = Indices[TriangleIndex * 3 + 0];
			B = Indices[TriangleIndex * 3 + 1];
			C = Indices[TriangleIndex * 3 + 2];
		}

		if (!CSMB_IsValidTriangleIndex(A, EffectiveVertexCount)
			|| !CSMB_IsValidTriangleIndex(B, EffectiveVertexCount)
			|| !CSMB_IsValidTriangleIndex(C, EffectiveVertexCount)
			|| VertexIDMap[A] == INDEX_NONE
			|| VertexIDMap[B] == INDEX_NONE
			|| VertexIDMap[C] == INDEX_NONE
			|| A == B || B == C || A == C)
		{
			continue;
		}

		if (bSkipDegenerateTriangles && CSMB_IsDegenerateTriangle(Vertices[A], Vertices[B], Vertices[C]))
		{
			continue;
		}

		if (bReverseOrientation)
		{
			// DynamicMeshComponent front-face rendering and the collected source-normal triangle
			// buffer currently use opposite winding conventions. Keep this explicit conversion
			// at the DynamicMesh boundary instead of changing the source triangle data.
			Swap(B, C);
		}

		const int32 NewTriangleID = Mesh.AppendTriangle(UE::Geometry::FIndex3i(VertexIDMap[A], VertexIDMap[B], VertexIDMap[C]), 0);
		if (NewTriangleID >= 0)
		{
			++AddedTriangles;
		}
	}

	if (AddedTriangles == 0)
	{
		return OutMesh;
	}

	OutMesh->SetMesh(MoveTemp(Mesh));

	if (bRecomputeNormals)
	{
		FGeometryScriptCalculateNormalsOptions CalculateOptions;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(OutMesh, CalculateOptions);
	}

	return OutMesh;
}
