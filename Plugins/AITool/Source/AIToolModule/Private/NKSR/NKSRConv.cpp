// Sparse 3x3x3 convolution: kernel-map building + gather-GEMM-scatter forward.
// Port spec: C_conv_kernel_eval.md §2 (conv). Conventions GC-3/GC-4/GC-9.
// Reference: csrc/conv/kmap_cpu.cpp, csrc/conv/convolution_cpu.cpp, nksr/nn/modules.py (Conv3d).

#include "NKSRConv.h"
#include "NKSRGrid.h"
#include "NKSRTensorOps.h"

#include "Async/ParallelFor.h"

namespace
{

/** Copies kernel slice OffsetIdx ([Cin,Cout] block of the flattened [KV*Cin,Cout] tensor, GC-4 order, no transpose). */
void CopyKernelSlice(const FNKSRMatrix& Kernel, int32 OffsetIdx, int32 Cin, int32 Cout, FNKSRMatrix& OutSlice)
{
	OutSlice.SetUninitialized(Cin, Cout);
	if (Cin > 0 && Cout > 0) FMemory::Memcpy(OutSlice.Data.GetData(), Kernel.Data.GetData() + (int64)OffsetIdx * Cin * Cout, sizeof(float) * Cin * Cout);
}

/** Row-broadcast bias add (python-side `out_feature += bias`, folded into the conv per spec C §2.3). */
void AddBiasRows(FNKSRMatrix& Out, const FNKSRMatrix& Bias)
{
	const float* B = Bias.Data.GetData();
	for (int32 R = 0; R < Out.Rows; ++R)
	{
		float* DstRow = Out.Row(R);
		for (int32 C = 0; C < Out.Cols; ++C) DstRow[C] += B[C];
	}
}

} // namespace

void NKSRBuildKernelMap(const FNKSRIndexGrid& InGrid, const FNKSRIndexGrid& OutGrid, int32 KernelSize, FNKSRKernelMap& Out)
{
	Out.NbMap.Reset();
	Out.NbSizes.Reset();

	if (KernelSize <= 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRBuildKernelMap: kernel size must be a positive integer (got %d)."), KernelSize);
		return;
	}
	// Reference precondition: source voxels are finer than or equal to target voxels (kmap_cpu.cpp).
	if (InGrid.VoxelSize() > OutGrid.VoxelSize())
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRBuildKernelMap: source must have smaller voxel size than target (%f > %f)."),
			InGrid.VoxelSize(), OutGrid.VoxelSize());
		return;
	}

	// Spec C §2.1: stride via std::round of the voxel-size ratio (ratio >= 1, so half-away == half-up).
	const int32 Stride = (int32)FMath::RoundToDouble(OutGrid.VoxelSize() / InGrid.VoxelSize());
	// kernelStart = floor(-k/2 + 1): kernel=3 -> -1, kernel=2 -> 0 (GC-4).
	const int32 KernelStart = (int32)FMath::FloorToDouble(-KernelSize / 2.0 + 1.0);
	const int32 KernelVolume = KernelSize * KernelSize * KernelSize;

	Out.NbSizes.SetNumZeroed(KernelVolume);

	const TConstArrayView<FNKSRIjk> TargetCoords = OutGrid.ActiveGridCoords();   // GC-3 order = target voxel offsets
	const int32 NumTargets = TargetCoords.Num();
	if (NumTargets == 0 || InGrid.NumVoxels() == 0) return;   // empty kmap, NbSizes stays all zero

	// Directly emit the python-reordered form (spec C §2.2): entries grouped by kernel offset
	// (GC-4 enumeration: x outermost, z innermost), ascending target index inside each group.
	Out.NbMap.Reserve(NumTargets);
	TArray<int32> SourceIdx;
	SourceIdx.SetNumUninitialized(NumTargets);
	const EParallelForFlags ForFlags = NumTargets >= 1024 ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread;

	for (int32 KIdx = 0; KIdx < KernelVolume; ++KIdx)
	{
		// DeltaFromCount (spec C §1.3): x outermost, z innermost.
		const int32 Kx = KernelStart + KIdx / (KernelSize * KernelSize);
		const int32 Ky = KernelStart + (KIdx / KernelSize) % KernelSize;
		const int32 Kz = KernelStart + KIdx % KernelSize;

		// Per-target lookup: each SourceIdx element written by exactly one thread.
		ParallelFor(NumTargets, [&InGrid, &TargetCoords, &SourceIdx, Stride, Kx, Ky, Kz](int32 T)
		{
			// GC-4: offset applied on the SOURCE voxel coordinates: sCoord = tCoord * stride + (kx,ky,kz).
			const FNKSRIjk SCoord(
				TargetCoords[T].X * Stride + Kx,
				TargetCoords[T].Y * Stride + Ky,
				TargetCoords[T].Z * Stride + Kz);
			SourceIdx[T] = InGrid.IndexOf(SCoord);
		}, ForFlags);

		// Compaction stays serial and in ascending target order (deterministic, GC-3).
		const int32 GroupStart = Out.NbMap.Num();
		for (int32 T = 0; T < NumTargets; ++T) if (SourceIdx[T] >= 0) Out.NbMap.Add(FIntPoint(SourceIdx[T], T));   // {source, target}
		Out.NbSizes[KIdx] = Out.NbMap.Num() - GroupStart;
	}
}

void NKSRSparseConvolution(
	const FNKSRMatrix& InFeat,
	const FNKSRMatrix& Kernel,
	int32 KernelVolume,
	const FNKSRKernelMap& KMap,
	int32 OutRows,
	const FNKSRMatrix* Bias,
	FNKSRMatrix& Out)
{
	Out = FNKSRMatrix();

	if (KernelVolume <= 0 || Kernel.Rows <= 0 || Kernel.Rows % KernelVolume != 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: kernel [%d,%d] not divisible into %d offset slices."),
			Kernel.Rows, Kernel.Cols, KernelVolume);
		return;
	}
	const int32 Cin = Kernel.Rows / KernelVolume;
	const int32 Cout = Kernel.Cols;
	if (InFeat.Cols != Cin)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: input feature size and kernel size mismatch (%d vs %d)."), InFeat.Cols, Cin);
		return;
	}
	if (OutRows < 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: negative OutRows %d."), OutRows);
		return;
	}
	if (KMap.NbSizes.Num() != KernelVolume)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: NbSizes has %d groups, expected %d."), KMap.NbSizes.Num(), KernelVolume);
		return;
	}
	int64 TotalEntries = 0;
	for (const int32 GroupSize : KMap.NbSizes)
	{
		if (GroupSize < 0)
		{
			UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: negative NbSizes entry %d."), GroupSize);
			return;
		}
		TotalEntries += GroupSize;
	}
	if (TotalEntries != KMap.NbMap.Num())
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: NbSizes sum %lld != NbMap entries %d."), TotalEntries, KMap.NbMap.Num());
		return;
	}
	if (Bias && Bias->Data.Num() != Cout)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: bias has %d values, expected %d."), Bias->Data.Num(), Cout);
		return;
	}
	// Reference gather/scatter skip negative entries defensively but they "cannot happen" (spec C §2.3);
	// treat any out-of-range entry as a hard input error instead.
	for (const FIntPoint& Entry : KMap.NbMap)
	{
		if (Entry.X >= 0 && Entry.X < InFeat.Rows && Entry.Y >= 0 && Entry.Y < OutRows) continue;
		UE_LOG(LogNKSR, Error, TEXT("NKSRSparseConvolution: NbMap entry {%d,%d} out of range (in %d rows, out %d rows)."),
			Entry.X, Entry.Y, InFeat.Rows, OutRows);
		return;
	}

	Out.SetZeroed(OutRows, Cout);

	// Center optimization (convolution_cpu.cpp): odd kernel volume + same row count => the center offset
	// (kIdx = KV/2, identity map for stride-1 same-grid conv) is one whole-matrix GEMM overwriting the zeros.
	const bool bCenterFlag = (KernelVolume % 2 == 1) && (OutRows == InFeat.Rows);
	FNKSRMatrix KernelSlice;
	if (bCenterFlag)
	{
		CopyKernelSlice(Kernel, KernelVolume / 2, Cin, Cout, KernelSlice);
		NKSRTensorOps::Gemm(InFeat, KernelSlice, Out);
	}

	// gather -> GEMM -> scatter-add per offset group. Cursor stepping mirrors the reference verbatim:
	// the center group is skipped but still advances the cursor; empty groups advance by zero.
	TArray<int32> GatherIdx;
	FNKSRMatrix InBuf, OutBuf;
	int32 Cursor = 0;   // NbMap entry cursor (reference walks 2*int per entry; ours walks FIntPoint entries)
	for (int32 I = 0; I < KernelVolume; ++I)
	{
		const int32 GroupSize = KMap.NbSizes[I];
		if (bCenterFlag && I == KernelVolume / 2)
		{
			Cursor += GroupSize;
			continue;
		}
		if (GroupSize == 0) continue;

		// gather: source rows (non-transposed conv reads NbMap[i].X; transposed is not ported).
		GatherIdx.SetNumUninitialized(GroupSize, EAllowShrinking::No);
		for (int32 E = 0; E < GroupSize; ++E) GatherIdx[E] = KMap.NbMap[Cursor + E].X;
		NKSRTensorOps::GatherRows(InFeat, GatherIdx, InBuf);

		// GEMM with the i-th [Cin,Cout] kernel slice (GC-4 order, no transpose).
		CopyKernelSlice(Kernel, I, Cin, Cout, KernelSlice);
		NKSRTensorOps::Gemm(InBuf, KernelSlice, OutBuf);

		// scatter-add to target rows; rows may repeat => stays single-threaded for determinism.
		for (int32 E = 0; E < GroupSize; ++E)
		{
			const float* SrcRow = OutBuf.Row(E);
			float* DstRow = Out.Row(KMap.NbMap[Cursor + E].Y);
			for (int32 C = 0; C < Cout; ++C) DstRow[C] += SrcRow[C];
		}
		Cursor += GroupSize;
	}

	if (Bias) AddBiasRows(Out, *Bias);
}

void NKSRConv1x1(const FNKSRMatrix& InFeat, const FNKSRMatrix& Kernel, const FNKSRMatrix* Bias, FNKSRMatrix& Out)
{
	Out = FNKSRMatrix();

	if (InFeat.Cols != Kernel.Rows)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRConv1x1: input feature size and kernel size mismatch (%d vs %d)."), InFeat.Cols, Kernel.Rows);
		return;
	}
	if (Bias && Bias->Data.Num() != Kernel.Cols)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRConv1x1: bias has %d values, expected %d."), Bias->Data.Num(), Kernel.Cols);
		return;
	}

	// kernel_size==1 && stride==1 special case (Conv3d.forward): out = in_feat.matmul(kernel),
	// kernel already stored [Cin,Cout] (Linear semantics WITHOUT the nn.Linear transpose).
	NKSRTensorOps::Gemm(InFeat, Kernel, Out);
	if (Bias) AddBiasRows(Out, *Bias);
}
