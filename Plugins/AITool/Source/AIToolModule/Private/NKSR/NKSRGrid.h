#pragma once

// Sparse index grid — hash-map replacement of NKSR's nanovdb IndexGrid.
// Port spec: Docs/PortSpecs/D_vdbops_grid.md. Conventions: G_cpp_design.md GC-1..GC-5.
//
// Contract (GC-3): active voxels are stored sorted by (i,j,k) lexicographic order;
// ActiveGridCoords()[r] is the ijk of linear index r, and IjkToIndex inverts that mapping.
// Every per-voxel tensor in the pipeline follows this row order.

#include "NKSRCommon.h"

class FNKSRIndexGrid
{
public:
	FNKSRIndexGrid() = default;
	FNKSRIndexGrid(double InVoxelSize, double InOrigin);

	// --- topology building (spec D §3) ---

	/** build_from_pointcloud_nearest_voxels: activate the 2x2x2 stencil floor(g)+{0,1}^3 around each point. */
	void BuildFromPointsNearestVoxels(TConstArrayView<FVector3f> Points);

	/** build_from_ijk_coords with per-voxel padding window [PadMin, PadMax] (inclusive). */
	void BuildFromIjkCoords(TConstArrayView<FNKSRIjk> Ijk, const FIntVector& PadMin = FIntVector::ZeroValue, const FIntVector& PadMax = FIntVector::ZeroValue);

	/** coarsened_grid: parent voxels floor-div(ijk, Factor); voxel size *= Factor, origin follows SVH convention (spec D). */
	FNKSRIndexGrid Coarsened(int32 Factor) const;

	/** subdivided_grid: children of (masked) voxels; Mask may be empty (= all true). */
	FNKSRIndexGrid Subdivided(int32 Factor, TConstArrayView<bool> Mask) const;

	/** dual_grid: pad [0,1]^3 over active voxels, dual transform (same voxel size, origin - half voxel). */
	FNKSRIndexGrid Dual() const;

	// --- queries ---

	FORCEINLINE int32 NumVoxels() const { return Coords.Num(); }
	FORCEINLINE double VoxelSize() const { return VoxelSizeD; }
	FORCEINLINE double Origin() const { return OriginD; }
	FORCEINLINE TConstArrayView<FNKSRIjk> ActiveGridCoords() const { return Coords; }

	/** Single-voxel lookup; -1 when inactive. */
	FORCEINLINE int32 IndexOf(const FNKSRIjk& Ijk) const
	{
		const int32* Found = Lookup.Find(Ijk);
		return Found ? *Found : -1;
	}
	void IjkToIndex(TConstArrayView<FNKSRIjk> Ijk, TArray<int32>& Out) const;

	/** points_in_active_voxel: whether RoundHalfUp(WorldToGrid(p)) hits an active voxel. */
	void PointsInActiveVoxel(TConstArrayView<FVector3f> Points, TArray<bool>& Out) const;

	// --- coordinate transform (GC-2; exact float formulas per spec D, implemented in .cpp) ---

	FVector3f WorldToGridPoint(const FVector3f& P) const;
	FVector3f GridToWorldPoint(const FVector3f& G) const;
	void WorldToGrid(TConstArrayView<FVector3f> Points, TArray<FVector3f>& Out) const;
	void GridToWorld(TConstArrayView<FVector3f> GridPts, TArray<FVector3f>& Out) const;

	// --- interpolation / splatting (spec D §4; GC-5 zero contribution for inactive voxels) ---

	/** 8-corner trilinear sample; OutGrad (optional) is d(sample)/d(world xyz). */
	void SampleTrilinear(TConstArrayView<FVector3f> Points, const FNKSRMatrix& GridData, FNKSRMatrix& Out, FNKSRGradTensor* OutGrad = nullptr) const;

	/** 27-neighbor quadratic Bezier sample (base = RoundHalfUp, weights sum to 8, NOT normalized). */
	void SampleBezier(TConstArrayView<FVector3f> Points, const FNKSRMatrix& GridData, FNKSRMatrix& Out, FNKSRGradTensor* OutGrad = nullptr) const;

	/** splat_trilinear: accumulate PointData into the 8 surrounding voxels (inactive skipped, no renormalize). */
	void SplatTrilinear(TConstArrayView<FVector3f> Points, const FNKSRMatrix& PointData, FNKSRMatrix& Out) const;

	// --- hierarchy data movement (spec D §4) ---

	/** subdivide (nearest upsample): fine voxel value = coarse parent value (floor-div), missing parent -> 0. */
	void Subdivide(const FNKSRMatrix& CoarseData, int32 Factor, const FNKSRIndexGrid& FineGrid, FNKSRMatrix& Out) const;

	/** max_pool: coarse value = max over active children, then -inf -> 0 (GC-5). 'this' is the fine grid. */
	void MaxPool(const FNKSRMatrix& FineData, int32 Factor, const FNKSRIndexGrid& CoarseGrid, FNKSRMatrix& Out) const;

private:
	/** Sorts pending coords (GC-3), dedupes, rebuilds Lookup. */
	void FinalizeCoords(TArray<FNKSRIjk>&& InCoords);

	double VoxelSizeD = 1.0;
	double OriginD = 0.0;
	// GC-2: computed in double at construction, truncated to float, kept in this algebraic form.
	float Scale = 1.f;       // 1/voxelSize
	float Translate = 0.f;   // -origin/voxelSize
	float InvScale = 1.f;    // voxelSize (inverse transform factor; exact usage per spec D)

	TArray<FNKSRIjk> Coords;
	TMap<FNKSRIjk, int32> Lookup;
};
