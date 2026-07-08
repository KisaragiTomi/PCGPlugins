// NKSRGrid.cpp — sparse index grid, hash-map replacement of NKSR's nanovdb IndexGrid.
// Port spec: Docs/PortSpecs/D_vdbops_grid.md; reference csrc/vdbops (SparseFeatureIndexGrid.h,
// utils/IndexGridBuilders.h, utils/*InterpolationIterator.h, kernels/cpu/*.cpp).
// Conventions GC-1..GC-5 per Docs/PortSpecs/G_cpp_design.md.

#include "NKSRGrid.h"

#include "Async/ParallelFor.h"

#include <limits>

namespace
{
	// Spec D §3.3 — quadratic bezier bump, support (-1.5, 1.5), half-open pieces exactly as the
	// reference cascade (BezierInterpolationWithGradIterator::bezier). Weights sum to 8, no normalize.
	FORCEINLINE float NKSRBezierB(float X)
	{
		const bool R1 = X < -1.5f, R2 = X < -0.5f, R3 = X < 0.5f, R4 = X < 1.5f;
		if (!R1 && R2) return (X + 1.5f) * (X + 1.5f);
		if (!R2 && R3) return -2.f * X * X + 1.5f;
		if (!R3 && R4) return (X - 1.5f) * (X - 1.5f);
		return 0.f;
	}

	FORCEINLINE float NKSRBezierDB(float X)
	{
		const bool R1 = X < -1.5f, R2 = X < -0.5f, R3 = X < 0.5f, R4 = X < 1.5f;
		if (!R1 && R2) return 2.f * X + 3.f;
		if (!R2 && R3) return -4.f * X;
		if (!R3 && R4) return 2.f * X - 3.f;
		return 0.f;
	}
}

FNKSRIndexGrid::FNKSRIndexGrid(double InVoxelSize, double InOrigin)
	: VoxelSizeD(InVoxelSize)
	, OriginD(InOrigin)
{
	if (InVoxelSize <= 0.0)
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRIndexGrid: invalid voxel size %f, keeping identity transform"), InVoxelSize);
		VoxelSizeD = 1.0;
		OriginD = 0.0;
	}
	// GC-2: parameters computed in double, truncated to float; per-point transforms then stay
	// in float in the algebraic form g = x*Scale + Translate (spec D §1.1, do NOT rewrite).
	const double ScaleDouble = 1.0 / VoxelSizeD;
	const double TranslateDouble = -OriginD / VoxelSizeD;
	Scale = (float)ScaleDouble;
	Translate = (float)TranslateDouble;
	InvScale = (float)VoxelSizeD;
}

void FNKSRIndexGrid::FinalizeCoords(TArray<FNKSRIjk>&& InCoords)
{
	// GC-3: sort by (i,j,k) lexicographic order, dedupe, then Coords[r] <-> Lookup are inverses.
	// All build paths funnel through here so every grid shares the same total order.
	InCoords.Sort([](const FNKSRIjk& A, const FNKSRIjk& B) { return NKSRIjkLess(A, B); });
	int32 Write = 0;
	for (int32 Read = 0; Read < InCoords.Num(); ++Read)
	{
		if (Write > 0 && InCoords[Read] == InCoords[Write - 1]) continue;
		InCoords[Write++] = InCoords[Read];
	}
	InCoords.SetNum(Write, EAllowShrinking::No);
	Coords = MoveTemp(InCoords);

	Lookup.Empty(Coords.Num());
	for (int32 R = 0; R < Coords.Num(); ++R) Lookup.Add(Coords[R], R);
}

// --- topology building (spec D §2.3) ---

void FNKSRIndexGrid::BuildFromPointsNearestVoxels(TConstArrayView<FVector3f> Points)
{
	if (Points.Num() == 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("BuildFromPointsNearestVoxels: empty point set"));
		FinalizeCoords(TArray<FNKSRIjk>());
		return;
	}

	TArray<FNKSRIjk> Pending;
	Pending.Reserve(Points.Num() * 8);
	for (const FVector3f& P : Points)
	{
		const FVector3f G = WorldToGridPoint(P);
		// Reference (buildNearestNeighborGridFromPoints): activate round(g + s), s in {-0.5, +0.5}^3.
		// GC-1: round = RoundHalfUp = floor(x + 0.5).
		for (int32 N = 0; N < 8; ++N)
		{
			const float Sx = (N & 4) ? 0.5f : -0.5f;
			const float Sy = (N & 2) ? 0.5f : -0.5f;
			const float Sz = (N & 1) ? 0.5f : -0.5f;
			Pending.Add(FNKSRIjk(NKSRRoundHalfUp(G.X + Sx), NKSRRoundHalfUp(G.Y + Sy), NKSRRoundHalfUp(G.Z + Sz)));
		}
	}
	FinalizeCoords(MoveTemp(Pending));
}

void FNKSRIndexGrid::BuildFromIjkCoords(TConstArrayView<FNKSRIjk> Ijk, const FIntVector& PadMin, const FIntVector& PadMax)
{
	if (Ijk.Num() == 0)
	{
		// Reference TORCH_CHECK: "Cannot build empty grid, coords tensor was empty."
		UE_LOG(LogNKSR, Error, TEXT("BuildFromIjkCoords: empty coord set"));
		FinalizeCoords(TArray<FNKSRIjk>());
		return;
	}

	const int64 PerCoord =
		int64(FMath::Max(0, PadMax.X - PadMin.X + 1)) *
		int64(FMath::Max(0, PadMax.Y - PadMin.Y + 1)) *
		int64(FMath::Max(0, PadMax.Z - PadMin.Z + 1));

	TArray<FNKSRIjk> Pending;
	Pending.Reserve(int32(FMath::Min<int64>(Ijk.Num() * PerCoord, MAX_int32)));
	for (const FNKSRIjk& C : Ijk)
	{
		for (int32 Di = PadMin.X; Di <= PadMax.X; ++Di)
		{
			for (int32 Dj = PadMin.Y; Dj <= PadMax.Y; ++Dj)
			{
				for (int32 Dk = PadMin.Z; Dk <= PadMax.Z; ++Dk) Pending.Add(FNKSRIjk(C.X + Di, C.Y + Dj, C.Z + Dk));
			}
		}
	}
	FinalizeCoords(MoveTemp(Pending));
}

FNKSRIndexGrid FNKSRIndexGrid::Coarsened(int32 Factor) const
{
	if (Factor <= 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("Coarsened: factor must be > 0 (got %d)"), Factor);
		return FNKSRIndexGrid();
	}
	// Reference coarseVoxSizeAndOrigin: vs_c = b*vs, origin_c = origin + (b-1)*vs*0.5 (double).
	FNKSRIndexGrid Result(double(Factor) * VoxelSizeD, double(Factor - 1) * VoxelSizeD * 0.5 + OriginD);

	TArray<FNKSRIjk> Pending;
	Pending.Reserve(Coords.Num());
	// GC-1: fine -> coarse via FloorDiv (reference: floor(double(ijk)/b), exact for int32 range).
	for (const FNKSRIjk& C : Coords) Pending.Add(FNKSRIjk(NKSRFloorDiv(C.X, Factor), NKSRFloorDiv(C.Y, Factor), NKSRFloorDiv(C.Z, Factor)));
	Result.FinalizeCoords(MoveTemp(Pending));
	return Result;
}

FNKSRIndexGrid FNKSRIndexGrid::Subdivided(int32 Factor, TConstArrayView<bool> Mask) const
{
	if (Factor <= 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("Subdivided: factor must be > 0 (got %d)"), Factor);
		return FNKSRIndexGrid();
	}
	if (Mask.Num() > 0 && Mask.Num() != Coords.Num())
	{
		UE_LOG(LogNKSR, Error, TEXT("Subdivided: mask size %d != num voxels %d"), Mask.Num(), Coords.Num());
		return FNKSRIndexGrid();
	}
	// Reference fineVoxSizeAndOrigin: vs_f = vs/s, origin_f = origin - (s-1)*vs_f*0.5 (double).
	const double FineVs = VoxelSizeD / double(Factor);
	FNKSRIndexGrid Result(FineVs, OriginD - double(Factor - 1) * FineVs * 0.5);

	TArray<FNKSRIjk> Pending;
	for (int32 Idx = 0; Idx < Coords.Num(); ++Idx)
	{
		if (Mask.Num() > 0 && !Mask[Idx]) continue;   // empty mask = all true
		const FNKSRIjk Base(Coords[Idx].X * Factor, Coords[Idx].Y * Factor, Coords[Idx].Z * Factor);
		for (int32 I = 0; I < Factor; ++I)
		{
			for (int32 J = 0; J < Factor; ++J)
			{
				for (int32 K = 0; K < Factor; ++K) Pending.Add(FNKSRIjk(Base.X + I, Base.Y + J, Base.Z + K));
			}
		}
	}
	Result.FinalizeCoords(MoveTemp(Pending));
	return Result;
}

FNKSRIndexGrid FNKSRIndexGrid::Dual() const
{
	// Reference buildDualFromPrimalGrid: same voxel size, primal transform := parent dual transform.
	// GC-2: dual translate computed in double as (-origin/vs + 0.5) then truncated (setTransform form).
	FNKSRIndexGrid Result(VoxelSizeD, OriginD - 0.5 * VoxelSizeD);
	Result.Scale = (float)(1.0 / VoxelSizeD);
	Result.Translate = (float)(-OriginD / VoxelSizeD + 0.5);

	TArray<FNKSRIjk> Pending;
	Pending.Reserve(Coords.Num() * 8);
	for (const FNKSRIjk& C : Coords)
	{
		for (int32 Di = 0; Di <= 1; ++Di)
		{
			for (int32 Dj = 0; Dj <= 1; ++Dj)
			{
				for (int32 Dk = 0; Dk <= 1; ++Dk) Pending.Add(FNKSRIjk(C.X + Di, C.Y + Dj, C.Z + Dk));
			}
		}
	}
	Result.FinalizeCoords(MoveTemp(Pending));
	return Result;
}

// --- queries ---

void FNKSRIndexGrid::IjkToIndex(TConstArrayView<FNKSRIjk> Ijk, TArray<int32>& Out) const
{
	Out.SetNumUninitialized(Ijk.Num());
	int32* OutData = Out.GetData();
	ParallelFor(Ijk.Num(), [this, Ijk, OutData](int32 I) { OutData[I] = IndexOf(Ijk[I]); });
}

void FNKSRIndexGrid::PointsInActiveVoxel(TConstArrayView<FVector3f> Points, TArray<bool>& Out) const
{
	Out.SetNumUninitialized(Points.Num());
	bool* OutData = Out.GetData();
	// Spec D §4.2: isActive(round(primal(p))), round = RoundHalfUp (GC-1).
	ParallelFor(Points.Num(), [this, Points, OutData](int32 I)
	{
		const FVector3f G = WorldToGridPoint(Points[I]);
		OutData[I] = IndexOf(FNKSRIjk(NKSRRoundHalfUp(G.X), NKSRRoundHalfUp(G.Y), NKSRRoundHalfUp(G.Z))) >= 0;
	});
}

// --- coordinate transform (GC-2; spec D §1.1 exact float forms) ---

FVector3f FNKSRIndexGrid::WorldToGridPoint(const FVector3f& P) const
{
	return FVector3f(P.X * Scale + Translate, P.Y * Scale + Translate, P.Z * Scale + Translate);
}

FVector3f FNKSRIndexGrid::GridToWorldPoint(const FVector3f& G) const
{
	// applyInv must stay (g - translate) / scale — NOT g*voxSize + origin (spec D §1.1).
	return FVector3f((G.X - Translate) / Scale, (G.Y - Translate) / Scale, (G.Z - Translate) / Scale);
}

void FNKSRIndexGrid::WorldToGrid(TConstArrayView<FVector3f> Points, TArray<FVector3f>& Out) const
{
	Out.SetNumUninitialized(Points.Num());
	FVector3f* OutData = Out.GetData();
	ParallelFor(Points.Num(), [this, Points, OutData](int32 I) { OutData[I] = WorldToGridPoint(Points[I]); });
}

void FNKSRIndexGrid::GridToWorld(TConstArrayView<FVector3f> GridPts, TArray<FVector3f>& Out) const
{
	Out.SetNumUninitialized(GridPts.Num());
	FVector3f* OutData = Out.GetData();
	ParallelFor(GridPts.Num(), [this, GridPts, OutData](int32 I) { OutData[I] = GridToWorldPoint(GridPts[I]); });
}

// --- interpolation / splatting (spec D §3 / §4) ---

void FNKSRIndexGrid::SampleTrilinear(TConstArrayView<FVector3f> Points, const FNKSRMatrix& GridData, FNKSRMatrix& Out, FNKSRGradTensor* OutGrad) const
{
	if (GridData.Rows != NumVoxels())
	{
		UE_LOG(LogNKSR, Error, TEXT("SampleTrilinear: grid data rows %d != num voxels %d"), GridData.Rows, NumVoxels());
		Out.SetZeroed(0, 0);
		if (OutGrad) OutGrad->SetZeroed(0, 0);
		return;
	}
	const int32 M = Points.Num();
	const int32 D = GridData.Cols;
	Out.SetZeroed(M, D);
	if (OutGrad) OutGrad->SetZeroed(M, D);

	const float ScaleLocal = Scale;
	// ParallelFor OK: each point pi writes only its own output row (accumulation order per point
	// stays the §3.1 stencil order, bit-identical to the single-threaded reference).
	ParallelFor(M, [this, Points, &GridData, &Out, OutGrad, D, ScaleLocal](int32 Pi)
	{
		const FVector3f G = WorldToGridPoint(Points[Pi]);
		// Spec D §3.1: base = floor(g).
		const int32 Bx = FMath::FloorToInt32(G.X), By = FMath::FloorToInt32(G.Y), Bz = FMath::FloorToInt32(G.Z);
		const float Fu = G.X - (float)Bx, Fv = G.Y - (float)By, Fw = G.Z - (float)Bz;
		const float U0 = 1.f - Fu, V0 = 1.f - Fv, W0 = 1.f - Fw;

		// §3.2 table, enumeration n = 0..7 with di=(n>>2)&1, dj=(n>>1)&1, dk=n&1 (k fastest).
		const float Wt[8] = { U0 * V0 * W0, U0 * V0 * Fw, U0 * Fv * W0, U0 * Fv * Fw,
		                      Fu * V0 * W0, Fu * V0 * Fw, Fu * Fv * W0, Fu * Fv * Fw };
		const float Gu[8] = { -V0 * W0, -V0 * Fw, -Fv * W0, -Fv * Fw, V0 * W0, V0 * Fw, Fv * W0, Fv * Fw };
		const float Gv[8] = { -U0 * W0, -U0 * Fw, U0 * W0, U0 * Fw, -Fu * W0, -Fu * Fw, Fu * W0, Fu * Fw };
		const float Gw[8] = { -U0 * V0, U0 * V0, -U0 * Fv, U0 * Fv, -Fu * V0, Fu * V0, -Fu * Fv, Fu * Fv };

		float* OutRow = Out.Row(Pi);
		for (int32 N = 0; N < 8; ++N)
		{
			const FNKSRIjk Ijk(Bx + ((N >> 2) & 1), By + ((N >> 1) & 1), Bz + (N & 1));
			const int32 Idx = IndexOf(Ijk);
			if (Idx < 0) continue;   // GC-5: inactive neighbor contributes 0, no renormalize
			const float* GridRow = GridData.Row(Idx);
			for (int32 J = 0; J < D; ++J)
			{
				OutRow[J] += Wt[N] * GridRow[J];
				if (!OutGrad) continue;
				// world-space gradient = grid gradient * scale (spec D §4.6, same multiply order)
				OutGrad->At(Pi, J, 0) += Gu[N] * GridRow[J] * ScaleLocal;
				OutGrad->At(Pi, J, 1) += Gv[N] * GridRow[J] * ScaleLocal;
				OutGrad->At(Pi, J, 2) += Gw[N] * GridRow[J] * ScaleLocal;
			}
		}
	});
}

void FNKSRIndexGrid::SampleBezier(TConstArrayView<FVector3f> Points, const FNKSRMatrix& GridData, FNKSRMatrix& Out, FNKSRGradTensor* OutGrad) const
{
	if (GridData.Rows != NumVoxels())
	{
		UE_LOG(LogNKSR, Error, TEXT("SampleBezier: grid data rows %d != num voxels %d"), GridData.Rows, NumVoxels());
		Out.SetZeroed(0, 0);
		if (OutGrad) OutGrad->SetZeroed(0, 0);
		return;
	}
	const int32 M = Points.Num();
	const int32 D = GridData.Cols;
	Out.SetZeroed(M, D);
	if (OutGrad) OutGrad->SetZeroed(M, D);

	const float ScaleLocal = Scale;
	ParallelFor(M, [this, Points, &GridData, &Out, OutGrad, D, ScaleLocal](int32 Pi)
	{
		const FVector3f G = WorldToGridPoint(Points[Pi]);
		// Spec D §3.3: base = round(g) = RoundHalfUp (GC-1), uvw in [-0.5, 0.5].
		const int32 Bx = NKSRRoundHalfUp(G.X), By = NKSRRoundHalfUp(G.Y), Bz = NKSRRoundHalfUp(G.Z);
		const float Fu = G.X - (float)Bx, Fv = G.Y - (float)By, Fw = G.Z - (float)Bz;

		float* OutRow = Out.Row(Pi);
		// Enumeration n = 0..26: dz = n%3-1 (fastest), dy = (n/3)%3-1, dx = n/9-1 (slowest).
		for (int32 N = 0; N < 27; ++N)
		{
			const int32 Dz = N % 3 - 1;
			const int32 Dy = (N / 3) % 3 - 1;
			const int32 Dx = N / 9 - 1;
			const int32 Idx = IndexOf(FNKSRIjk(Bx + Dx, By + Dy, Bz + Dz));
			if (Idx < 0) continue;   // GC-5: inactive contributes 0; weights sum to 8, NOT normalized

			const float BxW = NKSRBezierB(Fu - (float)Dx), ByW = NKSRBezierB(Fv - (float)Dy), BzW = NKSRBezierB(Fw - (float)Dz);
			const float W = BxW * ByW * BzW;
			const float* GridRow = GridData.Row(Idx);
			if (!OutGrad)
			{
				for (int32 J = 0; J < D; ++J) OutRow[J] += W * GridRow[J];
				continue;
			}
			const float DBx = NKSRBezierDB(Fu - (float)Dx), DBy = NKSRBezierDB(Fv - (float)Dy), DBz = NKSRBezierDB(Fw - (float)Dz);
			const float Wx = DBx * ByW * BzW, Wy = BxW * DBy * BzW, Wz = BxW * ByW * DBz;
			for (int32 J = 0; J < D; ++J)
			{
				OutRow[J] += W * GridRow[J];
				OutGrad->At(Pi, J, 0) += Wx * GridRow[J] * ScaleLocal;
				OutGrad->At(Pi, J, 1) += Wy * GridRow[J] * ScaleLocal;
				OutGrad->At(Pi, J, 2) += Wz * GridRow[J] * ScaleLocal;
			}
		}
	});
}

void FNKSRIndexGrid::SplatTrilinear(TConstArrayView<FVector3f> Points, const FNKSRMatrix& PointData, FNKSRMatrix& Out) const
{
	if (Points.Num() != PointData.Rows)
	{
		UE_LOG(LogNKSR, Error, TEXT("SplatTrilinear: point count %d != data rows %d"), Points.Num(), PointData.Rows);
		Out.SetZeroed(0, 0);
		return;
	}
	const int32 D = PointData.Cols;
	Out.SetZeroed(NumVoxels(), D);

	// Scatter accumulation: MUST stay single-threaded (float accumulation order = input point
	// order, inner stencil order = §3.1) to match the reference bit for bit.
	for (int32 Pi = 0; Pi < Points.Num(); ++Pi)
	{
		const FVector3f G = WorldToGridPoint(Points[Pi]);
		const int32 Bx = FMath::FloorToInt32(G.X), By = FMath::FloorToInt32(G.Y), Bz = FMath::FloorToInt32(G.Z);
		const float Fu = G.X - (float)Bx, Fv = G.Y - (float)By, Fw = G.Z - (float)Bz;
		const float U0 = 1.f - Fu, V0 = 1.f - Fv, W0 = 1.f - Fw;
		const float Wt[8] = { U0 * V0 * W0, U0 * V0 * Fw, U0 * Fv * W0, U0 * Fv * Fw,
		                      Fu * V0 * W0, Fu * V0 * Fw, Fu * Fv * W0, Fu * Fv * Fw };

		const float* DataRow = PointData.Row(Pi);
		for (int32 N = 0; N < 8; ++N)
		{
			const int32 Idx = IndexOf(FNKSRIjk(Bx + ((N >> 2) & 1), By + ((N >> 1) & 1), Bz + (N & 1)));
			if (Idx < 0) continue;   // GC-5: out-of-grid contribution dropped, no renormalize
			float* OutRow = Out.Row(Idx);
			for (int32 J = 0; J < D; ++J) OutRow[J] += Wt[N] * DataRow[J];
		}
	}
}

// --- hierarchy data movement (spec D §4.8 / §4.9) ---

void FNKSRIndexGrid::Subdivide(const FNKSRMatrix& CoarseData, int32 Factor, const FNKSRIndexGrid& FineGrid, FNKSRMatrix& Out) const
{
	if (Factor <= 0 || CoarseData.Rows != NumVoxels())
	{
		UE_LOG(LogNKSR, Error, TEXT("Subdivide: bad factor %d or coarse data rows %d != num voxels %d"), Factor, CoarseData.Rows, NumVoxels());
		Out.SetZeroed(0, 0);
		return;
	}
	const int32 D = CoarseData.Cols;
	Out.SetZeroed(FineGrid.NumVoxels(), D);

	TConstArrayView<FNKSRIjk> FineCoords = FineGrid.ActiveGridCoords();
	// Each fine row is written by exactly one thread.
	ParallelFor(FineCoords.Num(), [this, FineCoords, &CoarseData, &Out, Factor, D](int32 FineIdx)
	{
		const FNKSRIjk& FineIjk = FineCoords[FineIdx];
		// GC-1: fine -> coarse via FloorDiv (reference: floor(float(ijk)/f), exact in range).
		const FNKSRIjk CoarseIjk(NKSRFloorDiv(FineIjk.X, Factor), NKSRFloorDiv(FineIjk.Y, Factor), NKSRFloorDiv(FineIjk.Z, Factor));
		const int32 CoarseIdx = IndexOf(CoarseIjk);
		if (CoarseIdx < 0) return;   // missing parent -> fine row stays 0 (GC-5)
		FMemory::Memcpy(Out.Row(FineIdx), CoarseData.Row(CoarseIdx), D * sizeof(float));
	});
}

void FNKSRIndexGrid::MaxPool(const FNKSRMatrix& FineData, int32 Factor, const FNKSRIndexGrid& CoarseGrid, FNKSRMatrix& Out) const
{
	if (Factor <= 0 || FineData.Rows != NumVoxels())
	{
		UE_LOG(LogNKSR, Error, TEXT("MaxPool: bad factor %d or fine data rows %d != num voxels %d"), Factor, FineData.Rows, NumVoxels());
		Out.SetZeroed(0, 0);
		return;
	}
	const int32 D = FineData.Cols;
	Out.SetUninitialized(CoarseGrid.NumVoxels(), D);

	constexpr float NegInf = -std::numeric_limits<float>::infinity();
	TConstArrayView<FNKSRIjk> CoarseCoords = CoarseGrid.ActiveGridCoords();
	// Each coarse row is written by exactly one thread.
	ParallelFor(CoarseCoords.Num(), [this, CoarseCoords, &FineData, &Out, Factor, D, NegInf](int32 CoarseIdx)
	{
		const FNKSRIjk& CoarseIjk = CoarseCoords[CoarseIdx];
		float* OutRow = Out.Row(CoarseIdx);
		for (int32 L = 0; L < D; ++L) OutRow[L] = NegInf;

		const FNKSRIjk Base(CoarseIjk.X * Factor, CoarseIjk.Y * Factor, CoarseIjk.Z * Factor);
		for (int32 I = 0; I < Factor; ++I)
		{
			for (int32 J = 0; J < Factor; ++J)
			{
				for (int32 K = 0; K < Factor; ++K)
				{
					const int32 FineIdx = IndexOf(FNKSRIjk(Base.X + I, Base.Y + J, Base.Z + K));
					if (FineIdx < 0) continue;
					const float* FineRow = FineData.Row(FineIdx);
					for (int32 L = 0; L < D; ++L) OutRow[L] = FMath::Max(FineRow[L], OutRow[L]);
				}
			}
		}
		// GC-5: python nn/modules does feat[isinf(feat)] = 0 right after max_pool;
		// per contract this replacement lives inside Grid::MaxPool.
		for (int32 L = 0; L < D; ++L) if (OutRow[L] == NegInf || OutRow[L] == -NegInf) OutRow[L] = 0.f;
	});
}
