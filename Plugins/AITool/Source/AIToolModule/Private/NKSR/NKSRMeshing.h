#pragma once

// Dual marching-cubes extraction (mirror of _C.meshing, CPU path) + MISE subdivision + trim.
// Port spec: E_meshing_solver.md §meshing, A_inference_flow.md §extract_dual_mesh.
// MC tables live in NKSRMCTables.h (copied verbatim from csrc/meshing/mc_data.h).
// GC-7 sign conventions apply throughout.

#include "NKSRCommon.h"

class FNKSRIndexGrid;

/**
 * build_flattened_grid: removes voxels covered by the finer level (FinerGrid may be null for d==0)
 * and, when bConforming (d != depth-1), pads partially-refined octants with sibling voxels.
 */
FNKSRIndexGrid NKSRBuildFlattenedGrid(const FNKSRIndexGrid& Grid, const FNKSRIndexGrid* FinerGrid, bool bConforming);

/** build_joint_dual_grid: dual lattice (voxelSize = vs0, origin = org0 - 0.5*vs0) at all flattened-voxel corners. */
FNKSRIndexGrid NKSRBuildJointDualGrid(TConstArrayView<const FNKSRIndexGrid*> FlattenedGrids);

/**
 * dual_cube_graph: per dual cell, the 8 primal voxels at its corners as GLOBAL vertex indices
 * (per-level voxel index + prefix over non-empty levels in ascending depth). Rows with any
 * missing corner are dropped. OutGraph: flattened [NumCubes][8] int64.
 */
void NKSRDualCubeGraph(TConstArrayView<const FNKSRIndexGrid*> FlattenedGrids, const FNKSRIndexGrid& DualGrid, TArray<int64>& OutGraph);

/**
 * marching_cubes over the cube graph: CornerPos/CornerValue indexed by the graph's global indices.
 * cubeType bit i set iff CornerValue < 0 (GC-7). Vertices welded on sorted global corner-index pairs.
 */
void NKSRMarchingCubes(
	TConstArrayView<int64> CubeGraph,
	TConstArrayView<FVector3f> CornerPos,
	TConstArrayView<float> CornerValue,
	TArray<FVector3f>& OutV,
	TArray<FIntVector>& OutF);

/**
 * MISE step (utils.subdivide_cube_indices): splits every cube 8-ways with dedup'd edge / face /
 * center vertices; degenerate (collapsed) edges and faces reuse the first corner index.
 * CubeGraph/Vertices are updated in place. Corner-label conventions per spec A §MISE.
 */
void NKSRSubdivideCubeIndices(TArray<int64>& CubeGraph, TArray<FVector3f>& Vertices);

/**
 * MISE filtering helper: keeps cubes whose corner values cross the level set (Value > 0 test, GC-7),
 * compacts the vertex set with sorted-unique + inverse remap (GC-6).
 */
void NKSRFilterCrossingCubes(TArray<int64>& CubeGraph, TArray<FVector3f>& Vertices, TArray<float>& Values);

/** apply_vertex_mask: keep vertices with Keep[v], drop faces touching removed vertices, remap indices. */
void NKSRApplyVertexMask(TArray<FVector3f>& V, TArray<FIntVector>& F, TConstArrayView<bool> Keep);
