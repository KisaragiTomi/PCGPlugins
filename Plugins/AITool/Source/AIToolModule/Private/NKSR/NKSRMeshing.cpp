// Dual marching-cubes extraction + MISE subdivision + trim (mirror of _C.meshing CPU path).
// Port spec: Docs/PortSpecs/E_meshing_solver.md (§2-9, §12), A_inference_flow.md §8.
// Reference sources: csrc/meshing/{grid_builder.h, inds_cpu.cpp, mc_cpu.cpp, meshing.h},
// csrc/common/iter_util.h, nksr/utils.py (subdivide_cube_indices, apply_vertex_mask).
// Conventions GC-1..GC-7 per G_cpp_design.md.

#include "NKSRMeshing.h"
#include "NKSRGrid.h"
#include "NKSRMCTables.h"

#include "Algo/Sort.h"
#include "Async/ParallelFor.h"

namespace
{

using namespace NKSRMC;

// GC-1: floor-to-even per component. Mirrors OctChildrenIterator's "(p >> 1) << 1"
// (iter_util.h) which relies on arithmetic shift: (-3 >> 1) << 1 == -4. Implemented with
// FloorDiv so the negative-coordinate behavior is guaranteed, not implementation-defined.
FORCEINLINE FNKSRIjk NKSRFloorEven(const FNKSRIjk& P)
{
	return FNKSRIjk(NKSRFloorDiv(P.X, 2) * 2, NKSRFloorDiv(P.Y, 2) * 2, NKSRFloorDiv(P.Z, 2) * 2);
}

// OctChildrenIterator step: child #Count of the (even) octant base. dz = count % 2,
// dy = (count / 2) % 2, dx = count / 4 -> z fastest, x slowest (iter_util.h DeltaFromCount).
FORCEINLINE FNKSRIjk NKSROctChild(const FNKSRIjk& EvenBase, int32 Count)
{
	return FNKSRIjk(EvenBase.X + Count / 4, EvenBase.Y + (Count / 2) % 2, EvenBase.Z + Count % 2);
}

/**
 * Mirror of CubeFaceIterator<Stride> (iter_util.h): enumerates every dual lattice point on the
 * surface of the Stride^3 cube based at BaseStrided, together with the dual-cube slots that this
 * primal voxel occupies at that point. Enumeration order: 8 corners, then 12 edges x (Stride-1)
 * inner points, then 6 faces x (Stride-1)^2 inner points (order preserved for fidelity; results
 * do not depend on it because slots are unique per primal voxel within one level).
 * Visit signature: (const FNKSRIjk& Coord, const int32* Slots, int32 NumSlots).
 */
template <typename VisitorT>
void NKSREnumCubeFacePoints(const FNKSRIjk& BaseStrided, int32 Stride, VisitorT&& Visit)
{
	// Corners.
	for (int32 C = 0; C < 8; ++C)
	{
		const FNKSRIjk Coord(
			BaseStrided.X + cornerAxisTable[C][0] * Stride,
			BaseStrided.Y + cornerAxisTable[C][1] * Stride,
			BaseStrided.Z + cornerAxisTable[C][2] * Stride);
		Visit(Coord, &cornerAccIndsTable[C], 1);
	}
	if (Stride <= 1) return;

	// Edge inner points.
	for (int32 E = 0; E < 12; ++E)
	{
		for (int32 Inner = 0; Inner < Stride - 1; ++Inner)
		{
			FNKSRIjk Coord = BaseStrided;
			Coord[edgeAxisTable[E][2]] += edgeAxisTable[E][0] * Stride;
			Coord[edgeAxisTable[E][3]] += edgeAxisTable[E][1] * Stride;
			Coord[edgeAxisTable[E][4]] += Inner + 1;
			Visit(Coord, edgeAccIndsTable[E], 2);
		}
	}

	// Face inner points.
	for (int32 F = 0; F < 6; ++F)
	{
		for (int32 Inner = 0; Inner < (Stride - 1) * (Stride - 1); ++Inner)
		{
			FNKSRIjk Coord = BaseStrided;
			Coord[faceAxisTable[F][1]] += faceAxisTable[F][0] * Stride;
			Coord[faceAxisTable[F][2]] += Inner % (Stride - 1) + 1;
			Coord[faceAxisTable[F][3]] += Inner / (Stride - 1) + 1;
			Visit(Coord, faceAccIndsTable[F], 4);
		}
	}
}

/**
 * GC-6: torch.unique(dim=0, return_inverse=True) over fixed-width int64 rows.
 * OutUniqueFlat receives the distinct rows in ascending lexicographic order;
 * OutInverse[i] is the position of input row i inside the unique set.
 */
void NKSRUniqueRowsSorted(const TArray<int64>& Flat, int32 Width, TArray<int64>& OutUniqueFlat, TArray<int32>& OutInverse)
{
	const int32 NumRows = Flat.Num() / Width;
	TArray<int32> Perm;
	Perm.SetNumUninitialized(NumRows);
	for (int32 I = 0; I < NumRows; ++I) Perm[I] = I;

	const int64* Data = Flat.GetData();
	auto RowLess = [Data, Width](int32 A, int32 B)
	{
		const int64* Ra = Data + (int64)A * Width;
		const int64* Rb = Data + (int64)B * Width;
		for (int32 C = 0; C < Width; ++C) if (Ra[C] != Rb[C]) return Ra[C] < Rb[C];
		return false;
	};
	Algo::Sort(Perm, RowLess);

	OutUniqueFlat.Reset();
	OutInverse.SetNumUninitialized(NumRows);
	int32 NumUnique = 0;
	for (int32 I = 0; I < NumRows; ++I)
	{
		const int32 Src = Perm[I];
		if (I == 0 || RowLess(Perm[I - 1], Src))
		{
			OutUniqueFlat.Append(Data + (int64)Src * Width, Width);
			++NumUnique;
		}
		OutInverse[Src] = NumUnique - 1;
	}
}

// sdf_interp (csrc/meshing/meshing.h): edge zero-crossing with the 1e-5 guards of spec E §8.2.
FORCEINLINE FVector3f NKSRSdfInterp(const FVector3f& P1, const FVector3f& P2, float V1, float V2)
{
	if (FMath::Abs(V1) < 1.0e-5f) return P1;
	if (FMath::Abs(V2) < 1.0e-5f) return P2;
	if (FMath::Abs(V1 - V2) < 1.0e-5f) return P1;
	const float W2 = (0.f - V1) / (V2 - V1);
	const float W1 = 1.f - W2;
	return P1 * W1 + P2 * W2;
}

bool NKSRValidateGraphIndices(TConstArrayView<int64> CubeGraph, int64 NumVertices, const TCHAR* Caller)
{
	if (CubeGraph.Num() % 8 != 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("%s: cube graph size %d is not a multiple of 8."), Caller, CubeGraph.Num());
		return false;
	}
	for (const int64 Idx : CubeGraph)
	{
		if (Idx < 0 || Idx >= NumVertices)
		{
			UE_LOG(LogNKSR, Error, TEXT("%s: graph index %lld out of range [0, %lld)."), Caller, Idx, NumVertices);
			return false;
		}
	}
	return true;
}

} // namespace

FNKSRIndexGrid NKSRBuildFlattenedGrid(const FNKSRIndexGrid& Grid, const FNKSRIndexGrid* FinerGrid, bool bConforming)
{
	// buildFlattenedGrid (grid_builder.h): remove voxels covered by the finer level; on
	// conforming levels additionally pad each partially refined octant with all 8 siblings.
	const TConstArrayView<FNKSRIjk> Coords = Grid.ActiveGridCoords();

	// ChildMask[vi]: voxel vi has at least one active child in the finer grid.
	TArray<bool> ChildMask;
	ChildMask.SetNumZeroed(Coords.Num());
	if (FinerGrid)
	{
		for (int32 Vi = 0; Vi < Coords.Num(); ++Vi)
		{
			// OctChildrenIterator(v << 1): v*2 is even, so the octant base is v*2 itself.
			const FNKSRIjk Doubled(Coords[Vi].X * 2, Coords[Vi].Y * 2, Coords[Vi].Z * 2);
			for (int32 C = 0; C < 8; ++C)
			{
				if (FinerGrid->IndexOf(NKSROctChild(Doubled, C)) != -1)
				{
					ChildMask[Vi] = true;
					break;
				}
			}
		}
	}

	TArray<FNKSRIjk> NewVoxels;
	if (!bConforming)
	{
		// Coarsest level: just drop voxels that have children.
		NewVoxels.Reserve(Coords.Num());
		for (int32 Vi = 0; Vi < Coords.Num(); ++Vi) if (!ChildMask[Vi]) NewVoxels.Add(Coords[Vi]);
	}
	else
	{
		// Other levels: for every active voxel emit all 8 octant siblings that are either
		// inactive in this grid (intentional padding!) or active without children.
		NewVoxels.Reserve(Coords.Num() * 8);
		for (const FNKSRIjk& V : Coords)
		{
			const FNKSRIjk OctBase = NKSRFloorEven(V);
			for (int32 C = 0; C < 8; ++C)
			{
				const FNKSRIjk Sibling = NKSROctChild(OctBase, C);
				const int32 Si = Grid.IndexOf(Sibling);
				if (Si == -1 || !ChildMask[Si]) NewVoxels.Add(Sibling);
			}
		}
	}

	FNKSRIndexGrid Out(Grid.VoxelSize(), Grid.Origin());
	Out.BuildFromIjkCoords(NewVoxels); // sorts + dedupes (GC-3)
	return Out;
}

FNKSRIndexGrid NKSRBuildJointDualGrid(TConstArrayView<const FNKSRIndexGrid*> FlattenedGrids)
{
	// buildJointDualGrid (grid_builder.h): dual lattice at the corners of all flattened voxels.
	if (FlattenedGrids.Num() == 0 || FlattenedGrids[0] == nullptr)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRBuildJointDualGrid: level-0 grid is required."));
		return FNKSRIndexGrid();
	}
	const double Vs0 = FlattenedGrids[0]->VoxelSize();
	const double Org0 = FlattenedGrids[0]->Origin();
	const double DualOrigin = Org0 - 0.5 * Vs0;

	TArray<FNKSRIjk> DualVoxels;
	for (const FNKSRIndexGrid* G : FlattenedGrids)
	{
		if (!G) continue;
		const double Vs = G->VoxelSize();
		const double Org = G->Origin();
		for (const FNKSRIjk& Ijk : G->ActiveGridCoords())
		{
			for (int32 Ax = -1; Ax < 2; Ax += 2)
			{
				for (int32 Ay = -1; Ay < 2; Ay += 2)
				{
					for (int32 Az = -1; Az < 2; Az += 2)
					{
						// Corner world position -> dual grid coordinate. Double precision on
						// purpose (matches the reference and the GC-2 construction exception):
						// corners land exactly on dual lattice points, double keeps RoundHalfUp
						// (GC-1) away from .5-boundary jitter (spec E §13).
						const double Wx = ((double)Ijk.X + 0.5 * Ax) * Vs + Org;
						const double Wy = ((double)Ijk.Y + 0.5 * Ay) * Vs + Org;
						const double Wz = ((double)Ijk.Z + 0.5 * Az) * Vs + Org;
						DualVoxels.Emplace(
							(int32)NKSRRoundHalfUp64((Wx - DualOrigin) / Vs0),
							(int32)NKSRRoundHalfUp64((Wy - DualOrigin) / Vs0),
							(int32)NKSRRoundHalfUp64((Wz - DualOrigin) / Vs0));
					}
				}
			}
		}
	}

	FNKSRIndexGrid Out(Vs0, DualOrigin);
	Out.BuildFromIjkCoords(DualVoxels); // sorts + dedupes (GC-3)
	return Out;
}

void NKSRDualCubeGraph(TConstArrayView<const FNKSRIndexGrid*> FlattenedGrids, const FNKSRIndexGrid& DualGrid, TArray<int64>& OutGraph)
{
	// dualCubeGraph (inds_cpu.cpp): fill the 8 corner slots of every dual cell with GLOBAL primal
	// voxel indices; global index = per-level voxel index + prefix sum of preceding non-empty
	// level sizes (levels ascending, null/empty levels skipped and not counted) -- must match the
	// dmcVertices concatenation order (spec E §5/§6).
	OutGraph.Reset();
	const int32 NumCorner = DualGrid.NumVoxels();
	if (NumCorner > MAX_int32 / 8)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRDualCubeGraph: dual grid too large (%d voxels)."), NumCorner);
		return;
	}

	TArray<int64> Graph;
	Graph.Init(-1, NumCorner * 8);

	int64 BaseIdx = 0;
	for (int32 Level = 0; Level < FlattenedGrids.Num(); ++Level)
	{
		const FNKSRIndexGrid* G = FlattenedGrids[Level];
		if (!G || G->NumVoxels() <= 0) continue;
		if (Level > 3)
		{
			// Reference only instantiates Stride = 1/2/4/8 and throws beyond (inds_cpu.cpp).
			UE_LOG(LogNKSR, Error, TEXT("NKSRDualCubeGraph: DMC with stride > 8 (level %d) not supported."), Level);
			OutGraph.Reset();
			return;
		}
		const int32 Stride = 1 << Level;

		const TConstArrayView<FNKSRIjk> Coords = G->ActiveGridCoords();
		for (int32 Pi = 0; Pi < Coords.Num(); ++Pi)
		{
			const FNKSRIjk BaseStrided(Coords[Pi].X * Stride, Coords[Pi].Y * Stride, Coords[Pi].Z * Stride);
			NKSREnumCubeFacePoints(BaseStrided, Stride,
				[&Graph, &DualGrid, BaseIdx, Pi](const FNKSRIjk& Coord, const int32* Slots, int32 NumSlots)
				{
					const int32 Vi = DualGrid.IndexOf(Coord);
					if (Vi == -1) return;
					// Later (coarser) levels overwrite earlier writes -- keep levels ascending.
					for (int32 A = 0; A < NumSlots; ++A) Graph[Vi * 8 + Slots[A]] = BaseIdx + Pi;
				});
		}
		BaseIdx += G->NumVoxels();
	}

	// Keep only rows with all 8 slots filled, preserving row order.
	OutGraph.Reserve(Graph.Num());
	for (int32 R = 0; R < NumCorner; ++R)
	{
		bool bFull = true;
		for (int32 S = 0; S < 8; ++S)
		{
			if (Graph[R * 8 + S] == -1)
			{
				bFull = false;
				break;
			}
		}
		if (bFull) OutGraph.Append(Graph.GetData() + R * 8, 8);
	}
}

void NKSRMarchingCubes(
	TConstArrayView<int64> CubeGraph,
	TConstArrayView<FVector3f> CornerPos,
	TConstArrayView<float> CornerValue,
	TArray<FVector3f>& OutV,
	TArray<FIntVector>& OutF)
{
	// MarchingCubesCPU (mc_cpu.cpp): two passes + weld on sorted corner-index pairs.
	OutV.Reset();
	OutF.Reset();
	if (CornerPos.Num() != CornerValue.Num())
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRMarchingCubes: CornerPos (%d) / CornerValue (%d) size mismatch."), CornerPos.Num(), CornerValue.Num());
		return;
	}
	if (!NKSRValidateGraphIndices(CubeGraph, CornerPos.Num(), TEXT("NKSRMarchingCubes"))) return;

	const int32 NumCubes = CubeGraph.Num() / 8;
	if (NumCubes == 0) return;

	// Pass 1: per-cube output vertex count. ParallelFor OK: one writer per element.
	TArray<int32> VertCount;
	VertCount.SetNumUninitialized(NumCubes);
	ParallelFor(NumCubes, [&CubeGraph, &CornerValue, &VertCount](int32 CubeIdx)
	{
		int32 CubeType = 0;
		// GC-7: bit i set iff sdf[i] < 0 (strict).
		for (int32 I = 0; I < 8; ++I) if (CornerValue[(int32)CubeGraph[CubeIdx * 8 + I]] < 0.f) CubeType |= (1 << I);
		VertCount[CubeIdx] = numVertsTable[CubeType];
	});

	// Exclusive prefix sum (serial, deterministic).
	TArray<int64> BaseVert;
	BaseVert.SetNumUninitialized(NumCubes);
	int64 TotalVerts = 0;
	for (int32 C = 0; C < NumCubes; ++C)
	{
		BaseVert[C] = TotalVerts;
		TotalVerts += VertCount[C];
	}
	if (TotalVerts * 2 > MAX_int32)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRMarchingCubes: %lld output triangle corners exceed TArray capacity."), TotalVerts);
		return;
	}
	const int32 NumTris = (int32)(TotalVerts / 3);
	if (NumTris == 0) return;

	// Pass 2: emit triangle corner positions + weld keys. ParallelFor OK: each cube owns a
	// disjoint triangle range starting at BaseVert[cube] / 3.
	TArray<FVector3f> TriPos;
	TriPos.SetNumZeroed(NumTris * 3);
	TArray<int64> TriKey; // (NumTris*3) x 2 flat, key = (max, min) global corner ids
	TriKey.SetNumZeroed(NumTris * 3 * 2);

	ParallelFor(NumCubes, [&CubeGraph, &CornerPos, &CornerValue, &BaseVert, &TriPos, &TriKey](int32 CubeIdx)
	{
		int64 PointIds[8];
		float Sdf[8];
		FVector3f Pos[8];
		for (int32 I = 0; I < 8; ++I)
		{
			PointIds[I] = CubeGraph[CubeIdx * 8 + I];
			Sdf[I] = CornerValue[(int32)PointIds[I]];
			Pos[I] = CornerPos[(int32)PointIds[I]];
		}
		int32 CubeType = 0;
		for (int32 I = 0; I < 8; ++I) if (Sdf[I] < 0.f) CubeType |= (1 << I); // GC-7
		const int32 EdgeConfig = edgeTable[CubeType];
		if (EdgeConfig == 0) return;

		FVector3f VertList[12];
		for (int32 E = 0; E < 12; ++E)
		{
			if ((EdgeConfig & (1 << E)) == 0) continue;
			const int32 A = interpEdgeTable[E][0];
			const int32 B = interpEdgeTable[E][1];
			VertList[E] = NKSRSdfInterp(Pos[A], Pos[B], Sdf[A], Sdf[B]);
		}

		for (int32 I = 0; triangleTable[CubeType][I] != -1; I += 3)
		{
			const int32 TriangleId = (int32)(BaseVert[CubeIdx] / 3) + I / 3;
			for (int32 Vi = 0; Vi < 3; ++Vi)
			{
				const int32 Vlid = triangleTable[CubeType][I + Vi];
				TriPos[TriangleId * 3 + Vi] = VertList[Vlid];
				int64 Vid0 = PointIds[e2iTable[Vlid][0]];
				int64 Vid1 = PointIds[e2iTable[Vlid][1]];
				if (Vid0 < Vid1) Swap(Vid0, Vid1); // normalize key to (max, min)
				TriKey[(TriangleId * 3 + Vi) * 2 + 0] = Vid0;
				TriKey[(TriangleId * 3 + Vi) * 2 + 1] = Vid1;
			}
		}
	});

	// Weld: torch.unique_dim(dim=0) over the keys (GC-6: ascending + inverse).
	TArray<int64> UniqueKeys;
	TArray<int32> Inverse;
	NKSRUniqueRowsSorted(TriKey, 2, UniqueKeys, Inverse);
	const int32 NumVerts = UniqueKeys.Num() / 2;

	// Scatter positions: duplicate targets overwrite, last write wins (matches index_put_). Serial.
	OutV.SetNumZeroed(NumVerts);
	for (int32 T = 0; T < NumTris * 3; ++T) OutV[Inverse[T]] = TriPos[T];

	OutF.SetNumUninitialized(NumTris);
	for (int32 T = 0; T < NumTris; ++T) OutF[T] = FIntVector(Inverse[T * 3 + 0], Inverse[T * 3 + 1], Inverse[T * 3 + 2]);
}

void NKSRFilterCrossingCubes(TArray<int64>& CubeGraph, TArray<FVector3f>& Vertices, TArray<float>& Values)
{
	// MISE steps (a)+(b) of spec E §7 / A §8.2: keep sign-crossing cubes, then compact the
	// vertex table with sorted-unique + inverse (GC-6). Values are compacted alongside Vertices.
	if (Values.Num() != Vertices.Num())
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRFilterCrossingCubes: Values (%d) / Vertices (%d) size mismatch."), Values.Num(), Vertices.Num());
		return;
	}
	if (!NKSRValidateGraphIndices(CubeGraph, Vertices.Num(), TEXT("NKSRFilterCrossingCubes"))) return;
	const int32 NumCubes = CubeGraph.Num() / 8;

	// (a) sign = value > 0 (strict, GC-7: value == 0 counts as the negative side here).
	TArray<int64> Kept;
	Kept.Reserve(CubeGraph.Num());
	for (int32 C = 0; C < NumCubes; ++C)
	{
		int32 NumPositive = 0;
		for (int32 S = 0; S < 8; ++S) if (Values[(int32)CubeGraph[C * 8 + S]] > 0.f) ++NumPositive;
		if (NumPositive != 0 && NumPositive != 8) Kept.Append(CubeGraph.GetData() + C * 8, 8);
	}

	// (b) unq = sorted unique of the kept graph; remap graph, gather vertices/values.
	TArray<int64> UniqueIdx;
	TArray<int32> Inverse;
	NKSRUniqueRowsSorted(Kept, 1, UniqueIdx, Inverse);
	for (int32 T = 0; T < Kept.Num(); ++T) Kept[T] = Inverse[T];

	TArray<FVector3f> NewVertices;
	NewVertices.SetNumUninitialized(UniqueIdx.Num());
	TArray<float> NewValues;
	NewValues.SetNumUninitialized(UniqueIdx.Num());
	for (int32 U = 0; U < UniqueIdx.Num(); ++U)
	{
		NewVertices[U] = Vertices[(int32)UniqueIdx[U]];
		NewValues[U] = Values[(int32)UniqueIdx[U]];
	}

	CubeGraph = MoveTemp(Kept);
	Vertices = MoveTemp(NewVertices);
	Values = MoveTemp(NewValues);
}

void NKSRSubdivideCubeIndices(TArray<int64>& CubeGraph, TArray<FVector3f>& Vertices)
{
	// 1:1 port of nksr/utils.py subdivide_cube_indices. Child cube i's corner j is the midpoint
	// of parent corners i and j (i == j -> corner, edge pair -> edge mid, face diagonal -> face
	// center, body diagonal -> cube center). Collapsed edges/faces reuse the first corner index.
	if (!NKSRValidateGraphIndices(CubeGraph, Vertices.Num(), TEXT("NKSRSubdivideCubeIndices"))) return;
	const int32 NumCubes = CubeGraph.Num() / 8;
	if (NumCubes == 0) return;
	if (NumCubes > MAX_int32 / 64)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRSubdivideCubeIndices: %d cubes exceed TArray capacity after 8-way split."), NumCubes);
		return;
	}

	// NG[i][j]: column (length NumCubes) of vertex indices; corner j of child cube i.
	TArray<int64> NG[8][8];
	int64 Base = Vertices.Num();

	// 1) Corners: NG[v][v] = graph[:, v].
	for (int32 V = 0; V < 8; ++V)
	{
		NG[V][V].SetNumUninitialized(NumCubes);
		for (int32 C = 0; C < NumCubes; ++C) NG[V][V][C] = CubeGraph[C * 8 + V];
	}

	// 2) Edge midpoints, three axis groups in fixed order (spec E §12.3, verbatim).
	static constexpr int32 EdgeSets[3][4][2] = {
		{ {0, 4}, {1, 5}, {3, 7}, {2, 6} },
		{ {0, 1}, {3, 2}, {4, 5}, {7, 6} },
		{ {0, 3}, {1, 2}, {4, 7}, {5, 6} } };
	for (int32 Set = 0; Set < 3; ++Set)
	{
		// pairs = cat over the 4 edges of stack(graph[:,e0], graph[:,e1]) -- keep the (e0,e1)
		// order, do NOT sort the pair: shared edges appear as identical ordered tuples across
		// neighboring cubes (spec E §7.1).
		TArray<int64> Pairs;
		Pairs.SetNumUninitialized(4 * NumCubes * 2);
		for (int32 G = 0; G < 4; ++G)
		{
			const int32 E0 = EdgeSets[Set][G][0];
			const int32 E1 = EdgeSets[Set][G][1];
			for (int32 C = 0; C < NumCubes; ++C)
			{
				Pairs[(G * NumCubes + C) * 2 + 0] = CubeGraph[C * 8 + E0];
				Pairs[(G * NumCubes + C) * 2 + 1] = CubeGraph[C * 8 + E1];
			}
		}
		TArray<int64> Uniq;
		TArray<int32> Inverse;
		NKSRUniqueRowsSorted(Pairs, 2, Uniq, Inverse);

		const int32 NumUniq = Uniq.Num() / 2;
		TArray<int64> NewIdx;
		NewIdx.SetNumUninitialized(NumUniq);
		for (int32 U = 0; U < NumUniq; ++U)
		{
			const int64 A = Uniq[U * 2 + 0];
			const int64 B = Uniq[U * 2 + 1];
			if (A != B)
			{
				Vertices.Add((Vertices[(int32)A] + Vertices[(int32)B]) / 2.f);
				NewIdx[U] = Base++;
			}
			else NewIdx[U] = A; // collapsed edge: reuse the first corner index
		}

		for (int32 G = 0; G < 4; ++G)
		{
			const int32 E0 = EdgeSets[Set][G][0];
			const int32 E1 = EdgeSets[Set][G][1];
			TArray<int64>& Col = NG[E0][E1];
			Col.SetNumUninitialized(NumCubes);
			for (int32 C = 0; C < NumCubes; ++C) Col[C] = NewIdx[Inverse[G * NumCubes + C]];
			NG[E1][E0] = Col;
		}
	}

	// 3) Face centers, three face groups in fixed order.
	static constexpr int32 FaceSets[3][2][4] = {
		{ {0, 1, 5, 4}, {3, 2, 6, 7} },
		{ {1, 2, 6, 5}, {0, 3, 7, 4} },
		{ {0, 1, 2, 3}, {4, 5, 6, 7} } };
	for (int32 Set = 0; Set < 3; ++Set)
	{
		TArray<int64> Quads;
		Quads.SetNumUninitialized(2 * NumCubes * 4);
		for (int32 G = 0; G < 2; ++G)
		{
			for (int32 C = 0; C < NumCubes; ++C)
			{
				for (int32 I = 0; I < 4; ++I) Quads[(G * NumCubes + C) * 4 + I] = CubeGraph[C * 8 + FaceSets[Set][G][I]];
			}
		}
		TArray<int64> Uniq;
		TArray<int32> Inverse;
		NKSRUniqueRowsSorted(Quads, 4, Uniq, Inverse);

		const int32 NumUniq = Uniq.Num() / 4;
		TArray<int64> NewIdx;
		NewIdx.SetNumUninitialized(NumUniq);
		for (int32 U = 0; U < NumUniq; ++U)
		{
			const int64* Row = Uniq.GetData() + (int64)U * 4;
			const bool bCollapsed = Row[0] == Row[1] && Row[0] == Row[2] && Row[0] == Row[3];
			if (!bCollapsed)
			{
				// Same summation order as python: ((v0 + v1) + v2 + v3) / 4.
				const FVector3f Center =
					(Vertices[(int32)Row[0]] + Vertices[(int32)Row[1]] + Vertices[(int32)Row[2]] + Vertices[(int32)Row[3]]) / 4.f;
				Vertices.Add(Center);
				NewIdx[U] = Base++;
			}
			else NewIdx[U] = Row[0]; // collapsed face: reuse the first corner index
		}

		for (int32 G = 0; G < 2; ++G)
		{
			TArray<int64> Col;
			Col.SetNumUninitialized(NumCubes);
			for (int32 C = 0; C < NumCubes; ++C) Col[C] = NewIdx[Inverse[G * NumCubes + C]];
			// Face-diagonal assignment: child at f[i] gets the face center at corner f[(i+2)%4].
			for (int32 I = 0; I < 4; ++I) NG[FaceSets[Set][G][I]][FaceSets[Set][G][(I + 2) % 4]] = Col;
		}
	}

	// 4) Body centers: always appended, no dedup / degeneracy check (matches python).
	static constexpr int32 BodyDiag[8] = { 6, 7, 4, 5, 2, 3, 0, 1 };
	TArray<int64> CenterIdx;
	CenterIdx.SetNumUninitialized(NumCubes);
	for (int32 C = 0; C < NumCubes; ++C)
	{
		FVector3f Sum = Vertices[(int32)CubeGraph[C * 8 + 0]];
		for (int32 I = 1; I < 8; ++I) Sum += Vertices[(int32)CubeGraph[C * 8 + I]];
		Vertices.Add(Sum / 8.f);
		CenterIdx[C] = Base++;
	}
	for (int32 Cur = 0; Cur < 8; ++Cur) NG[Cur][BodyDiag[Cur]] = CenterIdx;

	// Output: vertical concat of 8 blocks; block i row c = (NG[i][0][c], ..., NG[i][7][c]).
	TArray<int64> NewGraph;
	NewGraph.SetNumUninitialized(8 * NumCubes * 8);
	for (int32 I = 0; I < 8; ++I)
	{
		for (int32 C = 0; C < NumCubes; ++C)
		{
			for (int32 J = 0; J < 8; ++J) NewGraph[(I * NumCubes + C) * 8 + J] = NG[I][J][C];
		}
	}
	CubeGraph = MoveTemp(NewGraph);
}

void NKSRApplyVertexMask(TArray<FVector3f>& V, TArray<FIntVector>& F, TConstArrayView<bool> Keep)
{
	// apply_vertex_mask (nksr/utils.py): compact kept vertices, remap faces, drop any face
	// touching a removed vertex.
	if (Keep.Num() != V.Num())
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRApplyVertexMask: Keep (%d) / V (%d) size mismatch."), Keep.Num(), V.Num());
		return;
	}
	for (const FIntVector& Face : F)
	{
		if (Face.X < 0 || Face.X >= V.Num() || Face.Y < 0 || Face.Y >= V.Num() || Face.Z < 0 || Face.Z >= V.Num())
		{
			UE_LOG(LogNKSR, Error, TEXT("NKSRApplyVertexMask: face index out of range [0, %d)."), V.Num());
			return;
		}
	}

	TArray<int32> Map;
	Map.SetNumUninitialized(V.Num());
	int32 NumKept = 0;
	for (int32 I = 0; I < V.Num(); ++I) Map[I] = Keep[I] ? NumKept++ : -1;

	TArray<FVector3f> NewV;
	NewV.Reserve(NumKept);
	for (int32 I = 0; I < V.Num(); ++I) if (Keep[I]) NewV.Add(V[I]);

	TArray<FIntVector> NewF;
	NewF.Reserve(F.Num());
	for (const FIntVector& Face : F)
	{
		const FIntVector Mapped(Map[Face.X], Map[Face.Y], Map[Face.Z]);
		if (Mapped.X != -1 && Mapped.Y != -1 && Mapped.Z != -1) NewF.Add(Mapped);
	}

	V = MoveTemp(NewV);
	F = MoveTemp(NewF);
}
