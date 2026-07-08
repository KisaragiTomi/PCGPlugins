#pragma once

// NKSR C++ port shared types & numeric conventions.
// Conventions GC-1..GC-9: see Docs/PortSpecs/G_cpp_design.md. Violating any of them
// produces silently wrong results, not errors.

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogNKSR, Log, All);

using FNKSRIjk = FIntVector;

// GC-1: torch/nanovdb rounding — half-up, NOT std::round (half away from zero) nor rint (banker's).
FORCEINLINE int32 NKSRRoundHalfUp(float X) { return (int32)FMath::FloorToFloat(X + 0.5f); }
FORCEINLINE int64 NKSRRoundHalfUp64(double X) { return (int64)FMath::FloorToDouble(X + 0.5); }

// GC-1: floor division (negative operands round toward -inf, unlike C++ '/').
FORCEINLINE int32 NKSRFloorDiv(int32 A, int32 B)
{
	int32 Q = A / B, R = A % B;
	return (R != 0 && ((R < 0) != (B < 0))) ? Q - 1 : Q;
}

// GC-1: non-negative modulo in [0, M) (torch '%' semantics for float).
FORCEINLINE float NKSRPosModF(float X, float M)
{
	float R = FMath::Fmod(X, M);
	return R < 0.f ? R + M : R;
}

/** Row-major float32 matrix. Cols==1 doubles as a column vector. */
struct FNKSRMatrix
{
	TArray<float> Data;
	int32 Rows = 0;
	int32 Cols = 0;

	void SetZeroed(int32 InRows, int32 InCols)
	{
		Rows = InRows; Cols = InCols;
		Data.SetNumZeroed(Rows * Cols);
	}
	void SetUninitialized(int32 InRows, int32 InCols)
	{
		Rows = InRows; Cols = InCols;
		Data.SetNumUninitialized(Rows * Cols);
	}
	FORCEINLINE float& At(int32 R, int32 C) { return Data[R * Cols + C]; }
	FORCEINLINE float At(int32 R, int32 C) const { return Data[R * Cols + C]; }
	FORCEINLINE float* Row(int32 R) { return Data.GetData() + R * Cols; }
	FORCEINLINE const float* Row(int32 R) const { return Data.GetData() + R * Cols; }
	FORCEINLINE bool IsEmpty() const { return Rows == 0 || Cols == 0; }
};

/**
 * Dense [Rows][Channels][3] float32 tensor (kernel gradients w.r.t. xyz).
 * Rows==0 encodes python's zeros((0,C,3)) placeholder: consumers treat it as "no gradient kernel".
 */
struct FNKSRGradTensor
{
	TArray<float> Data;
	int32 Rows = 0;
	int32 Channels = 0;

	void SetZeroed(int32 InRows, int32 InChannels)
	{
		Rows = InRows; Channels = InChannels;
		Data.SetNumZeroed(Rows * Channels * 3);
	}
	FORCEINLINE float& At(int32 R, int32 C, int32 Axis) { return Data[(R * Channels + C) * 3 + Axis]; }
	FORCEINLINE float At(int32 R, int32 C, int32 Axis) const { return Data[(R * Channels + C) * 3 + Axis]; }
	FORCEINLINE bool IsEmpty() const { return Rows == 0; }
};

/** GC-3: lexicographic (i, then j, then k) comparison used to order active voxels. */
FORCEINLINE bool NKSRIjkLess(const FNKSRIjk& A, const FNKSRIjk& B)
{
	if (A.X != B.X) return A.X < B.X;
	if (A.Y != B.Y) return A.Y < B.Y;
	return A.Z < B.Z;
}
