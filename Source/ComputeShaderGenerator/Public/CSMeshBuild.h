#pragma once

#include "CoreMinimal.h"

class UCSMesh;
struct FCSGpuMeshCPUData;

// -----------------------------------------------------------------------------
// Shared CS-triangle -> UCSMesh builder
//
// Single source of truth for turning CPU-side CS triangle data (triangle soup or
// indexed) into a GPU mesh object. Callers: ComputeShaderMeshGenerator.cpp and
// GeometryGenerate.cpp.
//
// It produces a UCSMesh and nothing else. Callers that want a UDynamicMesh run the
// result through UCSMeshOps::CopyToDynamicMesh — one bridge out of the GPU pipeline
// rather than one per producer, so a fix to the conversion (material ids, colours,
// per-corner attributes) reaches every path instead of the one that was edited.
// -----------------------------------------------------------------------------
namespace CSMeshBuild
{
	// -------------------------------------------------------------------------
	// 常驻流的面法线口径：cross(B-A, C-A)。所有"从三角形推法线"的代码都必须从这里取，
	// 别再各自写一遍叉积 —— 写反了不会报错，只会让整块网格朝里。
	//
	// 这个口径与 UE StaticMesh / FDynamicMesh3 **差一个负号**：那两边取的是反向叉积
	// cross(C-A, B-A)（见 CSGpuMeshComponent.cpp 里 MeshDescription 转换的 FaceNormal，
	// 以及引擎 VectorUtil::NormalArea 的 "Unreal has Left-Hand Coordinate System" 注释）。
	//
	// 这个差别是真实的、承重的，不能"统一掉"：常驻流的绕序本来就与 DynamicMeshComponent
	// 的正面约定相反，BuildSnapshotFromCSTriangleData 的 bReverseOrientation 正是这条
	// 边界上的显式转换。CSGpuMeshObjectTests 的 bridge 用例（XY 平面顺时针四边形必须朝
	// -Z）把这个口径钉死了。要改口径，必须连同所有 bReverseOrientation 调用方和那个用例
	// 一起改，只改这里会让两个 sink 同时错。
	template <typename T>
	UE::Math::TVector<T> ResidentFaceNormalScaled(
		const UE::Math::TVector<T>& A, const UE::Math::TVector<T>& B, const UE::Math::TVector<T>& C)
	{
		// 不归一化：模长 = 2×三角面积，逐顶点累加时这就是面积权重本身。
		return UE::Math::TVector<T>::CrossProduct(B - A, C - A);
	}

	// 同一口径的单位面法线。退化三角回退 +Z，与逐顶点累加的兜底一致。
	template <typename T>
	UE::Math::TVector<T> ResidentFaceNormal(
		const UE::Math::TVector<T>& A, const UE::Math::TVector<T>& B, const UE::Math::TVector<T>& C)
	{
		return ResidentFaceNormalScaled(A, B, C).GetSafeNormal(UE_SMALL_NUMBER, UE::Math::TVector<T>::UpVector);
	}

	// 面积加权的逐顶点法线，本模块唯一的一份法线累加实现。
	//
	// 逐顶点、按索引累加：共享同一个索引的角点会被完全平滑，没有角度阈值、没有平滑组、
	// 也不做位置焊接 —— 也就是说"硬边"完全由调用方的索引缓冲决定。传三角汤（IndexCount==0，
	// 每个角点各自成点）进来时，同一份代码给出的就是逐面平坦法线；VDB 的平坦档正是这么用的。
	//
	// 与 UCSMeshOps::CopyToDynamicMesh 里那条 FMeshNormals 路径**不是**同一种加权：那边是
	// 面积×角度双重加权、且用 UE 的反向叉积口径。两者服务不同的 sink（常驻流 vs
	// FDynamicMesh3），刻意保持独立，不要合并。
	COMPUTESHADERGENERATOR_API void ComputeAreaWeightedVertexNormals(
		const TArray<FVector3f>& Positions,
		const TArray<uint32>& Indices,
		TArray<FVector3f>& OutNormals);

	// Vertices / Indices / VertexNormals: CPU triangle data.
	// VertexCount / IndexCount < 0 mean "use the array length".
	// IndexCount == 0 => interpret Vertices as a triangle soup (0/1/2, 3/4/5, ...).
	// bReverseOrientation swaps winding; bSkipDegenerateTriangles drops zero-area triangles;
	// bRecomputeNormals ignores VertexNormals and derives them from the triangles.
	//
	// Fills every array FCSGpuMeshCPUData::IsValid() demands, tangents and UV0 included even
	// though CS triangle data carries neither: a snapshot one array short is rejected wholesale
	// by CopyFromMeshSnapshot, and the rejection says nothing about which array was missing.
	// Returns false when nothing survives sanitising (no finite vertices, no triangles).
	COMPUTESHADERGENERATOR_API bool BuildSnapshotFromCSTriangleData(
		const TArray<FVector>& Vertices,
		const TArray<int32>& Indices,
		const TArray<FVector>& VertexNormals,
		int32 VertexCount,
		int32 IndexCount,
		bool bReverseOrientation,
		bool bSkipDegenerateTriangles,
		bool bRecomputeNormals,
		FCSGpuMeshCPUData& OutSnapshot);

	// Same inputs, uploaded straight into TargetMesh, replacing its contents. A null TargetMesh
	// allocates one under Outer (null Outer parks it on the transient package), the same
	// "pass null to get one" contract UCSMeshOps::CopyToDynamicMesh uses.
	//
	// Empty input empties the mesh instead of returning null, because callers chain on the
	// result; only a failed allocation gives back null.
	COMPUTESHADERGENERATOR_API UCSMesh* BuildGpuMeshFromCSTriangleData(
		UCSMesh* TargetMesh,
		UObject* Outer,
		const TArray<FVector>& Vertices,
		const TArray<int32>& Indices,
		const TArray<FVector>& VertexNormals,
		int32 VertexCount,
		int32 IndexCount,
		bool bReverseOrientation,
		bool bSkipDegenerateTriangles,
		bool bRecomputeNormals);
}
