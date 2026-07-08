#pragma once

// Symmetric-block Jacobi-preconditioned CG (mirror of _C.sparse_solve, CPU path).
// Port spec: E_meshing_solver.md §solver. GC-9: float32, ||r|| <= tol*||b||, non-convergence warns only.

#include "NKSRCommon.h"

/** ind2ptr: COO row indices (sorted ascending) -> CSR row pointer of size NumRows+1. */
void NKSRInd2Ptr(TConstArrayView<int32> RowInds, int32 NumRows, TArray<int32>& OutPtr);

struct FNKSRCsrBlock
{
	TArray<int32> RowPtr;
	TArray<int32> ColInds;
	TArray<float> Values;
};

/**
 * Upper-triangular block matrix over hierarchy depths (block (di,dj) with di <= dj);
 * lower blocks are applied via transpose inside the matvec.
 */
struct FNKSRBlockMatrix
{
	int32 NumBlockRows = 0;                       // = svh depth
	TArray<int32> BlockSize;                      // rows per depth (0 for absent levels)
	TMap<FIntPoint, FNKSRCsrBlock> Blocks;        // key (di, dj)
	TArray<TArray<float>> InvDiagPerDepth;        // 1/diag from (di==dj) blocks, per depth

	explicit FNKSRBlockMatrix(int32 InNumBlockRows = 0)
		: NumBlockRows(InNumBlockRows)
	{
		BlockSize.SetNumZeroed(InNumBlockRows);
		InvDiagPerDepth.SetNum(InNumBlockRows);
	}

	/**
	 * SparseMatrix.add_block: COO (AI, AJ, AX) with AI sorted ascending; converts to CSR via NKSRInd2Ptr.
	 * For di == dj also extracts InvDiag from entries with AI == AJ (python: 1.0 / a_x[a_i == a_j]).
	 */
	void AddBlock(int32 Di, int32 Dj, int32 SizeI, int32 SizeJ,
	              TConstArrayView<int32> AI, TConstArrayView<int32> AJ, TConstArrayView<float> AX);
};

/**
 * solve_pcg: returns iteration count. Rhs is the depth-concatenated vector (ascending depth,
 * absent levels contribute nothing). OutX sized like Rhs. Guards b==0 / dot(p,q)==0 with early exit.
 */
int32 NKSRSolvePCG(const FNKSRBlockMatrix& A, TConstArrayView<float> Rhs, float Tol, int32 MaxIter, TArray<float>& OutX);
