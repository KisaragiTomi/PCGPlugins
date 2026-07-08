// NKSRSvh.cpp — SparseFeatureHierarchy port (nksr/svh.py).
// Port specs: A_inference_flow.md §4 (svh), D_vdbops_grid.md. Conventions GC-1..GC-3
// per Docs/PortSpecs/G_cpp_design.md. Level d: voxelSize_d = VoxelSize * 2^d, origin_d = 0.5 * voxelSize_d.

#include "NKSRSvh.h"

namespace
{
	// Default-constructed FNKSRSvh has an unsized Grids array; all build paths size it lazily.
	void NKSRSvhEnsureGridSlots(FNKSRSvh& Svh)
	{
		if (Svh.Grids.Num() != Svh.Depth) Svh.Grids.SetNum(Svh.Depth);
	}
}

void FNKSRSvh::BuildPointSplatting(TConstArrayView<FVector3f> Points)
{
	if (Points.Num() == 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRSvh::BuildPointSplatting: empty point set"));
		return;
	}
	NKSRSvhEnsureGridSlots(*this);

	// svh.py build_point_splatting: every level built independently from the same points.
	for (int32 D = 0; D < Depth; ++D)
	{
		double LevelVoxelSize = 0.0, LevelOrigin = 0.0;
		GetGridVoxelSizeOrigin(D, LevelVoxelSize, LevelOrigin);
		Grids[D] = MakeUnique<FNKSRIndexGrid>(LevelVoxelSize, LevelOrigin);
		Grids[D]->BuildFromPointsNearestVoxels(Points);
	}
}

void FNKSRSvh::BuildFromGridCoords(int32 D, TConstArrayView<FNKSRIjk> GridCoords, const FIntVector& PadMin, const FIntVector& PadMax)
{
	if (D < 0 || D >= Depth)
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRSvh::BuildFromGridCoords: depth %d out of range [0, %d)"), D, Depth);
		return;
	}
	NKSRSvhEnsureGridSlots(*this);
	if (Grids[D].IsValid())
	{
		// svh.py: assert self.grids[depth] is None, "Grid is not empty"
		UE_LOG(LogNKSR, Error, TEXT("FNKSRSvh::BuildFromGridCoords: grid at depth %d is not empty"), D);
		return;
	}

	double LevelVoxelSize = 0.0, LevelOrigin = 0.0;
	GetGridVoxelSizeOrigin(D, LevelVoxelSize, LevelOrigin);
	Grids[D] = MakeUnique<FNKSRIndexGrid>(LevelVoxelSize, LevelOrigin);
	Grids[D]->BuildFromIjkCoords(GridCoords, PadMin, PadMax);
}

void FNKSRSvh::BuildFromGrid(int32 D, FNKSRIndexGrid&& Grid)
{
	if (D < 0 || D >= Depth)
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRSvh::BuildFromGrid: depth %d out of range [0, %d)"), D, Depth);
		return;
	}
	NKSRSvhEnsureGridSlots(*this);
	if (Grids[D].IsValid())
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRSvh::BuildFromGrid: grid at depth %d is not empty"), D);
		return;
	}

	// svh.py build_from_grid: exact equality asserts on voxel size and origin.
	double LevelVoxelSize = 0.0, LevelOrigin = 0.0;
	GetGridVoxelSizeOrigin(D, LevelVoxelSize, LevelOrigin);
	if (Grid.VoxelSize() != LevelVoxelSize)
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRSvh::BuildFromGrid: voxel size does not match: %.17g vs %.17g"), Grid.VoxelSize(), LevelVoxelSize);
		return;
	}
	if (Grid.Origin() != LevelOrigin)
	{
		UE_LOG(LogNKSR, Error, TEXT("FNKSRSvh::BuildFromGrid: origin does not match: %.17g vs %.17g"), Grid.Origin(), LevelOrigin);
		return;
	}
	Grids[D] = MakeUnique<FNKSRIndexGrid>(MoveTemp(Grid));
}

void FNKSRSvh::GetVoxelCenters(int32 D, TArray<FVector3f>& Out) const
{
	Out.Reset();
	// svh.py get_voxel_centers: grid None -> zeros((0,3)).
	if (D < 0 || D >= Grids.Num() || !Grids[D].IsValid()) return;

	const FNKSRIndexGrid& Grid = *Grids[D];
	TConstArrayView<FNKSRIjk> GridCoords = Grid.ActiveGridCoords();

	// grid_to_world(active_grid_coords().float()): int ijk -> float, then float applyInv (GC-2).
	TArray<FVector3f> GridPts;
	GridPts.SetNumUninitialized(GridCoords.Num());
	for (int32 I = 0; I < GridCoords.Num(); ++I) GridPts[I] = FVector3f((float)GridCoords[I].X, (float)GridCoords[I].Y, (float)GridCoords[I].Z);
	Grid.GridToWorld(GridPts, Out);
}

void FNKSRSvh::EvaluateVoxelStatus(const FNKSRIndexGrid& Grid, int32 D, TArray<uint8>& OutStatus) const
{
	// svh.py evaluate_voxel_status, one-to-one. 0 = non-exist, 1 = exist-stop, 2 = exist-continue.
	OutStatus.Init(0, Grid.NumVoxels());

	if (D < 0 || D >= Grids.Num() || !Grids[D].IsValid()) return;

	// status[exist_idx[exist_idx != -1]] = VS_EXIST_STOP
	TArray<int32> ExistIdx;
	Grid.IjkToIndex(Grids[D]->ActiveGridCoords(), ExistIdx);
	for (int32 Idx : ExistIdx) if (Idx != -1) OutStatus[Idx] = 1;

	// child voxels present one level below: torch.div(coords, 2, rounding_mode='floor') = FloorDiv (GC-1)
	if (D > 0 && Grids[D - 1].IsValid())
	{
		TConstArrayView<FNKSRIjk> FinerCoords = Grids[D - 1]->ActiveGridCoords();
		TArray<FNKSRIjk> ChildCoords;
		ChildCoords.SetNumUninitialized(FinerCoords.Num());
		for (int32 I = 0; I < FinerCoords.Num(); ++I) ChildCoords[I] = FNKSRIjk(NKSRFloorDiv(FinerCoords[I].X, 2), NKSRFloorDiv(FinerCoords[I].Y, 2), NKSRFloorDiv(FinerCoords[I].Z, 2));

		TArray<int32> ChildIdx;
		Grid.IjkToIndex(ChildCoords, ChildIdx);
		for (int32 Idx : ChildIdx) if (Idx != -1) OutStatus[Idx] = 2;
	}
}
