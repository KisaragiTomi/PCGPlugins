// Field abstractions over the solved hierarchy + dual-mesh extraction driver.
// Mirrors nksr/fields/kernel_field.py (solve fused path / evaluate_f), neural_field.py,
// layer_field.py and base_field.py extract_dual_mesh. Port specs: A_inference_flow.md,
// E_meshing_solver.md. Conventions GC-1..GC-9 (G_cpp_design.md).

#include "NKSRFields.h"
#include "NKSRKernelEval.h"
#include "NKSRSolver.h"
#include "NKSRMeshing.h"
#include "NKSRNetwork.h"

// ---------------------------------------------------------------------------
// FNKSRKernelField
// ---------------------------------------------------------------------------

FNKSRKernelField::FNKSRKernelField(const FNKSRSvh& InSvh, const FNKSRNetwork& InNetwork, TArray<FNKSRMatrix>&& InBasisFeatures)
	: SvhRef(InSvh)
	, Network(InNetwork)
	, BasisFeatures(MoveTemp(InBasisFeatures))
{
	// balanced_kernel=False: grid_kernel[d] = features[d] (kernel_field.py __init__).
	Solutions.SetNum(SvhRef.Depth);
}

bool FNKSRKernelField::Solve(TConstArrayView<FVector3f> PosXyz,
                             TConstArrayView<FVector3f> NormalXyz, TConstArrayView<FVector3f> NormalValue,
                             float PosWeight, float NormalWeight, float RegWeight,
                             float SolverTol, int32 SolverMaxIter, FString& OutError)
{
	const int32 Depth = SvhRef.Depth;
	Solutions.Reset();
	Solutions.SetNum(Depth);

	// kernel_field.py solve() asserts -> external-input errors (no check()/ensure()).
	if (PosWeight <= 0.f || NormalWeight <= 0.f)
	{
		OutError = TEXT("KernelField.Solve: data weights have to be > 0.");
		return false;
	}
	if (NormalXyz.Num() != NormalValue.Num())
	{
		OutError = FString::Printf(TEXT("KernelField.Solve: normal_xyz (%d) / normal_value (%d) size mismatch."), NormalXyz.Num(), NormalValue.Num());
		return false;
	}
	if (SvhRef.Grids.Num() != Depth || BasisFeatures.Num() != Depth)
	{
		OutError = FString::Printf(TEXT("KernelField.Solve: svh grids (%d) / basis features (%d) must both match depth %d."), SvhRef.Grids.Num(), BasisFeatures.Num(), Depth);
		return false;
	}

	// Evaluate kernels at custom positions (pos_kernel / normal_kernel dicts; normal with grad).
	// evaluate_kernel: null grid -> None; approx_kernel_grad=False -> analytic jacobian chain.
	TArray<FNKSRMatrix> PosKernel;      PosKernel.SetNum(Depth);
	TArray<FNKSRMatrix> NormalKernel;   NormalKernel.SetNum(Depth);
	TArray<FNKSRGradTensor> NormalGrad; NormalGrad.SetNum(Depth);
	for (int32 D = 0; D < Depth; ++D)
	{
		const FNKSRIndexGrid* Grid = SvhRef.Grids[D].Get();
		if (!Grid) continue;
		Network.InterpolatorEvaluate(D, PosXyz, *Grid, BasisFeatures[D], /*bGrad=*/false, PosKernel[D], nullptr);
		Network.InterpolatorEvaluate(D, NormalXyz, *Grid, BasisFeatures[D], /*bGrad=*/true, NormalKernel[D], &NormalGrad[D]);
	}

	FNKSRBlockMatrix LhsMat(Depth);
	TArray<TArray<float>> RhsPerDepth;
	RhsPerDepth.SetNum(Depth);
	const FNKSRGradTensor EmptyGrad;   // zeros((0,0,3)) placeholder ("no gradient kernel")

	for (int32 D = Depth - 1; D >= 0; --D)   // d from depth-1 down to 0, null grids skipped
	{
		const FNKSRIndexGrid* GridD = SvhRef.Grids[D].Get();
		if (!GridD) continue;

		// rhs[d] = normal_weight * rhs_evaluation(...)
		TArray<float>& Rhs = RhsPerDepth[D];
		NKSRRhsEvaluation(*GridD, NormalXyz, NormalKernel[D], BasisFeatures[D], NormalGrad[D], NormalValue, Rhs);
		for (float& V : Rhs) V *= NormalWeight;

		// Nystrom branch (d >= nystrom_min_depth=100) never triggers:
		// pos_xyz_d = pos_xyz, pos_kernel_d = pos_kernel, pos_weight_d = pos_weight.
		for (int32 DD = Depth - 1; DD >= D; --DD)   // upper-triangular block (d, dd)
		{
			const FNKSRIndexGrid* GridDD = SvhRef.Grids[DD].Get();
			if (!GridDD) continue;

			TArray<int64> Indexer;
			NKSRBuildCooIndexer(*GridD, *GridDD, Indexer);
			FNKSRCooEntries Entries;
			NKSRCompressCooIndexer(Indexer, GridD->NumVoxels(), Entries);   // GC-3/row-major COO order

			TArray<float> Gtg;
			NKSRMatrixBuilding(*GridD, *GridDD, PosXyz, PosKernel[D], PosKernel[DD],
			                   BasisFeatures[D], BasisFeatures[DD], EmptyGrad, EmptyGrad,
			                   Indexer, /*bGrad=*/false, Entries.Num, Gtg);
			TArray<float> Qtq;
			NKSRMatrixBuilding(*GridD, *GridDD, NormalXyz, NormalKernel[D], NormalKernel[DD],
			                   BasisFeatures[D], BasisFeatures[DD], NormalGrad[D], NormalGrad[DD],
			                   Indexer, /*bGrad=*/true, Entries.Num, Qtq);

			TArray<float> Lhs;
			Lhs.SetNumUninitialized(Entries.Num);
			for (int32 I = 0; I < Entries.Num; ++I) Lhs[I] = PosWeight * Gtg[I] + NormalWeight * Qtq[I];

			if (D == DD && RegWeight > 0.f)
			{
				TArray<float> KReg;
				NKSRKBuilding(*GridD, BasisFeatures[D], Indexer, Entries.Num, KReg);
				for (int32 I = 0; I < Entries.Num; ++I) Lhs[I] += RegWeight * KReg[I];
			}

			LhsMat.AddBlock(D, DD, GridD->NumVoxels(), GridDD->NumVoxels(), Entries.DInds, Entries.DDInds, Lhs);
		}
	}

	// SparseMatrix.solve: rhs_vec = cat(rhs[d] for d ascending, absent levels contribute nothing).
	TArray<float> RhsVec;
	for (int32 D = 0; D < Depth; ++D) RhsVec.Append(RhsPerDepth[D]);
	if (RhsVec.Num() == 0)
	{
		OutError = TEXT("KernelField.Solve: hierarchy has no active grids.");
		return false;
	}

	int32 TotalBlockSize = 0;
	for (int32 D = 0; D < Depth; ++D) TotalBlockSize += LhsMat.BlockSize[D];
	if (TotalBlockSize != RhsVec.Num())
	{
		OutError = FString::Printf(TEXT("KernelField.Solve: block sizes (%d) do not match rhs length (%d)."), TotalBlockSize, RhsVec.Num());
		return false;
	}

	TArray<float> X;
	const int32 Iters = NKSRSolvePCG(LhsMat, RhsVec, SolverTol, SolverMaxIter, X);
	// GC-9: non-convergence warns only, result used anyway (solver.py _solve).
	if (SolverMaxIter > 0 && Iters == SolverMaxIter) UE_LOG(LogNKSR, Warning, TEXT("KernelField.Solve: PCG did not converge in %d iterations."), SolverMaxIter);

	// Solutions[d] = x[block_ptr[d] : block_ptr[d+1]] (ascending depth prefix slices).
	int32 Offset = 0;
	for (int32 D = 0; D < Depth; ++D)
	{
		const int32 Size = LhsMat.BlockSize[D];
		Solutions[D].SetNumUninitialized(Size);
		if (Size > 0) FMemory::Memcpy(Solutions[D].GetData(), X.GetData() + Offset, Size * sizeof(float));
		Offset += Size;
	}
	return true;
}

void FNKSRKernelField::EvaluateF(TConstArrayView<FVector3f> Xyz, TArray<float>& OutF) const
{
	// kernel_field.py evaluate_f: f = sum over depths of kernel_evaluation (grad=False, empty grad kernel).
	OutF.Reset();
	OutF.SetNumZeroed(Xyz.Num());
	const FNKSRGradTensor EmptyGrad;
	for (int32 D = 0; D < SvhRef.Depth; ++D)
	{
		const FNKSRIndexGrid* Grid = SvhRef.Grids.IsValidIndex(D) ? SvhRef.Grids[D].Get() : nullptr;
		if (!Grid) continue;
		if (!Solutions.IsValidIndex(D) || !BasisFeatures.IsValidIndex(D) || Solutions[D].Num() != Grid->NumVoxels())
		{
			UE_LOG(LogNKSR, Warning, TEXT("KernelField.EvaluateF: missing/mismatched solution at depth %d, skipping."), D);
			continue;
		}
		FNKSRMatrix Theta;
		Network.InterpolatorEvaluate(D, Xyz, *Grid, BasisFeatures[D], /*bGrad=*/false, Theta, nullptr);
		TArray<float> FDepth;
		NKSRKernelEvaluation(*Grid, Xyz, Theta, BasisFeatures[D], Solutions[D], EmptyGrad, /*bGrad=*/false, FDepth, nullptr);
		for (int32 I = 0; I < OutF.Num(); ++I) OutF[I] += FDepth[I];
	}
}

// ---------------------------------------------------------------------------
// FNKSRUdfField (NeuralField over udf features; level_set applied by the caller lambda)
// ---------------------------------------------------------------------------

FNKSRUdfField::FNKSRUdfField(const FNKSRSvh& InSvh, const FNKSRNetwork& InNetwork, TArray<FNKSRMatrix>&& InUdfFeatures)
	: SvhRef(InSvh)
	, Network(InNetwork)
	, UdfFeatures(MoveTemp(InUdfFeatures))
{
}

void FNKSRUdfField::EvaluateF(TConstArrayView<FVector3f> Xyz, TArray<float>& OutF) const
{
	// neural_field.py evaluate_f: res = decoder(xyz, svh, features)[:, 0].
	Network.UdfDecoderForward(Xyz, SvhRef, UdfFeatures, OutF);
}

// ---------------------------------------------------------------------------
// FNKSRLayerField
// ---------------------------------------------------------------------------

void FNKSRLayerField::EvaluateF(TConstArrayView<FVector3f> Xyz, TArray<float>& OutF) const
{
	// layer_field.py: f = +1 everywhere, -1 where grids[inside_depth-1].points_in_active_voxel(xyz).
	OutF.Init(1.f, Xyz.Num());
	const int32 GridDepth = InsideDepth - 1;
	const FNKSRIndexGrid* Grid = SvhRef.Grids.IsValidIndex(GridDepth) ? SvhRef.Grids[GridDepth].Get() : nullptr;
	if (!Grid)
	{
		UE_LOG(LogNKSR, Warning, TEXT("LayerField.EvaluateF: grid at depth %d is null; returning +1 everywhere."), GridDepth);
		return;
	}
	TArray<bool> InMask;
	Grid->PointsInActiveVoxel(Xyz, InMask);
	for (int32 I = 0; I < Xyz.Num(); ++I) if (InMask[I]) OutF[I] = -1.f;
}

// ---------------------------------------------------------------------------
// NKSRExtractDualMesh (base_field.py extract_dual_mesh; grid_upsample=1, max_depth=100, trim=True)
// ---------------------------------------------------------------------------

bool NKSRExtractDualMesh(
	const FNKSRSvh& Svh,
	float GlobalScale,
	TFunctionRef<void(TConstArrayView<FVector3f>, TArray<float>&)> EvalFBar,
	TFunctionRef<void(TConstArrayView<FVector3f>, TArray<float>&)> MaskFBar,
	int32 MiseIter,
	FNKSRMeshBuffers& OutMesh,
	FString& OutError)
{
	OutMesh.Vertices.Reset();
	OutMesh.Triangles.Reset();

	const int32 Depth = Svh.Depth;
	if (Svh.Grids.Num() != Depth)
	{
		OutError = FString::Printf(TEXT("ExtractDualMesh: svh grids (%d) do not match depth %d."), Svh.Grids.Num(), Depth);
		return false;
	}

	// 1) Per-depth flattened grids (d != depth-1 conforming). Null grids (UNet early stop) become
	// EMPTY placeholder layers so layer positions / dual strides (1 << l) stay aligned — the python
	// original would dereference None and crash here; this is a deliberate porting protection.
	TArray<FNKSRIndexGrid> Flattened;
	Flattened.Reserve(Depth);
	for (int32 D = 0; D < Depth; ++D)
	{
		const FNKSRIndexGrid* Grid = Svh.Grids[D].Get();
		if (!Grid)
		{
			double Vs = 0.0, Org = 0.0;
			Svh.GetGridVoxelSizeOrigin(D, Vs, Org);
			Flattened.Emplace(Vs, Org);
			UE_LOG(LogNKSR, Warning, TEXT("ExtractDualMesh: svh grid at depth %d is null (UNet early stop); substituting an empty layer."), D);
			continue;
		}
		const FNKSRIndexGrid* FinerGrid = (D > 0) ? Svh.Grids[D - 1].Get() : nullptr;
		Flattened.Add(NKSRBuildFlattenedGrid(*Grid, FinerGrid, /*bConforming=*/D != Depth - 1));
	}

	TArray<const FNKSRIndexGrid*> FlatPtrs;
	FlatPtrs.Reserve(Depth);
	for (const FNKSRIndexGrid& G : Flattened) FlatPtrs.Add(&G);

	// 2) Joint dual grid + dual cube graph ([Nc][8] flattened int64).
	const FNKSRIndexGrid DualGrid = NKSRBuildJointDualGrid(FlatPtrs);
	TArray<int64> DmcGraph;
	NKSRDualCubeGraph(FlatPtrs, DualGrid, DmcGraph);

	// 3) dmc_vertices = cat over non-empty flattened layers (ascending depth) of grid_to_world(coords).
	// Row order == global corner index order used by the graph (GC-3 voxel order within each layer).
	TArray<FVector3f> DmcVertices;
	for (const FNKSRIndexGrid& G : Flattened)
	{
		if (G.NumVoxels() == 0) continue;
		TArray<FVector3f> CoordsF;
		CoordsF.Reserve(G.NumVoxels());
		for (const FNKSRIjk& Ijk : G.ActiveGridCoords()) CoordsF.Emplace((float)Ijk.X, (float)Ijk.Y, (float)Ijk.Z);
		TArray<FVector3f> World;
		G.GridToWorld(CoordsF, World);
		DmcVertices.Append(World);
	}
	if (DmcVertices.Num() == 0)
	{
		OutError = TEXT("ExtractDualMesh: hierarchy has no active voxels.");
		return false;
	}

	// 4) Back to the input frame (GC-8: global_scale multiplies output-side positions).
	if (GlobalScale != 1.f) for (FVector3f& V : DmcVertices) V *= GlobalScale;

	// 5) f_bar at corners. EvalFBar already folds /scale and level_set (GC-8).
	TArray<float> DmcValue;
	EvalFBar(DmcVertices, DmcValue);

	// 6) MISE refinement: filter crossing cubes -> subdivide -> FULL re-evaluation (spec A §8.2).
	for (int32 It = 0; It < MiseIter; ++It)
	{
		NKSRFilterCrossingCubes(DmcGraph, DmcVertices, DmcValue);
		if (DmcGraph.Num() == 0) break;   // no cube crosses the level set; MC below yields an empty mesh
		NKSRSubdivideCubeIndices(DmcGraph, DmcVertices);
		EvalFBar(DmcVertices, DmcValue);
	}

	// 7) Marching cubes (cubeType bit i set iff value < 0, GC-7).
	NKSRMarchingCubes(DmcGraph, DmcVertices, DmcValue, OutMesh.Vertices, OutMesh.Triangles);

	// 8) trim: keep vertices with mask f_bar < 0 (GC-7), drop faces touching removed vertices.
	if (OutMesh.Vertices.Num() > 0)
	{
		TArray<float> MaskVals;
		MaskFBar(OutMesh.Vertices, MaskVals);
		if (MaskVals.Num() != OutMesh.Vertices.Num())
		{
			OutError = FString::Printf(TEXT("ExtractDualMesh: mask evaluator returned %d values for %d vertices."), MaskVals.Num(), OutMesh.Vertices.Num());
			return false;
		}
		TArray<bool> Keep;
		Keep.SetNumUninitialized(OutMesh.Vertices.Num());
		for (int32 I = 0; I < MaskVals.Num(); ++I) Keep[I] = MaskVals[I] < 0.f;
		NKSRApplyVertexMask(OutMesh.Vertices, OutMesh.Triangles, Keep);
	}

	return true;
}
