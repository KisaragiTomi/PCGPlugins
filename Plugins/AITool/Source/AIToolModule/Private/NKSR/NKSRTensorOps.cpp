// Dense float32 tensor primitives for the NKSR port.
// This is the ONLY translation unit in the NKSR module allowed to include Eigen.
// Semantics: B_network_arch.md §2/§3.2/§4, G_cpp_design.md GC-5/GC-6/GC-9.

#include "NKSRTensorOps.h"

THIRD_PARTY_INCLUDES_START
#include <Eigen/Dense>
THIRD_PARTY_INCLUDES_END

#include <limits>

namespace
{
	using FEigenRowMajor = Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
	using FConstMap = Eigen::Map<const FEigenRowMajor>;
	using FMutableMap = Eigen::Map<FEigenRowMajor>;

	FConstMap MapConst(const FNKSRMatrix& M) { return FConstMap(M.Data.GetData(), M.Rows, M.Cols); }
	FMutableMap MapMutable(FNKSRMatrix& M) { return FMutableMap(M.Data.GetData(), M.Rows, M.Cols); }
}

namespace NKSRTensorOps
{

void Gemm(const FNKSRMatrix& A, const FNKSRMatrix& B, FNKSRMatrix& Out)
{
	if (A.Cols != B.Rows)
	{
		UE_LOG(LogNKSR, Error, TEXT("Gemm: inner dim mismatch A[%d,%d] * B[%d,%d]"), A.Rows, A.Cols, B.Rows, B.Cols);
		Out = FNKSRMatrix();
		return;
	}
	if (&Out == &A || &Out == &B)
	{
		FNKSRMatrix Tmp;
		Gemm(A, B, Tmp);
		Out = MoveTemp(Tmp);
		return;
	}
	Out.SetUninitialized(A.Rows, B.Cols);
	MapMutable(Out).noalias() = MapConst(A) * MapConst(B);
}

void Linear(const FNKSRMatrix& X, const FNKSRMatrix& W, const FNKSRMatrix* Bias, FNKSRMatrix& Out)
{
	// torch nn.Linear: W is [OutDim, InDim] straight from the checkpoint; Out = X * W^T + b.
	if (X.Cols != W.Cols)
	{
		UE_LOG(LogNKSR, Error, TEXT("Linear: X[%d,%d] incompatible with W[%d,%d]"), X.Rows, X.Cols, W.Rows, W.Cols);
		Out = FNKSRMatrix();
		return;
	}
	if (Bias && Bias->Data.Num() != W.Rows)
	{
		UE_LOG(LogNKSR, Error, TEXT("Linear: bias has %d elements, expected %d"), Bias->Data.Num(), W.Rows);
		Out = FNKSRMatrix();
		return;
	}
	if (&Out == &X || &Out == &W || &Out == Bias)
	{
		FNKSRMatrix Tmp;
		Linear(X, W, Bias, Tmp);
		Out = MoveTemp(Tmp);
		return;
	}
	Out.SetUninitialized(X.Rows, W.Rows);
	FMutableMap OutMap = MapMutable(Out);
	OutMap.noalias() = MapConst(X) * MapConst(W).transpose();
	if (Bias) OutMap.rowwise() += Eigen::Map<const Eigen::RowVectorXf>(Bias->Data.GetData(), W.Rows);
}

void ReluInPlace(FNKSRMatrix& X)
{
	if (X.IsEmpty()) return;
	MapMutable(X) = MapMutable(X).cwiseMax(0.f);
}

void GroupNorm(FNKSRMatrix& X, int32 NumGroups, TConstArrayView<float> Weight, TConstArrayView<float> Bias, float Eps)
{
	const int32 N = X.Rows;
	const int32 C = X.Cols;
	if (NumGroups <= 0 || C <= 0 || (C % NumGroups) != 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("GroupNorm: channels %d not divisible by %d groups"), C, NumGroups);
		return;
	}
	if (Weight.Num() != C || Bias.Num() != C)
	{
		UE_LOG(LogNKSR, Error, TEXT("GroupNorm: affine params (%d/%d) do not match %d channels"), Weight.Num(), Bias.Num(), C);
		return;
	}
	if (N == 0)
	{
		// torch would produce NaN statistics here; ks inference guarantees N>0 upstream (spec B §3.2).
		UE_LOG(LogNKSR, Warning, TEXT("GroupNorm: called with 0 rows, leaving input unchanged"));
		return;
	}

	// GC-6: statistics per group over the ENTIRE sparse tensor at once (all N rows x C/G channels),
	// biased variance, eps added to variance before rsqrt. Matches torch nn.GroupNorm eval on
	// input reshaped to [1, C, N]. GC-9: float32 accumulation throughout.
	const int32 ChannelsPerGroup = C / NumGroups;
	FMutableMap XMap = MapMutable(X);

	// Fold mean/rstd into per-channel scale/shift like ATen's CPU GroupNorm kernel does:
	// y = x * (rstd * w[c]) + (b[c] - mean * rstd * w[c]).
	Eigen::Array<float, 1, Eigen::Dynamic> ScaleArr(C);
	Eigen::Array<float, 1, Eigen::Dynamic> ShiftArr(C);
	for (int32 G = 0; G < NumGroups; ++G)
	{
		const int32 C0 = G * ChannelsPerGroup;
		const auto Block = XMap.block(0, C0, N, ChannelsPerGroup);
		const float Count = (float)N * (float)ChannelsPerGroup;
		const float Mean = Block.sum() / Count;
		const float Var = (Block.array() - Mean).square().sum() / Count; // biased (divide by n)
		const float RStd = 1.f / FMath::Sqrt(Var + Eps);
		for (int32 Ch = 0; Ch < ChannelsPerGroup; ++Ch)
		{
			const int32 CIdx = C0 + Ch;
			ScaleArr(CIdx) = RStd * Weight[CIdx];
			ShiftArr(CIdx) = Bias[CIdx] - Mean * ScaleArr(CIdx);
		}
	}
	XMap.array().rowwise() *= ScaleArr;
	XMap.array().rowwise() += ShiftArr;
}

void ScatterMax(const FNKSRMatrix& Src, TConstArrayView<int32> Index, int32 DimSize, FNKSRMatrix& Out)
{
	if (Index.Num() != Src.Rows)
	{
		UE_LOG(LogNKSR, Error, TEXT("ScatterMax: %d indices for %d source rows"), Index.Num(), Src.Rows);
		Out = FNKSRMatrix();
		return;
	}
	if (DimSize < 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("ScatterMax: negative DimSize %d"), DimSize);
		Out = FNKSRMatrix();
		return;
	}
	const int32 C = Src.Cols;
	// torch_scatter behavior: initialize with lowest(), reduce max, then reset untouched slots to 0.
	// Non-empty buckets keep their max even when all contributions are negative; empty buckets -> 0 (GC-5).
	const float Lowest = std::numeric_limits<float>::lowest();
	Out.SetUninitialized(DimSize, C);
	for (float& V : Out.Data) V = Lowest;

	// Scatter loop: single-threaded for determinism (multiple rows may hit one bucket).
	for (int32 R = 0; R < Src.Rows; ++R)
	{
		const int32 B = Index[R];
		if (B < 0 || B >= DimSize) continue; // invalid index: skip
		const float* SrcRow = Src.Row(R);
		float* OutRow = Out.Row(B);
		for (int32 Ch = 0; Ch < C; ++Ch) if (SrcRow[Ch] > OutRow[Ch]) OutRow[Ch] = SrcRow[Ch];
	}
	for (float& V : Out.Data) if (V == Lowest) V = 0.f;
}

void ScatterMean(const FNKSRMatrix& Src, TConstArrayView<int32> Index, int32 DimSize, FNKSRMatrix& Out)
{
	if (Index.Num() != Src.Rows)
	{
		UE_LOG(LogNKSR, Error, TEXT("ScatterMean: %d indices for %d source rows"), Index.Num(), Src.Rows);
		Out = FNKSRMatrix();
		return;
	}
	if (DimSize < 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("ScatterMean: negative DimSize %d"), DimSize);
		Out = FNKSRMatrix();
		return;
	}
	const int32 C = Src.Cols;
	Out.SetZeroed(DimSize, C);
	TArray<int32> Counts;
	Counts.SetNumZeroed(DimSize);

	// Scatter loop: single-threaded for determinism.
	for (int32 R = 0; R < Src.Rows; ++R)
	{
		const int32 B = Index[R];
		if (B < 0 || B >= DimSize) continue; // invalid index: skip
		const float* SrcRow = Src.Row(R);
		float* OutRow = Out.Row(B);
		for (int32 Ch = 0; Ch < C; ++Ch) OutRow[Ch] += SrcRow[Ch];
		++Counts[B];
	}
	// torch_scatter scatter_mean = scatter_sum / count.clamp(min=1): empty buckets stay 0 (GC-5).
	// True division (not multiply-by-reciprocal) for bit-exact parity with torch.
	for (int32 B = 0; B < DimSize; ++B)
	{
		if (Counts[B] <= 1) continue;
		const float CountF = (float)Counts[B];
		float* OutRow = Out.Row(B);
		for (int32 Ch = 0; Ch < C; ++Ch) OutRow[Ch] /= CountF;
	}
}

void ConcatCols(const FNKSRMatrix& A, const FNKSRMatrix& B, FNKSRMatrix& Out)
{
	if (A.Rows != B.Rows)
	{
		UE_LOG(LogNKSR, Error, TEXT("ConcatCols: row mismatch A[%d,%d] | B[%d,%d]"), A.Rows, A.Cols, B.Rows, B.Cols);
		Out = FNKSRMatrix();
		return;
	}
	if (&Out == &A || &Out == &B)
	{
		FNKSRMatrix Tmp;
		ConcatCols(A, B, Tmp);
		Out = MoveTemp(Tmp);
		return;
	}
	Out.SetUninitialized(A.Rows, A.Cols + B.Cols);
	for (int32 R = 0; R < A.Rows; ++R)
	{
		float* OutRow = Out.Row(R);
		if (A.Cols > 0) FMemory::Memcpy(OutRow, A.Row(R), A.Cols * sizeof(float));
		if (B.Cols > 0) FMemory::Memcpy(OutRow + A.Cols, B.Row(R), B.Cols * sizeof(float));
	}
}

void GatherRows(const FNKSRMatrix& Src, TConstArrayView<int32> Index, FNKSRMatrix& Out)
{
	for (int32 R = 0; R < Index.Num(); ++R)
	{
		if (Index[R] < 0 || Index[R] >= Src.Rows)
		{
			UE_LOG(LogNKSR, Error, TEXT("GatherRows: index %d at row %d out of range [0,%d)"), Index[R], R, Src.Rows);
			Out = FNKSRMatrix();
			return;
		}
	}
	if (&Out == &Src)
	{
		FNKSRMatrix Tmp;
		GatherRows(Src, Index, Tmp);
		Out = MoveTemp(Tmp);
		return;
	}
	Out.SetUninitialized(Index.Num(), Src.Cols);
	if (Src.Cols == 0) return;
	for (int32 R = 0; R < Index.Num(); ++R) FMemory::Memcpy(Out.Row(R), Src.Row(Index[R]), Src.Cols * sizeof(float));
}

} // namespace NKSRTensorOps
