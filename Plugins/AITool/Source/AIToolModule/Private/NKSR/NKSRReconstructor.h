#pragma once

// Pipeline orchestration = Reconstructor.reconstruct + field.extract_dual_mesh (ks config).
// Port spec: A_inference_flow.md. Stage order:
//   density scale -> svh point splatting -> point encoder -> unet -> kernel field solve ->
//   udf mask field -> extract dual mesh (mise) -> rescale vertices.

#include "NKSRCommon.h"
#include "NKSRPointCloudIO.h"

struct FNKSRRunSettings
{
	float DetailLevel = 1.0f;        // used when VoxelSizeOverride <= 0 and density_range applies
	float VoxelSizeOverride = 0.f;   // > 0: global_scale = VoxelSizeOverride / 0.1
	int32 MiseIter = 1;
	int32 SolverMaxIter = 2000;
	float SolverTol = 1e-5f;
	bool bVerbose = false;
};

/** Optional per-run observation: stage logging, cancellation, golden dumps. */
struct FNKSRProgressSink
{
	TFunction<void(const TCHAR* /*StageName*/)> OnStage;
	TFunction<bool()> ShouldCancel;
	FString DumpDir;   // non-empty: write per-stage .npy dumps for golden comparison
};

/**
 * Full reconstruction. Normals may be empty ONLY if the caller estimated them beforehand —
 * this function requires normals (feature='normal'); use NKSREstimateNormals upstream.
 * Returns false + OutError on any failure (weights missing, empty input, solver failure, cancel).
 */
bool NKSRReconstruct(
	TConstArrayView<FVector3f> Xyz,
	TConstArrayView<FVector3f> Normals,
	const FNKSRRunSettings& Settings,
	FNKSRMeshBuffers& OutMesh,
	FString& OutError,
	const FNKSRProgressSink* Sink = nullptr);
