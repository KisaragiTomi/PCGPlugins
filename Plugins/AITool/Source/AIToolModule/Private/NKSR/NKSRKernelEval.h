#pragma once

// Neural-kernel evaluation & linear-system assembly (mirror of _C.kernel_eval, CPU path).
// Port spec: C_conv_kernel_eval.md §kernel_eval. Conventions GC-1/GC-3/GC-5/GC-9.
//
// Tensor shape glossary (K = kernel_dim = 4):
//   XyzKernel      [N, K]  MLP-interpolated theta at query points
//   GridKernel     [Nv, K] per-voxel basis features (KernelField.grid_kernel)
//   GradKernel     [N, K, 3] d(theta)/d(world xyz); Rows==0 = "no gradient kernel" placeholder
//   Indexer        [Nd, 125] int64: 5x5x5 neighborhood table (see BuildCooIndexer), -1 = empty slot

#include "NKSRCommon.h"

class FNKSRIndexGrid;

/** kernel_evaluation: f[n] = sum_v K(xyz[n], v) * Solution[v]; optionally d f / d xyz. */
void NKSRKernelEvaluation(
	const FNKSRIndexGrid& Grid,
	TConstArrayView<FVector3f> Xyz,
	const FNKSRMatrix& XyzKernel,
	const FNKSRMatrix& GridKernel,
	TConstArrayView<float> Solution,
	const FNKSRGradTensor& GradKernelXyz,
	bool bGrad,
	TArray<float>& OutF,
	TArray<FVector3f>* OutGradF);

/**
 * build_coo_indexer(gridD, gridDD): for each voxel of GridD, the 125-slot table of overlapping
 * GridDD voxel indices (or -1). OutIndexer size = NumVoxelsD * 125, row-major.
 */
void NKSRBuildCooIndexer(const FNKSRIndexGrid& GridD, const FNKSRIndexGrid& GridDD, TArray<int64>& OutIndexer);

/**
 * Python-side torch.where compression: scans Indexer row-major, replaces every valid slot with its
 * COO entry ordinal (0..Num-1) in place, and records (DInds, DDInds) per entry.
 * Equivalent to: d_inds, dd_local = where(idx != -1); dd_inds = idx[d_inds, dd_local]; idx[...] = arange.
 */
struct FNKSRCooEntries
{
	TArray<int32> DInds;
	TArray<int32> DDInds;
	int32 Num = 0;
};
void NKSRCompressCooIndexer(TArray<int64>& Indexer, int32 NumRowsD, FNKSRCooEntries& OutEntries);

/**
 * matrix_building (fused G^T G / Q^T Q assembly). Indexer must already be compressed
 * (slots hold entry ordinals). bGrad selects the QTQ variant (uses gradient kernels).
 * OutValues: size NumEntries, accumulated (caller zero-initializes semantics handled inside).
 */
void NKSRMatrixBuilding(
	const FNKSRIndexGrid& GridD,
	const FNKSRIndexGrid& GridDD,
	TConstArrayView<FVector3f> Xyz,
	const FNKSRMatrix& KernelD,
	const FNKSRMatrix& KernelDD,
	const FNKSRMatrix& GridKernelD,
	const FNKSRMatrix& GridKernelDD,
	const FNKSRGradTensor& GradD,
	const FNKSRGradTensor& GradDD,
	TConstArrayView<int64> CompressedIndexer,
	bool bGrad,
	int32 NumEntries,
	TArray<float>& OutValues);

/** k_building: regularizer K matrix over the 27-neighborhood of GridD (diagonal blocks only). */
void NKSRKBuilding(
	const FNKSRIndexGrid& GridD,
	const FNKSRMatrix& GridKernelD,
	TConstArrayView<int64> CompressedIndexer,
	int32 NumEntries,
	TArray<float>& OutValues);

/** rhs_evaluation: OutRhs[v] += NormalValue[n] . grad_v K(xyz[n], v). OutRhs sized NumVoxels(GridD). */
void NKSRRhsEvaluation(
	const FNKSRIndexGrid& GridD,
	TConstArrayView<FVector3f> NormalXyz,
	const FNKSRMatrix& NormalKernel,
	const FNKSRMatrix& GridKernel,
	const FNKSRGradTensor& NormalGradKernel,
	TConstArrayView<FVector3f> NormalValue,
	TArray<float>& OutRhs);
