// Pipeline orchestration = nksr.Reconstructor.reconstruct (config='ks') + extract_dual_mesh driver hookup.
// Port spec: A_inference_flow.md; reference: nksr/__init__.py. Conventions GC-1..GC-9 (G_cpp_design.md).

#include "NKSRReconstructor.h"
#include "NKSRSvh.h"
#include "NKSRNetwork.h"
#include "NKSRFields.h"
#include "NKSRWeights.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
// ks hyper-parameters, hardcoded from configs.py (the checkpoint carries no hparams). Spec A §1.1.
constexpr double KsVoxelSize = 0.1;
constexpr int32 KsTreeDepth = 4;
constexpr int32 KsAdaptiveDepth = 2;
constexpr double KsDensityMin = 1.0;      // density_range = [1, 20]
constexpr double KsDensityMax = 20.0;
constexpr double KsPosWeight = 1.0e4;     // solver.pos_weight (divided by N at the call site)
constexpr double KsNormalWeight = 1.0e4;  // solver.normal_weight (x vs^2 / M at the call site)
constexpr float KsRegWeight = 1.f;
constexpr float KsUdfLevelSet = 0.2f;     // 2 * voxel_size, fixed in the WORKING frame (GC-8)

// int64 voxel key for the detail_level density formula (torch .long() semantics).
struct FNKSRVoxelKey64
{
	int64 X, Y, Z;
};

FORCEINLINE bool VoxelKeyLess(const FNKSRVoxelKey64& A, const FNKSRVoxelKey64& B)
{
	if (A.X != B.X) return A.X < B.X;
	if (A.Y != B.Y) return A.Y < B.Y;
	return A.Z < B.Z;
}

FORCEINLINE bool VoxelKeyEqual(const FNKSRVoxelKey64& A, const FNKSRVoxelKey64& B)
{
	return A.X == B.X && A.Y == B.Y && A.Z == B.Z;
}
}

bool NKSRReconstruct(
	TConstArrayView<FVector3f> Xyz,
	TConstArrayView<FVector3f> Normals,
	const FNKSRRunSettings& Settings,
	FNKSRMeshBuffers& OutMesh,
	FString& OutError,
	const FNKSRProgressSink* Sink)
{
	OutMesh.Vertices.Reset();
	OutMesh.Triangles.Reset();
	OutError.Reset();

	// --- input validation (spec A §9: empty input -> failure, feature=='normal' requires normals) ---
	const int32 NumPts = Xyz.Num();
	if (NumPts == 0)
	{
		OutError = TEXT("NKSRReconstruct: empty input point cloud.");
		return false;
	}
	if (Normals.Num() != NumPts)
	{
		OutError = FString::Printf(TEXT("NKSRReconstruct: normals (%d) must match points (%d); estimate normals upstream (NKSREstimateNormals)."), Normals.Num(), NumPts);
		return false;
	}

	// --- weights + network binding ---
	FString WeightsError;
	const FNKSRWeightStore* Store = FNKSRWeightStore::Get(WeightsError);
	if (!Store)
	{
		OutError = FString::Printf(TEXT("NKSRReconstruct: weight store unavailable: %s"), *WeightsError);
		return false;
	}
	FNKSRNetwork Network;
	if (!Network.Init(*Store, OutError)) return false;

	// --- progress / dump plumbing ---
	const FString DumpDir = Sink ? Sink->DumpDir : FString();
	const bool bDump = !DumpDir.IsEmpty();
	if (bDump) IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/true);

	bool bCancelled = false;
	auto EnterStage = [&](const TCHAR* StageName) -> bool
	{
		if (Sink && Sink->ShouldCancel && Sink->ShouldCancel())
		{
			bCancelled = true;
			OutError = FString::Printf(TEXT("NKSRReconstruct: cancelled before stage '%s'."), StageName);
			return false;
		}
		if (Sink && Sink->OnStage) Sink->OnStage(StageName);
		if (Settings.bVerbose) UE_LOG(LogNKSR, Log, TEXT("NKSR stage: %s"), StageName);
		return true;
	};

	auto DumpMatrix = [&](const FString& Name, const FNKSRMatrix& M)
	{
		if (!bDump) return;
		FString Err;
		if (!NKSRSaveNpyFloat(FPaths::Combine(DumpDir, Name), M, Err)) UE_LOG(LogNKSR, Warning, TEXT("NKSR dump '%s' failed: %s"), *Name, *Err);
	};
	auto DumpGridCoords = [&](const FString& Name, const FNKSRIndexGrid* Grid)
	{
		if (!bDump) return;
		TArray<int32> Flat;
		int32 NumRows = 0;
		if (Grid)
		{
			NumRows = Grid->NumVoxels();
			Flat.Reserve(NumRows * 3);
			for (const FNKSRIjk& C : Grid->ActiveGridCoords())
			{
				Flat.Add(C.X); Flat.Add(C.Y); Flat.Add(C.Z);
			}
		}
		FString Err;
		if (!NKSRSaveNpyInt32(FPaths::Combine(DumpDir, Name), Flat, NumRows, 3, Err)) UE_LOG(LogNKSR, Warning, TEXT("NKSR dump '%s' failed: %s"), *Name, *Err);
	};

	// ------------------------------------------------------------------
	// 1) detail_level / voxel_size -> global_scale (spec A §2)
	// ------------------------------------------------------------------
	if (!EnterStage(TEXT("GlobalScale"))) return false;

	double GlobalScaleD = 1.0;
	if (Settings.VoxelSizeOverride > 0.f)
	{
		GlobalScaleD = (double)Settings.VoxelSizeOverride / KsVoxelSize;
	}
	else
	{
		// vox_ijk = unique_rows(floor(xyz / 0.1)): float32 division + math floor (GC-1), then int64.
		// Deterministic uniqueness via explicit sort + adjacent dedup (never iterate a TMap for results).
		TArray<FNKSRVoxelKey64> Vox;
		Vox.SetNumUninitialized(NumPts);
		for (int32 I = 0; I < NumPts; ++I)
		{
			const FVector3f& P = Xyz[I];
			Vox[I].X = (int64)FMath::FloorToDouble(P.X / 0.1f);
			Vox[I].Y = (int64)FMath::FloorToDouble(P.Y / 0.1f);
			Vox[I].Z = (int64)FMath::FloorToDouble(P.Z / 0.1f);
		}
		Vox.Sort(&VoxelKeyLess);
		int32 NumUnique = 1;
		for (int32 I = 1; I < NumPts; ++I) if (!VoxelKeyEqual(Vox[I], Vox[I - 1])) ++NumUnique;

		const double CurDensity = (double)NumPts / (double)NumUnique;
		double TargetDensity = KsDensityMin + (KsDensityMax - KsDensityMin) * (1.0 - (double)Settings.DetailLevel);
		TargetDensity = FMath::Max(TargetDensity, 0.01);
		GlobalScaleD = FMath::Sqrt(TargetDensity / CurDensity);
	}

	const bool bScaled = GlobalScaleD != 1.0;
	const float GlobalScale = (float)GlobalScaleD;   // GC-9: per-point math stays float32 below
	if (bScaled && Settings.bVerbose) UE_LOG(LogNKSR, Log, TEXT("NKSR: input scale factor: %.4f"), 1.0 / GlobalScaleD);

	// xyz /= global_scale (working frame); normals unchanged.
	TArray<FVector3f> XyzScaled;
	TConstArrayView<FVector3f> Work = Xyz;
	if (bScaled)
	{
		XyzScaled.SetNumUninitialized(NumPts);
		for (int32 I = 0; I < NumPts; ++I) XyzScaled[I] = Xyz[I] / GlobalScale;
		Work = XyzScaled;
	}

	// ------------------------------------------------------------------
	// 2) SVH point splatting (spec A §4)
	// ------------------------------------------------------------------
	if (!EnterStage(TEXT("PointSplatting"))) return false;
	FNKSRSvh Svh(KsVoxelSize, KsTreeDepth);
	Svh.BuildPointSplatting(Work);
	for (int32 D = 0; D < KsTreeDepth; ++D) DumpGridCoords(FString::Printf(TEXT("svh_enc_d%d.npy"), D), Svh.Grids[D].Get());

	// ------------------------------------------------------------------
	// 3) PointEncoder (spec A §5.1)
	// ------------------------------------------------------------------
	if (!EnterStage(TEXT("PointEncoder"))) return false;
	FNKSRMatrix EncoderFeat;
	Network.PointEncoderForward(Work, Normals, Svh, 0, EncoderFeat);
	DumpMatrix(TEXT("encoder_feat.npy"), EncoderFeat);

	// ------------------------------------------------------------------
	// 4) UNet (spec A §5.2) — returns the predicted decoder svh + pre-prune tmp svh
	// ------------------------------------------------------------------
	if (!EnterStage(TEXT("UNet"))) return false;
	FNKSRFeaturesSet Features;
	FNKSRSvh DecoderSvh;
	FNKSRSvh DecoderTmpSvh;
	Network.UnetForward(EncoderFeat, Svh, KsAdaptiveDepth, Features, DecoderSvh, DecoderTmpSvh);
	for (int32 D = 0; D < KsTreeDepth; ++D)
	{
		DumpGridCoords(FString::Printf(TEXT("svh_dec_d%d.npy"), D), DecoderSvh.Grids[D].Get());
		DumpGridCoords(FString::Printf(TEXT("svh_tmp_d%d.npy"), D), DecoderTmpSvh.Grids[D].Get());
		DumpMatrix(FString::Printf(TEXT("feat_struct_d%d.npy"), D), Features.Structure[D]);
		DumpMatrix(FString::Printf(TEXT("feat_basis_d%d.npy"), D), Features.Basis[D]);
		DumpMatrix(FString::Printf(TEXT("feat_normal_d%d.npy"), D), Features.Normal[D]);
		DumpMatrix(FString::Printf(TEXT("feat_udf_d%d.npy"), D), Features.Udf[D]);
	}

	// ------------------------------------------------------------------
	// 5) Normal supervision assembly: voxel centers of decoder depths 0..adaptive_depth-1
	//    (ascending d), values = NEGATED unet normal features (GC-7 negation happens HERE).
	// ------------------------------------------------------------------
	TArray<FVector3f> NormalXyz;
	TArray<FVector3f> NormalValue;
	for (int32 D = 0; D < KsAdaptiveDepth; ++D)
	{
		TArray<FVector3f> Centers;
		DecoderSvh.GetVoxelCenters(D, Centers);
		const FNKSRMatrix& NF = Features.Normal[D];
		if (Centers.Num() != NF.Rows)
		{
			OutError = FString::Printf(TEXT("NKSRReconstruct: depth %d voxel centers (%d) do not match normal features (%d)."), D, Centers.Num(), NF.Rows);
			return false;
		}
		NormalXyz.Append(Centers);
		for (int32 R = 0; R < NF.Rows; ++R) NormalValue.Emplace(-NF.At(R, 0), -NF.At(R, 1), -NF.At(R, 2));
	}
	const int32 NumNormal = NormalXyz.Num();
	if (NumNormal == 0)
	{
		// spec A §9: normal_xyz empty would divide by zero in normal_weight — explicit error instead.
		OutError = TEXT("NKSRReconstruct: UNet produced no voxels at adaptive depths (0..1); cannot assemble the normal system.");
		return false;
	}

	// ------------------------------------------------------------------
	// 6) KernelField solve (spec A §6): pos_weight = 1e4/N, normal_weight = 1e4/M * vs^2
	// ------------------------------------------------------------------
	if (!EnterStage(TEXT("SolveKernelField"))) return false;
	FNKSRKernelField Field(DecoderSvh, Network, MoveTemp(Features.Basis));
	const float PosWeight = (float)(KsPosWeight / (double)NumPts);
	const float NormalWeight = (float)(KsNormalWeight / (double)NumNormal * (KsVoxelSize * KsVoxelSize));
	if (!Field.Solve(Work, NormalXyz, NormalValue, PosWeight, NormalWeight, KsRegWeight,
	                 Settings.SolverTol, Settings.SolverMaxIter, OutError)) return false;

	if (bDump)
	{
		const TArray<TArray<float>>& Solutions = Field.GetSolutions();
		for (int32 D = 0; D < KsTreeDepth; ++D)
		{
			FNKSRMatrix Sol;
			Sol.SetUninitialized(Solutions.IsValidIndex(D) ? Solutions[D].Num() : 0, 1);
			if (Sol.Rows > 0) FMemory::Memcpy(Sol.Data.GetData(), Solutions[D].GetData(), Sol.Rows * sizeof(float));
			DumpMatrix(FString::Printf(TEXT("solution_d%d.npy"), D), Sol);
		}
	}

	// ------------------------------------------------------------------
	// 7) Mask field (NeuralField over udf features, level_set = 0.2, spec A §7.3)
	// ------------------------------------------------------------------
	FNKSRUdfField MaskField(DecoderTmpSvh, Network, MoveTemp(Features.Udf));

	// ------------------------------------------------------------------
	// 8) extract_dual_mesh (spec A §1.3 / §8). Evaluators receive INPUT-frame points and fold
	//    /scale + level_set internally (GC-8: level_set 0.2 is NOT scaled).
	// ------------------------------------------------------------------
	if (!EnterStage(TEXT("ExtractDualMesh"))) return false;

	TArray<FVector3f> QueryScratch;
	auto ToWorkFrame = [&](TConstArrayView<FVector3f> Pts) -> TConstArrayView<FVector3f>
	{
		if (!bScaled) return Pts;
		QueryScratch.SetNumUninitialized(Pts.Num());
		for (int32 I = 0; I < Pts.Num(); ++I) QueryScratch[I] = Pts[I] / GlobalScale;
		return QueryScratch;
	};
	auto EvalFBar = [&](TConstArrayView<FVector3f> Pts, TArray<float>& Out)
	{
		Field.EvaluateF(ToWorkFrame(Pts), Out);   // KernelField level_set == 0
	};
	auto MaskFBar = [&](TConstArrayView<FVector3f> Pts, TArray<float>& Out)
	{
		MaskField.EvaluateF(ToWorkFrame(Pts), Out);
		for (float& V : Out) V -= KsUdfLevelSet;
	};

	if (!NKSRExtractDualMesh(DecoderSvh, GlobalScale, EvalFBar, MaskFBar,
	                         FMath::Max(0, Settings.MiseIter), OutMesh, OutError)) return false;

	if (bDump)
	{
		FNKSRMatrix MeshV;
		MeshV.SetUninitialized(OutMesh.Vertices.Num(), 3);
		for (int32 I = 0; I < OutMesh.Vertices.Num(); ++I)
		{
			MeshV.At(I, 0) = OutMesh.Vertices[I].X;
			MeshV.At(I, 1) = OutMesh.Vertices[I].Y;
			MeshV.At(I, 2) = OutMesh.Vertices[I].Z;
		}
		DumpMatrix(TEXT("mesh_v.npy"), MeshV);

		TArray<int64> MeshF;
		MeshF.Reserve(OutMesh.Triangles.Num() * 3);
		for (const FIntVector& T : OutMesh.Triangles)
		{
			MeshF.Add(T.X); MeshF.Add(T.Y); MeshF.Add(T.Z);
		}
		FString Err;
		if (!NKSRSaveNpyInt64(FPaths::Combine(DumpDir, TEXT("mesh_f.npy")), MeshF, OutMesh.Triangles.Num(), 3, Err)) UE_LOG(LogNKSR, Warning, TEXT("NKSR dump 'mesh_f.npy' failed: %s"), *Err);
	}

	if (OutMesh.Vertices.Num() == 0) UE_LOG(LogNKSR, Warning, TEXT("NKSRReconstruct: reconstruction produced an empty mesh."));
	if (Settings.bVerbose) UE_LOG(LogNKSR, Log, TEXT("NKSRReconstruct: %d vertices, %d triangles."), OutMesh.Vertices.Num(), OutMesh.Triangles.Num());
	return true;
}
