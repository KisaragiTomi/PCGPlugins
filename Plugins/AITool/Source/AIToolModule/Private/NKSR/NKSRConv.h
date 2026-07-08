#pragma once

// Sparse 3x3x3 convolution: kernel-map building + gather-GEMM-scatter forward.
// Port spec: C_conv_kernel_eval.md §conv. Conventions GC-3/GC-4.
// Transposed convolution is off the ks inference path and is NOT ported.

#include "NKSRCommon.h"

class FNKSRIndexGrid;

/**
 * Mirrors the python-side (nbmap, nbsizes) produced from _C.conv.convolution_kernel_map:
 * entries grouped by kernel offset (GC-4 enumeration: x outermost, z innermost, kernelStart=-1),
 * ascending target index inside each group. NbMap[i] = {SourceVoxel, TargetVoxel} (X=source, Y=target).
 */
struct FNKSRKernelMap
{
	TArray<FIntPoint> NbMap;
	TArray<int32> NbSizes;   // one per kernel offset slot (KernelSize^3 entries)
};

/** convolution_kernel_map + python reordering, stride 1 only (the ks path). */
void NKSRBuildKernelMap(const FNKSRIndexGrid& InGrid, const FNKSRIndexGrid& OutGrid, int32 KernelSize, FNKSRKernelMap& Out);

/**
 * sparse_convolution forward.
 * InFeat: [InRows, Cin]. Kernel: flattened [KernelVolume * Cin, Cout] (original [KV][Cin][Cout], GC-4 order,
 * no transpose). Bias (optional, size Cout) added at the end. Out: [OutRows, Cout] zero-initialized.
 * Includes the center-offset fast path (single whole-matrix GEMM for the center group) per spec C.
 */
void NKSRSparseConvolution(
	const FNKSRMatrix& InFeat,
	const FNKSRMatrix& Kernel,
	int32 KernelVolume,
	const FNKSRKernelMap& KMap,
	int32 OutRows,
	const FNKSRMatrix* Bias,
	FNKSRMatrix& Out);

/** kernel_size==1 path: plain GEMM with kernel [Cin, Cout] (+ optional bias). */
void NKSRConv1x1(const FNKSRMatrix& InFeat, const FNKSRMatrix& Kernel, const FNKSRMatrix* Bias, FNKSRMatrix& Out);
