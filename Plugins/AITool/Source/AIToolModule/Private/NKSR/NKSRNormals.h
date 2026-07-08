#pragma once

// Point-cloud normal estimation: kNN PCA + consistent orientation (MST propagation).
// Replaces the python-side open3d estimate_normals + orient_normals_consistent_tangent_plane.
// Deterministic: neighbor sets and MST tie-breaks are ordered by (weight, indexA, indexB).

#include "NKSRCommon.h"

/**
 * Estimates unit normals for Points via k-nearest-neighbor PCA (smallest eigenvector),
 * then orients them consistently: Riemannian kNN graph with edge weight 1 - |n_i . n_j|,
 * minimum spanning tree, BFS flip propagation from the point with max Z.
 * Fails (false + OutError) when Points.Num() < 3 or K < 3.
 */
bool NKSREstimateNormals(TConstArrayView<FVector3f> Points, int32 K, TArray<FVector3f>& OutNormals, FString& OutError);
