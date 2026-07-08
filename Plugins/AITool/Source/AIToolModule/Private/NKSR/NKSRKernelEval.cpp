// NKSRKernelEval.cpp — Neural-kernel evaluation & linear-system assembly (CPU, float32).
// Mirror of NKSR csrc/kernel_eval CPU path:
//   keval_cpu.cpp (kernelEvaluation / buildCOOIndexer), matrixb_cpu.cpp (matrixBuilding),
//   kbuild_cpu.cpp (kBuilding), rhs_cpu.cpp (rhsEvaluation), keval.h (kernel_grad_evaluation_fwd),
//   common/iter_util.h (NNIterator).
// Port spec: Docs/PortSpecs/C_conv_kernel_eval.md §3. Conventions GC-1/GC-3/GC-5/GC-9.

#include "NKSRKernelEval.h"
#include "NKSRGrid.h"

#include "Async/ParallelFor.h"

namespace
{
	// ---- Bezier quadratic basis, support (-1.5, 1.5) (keval.h bezier_1dim, exact boundary open/close) ----
	// Cascade of "x < bound" tests: x==-1.5 -> first segment (0), x==-0.5 / 0.5 -> value 1, x==1.5 -> 0.
	// Peak bezier_1dim(0) = 1.5.
	FORCEINLINE float NKSRBezier1D(float X)
	{
		const bool R1 = X < -1.5f, R2 = X < -0.5f, R3 = X < 0.5f, R4 = X < 1.5f;
		if (!R1 && R2) return (X + 1.5f) * (X + 1.5f);
		if (!R2 && R3) return -2.f * X * X + 1.5f;
		if (!R3 && R4) return (X - 1.5f) * (X - 1.5f);
		return 0.f;
	}

	FORCEINLINE float NKSRBezierGrad1D(float X)
	{
		const bool R1 = X < -1.5f, R2 = X < -0.5f, R3 = X < 0.5f, R4 = X < 1.5f;
		if (!R1 && R2) return 2.f * X + 3.f;
		if (!R2 && R3) return -4.f * X;
		if (!R3 && R4) return 2.f * X - 3.f;
		return 0.f;
	}

	// ---- NNIterator<N> delta enumeration: x outermost, z innermost (iter_util.h, GC-4 order) ----
	FORCEINLINE FNKSRIjk NKSRDelta3(int32 Count)
	{
		return FNKSRIjk(Count / 9 - 1, (Count / 3) % 3 - 1, Count % 3 - 1);
	}

	FORCEINLINE FNKSRIjk NKSRDelta5(int32 Count)
	{
		return FNKSRIjk(Count / 25 - 2, (Count / 5) % 5 - 2, Count % 5 - 2);
	}

	// NNIterator<5>::CountFromDelta (spec §1.3, N=5).
	FORCEINLINE int32 NKSRCountFromDelta5(const FNKSRIjk& Delta)
	{
		return (Delta.X + 2) * 25 + (Delta.Y + 2) * 5 + (Delta.Z + 2);
	}

	// GC-1: nanovdb Vec3f::round == per-component floor(x + 0.5).
	FORCEINLINE FNKSRIjk NKSRRoundVec(const FVector3f& P)
	{
		return FNKSRIjk(NKSRRoundHalfUp(P.X), NKSRRoundHalfUp(P.Y), NKSRRoundHalfUp(P.Z));
	}

	FORCEINLINE FVector3f NKSRIjkToVec(const FNKSRIjk& Ijk)
	{
		return FVector3f((float)Ijk.X, (float)Ijk.Y, (float)Ijk.Z);
	}

	// GC-2: transform scale factor = 1/voxelSize computed in double, truncated to float
	// (matches FNKSRIndexGrid's internal Scale construction; keval uses it as the chain-rule factor).
	FORCEINLINE float NKSRGridScale(const FNKSRIndexGrid& Grid)
	{
		return (float)(1.0 / Grid.VoxelSize());
	}

	// ---- has_overlap: closed-interval world-space box test (keval.h) ----
	FORCEINLINE bool NKSRHasOverlap(const FVector3f& AMin, const FVector3f& AMax, const FVector3f& BMin, const FVector3f& BMax)
	{
		return AMax.X >= BMin.X && BMax.X >= AMin.X
			&& AMax.Y >= BMin.Y && BMax.Y >= AMin.Y
			&& AMax.Z >= BMin.Z && BMax.Z >= AMin.Z;
	}

	/**
	 * kernel_grad_evaluation_fwd (keval.h): K(x,v) = B(pc) * <pKernel[Pi], cKernel[Offset]>,
	 * and (bGrad) its gradient w.r.t. world xyz. K==0 feature dim degenerates to pure Bezier
	 * (dot product term == 1). Empty GradKernelQuery (Rows==0) => feature-gradient term is 0
	 * (approx_kernel_grad mode, GC-5-style zero placeholder).
	 */
	FORCEINLINE void NKSRKernelGradEvalFwd(
		int32 Offset, int32 Pi, float Scale,
		float Pcx, float Pcy, float Pcz,
		const FNKSRMatrix& PKernel, const FNKSRMatrix& CKernel,
		const FNKSRGradTensor& GradKernelQuery,
		bool bGrad,
		float& OutF, FVector3f& OutGradF)
	{
		const float Bx = NKSRBezier1D(Pcx), By = NKSRBezier1D(Pcy), Bz = NKSRBezier1D(Pcz);
		const float BezierKernel = Bx * By * Bz;

		// GC-9: plain float multiply-add (source explicitly avoids fma).
		const int32 K = CKernel.Cols;
		float DpKernel = 1.f;
		if (K > 0)
		{
			DpKernel = 0.f;
			const float* CRow = CKernel.Row(Offset);
			const float* PRow = PKernel.Row(Pi);
			for (int32 Ki = 0; Ki < K; ++Ki) DpKernel += CRow[Ki] * PRow[Ki];
		}
		OutF = BezierKernel * DpKernel;

		if (!bGrad) return;

		// Chain rule: local derivative * scale(=1/voxelSize) = world derivative.
		const float Dbx = NKSRBezierGrad1D(Pcx) * Scale;
		const float Dby = NKSRBezierGrad1D(Pcy) * Scale;
		const float Dbz = NKSRBezierGrad1D(Pcz) * Scale;
		const float Db[3] = { Dbx * By * Bz, Bx * Dby * Bz, Bx * By * Dbz };

		for (int32 Dim = 0; Dim < 3; ++Dim)
		{
			float GradDot = 0.f;
			if (K > 0 && GradKernelQuery.Rows > 0)
			{
				const float* CRow = CKernel.Row(Offset);
				for (int32 Ki = 0; Ki < K; ++Ki) GradDot += CRow[Ki] * GradKernelQuery.At(Pi, Ki, Dim);
			}
			OutGradF[Dim] = BezierKernel * GradDot + DpKernel * Db[Dim];
		}
	}
} // namespace

// ---------------------------------------------------------------------------
// kernel_evaluation (keval_cpu.cpp): f[n] = sum over active 27-neighborhood of alpha_v * K(x_n, v).
// ---------------------------------------------------------------------------
void NKSRKernelEvaluation(
	const FNKSRIndexGrid& Grid,
	TConstArrayView<FVector3f> Xyz,
	const FNKSRMatrix& XyzKernel,
	const FNKSRMatrix& GridKernel,
	TConstArrayView<float> Solution,
	const FNKSRGradTensor& GradKernelXyz,
	bool bGrad,
	TArray<float>& OutF,
	TArray<FVector3f>* OutGradF)
{
	OutF.Reset();
	if (OutGradF) OutGradF->Reset();

	const int32 N = Xyz.Num();
	const int32 Nv = Grid.NumVoxels();
	const int32 K = GridKernel.Cols;

	if (Solution.Num() != Nv)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRKernelEvaluation: Solution size %d != NumVoxels %d"), Solution.Num(), Nv);
		return;
	}
	if (K > 0 && (GridKernel.Rows != Nv || XyzKernel.Rows != N || XyzKernel.Cols != K))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRKernelEvaluation: kernel shape mismatch (GridKernel %dx%d vs Nv=%d, XyzKernel %dx%d vs N=%d)"),
			GridKernel.Rows, GridKernel.Cols, Nv, XyzKernel.Rows, XyzKernel.Cols, N);
		return;
	}
	if (GradKernelXyz.Rows > 0 && (GradKernelXyz.Rows != N || GradKernelXyz.Channels != K))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRKernelEvaluation: GradKernelXyz shape [%d,%d,3] mismatch (N=%d, K=%d)"),
			GradKernelXyz.Rows, GradKernelXyz.Channels, N, K);
		return;
	}
	if (bGrad && !OutGradF) UE_LOG(LogNKSR, Warning, TEXT("NKSRKernelEvaluation: bGrad=true but OutGradF is null; gradients discarded"));

	OutF.SetNumZeroed(N);
	const bool bWriteGrad = bGrad && OutGradF != nullptr;
	if (bWriteGrad) OutGradF->SetNumZeroed(N);
	if (N == 0) return;

	const float Scale = NKSRGridScale(Grid);
	float* OutFData = OutF.GetData();
	FVector3f* OutGradData = bWriteGrad ? OutGradF->GetData() : nullptr;

	// Each query point writes exactly OutF[Pi] (and OutGradF[Pi]) — safe to ParallelFor.
	ParallelFor(N, [&](int32 Pi)
	{
		const FVector3f P = Grid.WorldToGridPoint(Xyz[Pi]);
		const FNKSRIjk Center = NKSRRoundVec(P);

		float Func = 0.f;
		FVector3f DFunc = FVector3f::ZeroVector;
		for (int32 Count = 0; Count < 27; ++Count)
		{
			const FNKSRIjk It = Center + NKSRDelta3(Count);
			const int32 Offset = Grid.IndexOf(It);
			if (Offset < 0) continue;

			float Kiv = 0.f;
			FVector3f GradKiv = FVector3f::ZeroVector;
			NKSRKernelGradEvalFwd(Offset, Pi, Scale,
				P.X - (float)It.X, P.Y - (float)It.Y, P.Z - (float)It.Z,
				XyzKernel, GridKernel, GradKernelXyz, bGrad, Kiv, GradKiv);

			Func += Solution[Offset] * Kiv;
			if (bGrad) DFunc += Solution[Offset] * GradKiv;
		}
		OutFData[Pi] = Func;
		if (OutGradData) OutGradData[Pi] = DFunc;
	});
}

// ---------------------------------------------------------------------------
// build_coo_indexer (keval_cpu.cpp buildCOOIndexer): per GridD voxel, the 125-slot (5x5x5)
// table of overlapping GridDD voxel indices. Slot = NN5 enumeration count around
// round(Tdd.apply(Td.applyInv(iCoord))). Overlap = closed world-space boxes of +-2.5 local voxels.
// CPU stores int64 (spec §3.4: CPU forces long indexer).
// ---------------------------------------------------------------------------
void NKSRBuildCooIndexer(const FNKSRIndexGrid& GridD, const FNKSRIndexGrid& GridDD, TArray<int64>& OutIndexer)
{
	const int32 Nd = GridD.NumVoxels();
	OutIndexer.Init(INDEX_NONE, Nd * 125);
	if (Nd == 0 || GridDD.NumVoxels() == 0) return;

	TConstArrayView<FNKSRIjk> CoordsD = GridD.ActiveGridCoords();     // GC-3 ordered
	int64* Indexer = OutIndexer.GetData();
	const FVector3f PrimalRange(2.5f, 2.5f, 2.5f);

	// Each GridD row [IIdx*125, IIdx*125+125) is written by exactly one iteration — safe to ParallelFor.
	ParallelFor(Nd, [&](int32 IIdx)
	{
		const FVector3f IPrimal = NKSRIjkToVec(CoordsD[IIdx]);
		const FVector3f IjWorld = GridD.GridToWorldPoint(IPrimal);
		const FVector3f JcPrimal = GridDD.WorldToGridPoint(IjWorld);
		const FNKSRIjk JCenter = NKSRRoundVec(JcPrimal);

		// applyInv is order-preserving (scale > 0) — min/max corners need no re-sorting.
		const FVector3f AMin = GridD.GridToWorldPoint(IPrimal - PrimalRange);
		const FVector3f AMax = GridD.GridToWorldPoint(IPrimal + PrimalRange);

		int64* Row = Indexer + (int64)IIdx * 125;
		for (int32 Count = 0; Count < 125; ++Count)
		{
			const FNKSRIjk Jt = JCenter + NKSRDelta5(Count);
			const int32 JIdx = GridDD.IndexOf(Jt);
			if (JIdx < 0) continue;

			const FVector3f JPrimal = NKSRIjkToVec(Jt);
			const FVector3f BMin = GridDD.GridToWorldPoint(JPrimal - PrimalRange);
			const FVector3f BMax = GridDD.GridToWorldPoint(JPrimal + PrimalRange);
			if (!NKSRHasOverlap(AMin, AMax, BMin, BMax)) continue;

			Row[Count] = (int64)JIdx;
		}
	});
}

// ---------------------------------------------------------------------------
// torch.where(indexer != -1) compression (spec §3.4): strict row-major scan (row asc, slot asc),
// record (DInds, DDInds) per valid slot and replace the slot value with its entry ordinal in place.
// ---------------------------------------------------------------------------
void NKSRCompressCooIndexer(TArray<int64>& Indexer, int32 NumRowsD, FNKSRCooEntries& OutEntries)
{
	OutEntries.DInds.Reset();
	OutEntries.DDInds.Reset();
	OutEntries.Num = 0;

	if (Indexer.Num() != NumRowsD * 125)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRCompressCooIndexer: Indexer size %d != NumRowsD(%d)*125"), Indexer.Num(), NumRowsD);
		return;
	}

	int64* Data = Indexer.GetData();
	int32 Entry = 0;
	for (int32 Row = 0; Row < NumRowsD; ++Row)
	{
		int64* RowData = Data + (int64)Row * 125;
		for (int32 Slot = 0; Slot < 125; ++Slot)
		{
			if (RowData[Slot] == INDEX_NONE) continue;
			OutEntries.DInds.Add(Row);
			OutEntries.DDInds.Add((int32)RowData[Slot]);
			RowData[Slot] = Entry++;
		}
	}
	OutEntries.Num = Entry;
}

// ---------------------------------------------------------------------------
// matrix_building (matrixb_cpu.cpp): fused GTG (bGrad=false) / QTQ (bGrad=true) COO assembly.
// Scatter into OutValues via indexMap — accumulation order must match the source
// (points outer, i-neighborhood, j-neighborhood inner) => strictly single-threaded.
// ---------------------------------------------------------------------------
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
	TArray<float>& OutValues)
{
	OutValues.Reset();
	if (NumEntries < 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRMatrixBuilding: NumEntries %d < 0"), NumEntries);
		return;
	}

	const int32 P = Xyz.Num();
	const int32 NvD = GridD.NumVoxels();
	const int32 NvDD = GridDD.NumVoxels();
	const int32 KI = GridKernelD.Cols;
	const int32 KJ = GridKernelDD.Cols;

	if (CompressedIndexer.Num() != NvD * 125)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRMatrixBuilding: indexer size %d != NumVoxelsD(%d)*125"), CompressedIndexer.Num(), NvD);
		return;
	}
	if ((KI > 0 && (GridKernelD.Rows != NvD || KernelD.Rows != P || KernelD.Cols != KI)) ||
		(KJ > 0 && (GridKernelDD.Rows != NvDD || KernelDD.Rows != P || KernelDD.Cols != KJ)))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRMatrixBuilding: kernel feature shape mismatch (P=%d NvD=%d NvDD=%d)"), P, NvD, NvDD);
		return;
	}
	if ((GradD.Rows > 0 && (GradD.Rows != P || GradD.Channels != KI)) ||
		(GradDD.Rows > 0 && (GradDD.Rows != P || GradDD.Channels != KJ)))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRMatrixBuilding: grad kernel shape mismatch (P=%d)"), P);
		return;
	}

	// Zero-init then point-by-point accumulation, matching torch::zeros(numEntries) + serial CPU loop.
	OutValues.SetNumZeroed(NumEntries);
	if (P == 0 || NvD == 0) return;

	const float ScaleI = NKSRGridScale(GridD);
	const float ScaleJ = NKSRGridScale(GridDD);
	const int64* Indexer = CompressedIndexer.GetData();
	float* Values = OutValues.GetData();
	int64 NumBadSlots = 0;

	for (int32 Pi = 0; Pi < P; ++Pi)
	{
		const FVector3f PiLocal = GridD.WorldToGridPoint(Xyz[Pi]);
		const FNKSRIjk CenterI = NKSRRoundVec(PiLocal);
		const FVector3f PjLocal = GridDD.WorldToGridPoint(Xyz[Pi]);
		const FNKSRIjk CenterJ = NKSRRoundVec(PjLocal);

		for (int32 Ci = 0; Ci < 27; ++Ci)
		{
			const FNKSRIjk It = CenterI + NKSRDelta3(Ci);
			const int32 OffsetI = GridD.IndexOf(It);
			if (OffsetI < 0) continue;

			float KiF = 0.f;
			FVector3f GradKiF = FVector3f::ZeroVector;
			NKSRKernelGradEvalFwd(OffsetI, Pi, ScaleI,
				PiLocal.X - (float)It.X, PiLocal.Y - (float)It.Y, PiLocal.Z - (float)It.Z,
				KernelD, GridKernelD, GradD, bGrad, KiF, GradKiF);

			// Slot base: i coord mapped into j's grid, rounded half-up (spec §3.5 / GC-1).
			const FNKSRIjk IC = NKSRRoundVec(GridDD.WorldToGridPoint(GridD.GridToWorldPoint(NKSRIjkToVec(It))));

			for (int32 Cj = 0; Cj < 27; ++Cj)
			{
				const FNKSRIjk Jt = CenterJ + NKSRDelta3(Cj);
				const int32 OffsetJ = GridDD.IndexOf(Jt);
				if (OffsetJ < 0) continue;

				float KjF = 0.f;
				FVector3f GradKjF = FVector3f::ZeroVector;
				NKSRKernelGradEvalFwd(OffsetJ, Pi, ScaleJ,
					PjLocal.X - (float)Jt.X, PjLocal.Y - (float)Jt.Y, PjLocal.Z - (float)Jt.Z,
					KernelDD, GridKernelDD, GradDD, bGrad, KjF, GradKjF);

				const float OutVal = bGrad ? FVector3f::DotProduct(GradKiF, GradKjF) : KiF * KjF;

				const FNKSRIjk Delta = Jt - IC;
				const int32 Slot = NKSRCountFromDelta5(Delta);
				// Slot should always be valid (2.5-voxel overlap boxes cover the 1.5+1.5 support);
				// defensive guard instead of check() so grid inconsistencies do not crash (spec §4).
				const int64 Entry = (Slot >= 0 && Slot < 125) ? Indexer[(int64)OffsetI * 125 + Slot] : INDEX_NONE;
				if (Entry < 0 || Entry >= NumEntries)
				{
					++NumBadSlots;
					continue;
				}

				Values[(int32)Entry] += OutVal;
			}
		}
	}

	if (NumBadSlots > 0) UE_LOG(LogNKSR, Error, TEXT("NKSRMatrixBuilding: %lld contributions hit invalid indexer slots (grid/indexer inconsistency); result is incomplete"), NumBadSlots);
}

// ---------------------------------------------------------------------------
// k_building (kbuild_cpu.cpp): diagonal-block regularizer K(i,j) = B(j-i) * <kernel[j], kernel[i]>
// over the 27-neighborhood of GridD (integer centers, same grid on both sides).
// Scatter into OutValues — single-threaded to keep the source accumulation order.
// ---------------------------------------------------------------------------
void NKSRKBuilding(
	const FNKSRIndexGrid& GridD,
	const FNKSRMatrix& GridKernelD,
	TConstArrayView<int64> CompressedIndexer,
	int32 NumEntries,
	TArray<float>& OutValues)
{
	OutValues.Reset();
	if (NumEntries < 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRKBuilding: NumEntries %d < 0"), NumEntries);
		return;
	}

	const int32 Nv = GridD.NumVoxels();
	if (CompressedIndexer.Num() != Nv * 125)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRKBuilding: indexer size %d != NumVoxels(%d)*125"), CompressedIndexer.Num(), Nv);
		return;
	}
	if (GridKernelD.Cols > 0 && GridKernelD.Rows != Nv)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRKBuilding: GridKernelD rows %d != NumVoxels %d"), GridKernelD.Rows, Nv);
		return;
	}

	OutValues.SetNumZeroed(NumEntries);
	if (Nv == 0) return;

	const float Scale = NKSRGridScale(GridD);
	TConstArrayView<FNKSRIjk> Coords = GridD.ActiveGridCoords();      // GC-3: deterministic ijk order
	const int64* Indexer = CompressedIndexer.GetData();
	float* Values = OutValues.GetData();
	const FNKSRGradTensor EmptyGrad;                                  // grad path unused (bGrad=false)
	int64 NumBadSlots = 0;

	for (int32 OffsetI = 0; OffsetI < Nv; ++OffsetI)
	{
		const FNKSRIjk ICoord = Coords[OffsetI];
		for (int32 Count = 0; Count < 27; ++Count)
		{
			const FNKSRIjk Diff = NKSRDelta3(Count);                  // components in {-1,0,1}
			const FNKSRIjk Jt = ICoord + Diff;
			const int32 OffsetJ = GridD.IndexOf(Jt);
			if (OffsetJ < 0) continue;

			// Argument order per source: grid-side row = OffsetJ, point-side row = OffsetI,
			// both feature tensors are the same GridKernelD.
			float IjF = 0.f;
			FVector3f Dummy = FVector3f::ZeroVector;
			NKSRKernelGradEvalFwd(OffsetJ, OffsetI, Scale,
				(float)Diff.X, (float)Diff.Y, (float)Diff.Z,
				GridKernelD, GridKernelD, EmptyGrad, false, IjF, Dummy);

			const int32 Slot = NKSRCountFromDelta5(Diff);             // same grid: iC == iCoord
			const int64 Entry = Indexer[(int64)OffsetI * 125 + Slot];
			if (Entry < 0 || Entry >= NumEntries)
			{
				++NumBadSlots;
				continue;
			}

			Values[(int32)Entry] += IjF;                              // each (i,j) contributes exactly once
		}
	}

	if (NumBadSlots > 0) UE_LOG(LogNKSR, Error, TEXT("NKSRKBuilding: %lld contributions hit invalid indexer slots (grid/indexer inconsistency); result is incomplete"), NumBadSlots);
}

// ---------------------------------------------------------------------------
// rhs_evaluation (rhs_cpu.cpp): OutRhs[v] += NormalValue[n] . grad_x K(x_n, v), grad forced true.
// Scatter into OutRhs — single-threaded to keep the source accumulation order.
// ---------------------------------------------------------------------------
void NKSRRhsEvaluation(
	const FNKSRIndexGrid& GridD,
	TConstArrayView<FVector3f> NormalXyz,
	const FNKSRMatrix& NormalKernel,
	const FNKSRMatrix& GridKernel,
	const FNKSRGradTensor& NormalGradKernel,
	TConstArrayView<FVector3f> NormalValue,
	TArray<float>& OutRhs)
{
	OutRhs.Reset();

	const int32 P = NormalXyz.Num();
	const int32 Nv = GridD.NumVoxels();
	const int32 K = GridKernel.Cols;

	if (NormalValue.Num() != P)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRRhsEvaluation: NormalValue size %d != NormalXyz size %d"), NormalValue.Num(), P);
		return;
	}
	if (K > 0 && (GridKernel.Rows != Nv || NormalKernel.Rows != P || NormalKernel.Cols != K))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRRhsEvaluation: kernel shape mismatch (GridKernel %dx%d vs Nv=%d, NormalKernel %dx%d vs P=%d)"),
			GridKernel.Rows, GridKernel.Cols, Nv, NormalKernel.Rows, NormalKernel.Cols, P);
		return;
	}
	if (NormalGradKernel.Rows > 0 && (NormalGradKernel.Rows != P || NormalGradKernel.Channels != K))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRRhsEvaluation: NormalGradKernel shape [%d,%d,3] mismatch (P=%d, K=%d)"),
			NormalGradKernel.Rows, NormalGradKernel.Channels, P, K);
		return;
	}

	OutRhs.SetNumZeroed(Nv);                                          // = GridD.NumVoxels, zero-initialized
	if (P == 0 || Nv == 0) return;

	const float Scale = NKSRGridScale(GridD);
	float* Rhs = OutRhs.GetData();

	for (int32 Pi = 0; Pi < P; ++Pi)
	{
		const FVector3f PLocal = GridD.WorldToGridPoint(NormalXyz[Pi]);
		const FNKSRIjk Center = NKSRRoundVec(PLocal);
		const FVector3f PData = NormalValue[Pi];

		for (int32 Count = 0; Count < 27; ++Count)
		{
			const FNKSRIjk It = Center + NKSRDelta3(Count);
			const int32 Offset = GridD.IndexOf(It);
			if (Offset < 0) continue;

			float Kiv = 0.f;
			FVector3f GradKiv = FVector3f::ZeroVector;
			NKSRKernelGradEvalFwd(Offset, Pi, Scale,
				PLocal.X - (float)It.X, PLocal.Y - (float)It.Y, PLocal.Z - (float)It.Z,
				NormalKernel, GridKernel, NormalGradKernel, /*bGrad=*/true, Kiv, GradKiv);

			Rhs[Offset] += FVector3f::DotProduct(PData, GradKiv);
		}
	}
}
