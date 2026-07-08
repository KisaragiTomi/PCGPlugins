#pragma once

// Field abstractions over the solved hierarchy + dual-mesh extraction driver.
// Port specs: A_inference_flow.md (KernelField.solve fused path, extract_dual_mesh), E_meshing_solver.md.
// Fields evaluate in the UNSCALED working frame; global_scale / level_set composition happens in the
// evaluator lambdas assembled by the reconstructor (GC-8).

#include "NKSRCommon.h"
#include "NKSRSvh.h"
#include "NKSRPointCloudIO.h"

class FNKSRNetwork;

class FNKSRKernelField
{
public:
	FNKSRKernelField(const FNKSRSvh& InSvh, const FNKSRNetwork& InNetwork, TArray<FNKSRMatrix>&& InBasisFeatures);

	/**
	 * KernelField.solve (fused_mode=True, nystrom off, reg_weight=1):
	 * per-depth kernels via InterpolatorEvaluate, rhs_evaluation, COO indexer + matrix_building (GTG/QTQ),
	 * k_building on diagonal blocks, then Jacobi-PCG. NormalValue is the ALREADY-NEGATED value (GC-7).
	 */
	bool Solve(TConstArrayView<FVector3f> PosXyz,
	           TConstArrayView<FVector3f> NormalXyz, TConstArrayView<FVector3f> NormalValue,
	           float PosWeight, float NormalWeight, float RegWeight,
	           float SolverTol, int32 SolverMaxIter, FString& OutError);

	/** evaluate_f: sum over depths of kernel_evaluation. */
	void EvaluateF(TConstArrayView<FVector3f> Xyz, TArray<float>& OutF) const;

	const FNKSRSvh& Svh() const { return SvhRef; }

	/** Per-depth PCG solutions (golden-dump access). */
	const TArray<TArray<float>>& GetSolutions() const { return Solutions; }

private:
	const FNKSRSvh& SvhRef;
	const FNKSRNetwork& Network;
	TArray<FNKSRMatrix> BasisFeatures;   // grid_kernel per depth
	TArray<TArray<float>> Solutions;     // per depth
};

/** NeuralField over udf features (the ks mask field, level_set applied by the caller). */
class FNKSRUdfField
{
public:
	FNKSRUdfField(const FNKSRSvh& InSvh, const FNKSRNetwork& InNetwork, TArray<FNKSRMatrix>&& InUdfFeatures);
	void EvaluateF(TConstArrayView<FVector3f> Xyz, TArray<float>& OutF) const;

private:
	const FNKSRSvh& SvhRef;
	const FNKSRNetwork& Network;
	TArray<FNKSRMatrix> UdfFeatures;
};

/** LayerField: +1 outside the inside_depth-1 grid's active voxels, -1 inside (udf-disabled fallback). */
class FNKSRLayerField
{
public:
	FNKSRLayerField(const FNKSRSvh& InSvh, int32 InInsideDepth) : SvhRef(InSvh), InsideDepth(InInsideDepth) {}
	void EvaluateF(TConstArrayView<FVector3f> Xyz, TArray<float>& OutF) const;

private:
	const FNKSRSvh& SvhRef;
	int32 InsideDepth;
};

/**
 * BaseField.extract_dual_mesh driver. EvalFBar / MaskFBar receive points in the SCALED (input) frame
 * and must internally divide by global_scale and subtract their level sets (GC-8).
 * Steps: flattened grids -> joint dual grid -> dual cube graph -> f_bar at corners ->
 * MiseIter x (filter crossing + subdivide + re-eval) -> marching cubes -> mask trim.
 */
bool NKSRExtractDualMesh(
	const FNKSRSvh& Svh,
	float GlobalScale,
	TFunctionRef<void(TConstArrayView<FVector3f>, TArray<float>&)> EvalFBar,
	TFunctionRef<void(TConstArrayView<FVector3f>, TArray<float>&)> MaskFBar,
	int32 MiseIter,
	FNKSRMeshBuffers& OutMesh,
	FString& OutError);
