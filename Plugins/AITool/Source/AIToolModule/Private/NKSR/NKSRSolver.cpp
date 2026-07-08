// Symmetric-block Jacobi-preconditioned CG, CPU float32 (GC-9).
// Port of NKSR csrc/sparse_solve/solve_cpu.cpp (dispatch_ind2ptr_cpu, symblk_matmul,
// solve_pcg_cpu) + nksr/solver.py SparseMatrix.add_block/_solve.
// Spec: Docs/PortSpecs/E_meshing_solver.md §11. res_fix is always False on the inference
// path (solver.py _solve hardcodes it), so only that branch is implemented.

#include "NKSRSolver.h"

#include "Async/ParallelFor.h"

// COO row indices must be sorted ascending within [0, NumRows) (spec E §11.1: a_i comes
// from a row-major torch.where scan). Guarding here prevents out-of-bounds CSR writes.
static bool NKSRValidateRowInds(TConstArrayView<int32> RowInds, int32 NumRows)
{
	int32 Prev = 0;
	for (int32 E = 0; E < RowInds.Num(); ++E)
	{
		const int32 R = RowInds[E];
		if (R < 0 || R >= NumRows || R < Prev) return false;
		Prev = R;
	}
	return true;
}

// GC-9 + determinism: sequential float32 accumulation (single-threaded dot).
static float NKSRDot(TConstArrayView<float> A, TConstArrayView<float> B)
{
	float Sum = 0.f;
	for (int32 I = 0; I < A.Num(); ++I) Sum += A[I] * B[I];
	return Sum;
}

static float NKSRNorm2(TConstArrayView<float> V)
{
	return FMath::Sqrt(NKSRDot(V, V));
}

// y = A * v over the symmetric block structure (solve_cpu.cpp symblk_matmul):
// stored upper block (i,j) acts directly (y_i += A_ij * v_j); a missing lower block (i,j)
// with stored (j,i) acts through the CSR transpose (per-entry scatter y_i[aj] += ax * v_j[r]).
// The (i,j) double loop order mirrors the reference so float accumulation order matches.
static void NKSRSymblkMatmul(const FNKSRBlockMatrix& A, TConstArrayView<int32> BlockPtr,
                             TConstArrayView<float> V, TArray<float>& Y)
{
	FMemory::Memzero(Y.GetData(), Y.Num() * sizeof(float));
	for (int32 I = 0; I < A.NumBlockRows; ++I)
	{
		for (int32 J = 0; J < A.NumBlockRows; ++J)
		{
			const int32 OffI = BlockPtr[I];
			const int32 OffJ = BlockPtr[J];
			if (const FNKSRCsrBlock* Blk = A.Blocks.Find(FIntPoint(I, J)))
			{
				// Direct CSR: each output row written by exactly one iteration -> ParallelFor allowed.
				// Per-row sums stay sequential, so the result is thread-count independent.
				const int32* RowPtr = Blk->RowPtr.GetData();
				const int32* Cols = Blk->ColInds.GetData();
				const float* Vals = Blk->Values.GetData();
				const float* VData = V.GetData();
				float* YData = Y.GetData();
				ParallelFor(Blk->RowPtr.Num() - 1, [RowPtr, Cols, Vals, VData, YData, OffI, OffJ](int32 R)
				{
					float Sum = 0.f;
					for (int32 K = RowPtr[R]; K < RowPtr[R + 1]; ++K) Sum += Vals[K] * VData[OffJ + Cols[K]];
					YData[OffI + R] += Sum;
				});
			}
			else if (const FNKSRCsrBlock* BlkT = A.Blocks.Find(FIntPoint(J, I)))
			{
				// Transposed CSR scatter: multiple entries hit the same output element -> single-threaded.
				const int32 NumRowsT = BlkT->RowPtr.Num() - 1;
				for (int32 R = 0; R < NumRowsT; ++R)
				{
					const float VR = V[OffJ + R];
					for (int32 K = BlkT->RowPtr[R]; K < BlkT->RowPtr[R + 1]; ++K) Y[OffI + BlkT->ColInds[K]] += BlkT->Values[K] * VR;
				}
			}
		}
	}
}

void NKSRInd2Ptr(TConstArrayView<int32> RowInds, int32 NumRows, TArray<int32>& OutPtr)
{
	OutPtr.Reset();
	if (NumRows < 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRInd2Ptr: negative NumRows %d."), NumRows);
		return;
	}
	if (!NKSRValidateRowInds(RowInds, NumRows))
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRInd2Ptr: row indices not sorted ascending in [0, %d)."), NumRows);
		return;
	}
	OutPtr.SetNumUninitialized(NumRows + 1);
	const int32 E = RowInds.Num();
	if (E == 0)
	{
		FMemory::Memzero(OutPtr.GetData(), OutPtr.Num() * sizeof(int32));
		return;
	}
	// Mirror of dispatch_ind2ptr_cpu (solve_cpu.cpp), single-threaded.
	for (int32 I = 0; I <= RowInds[0]; ++I) OutPtr[I] = 0;
	for (int32 Ei = 0; Ei + 1 < E; ++Ei)
		for (int32 Idx = RowInds[Ei]; Idx < RowInds[Ei + 1]; ++Idx) OutPtr[Idx + 1] = Ei + 1;
	for (int32 I = RowInds[E - 1] + 1; I <= NumRows; ++I) OutPtr[I] = E;
}

void FNKSRBlockMatrix::AddBlock(int32 Di, int32 Dj, int32 SizeI, int32 SizeJ,
                                TConstArrayView<int32> AI, TConstArrayView<int32> AJ, TConstArrayView<float> AX)
{
	if (Di < 0 || Dj >= NumBlockRows || Di > Dj)
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRBlockMatrix::AddBlock: invalid block position (%d,%d) for %d block rows (upper triangular only)."), Di, Dj, NumBlockRows);
		return;
	}
	if (SizeI < 0 || SizeJ < 0 || AI.Num() != AJ.Num() || AI.Num() != AX.Num())
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRBlockMatrix::AddBlock(%d,%d): bad sizes (%d,%d) or mismatched COO arrays (%d/%d/%d)."),
			Di, Dj, SizeI, SizeJ, AI.Num(), AJ.Num(), AX.Num());
		return;
	}
	if (!NKSRValidateRowInds(AI, SizeI))
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRBlockMatrix::AddBlock(%d,%d): COO row indices not sorted ascending in [0, %d)."), Di, Dj, SizeI);
		return;
	}
	for (int32 E = 0; E < AJ.Num(); ++E)
	{
		if (AJ[E] < 0 || AJ[E] >= SizeJ)
		{
			UE_LOG(LogNKSR, Error, TEXT("FNKSRBlockMatrix::AddBlock(%d,%d): column index %d out of [0, %d) at entry %d."), Di, Dj, AJ[E], SizeJ, E);
			return;
		}
	}

	BlockSize[Di] = SizeI;
	BlockSize[Dj] = SizeJ;

	if (Di == Dj)
	{
		// Jacobi inverse diagonal (solver.py: 1.0 / a_x[a_i == a_j]); COO row-major order
		// keeps the entries ascending, one per row for a valid SPD assembly.
		TArray<float>& Inv = InvDiagPerDepth[Di];
		Inv.Reset();
		Inv.Reserve(SizeI);
		for (int32 E = 0; E < AI.Num(); ++E) if (AI[E] == AJ[E]) Inv.Add(1.0f / AX[E]);
		if (Inv.Num() != SizeI) UE_LOG(LogNKSR, Warning, TEXT("FNKSRBlockMatrix::AddBlock(%d,%d): %d diagonal entries for %d rows; NKSRSolvePCG will reject this matrix."), Di, Dj, Inv.Num(), SizeI);
	}

	FNKSRCsrBlock Blk;
	NKSRInd2Ptr(AI, SizeI, Blk.RowPtr);
	Blk.ColInds.Append(AJ.GetData(), AJ.Num());
	Blk.Values.Append(AX.GetData(), AX.Num());
	Blocks.Add(FIntPoint(Di, Dj), MoveTemp(Blk));
}

int32 NKSRSolvePCG(const FNKSRBlockMatrix& A, TConstArrayView<float> Rhs, float Tol, int32 MaxIter, TArray<float>& OutX)
{
	OutX.Reset();
	const int32 NumBlocks = A.NumBlockRows;
	if (A.BlockSize.Num() != NumBlocks || A.InvDiagPerDepth.Num() != NumBlocks)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSolvePCG: inconsistent block matrix (%d block rows, %d sizes, %d inv-diag levels)."), NumBlocks, A.BlockSize.Num(), A.InvDiagPerDepth.Num());
		return 0;
	}

	// block_ptr = prefix sums of block_size (solver.py _solve).
	TArray<int32> BlockPtr;
	BlockPtr.SetNumUninitialized(NumBlocks + 1);
	BlockPtr[0] = 0;
	for (int32 D = 0; D < NumBlocks; ++D)
	{
		if (A.BlockSize[D] < 0)
		{
			UE_LOG(LogNKSR, Error, TEXT("NKSRSolvePCG: negative block size %d at depth %d."), A.BlockSize[D], D);
			return 0;
		}
		BlockPtr[D + 1] = BlockPtr[D] + A.BlockSize[D];
	}
	const int32 N = BlockPtr[NumBlocks];
	if (Rhs.Num() != N)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSolvePCG: rhs size %d != total block size %d."), Rhs.Num(), N);
		return 0;
	}

	// Validate CSR blocks once so the matvec can index unchecked. TMap iteration is only
	// used for an order-independent validity check, never to derive result ordering.
	for (const TPair<FIntPoint, FNKSRCsrBlock>& Pair : A.Blocks)
	{
		const FIntPoint Key = Pair.Key;
		const FNKSRCsrBlock& Blk = Pair.Value;
		bool bValid = Key.X >= 0 && Key.X <= Key.Y && Key.Y < NumBlocks
			&& Blk.RowPtr.Num() == A.BlockSize[Key.X] + 1
			&& Blk.ColInds.Num() == Blk.Values.Num()
			&& Blk.RowPtr[0] == 0 && Blk.RowPtr.Last() == Blk.ColInds.Num();
		for (int32 R = 0; bValid && R + 1 < Blk.RowPtr.Num(); ++R) bValid = Blk.RowPtr[R] <= Blk.RowPtr[R + 1];
		for (int32 K = 0; bValid && K < Blk.ColInds.Num(); ++K) bValid = Blk.ColInds[K] >= 0 && Blk.ColInds[K] < A.BlockSize[Key.Y];
		if (!bValid)
		{
			UE_LOG(LogNKSR, Error, TEXT("NKSRSolvePCG: malformed CSR block (%d,%d)."), Key.X, Key.Y);
			return 0;
		}
	}

	// Jacobi preconditioner: concatenate per-depth inverse diagonals in ascending depth
	// (solver.py: torch.cat([t for t in inv_diag if t is not None]); absent levels are empty).
	TArray<float> InvDiag;
	InvDiag.Reserve(N);
	for (int32 D = 0; D < NumBlocks; ++D) InvDiag.Append(A.InvDiagPerDepth[D]);
	if (InvDiag.Num() != N)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSolvePCG: inverse diagonal length %d != %d (missing diagonal block or entries)."), InvDiag.Num(), N);
		return 0;
	}

	if (N == 0) return 0;

	// GC-9: float32 throughout, relative criterion ||r||2 <= Tol * ||b||2.
	const float BNorm = NKSRNorm2(Rhs);
	if (BNorm == 0.f)
	{
		// Port enhancement (spec E §11.3): the reference divides by zero on b == 0; return the exact zero solution.
		OutX.SetNumZeroed(N);
		return 0;
	}
	const float ATol = Tol * BNorm;

	TArray<float> X, P, Z, R, Q;
	X.SetNumZeroed(N);
	P.SetNumZeroed(N);
	Z.SetNumUninitialized(N);
	Q.SetNumUninitialized(N);
	R.Append(Rhs.GetData(), N);   // x = 0 -> r = b - A*x = b (the reference's explicit matvec subtracts exact zeros).

	int32 Iters = 0;
	float Rho = 0.f;
	while (MaxIter < 0 || Iters < MaxIter)
	{
		// z = invDiag ⊙ r (Jacobi preconditioner).
		for (int32 I = 0; I < N; ++I) Z[I] = InvDiag[I] * R[I];
		const float Rho1 = Rho;
		Rho = NKSRDot(R, Z);
		if (Iters == 0) FMemory::Memcpy(P.GetData(), Z.GetData(), N * sizeof(float));
		else
		{
			const float Beta = Rho / Rho1;
			for (int32 I = 0; I < N; ++I) P[I] = Z[I] + Beta * P[I];
		}
		NKSRSymblkMatmul(A, BlockPtr, P, Q);
		const float PQ = NKSRDot(P, Q);
		if (PQ == 0.f)
		{
			// Port enhancement: the reference would divide by zero; treat as stagnation and stop early.
			UE_LOG(LogNKSR, Warning, TEXT("NKSRSolvePCG: dot(p, A*p) == 0 at iteration %d, stopping early."), Iters);
			break;
		}
		const float Alpha = Rho / PQ;
		for (int32 I = 0; I < N; ++I) X[I] += Alpha * P[I];
		// res_fix = False on the inference path -> plain residual update only.
		for (int32 I = 0; I < N; ++I) R[I] -= Alpha * Q[I];
		++Iters;
		if (NKSRNorm2(R) <= ATol) break;
	}

	if (MaxIter > 0 && Iters == MaxIter) UE_LOG(LogNKSR, Warning, TEXT("NKSRSolvePCG: did not converge in %d iterations (||r|| = %g, target %g)."), MaxIter, NKSRNorm2(R), ATol);

	OutX = MoveTemp(X);
	return Iters;
}
