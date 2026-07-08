#pragma once

// SparseFeatureHierarchy port (svh.py). Level d: voxelSize_d = VoxelSize * 2^d,
// origin_d = 0.5 * voxelSize_d (voxel corners align with the world origin).
// Port specs: A_inference_flow.md §svh, D_vdbops_grid.md.

#include "NKSRCommon.h"
#include "NKSRGrid.h"
#include "NKSRConv.h"

struct FNKSRSvh
{
	double VoxelSize = 0.1;
	int32 Depth = 4;
	TArray<TUniquePtr<FNKSRIndexGrid>> Grids;                 // size Depth; nullptr = python None
	TMap<FIntPoint, FNKSRKernelMap> KernelMaps;               // (depth, kernelSize) -> cached kmap

	FNKSRSvh() = default;
	FNKSRSvh(double InVoxelSize, int32 InDepth)
		: VoxelSize(InVoxelSize), Depth(InDepth)
	{
		Grids.SetNum(Depth);
	}
	FNKSRSvh(FNKSRSvh&&) = default;
	FNKSRSvh& operator=(FNKSRSvh&&) = default;
	FNKSRSvh(const FNKSRSvh&) = delete;
	FNKSRSvh& operator=(const FNKSRSvh&) = delete;

	void GetGridVoxelSizeOrigin(int32 D, double& OutVoxelSize, double& OutOrigin) const
	{
		OutVoxelSize = VoxelSize * double(1 << D);
		OutOrigin = 0.5 * OutVoxelSize;
	}

	/** build_point_splatting: every level built with BuildFromPointsNearestVoxels. */
	void BuildPointSplatting(TConstArrayView<FVector3f> Points);

	/** build_from_grid_coords (grid at D must currently be null). */
	void BuildFromGridCoords(int32 D, TConstArrayView<FNKSRIjk> GridCoords, const FIntVector& PadMin = FIntVector::ZeroValue, const FIntVector& PadMax = FIntVector::ZeroValue);

	/** build_from_grid: adopt an externally built grid (voxel size / origin must match level D). */
	void BuildFromGrid(int32 D, FNKSRIndexGrid&& Grid);

	/** get_voxel_centers: world positions of active voxel centers at level D (empty if grid null). */
	void GetVoxelCenters(int32 D, TArray<FVector3f>& Out) const;

	/**
	 * evaluate_voxel_status against an external Grid at level D:
	 * 0 = non-exist, 1 = exist-stop, 2 = exist-continue (child voxels present at level D-1).
	 */
	void EvaluateVoxelStatus(const FNKSRIndexGrid& Grid, int32 D, TArray<uint8>& OutStatus) const;
};
