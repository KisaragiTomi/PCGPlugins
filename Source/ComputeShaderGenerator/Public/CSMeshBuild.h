#pragma once

#include "CoreMinimal.h"

class UDynamicMesh;

// -----------------------------------------------------------------------------
// Shared CS-triangle -> UDynamicMesh builder
//
// Single source of truth for turning CPU-side CS triangle data (triangle soup or
// indexed) into a UDynamicMesh. Callers: ComputeShaderMeshGenerator.cpp and
// GeometryGenerate.cpp.
// -----------------------------------------------------------------------------
namespace CSMeshBuild
{
	// Vertices / Indices / VertexNormals: CPU triangle data.
	// VertexCount / IndexCount < 0 mean "use the array length".
	// IndexCount == 0 => interpret Vertices as a triangle soup (0/1/2, 3/4/5, ...).
	// bReverseOrientation swaps winding at the DynamicMesh boundary; bSkipDegenerateTriangles
	// drops zero-area triangles; bRecomputeNormals recomputes normals after building.
	COMPUTESHADERGENERATOR_API UDynamicMesh* BuildDynamicMeshFromCSTriangleData(
		const TArray<FVector>& Vertices,
		const TArray<int32>& Indices,
		const TArray<FVector>& VertexNormals,
		int32 VertexCount,
		int32 IndexCount,
		bool bReverseOrientation,
		bool bSkipDegenerateTriangles,
		bool bRecomputeNormals);
}
