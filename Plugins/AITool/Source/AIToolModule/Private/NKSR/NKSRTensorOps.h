#pragma once

// Dense float32 primitives shared by the network / kernel modules.
// Implemented with Eigen (engine third-party) inside the .cpp — Eigen types must not leak here.
// Port spec: B_network_arch.md (GroupNorm / scatter semantics), G_cpp_design.md GC-5/GC-6.

#include "NKSRCommon.h"

namespace NKSRTensorOps
{

/** Out = A * B. A:[M,K], B:[K,N]. */
void Gemm(const FNKSRMatrix& A, const FNKSRMatrix& B, FNKSRMatrix& Out);

/** torch nn.Linear: Out = X * W^T (+ Bias). W stored [OutDim, InDim] as in the checkpoint; Bias may be null. */
void Linear(const FNKSRMatrix& X, const FNKSRMatrix& W, const FNKSRMatrix* Bias, FNKSRMatrix& Out);

void ReluInPlace(FNKSRMatrix& X);

/**
 * torch nn.GroupNorm eval semantics over a sparse tensor laid out [N, C]:
 * statistics per group over ALL rows at once (biased variance, Eps), then affine per channel.
 * GC-6: never chunk the statistics.
 */
void GroupNorm(FNKSRMatrix& X, int32 NumGroups, TConstArrayView<float> Weight, TConstArrayView<float> Bias, float Eps = 1e-5f);

/** torch_scatter.scatter_max(dim=0, dim_size): empty buckets produce 0 (GC-5). */
void ScatterMax(const FNKSRMatrix& Src, TConstArrayView<int32> Index, int32 DimSize, FNKSRMatrix& Out);

/** torch_scatter.scatter_mean(dim=0, dim_size): empty buckets produce 0 (GC-5). */
void ScatterMean(const FNKSRMatrix& Src, TConstArrayView<int32> Index, int32 DimSize, FNKSRMatrix& Out);

/** Out = [A | B] column-wise concat; rows must match. */
void ConcatCols(const FNKSRMatrix& A, const FNKSRMatrix& B, FNKSRMatrix& Out);

/** Row gather: Out[r] = Src[Index[r]]. Index entries must be valid rows of Src. */
void GatherRows(const FNKSRMatrix& Src, TConstArrayView<int32> Index, FNKSRMatrix& Out);

} // namespace NKSRTensorOps
