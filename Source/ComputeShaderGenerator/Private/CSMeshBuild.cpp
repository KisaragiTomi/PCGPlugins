#include "CSMeshBuild.h"

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"

// NOTE: helpers are given CSMB_-prefixed names on purpose. UE unity/jumbo builds
// concatenate this .cpp with sibling module .cpp files (e.g. ComputeShaderMeshGenerator.cpp)
// into one translation unit, so an anonymous-namespace helper that merely shared a name
// with one there would be a redefinition. Unique names keep this unit unity-safe.
namespace
{
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

// 面积加权的逐顶点法线。既服务显式的重算请求，也服务"源数据根本没有法线"的情况 ——
// 常驻流永远带切线基，"没有法线"不是 GPU mesh 能保持的状态，交给
// CopyFromMeshSnapshot 的逐顶点兜底会把整块网格当成朝 +Z 来着色。
//
// 加权方式与口径的完整说明（以及为什么不能和 CopyToDynamicMesh 那条路合并）见头文件。
void CSMeshBuild::ComputeAreaWeightedVertexNormals(
	const TArray<FVector3f>& Positions, const TArray<uint32>& Indices, TArray<FVector3f>& OutNormals)
{
	OutNormals.Init(FVector3f::ZeroVector, Positions.Num());
	for (int32 Corner = 0; Corner + 2 < Indices.Num(); Corner += 3)
	{
		const int32 I0 = int32(Indices[Corner + 0]);
		const int32 I1 = int32(Indices[Corner + 1]);
		const int32 I2 = int32(Indices[Corner + 2]);
		if (!Positions.IsValidIndex(I0) || !Positions.IsValidIndex(I1) || !Positions.IsValidIndex(I2)) continue;

		const FVector3f Face = CSMeshBuild::ResidentFaceNormalScaled(Positions[I0], Positions[I1], Positions[I2]);
		OutNormals[I0] += Face;
		OutNormals[I1] += Face;
		OutNormals[I2] += Face;
	}

	for (FVector3f& Normal : OutNormals) Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
}

bool CSMeshBuild::BuildSnapshotFromCSTriangleData(const TArray<FVector>& Vertices,
	const TArray<int32>& Indices,
	const TArray<FVector>& VertexNormals,
	int32 VertexCount,
	int32 IndexCount,
	bool bReverseOrientation,
	bool bSkipDegenerateTriangles,
	bool bRecomputeNormals,
	FCSGpuMeshCPUData& OutSnapshot)
{
	OutSnapshot.Reset();

	const int32 EffectiveVertexCount = VertexCount >= 0
		? FMath::Clamp(VertexCount, 0, Vertices.Num())
		: Vertices.Num();
	const int32 EffectiveIndexCount = IndexCount >= 0
		? FMath::Clamp(IndexCount, 0, Indices.Num())
		: Indices.Num();

	if (EffectiveVertexCount < 3) return false;

	const bool bUseTriangleSoup = EffectiveIndexCount == 0;
	const int32 TriangleCount = bUseTriangleSoup ? EffectiveVertexCount / 3 : EffectiveIndexCount / 3;
	if (TriangleCount <= 0) return false;

	const bool bUseInputVertexNormals = !bRecomputeNormals && VertexNormals.Num() >= EffectiveVertexCount;

	// Non-finite vertices are dropped, and dropping renumbers: unlike FDynamicMesh3 there is no
	// "hole" a resident index buffer could point at, so the surviving positions are compacted and
	// VertexIDMap carries the old -> new mapping for the triangle loop.
	TArray<int32> VertexIDMap;
	VertexIDMap.SetNumUninitialized(EffectiveVertexCount);
	OutSnapshot.Positions.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector& Vertex = Vertices[VertexIndex];
		if (!CSMB_IsFiniteVertex(Vertex))
		{
			VertexIDMap[VertexIndex] = INDEX_NONE;
			continue;
		}

		VertexIDMap[VertexIndex] = OutSnapshot.Positions.Num();
		OutSnapshot.Positions.Add(FVector3f(Vertex));
	}

	const int32 KeptVertexCount = OutSnapshot.Positions.Num();
	if (KeptVertexCount < 3) return false;

	OutSnapshot.Indices.Reserve(TriangleCount * 3);
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

		if (bSkipDegenerateTriangles && CSMB_IsDegenerateTriangle(Vertices[A], Vertices[B], Vertices[C])) continue;

		// DynamicMeshComponent front-face rendering and the collected source-normal triangle buffer
		// use opposite winding conventions. Keep this explicit conversion at the mesh-object
		// boundary instead of changing the source triangle data.
		if (bReverseOrientation) Swap(B, C);

		OutSnapshot.Indices.Add(uint32(VertexIDMap[A]));
		OutSnapshot.Indices.Add(uint32(VertexIDMap[B]));
		OutSnapshot.Indices.Add(uint32(VertexIDMap[C]));
	}

	if (OutSnapshot.Indices.Num() < 3) return false;

	if (bUseInputVertexNormals)
	{
		OutSnapshot.Normals.Init(FVector3f::UnitZ(), KeptVertexCount);
		for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
		{
			const int32 Mapped = VertexIDMap[VertexIndex];
			if (Mapped == INDEX_NONE || VertexNormals[VertexIndex].ContainsNaN()) continue;
			OutSnapshot.Normals[Mapped] = FVector3f(VertexNormals[VertexIndex].GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector));
		}
	}
	else
	{
		CSMeshBuild::ComputeAreaWeightedVertexNormals(OutSnapshot.Positions, OutSnapshot.Indices, OutSnapshot.Normals);
	}

	// Tangents and UV0 are placeholders: CS triangle data has neither, but IsValid() requires
	// both to match the position count. CopyFromMeshSnapshot re-orthogonalises the tangent
	// against the normal and substitutes a perpendicular axis where that collapses, so a
	// constant seed here is the same basis a "no tangents supplied" snapshot would end up with.
	OutSnapshot.Tangents.Init(FVector3f::UnitX(), KeptVertexCount);
	OutSnapshot.TexCoords().Init(FVector2f::ZeroVector, KeptVertexCount);
	OutSnapshot.NumTexCoordChannels = 1;
	OutSnapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	OutSnapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	return true;
}

UCSMesh* CSMeshBuild::BuildGpuMeshFromCSTriangleData(UCSMesh* TargetMesh,
	UObject* Outer,
	const TArray<FVector>& Vertices,
	const TArray<int32>& Indices,
	const TArray<FVector>& VertexNormals,
	int32 VertexCount,
	int32 IndexCount,
	bool bReverseOrientation,
	bool bSkipDegenerateTriangles,
	bool bRecomputeNormals)
{
	if (!TargetMesh) TargetMesh = UCSMeshOps::AllocateGpuMesh(Outer, 3, 3);
	if (!TargetMesh) return nullptr;

	FCSGpuMeshCPUData Snapshot;
	if (!BuildSnapshotFromCSTriangleData(Vertices, Indices, VertexNormals, VertexCount, IndexCount,
		bReverseOrientation, bSkipDegenerateTriangles, bRecomputeNormals, Snapshot))
	{
		// Nothing to upload. Reset rather than leaving the previous contents behind: this call is
		// a replacement, and a caller that got "no geometry" must not go on drawing the old mesh.
		TargetMesh->Reset();
		return TargetMesh;
	}

	UCSMeshOps::CopyFromMeshSnapshot(TargetMesh, Snapshot);
	return TargetMesh;
}
