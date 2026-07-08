#pragma once

// NKSR network forward passes (ks config, fixed architecture).
// Port spec: B_network_arch.md; weight keys/shapes: F_checkpoint_report.md.
// Channels: [32, 32, 64, 128, 256]; order 'gcr' = GroupNorm(8) -> Conv3x3 (no bias) -> ReLU.
// All hyper-parameters are hardcoded from configs.py 'ks' (the checkpoint carries none).

#include "NKSRCommon.h"
#include "NKSRSvh.h"

class FNKSRWeightStore;

/** SparseStructureNet outputs (FeaturesSet). Indexed by depth; absent levels are Rows==0 with correct Cols. */
struct FNKSRFeaturesSet
{
	TArray<FNKSRMatrix> Structure;   // [Nv_tmp_d, 3]
	TArray<FNKSRMatrix> Normal;      // [Nv_d, 3] (only d < adaptive_depth)
	TArray<FNKSRMatrix> Basis;       // [Nv_d, 4]
	TArray<FNKSRMatrix> Udf;         // [Nv_tmp_d, 16]
};

class FNKSRNetwork
{
public:
	/** Binds and shape-checks every tensor on the ks inference path. False + OutError on any mismatch. */
	bool Init(const FNKSRWeightStore& Weights, FString& OutError);

	/**
	 * PointEncoder.forward at depth 0: local voxel coords + normals -> per-voxel feature [Nv0, 32].
	 * Points outside active voxels are dropped (pts_mask), matching encdec.py.
	 */
	void PointEncoderForward(TConstArrayView<FVector3f> Xyz, TConstArrayView<FVector3f> Normals, const FNKSRSvh& Svh, int32 Depth, FNKSRMatrix& OutC) const;

	/**
	 * SparseStructureNet.forward (inference: gt_decoder_svh == None, argmax structure decisions).
	 * Consumes EncoderSvh (kernel-map cache mutates); produces the pruned DecoderSvh (features' home)
	 * and DecoderTmpSvh (structure/udf features' home).
	 */
	void UnetForward(const FNKSRMatrix& Feat, FNKSRSvh& EncoderSvh, int32 AdaptiveDepth,
	                 FNKSRFeaturesSet& OutFeatures, FNKSRSvh& OutDecoderSvh, FNKSRSvh& OutDecoderTmpSvh) const;

	/**
	 * MLPFeatureInterpolator[depth].interpolate: trilinear-sample Features on Grid at Queries, then the
	 * 4->16->16->16->4 ReLU MLP; bGrad additionally chains the jacobian (interpolator.py MLPWithGrad).
	 */
	void InterpolatorEvaluate(int32 Depth, TConstArrayView<FVector3f> Queries, const FNKSRIndexGrid& Grid,
	                          const FNKSRMatrix& Features, bool bGrad, FNKSRMatrix& OutTheta, FNKSRGradTensor* OutGradTheta) const;

	/** udf_decoder (MultiscalePointDecoder, coords_depths=[2,3]) -> scalar UDF per query. */
	void UdfDecoderForward(TConstArrayView<FVector3f> Xyz, const FNKSRSvh& Svh, const TArray<FNKSRMatrix>& UdfFeatures, TArray<float>& OutVals) const;

	static constexpr int32 TreeDepth = 4;
	static constexpr int32 AdaptiveDepth = 2;
	static constexpr int32 KernelDim = 4;
	static constexpr int32 UdfBranchDim = 16;

private:
	const FNKSRWeightStore* Store = nullptr;
};
