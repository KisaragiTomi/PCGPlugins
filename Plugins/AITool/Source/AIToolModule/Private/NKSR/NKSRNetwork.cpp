// NKSR network forward passes (ks config). Port spec: B_network_arch.md; weights: F_checkpoint_report.md.
// Reference sources: nksr/nn/encdec.py, nn/unet.py, nn/modules.py, interpolator.py, __init__.py (NKSRNetwork).
// Conventions: G_cpp_design.md GC-1..GC-9 (float32 everywhere, GC-9).

#include "NKSRNetwork.h"
#include "NKSRGrid.h"
#include "NKSRConv.h"
#include "NKSRTensorOps.h"
#include "NKSRWeights.h"
#include "Async/ParallelFor.h"

#include <initializer_list>

namespace
{

constexpr int32 GNumGroups = 8;      // GroupNorm(8) on the whole ks path (all channels >= 32)
constexpr float GGnEps = 1e-5f;

const FNKSRMatrix& GetTensor(const FNKSRWeightStore& Store, const FString& Key)
{
	static const FNKSRMatrix Empty;
	const FNKSRMatrix* Found = Store.Find(Key);
	if (!Found)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRNetwork: missing weight '%s' (Init should have caught this)"), *Key);
		return Empty;
	}
	return *Found;
}

void AddInPlace(FNKSRMatrix& A, const FNKSRMatrix& B)
{
	if (A.Rows != B.Rows || A.Cols != B.Cols)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRNetwork: AddInPlace shape mismatch [%d,%d] vs [%d,%d]"), A.Rows, A.Cols, B.Rows, B.Cols);
		return;
	}
	const int32 Num = A.Data.Num();
	float* AD = A.Data.GetData();
	const float* BD = B.Data.GetData();
	for (int32 I = 0; I < Num; ++I) AD[I] += BD[I];
}

// encdec.py ResnetBlockFC: net = fc_0(relu(x)); dx = fc_1(relu(net)); out = (shortcut ? shortcut(x) : x) + dx.
// Shortcut presence detected from the checkpoint (encoder blocks have it, udf_decoder blocks do not).
void ResnetBlockFC(const FNKSRWeightStore& Store, const FString& Prefix, const FNKSRMatrix& X, FNKSRMatrix& Out)
{
	const FNKSRMatrix& W0 = GetTensor(Store, Prefix + TEXT(".fc_0.weight"));
	const FNKSRMatrix& B0 = GetTensor(Store, Prefix + TEXT(".fc_0.bias"));
	const FNKSRMatrix& W1 = GetTensor(Store, Prefix + TEXT(".fc_1.weight"));
	const FNKSRMatrix& B1 = GetTensor(Store, Prefix + TEXT(".fc_1.bias"));

	FNKSRMatrix Act = X;
	NKSRTensorOps::ReluInPlace(Act);
	FNKSRMatrix Net;
	NKSRTensorOps::Linear(Act, W0, &B0, Net);
	NKSRTensorOps::ReluInPlace(Net);           // net only feeds fc_1 afterwards — in-place is safe
	FNKSRMatrix Dx;
	NKSRTensorOps::Linear(Net, W1, &B1, Dx);

	const FNKSRMatrix* WShort = Store.Find(Prefix + TEXT(".shortcut.weight"));
	if (WShort) NKSRTensorOps::Linear(X, *WShort, nullptr, Out);
	else Out = X;
	AddInPlace(Out, Dx);
}

// kmap cache: (depth, kernelSize=3) on the owning svh; same grid in/out (all ks 3x3 convs are same-grid).
const FNKSRKernelMap& GetKernelMap3(FNKSRSvh& Svh, int32 Depth)
{
	const FIntPoint Key(Depth, 3);
	if (const FNKSRKernelMap* Found = Svh.KernelMaps.Find(Key)) return *Found;
	FNKSRKernelMap KMap;
	NKSRBuildKernelMap(*Svh.Grids[Depth], *Svh.Grids[Depth], 3, KMap);
	return Svh.KernelMaps.Add(Key, MoveTemp(KMap));
}

// SparseConvBlock order='gcr': GroupNorm(8, Cin) -> Conv3x3 (no bias) -> ReLU (spec B §3.4, F: norm-before-conv).
void ConvBlockGcr3(const FNKSRWeightStore& Store, const FString& Prefix, FNKSRSvh& Svh, int32 Depth, FNKSRMatrix& Feat)
{
	const FNKSRMatrix& GnW = GetTensor(Store, Prefix + TEXT(".GroupNorm.weight"));
	const FNKSRMatrix& GnB = GetTensor(Store, Prefix + TEXT(".GroupNorm.bias"));
	NKSRTensorOps::GroupNorm(Feat, GNumGroups, GnW.Data, GnB.Data, GGnEps);   // GC-6: global statistics

	const FNKSRMatrix& K = GetTensor(Store, Prefix + TEXT(".Conv.kernel"));   // [27, Cin, Cout], GC-4 order
	const int32 Cin = Feat.Cols;
	const int32 Denom = 27 * Cin;
	if (Denom == 0 || K.Data.Num() == 0 || K.Data.Num() % Denom != 0)
	{
		UE_LOG(LogNKSR, Error, TEXT("NKSRNetwork: conv kernel '%s.Conv.kernel' size %d incompatible with Cin=%d"), *Prefix, K.Data.Num(), Cin);
		return;
	}
	// Reshape [27][Cin][Cout] -> [27*Cin, Cout]: identical row-major data, per NKSRConv contract.
	FNKSRMatrix KFlat;
	KFlat.Rows = Denom;
	KFlat.Cols = K.Data.Num() / Denom;
	KFlat.Data = K.Data;

	const FNKSRKernelMap& KMap = GetKernelMap3(Svh, Depth);
	FNKSRMatrix Out;
	NKSRSparseConvolution(Feat, KFlat, 27, KMap, Feat.Rows, nullptr, Out);
	NKSRTensorOps::ReluInPlace(Out);
	Feat = MoveTemp(Out);
}

// SparseConvBlock order='gcr', kernel_size=1 (OneConv0): GroupNorm -> 1x1 conv (no bias) -> ReLU.
void ConvBlockGcr1(const FNKSRWeightStore& Store, const FString& Prefix, FNKSRMatrix& Feat)
{
	const FNKSRMatrix& GnW = GetTensor(Store, Prefix + TEXT(".GroupNorm.weight"));
	const FNKSRMatrix& GnB = GetTensor(Store, Prefix + TEXT(".GroupNorm.bias"));
	NKSRTensorOps::GroupNorm(Feat, GNumGroups, GnW.Data, GnB.Data, GGnEps);
	const FNKSRMatrix& K = GetTensor(Store, Prefix + TEXT(".Conv.kernel"));   // [Cin, Cout]
	FNKSRMatrix Out;
	NKSRConv1x1(Feat, K, nullptr, Out);
	NKSRTensorOps::ReluInPlace(Out);
	Feat = MoveTemp(Out);
}

// SparseHead (non-enhanced): SingleConv (gcr, C->C, K=3) -> OutConv (1x1, bias).
void HeadForward(const FNKSRWeightStore& Store, const FString& Prefix, FNKSRSvh& Svh, int32 Depth, const FNKSRMatrix& In, FNKSRMatrix& Out)
{
	FNKSRMatrix F = In;
	ConvBlockGcr3(Store, Prefix + TEXT(".SingleConv"), Svh, Depth, F);
	const FNKSRMatrix& OutK = GetTensor(Store, Prefix + TEXT(".OutConv.kernel"));
	const FNKSRMatrix& OutB = GetTensor(Store, Prefix + TEXT(".OutConv.bias"));
	NKSRConv1x1(F, OutK, &OutB, Out);
}

// SparseHead (enhanced, basis): SingleConv(C->C) -> SingleConv2(C->mid) -> OneConv0(mid->mid, K=1) -> OutConv(mid->4, bias).
void BasisHeadForward(const FNKSRWeightStore& Store, const FString& Prefix, FNKSRSvh& Svh, int32 Depth, const FNKSRMatrix& In, FNKSRMatrix& Out)
{
	FNKSRMatrix F = In;
	ConvBlockGcr3(Store, Prefix + TEXT(".SingleConv"), Svh, Depth, F);
	ConvBlockGcr3(Store, Prefix + TEXT(".SingleConv2"), Svh, Depth, F);
	ConvBlockGcr1(Store, Prefix + TEXT(".OneConv0"), F);
	const FNKSRMatrix& OutK = GetTensor(Store, Prefix + TEXT(".OutConv.kernel"));
	const FNKSRMatrix& OutB = GetTensor(Store, Prefix + TEXT(".OutConv.bias"));
	NKSRConv1x1(F, OutK, &OutB, Out);
}

// modules.py SparseZeroPadding: row order = out grid voxel order, missing source voxels -> 0 (GC-5).
void ZeroPad(const FNKSRMatrix& In, const FNKSRIndexGrid& InGrid, const FNKSRIndexGrid& OutGrid, FNKSRMatrix& Out)
{
	if (&InGrid == &OutGrid)
	{
		Out = In;
		return;
	}
	Out.SetZeroed(OutGrid.NumVoxels(), In.Cols);
	TArray<int32> SrcIdx;
	InGrid.IjkToIndex(OutGrid.ActiveGridCoords(), SrcIdx);
	for (int32 R = 0; R < Out.Rows; ++R)
	{
		const int32 S = SrcIdx[R];
		if (S != -1) FMemory::Memcpy(Out.Row(R), In.Row(S), sizeof(float) * In.Cols);
	}
}

// SparseZeroPadding on a 1D bool vector (upsample mask); missing -> false.
void ZeroPadMask(const TArray<bool>& In, const FNKSRIndexGrid& InGrid, const FNKSRIndexGrid& OutGrid, TArray<bool>& Out)
{
	if (&InGrid == &OutGrid)
	{
		Out = In;
		return;
	}
	Out.Init(false, OutGrid.NumVoxels());
	TArray<int32> SrcIdx;
	InGrid.IjkToIndex(OutGrid.ActiveGridCoords(), SrcIdx);
	for (int32 R = 0; R < Out.Num(); ++R)
	{
		const int32 S = SrcIdx[R];
		if (S != -1) Out[R] = In[S];
	}
}

// interpolator.py MLPWithGrad even-layer jacobian: J <- W @ J, per row (single writer per output row).
void LinearJacobian(const FNKSRMatrix& W /*[out,in]*/, const FNKSRGradTensor& JIn, FNKSRGradTensor& JOut)
{
	const int32 M = JIn.Rows;
	const int32 CIn = JIn.Channels;
	const int32 COut = W.Rows;
	JOut.SetZeroed(M, COut);
	ParallelFor(M, [&W, &JIn, &JOut, CIn, COut](int32 R)
	{
		const float* JRow = JIn.Data.GetData() + R * CIn * 3;
		float* ORow = JOut.Data.GetData() + R * COut * 3;
		for (int32 O = 0; O < COut; ++O)
		{
			const float* WRow = W.Row(O);
			float Acc0 = 0.f, Acc1 = 0.f, Acc2 = 0.f;
			for (int32 I = 0; I < CIn; ++I)
			{
				const float Wv = WRow[I];
				Acc0 += Wv * JRow[I * 3 + 0];
				Acc1 += Wv * JRow[I * 3 + 1];
				Acc2 += Wv * JRow[I * 3 + 2];
			}
			ORow[O * 3 + 0] = Acc0;
			ORow[O * 3 + 1] = Acc1;
			ORow[O * 3 + 2] = Acc2;
		}
	});
}

// interpolator.py odd-layer jacobian: J <- 1[x != 0] * J using the POST-ReLU activation (x > 0 after ReLU).
void ReluJacobianInPlace(const FNKSRMatrix& PostReluX, FNKSRGradTensor& J)
{
	ParallelFor(PostReluX.Rows, [&PostReluX, &J](int32 R)
	{
		for (int32 C = 0; C < PostReluX.Cols; ++C)
		{
			if (PostReluX.At(R, C) > 0.f) continue;
			J.At(R, C, 0) = 0.f;
			J.At(R, C, 1) = 0.f;
			J.At(R, C, 2) = 0.f;
		}
	});
}

} // namespace

//======================================================================================================
// Init: bind + shape-check every tensor on the ks inference path (spec B §9 / F inventory).
//======================================================================================================

bool FNKSRNetwork::Init(const FNKSRWeightStore& Weights, FString& OutError)
{
	Store = &Weights;
	bool bOk = true;
	auto Check = [&Weights, &OutError, &bOk](const FString& Key, std::initializer_list<int32> Dims)
	{
		if (!Weights.FindChecked(Key, MakeArrayView(Dims.begin(), (int32)Dims.size()), OutError)) bOk = false;
	};

	// --- encoder (PointEncoder dim=6, c_dim=32, hidden=32, n_blocks=3) ---
	Check(TEXT("encoder.fc_pos.weight"), { 64, 6 });
	Check(TEXT("encoder.fc_pos.bias"), { 64 });
	for (int32 B = 0; B < 3; ++B)
	{
		const FString P = FString::Printf(TEXT("encoder.blocks.%d."), B);
		Check(P + TEXT("fc_0.weight"), { 32, 64 });
		Check(P + TEXT("fc_0.bias"), { 32 });
		Check(P + TEXT("fc_1.weight"), { 32, 32 });
		Check(P + TEXT("fc_1.bias"), { 32 });
		Check(P + TEXT("shortcut.weight"), { 32, 64 });
	}
	Check(TEXT("encoder.fc_c.weight"), { 32, 32 });
	Check(TEXT("encoder.fc_c.bias"), { 32 });

	// --- interpolators (4 -> 16 -> 16 -> 16 -> 4, Sequential indices 0/2/4/6) ---
	for (int32 D = 0; D < TreeDepth; ++D)
	{
		const FString P = FString::Printf(TEXT("interpolators.%d.mlp.layers."), D);
		Check(P + TEXT("0.weight"), { 16, 4 });
		Check(P + TEXT("0.bias"), { 16 });
		Check(P + TEXT("2.weight"), { 16, 16 });
		Check(P + TEXT("2.bias"), { 16 });
		Check(P + TEXT("4.weight"), { 16, 16 });
		Check(P + TEXT("4.bias"), { 16 });
		Check(P + TEXT("6.weight"), { 4, 16 });
		Check(P + TEXT("6.bias"), { 4 });
	}

	// --- udf_decoder (MultiscalePointDecoder: fc_p 6->32, fc_c 2x 64->32, 2 blocks 32 (no shortcut), fc_out 32->1) ---
	Check(TEXT("udf_decoder.fc_p.weight"), { 32, 6 });
	Check(TEXT("udf_decoder.fc_p.bias"), { 32 });
	for (int32 I = 0; I < 2; ++I)
	{
		Check(FString::Printf(TEXT("udf_decoder.fc_c.%d.weight"), I), { 32, 64 });
		Check(FString::Printf(TEXT("udf_decoder.fc_c.%d.bias"), I), { 32 });
		const FString P = FString::Printf(TEXT("udf_decoder.blocks.%d."), I);
		Check(P + TEXT("fc_0.weight"), { 32, 32 });
		Check(P + TEXT("fc_0.bias"), { 32 });
		Check(P + TEXT("fc_1.weight"), { 32, 32 });
		Check(P + TEXT("fc_1.bias"), { 32 });
	}
	Check(TEXT("udf_decoder.fc_out.weight"), { 1, 32 });
	Check(TEXT("udf_decoder.fc_out.bias"), { 1 });

	// --- unet encoders / decoders (SparseDoubleConv; channel table spec B §9.2) ---
	struct FDoubleConvDims { const TCHAR* Name; int32 C1In, C1Out, C2In, C2Out; };
	static const FDoubleConvDims DoubleConvs[] =
	{
		{ TEXT("unet.encoders.Enc0"), 32, 32, 32, 32 },
		{ TEXT("unet.encoders.Enc1"), 32, 32, 32, 64 },
		{ TEXT("unet.encoders.Enc2"), 64, 64, 64, 128 },
		{ TEXT("unet.encoders.Enc3"), 128, 128, 128, 256 },
		{ TEXT("unet.decoders.Dec-2"), 384, 128, 128, 128 },
		{ TEXT("unet.decoders.Dec-3"), 192, 64, 64, 64 },
		{ TEXT("unet.decoders.Dec-4"), 96, 32, 32, 32 },
	};
	for (const FDoubleConvDims& DC : DoubleConvs)
	{
		const FString P(DC.Name);
		Check(P + TEXT(".SingleConv1.GroupNorm.weight"), { DC.C1In });
		Check(P + TEXT(".SingleConv1.GroupNorm.bias"), { DC.C1In });
		Check(P + TEXT(".SingleConv1.Conv.kernel"), { 27, DC.C1In, DC.C1Out });
		Check(P + TEXT(".SingleConv2.GroupNorm.weight"), { DC.C2In });
		Check(P + TEXT(".SingleConv2.GroupNorm.bias"), { DC.C2In });
		Check(P + TEXT(".SingleConv2.Conv.kernel"), { 27, DC.C2In, DC.C2Out });
	}

	// --- heads (j in 1..4, C = {256,128,64,32}, depth = 4-j) ---
	auto CheckPlainHead = [&Check](const FString& P, int32 C, int32 OutDim)
	{
		Check(P + TEXT(".SingleConv.GroupNorm.weight"), { C });
		Check(P + TEXT(".SingleConv.GroupNorm.bias"), { C });
		Check(P + TEXT(".SingleConv.Conv.kernel"), { 27, C, C });
		Check(P + TEXT(".OutConv.kernel"), { C, OutDim });
		Check(P + TEXT(".OutConv.bias"), { OutDim });
	};
	static const int32 HeadC[4] = { 256, 128, 64, 32 };
	for (int32 J = 1; J <= 4; ++J)
	{
		const int32 C = HeadC[J - 1];
		const int32 Mid = FMath::Min(64, C);
		CheckPlainHead(FString::Printf(TEXT("unet.struct_heads.Struct-%d"), J), C, 3);
		CheckPlainHead(FString::Printf(TEXT("unet.udf_heads.UDF-%d"), J), C, UdfBranchDim);
		// Normal-1 / Normal-2 exist in the checkpoint but are never evaluated (d < adaptive_depth) — not validated.
		if (J >= 3) CheckPlainHead(FString::Printf(TEXT("unet.normal_heads.Normal-%d"), J), C, 3);

		const FString BP = FString::Printf(TEXT("unet.basis_heads.Basis-%d"), J);
		Check(BP + TEXT(".SingleConv.GroupNorm.weight"), { C });
		Check(BP + TEXT(".SingleConv.GroupNorm.bias"), { C });
		Check(BP + TEXT(".SingleConv.Conv.kernel"), { 27, C, C });
		Check(BP + TEXT(".SingleConv2.GroupNorm.weight"), { C });
		Check(BP + TEXT(".SingleConv2.GroupNorm.bias"), { C });
		Check(BP + TEXT(".SingleConv2.Conv.kernel"), { 27, C, Mid });
		Check(BP + TEXT(".OneConv0.GroupNorm.weight"), { Mid });
		Check(BP + TEXT(".OneConv0.GroupNorm.bias"), { Mid });
		Check(BP + TEXT(".OneConv0.Conv.kernel"), { Mid, Mid });
		Check(BP + TEXT(".OutConv.kernel"), { Mid, KernelDim });
		Check(BP + TEXT(".OutConv.bias"), { KernelDim });
	}

	if (!bOk) Store = nullptr;
	return bOk;
}

//======================================================================================================
// PointEncoder.forward (encdec.py)
//======================================================================================================

void FNKSRNetwork::PointEncoderForward(TConstArrayView<FVector3f> Xyz, TConstArrayView<FVector3f> Normals, const FNKSRSvh& Svh, int32 Depth, FNKSRMatrix& OutC) const
{
	OutC = FNKSRMatrix();
	if (!Store)
	{
		UE_LOG(LogNKSR, Error, TEXT("PointEncoderForward: network not initialized"));
		return;
	}
	if (!Svh.Grids.IsValidIndex(Depth) || !Svh.Grids[Depth])
	{
		UE_LOG(LogNKSR, Error, TEXT("PointEncoderForward: grid at depth %d is not built"), Depth);
		return;
	}
	if (Xyz.Num() != Normals.Num())
	{
		UE_LOG(LogNKSR, Error, TEXT("PointEncoderForward: xyz/normal count mismatch (%d vs %d)"), Xyz.Num(), Normals.Num());
		return;
	}
	const FNKSRIndexGrid& Grid = *Svh.Grids[Depth];
	const int32 NumVox = Grid.NumVoxels();

	TArray<FVector3f> G;
	Grid.WorldToGrid(Xyz, G);

	// vid + pts_mask filtering; local coords = (g + 0.5) % 1 (torch '%' semantics, GC-1 PosMod).
	// GC-1: RoundHalfUp for voxel id (contract choice; torch.round is half-to-even, differs only at exact .5).
	TArray<int32> Vid;
	Vid.Reserve(Xyz.Num());
	FNKSRMatrix In;   // [n, 6] = [local(3), normal(3)]
	In.Data.Reserve(Xyz.Num() * 6);
	for (int32 I = 0; I < Xyz.Num(); ++I)
	{
		const FNKSRIjk R(NKSRRoundHalfUp(G[I].X), NKSRRoundHalfUp(G[I].Y), NKSRRoundHalfUp(G[I].Z));
		const int32 V = Grid.IndexOf(R);
		if (V == -1) continue;
		Vid.Add(V);
		In.Data.Add(NKSRPosModF(G[I].X + 0.5f, 1.f));
		In.Data.Add(NKSRPosModF(G[I].Y + 0.5f, 1.f));
		In.Data.Add(NKSRPosModF(G[I].Z + 0.5f, 1.f));
		In.Data.Add(Normals[I].X);
		In.Data.Add(Normals[I].Y);
		In.Data.Add(Normals[I].Z);
	}
	In.Rows = Vid.Num();
	In.Cols = 6;

	FNKSRMatrix H;
	NKSRTensorOps::Linear(In, GetTensor(*Store, TEXT("encoder.fc_pos.weight")), &GetTensor(*Store, TEXT("encoder.fc_pos.bias")), H);   // [n,64]

	FNKSRMatrix H32;
	ResnetBlockFC(*Store, TEXT("encoder.blocks.0"), H, H32);   // [n,32]
	for (int32 B = 1; B < 3; ++B)
	{
		FNKSRMatrix Pooled;
		NKSRTensorOps::ScatterMax(H32, Vid, NumVox, Pooled);   // per-voxel max; empty buckets -> 0 (GC-5), never read back
		FNKSRMatrix PooledGathered;
		NKSRTensorOps::GatherRows(Pooled, Vid, PooledGathered);
		FNKSRMatrix Cat;
		NKSRTensorOps::ConcatCols(H32, PooledGathered, Cat);   // [n,64]
		ResnetBlockFC(*Store, FString::Printf(TEXT("encoder.blocks.%d"), B), Cat, H32);
	}

	FNKSRMatrix C;
	NKSRTensorOps::Linear(H32, GetTensor(*Store, TEXT("encoder.fc_c.weight")), &GetTensor(*Store, TEXT("encoder.fc_c.bias")), C);
	NKSRTensorOps::ScatterMean(C, Vid, NumVox, OutC);          // empty voxels -> 0 (GC-5)
}

//======================================================================================================
// SparseStructureNet.forward (unet.py, inference path: argmax structure decisions)
//======================================================================================================

void FNKSRNetwork::UnetForward(const FNKSRMatrix& Feat, FNKSRSvh& EncoderSvh, int32 AdaptiveDepth_, FNKSRFeaturesSet& OutFeatures, FNKSRSvh& OutDecoderSvh, FNKSRSvh& OutDecoderTmpSvh) const
{
	// populate_empty semantics up-front: absent depths keep Rows==0 with correct Cols.
	OutFeatures = FNKSRFeaturesSet();
	OutFeatures.Structure.SetNum(TreeDepth);
	OutFeatures.Normal.SetNum(TreeDepth);
	OutFeatures.Basis.SetNum(TreeDepth);
	OutFeatures.Udf.SetNum(TreeDepth);
	for (int32 D = 0; D < TreeDepth; ++D)
	{
		OutFeatures.Structure[D].SetZeroed(0, 3);
		OutFeatures.Normal[D].SetZeroed(0, 3);
		OutFeatures.Basis[D].SetZeroed(0, KernelDim);
		OutFeatures.Udf[D].SetZeroed(0, UdfBranchDim);
	}
	OutDecoderSvh = FNKSRSvh(EncoderSvh.VoxelSize, EncoderSvh.Depth);
	OutDecoderTmpSvh = FNKSRSvh(EncoderSvh.VoxelSize, EncoderSvh.Depth);

	if (!Store)
	{
		UE_LOG(LogNKSR, Error, TEXT("UnetForward: network not initialized"));
		return;
	}
	if (EncoderSvh.Depth != TreeDepth)
	{
		UE_LOG(LogNKSR, Error, TEXT("UnetForward: svh depth %d != tree depth %d"), EncoderSvh.Depth, TreeDepth);
		return;
	}
	for (int32 D = 0; D < TreeDepth; ++D)
	{
		if (!EncoderSvh.Grids[D])
		{
			UE_LOG(LogNKSR, Error, TEXT("UnetForward: encoder grid at depth %d is not built"), D);
			return;
		}
	}
	if (Feat.Rows != EncoderSvh.Grids[0]->NumVoxels())
	{
		UE_LOG(LogNKSR, Error, TEXT("UnetForward: feature rows %d != depth-0 voxels %d"), Feat.Rows, EncoderSvh.Grids[0]->NumVoxels());
		return;
	}

	// --- encoder path (Enc0 no pooling; Enc1..3 MaxPool then two conv blocks) ---
	TArray<FNKSRMatrix> EncFeats;
	EncFeats.SetNum(TreeDepth);
	FNKSRMatrix F = Feat;
	for (int32 K = 0; K < TreeDepth; ++K)
	{
		if (K > 0)
		{
			FNKSRMatrix Pooled;
			EncoderSvh.Grids[K - 1]->MaxPool(F, 2, *EncoderSvh.Grids[K], Pooled);   // -inf -> 0 inside (GC-5)
			F = MoveTemp(Pooled);
		}
		const FString P = FString::Printf(TEXT("unet.encoders.Enc%d"), K);
		ConvBlockGcr3(*Store, P + TEXT(".SingleConv1"), EncoderSvh, K, F);
		ConvBlockGcr3(*Store, P + TEXT(".SingleConv2"), EncoderSvh, K, F);
		EncFeats[K] = F;
	}

	// --- dense neck (neck_type='dense', neck_expand=1): closed bbox of depth-3 active coords ---
	const int32 NeckDepth = TreeDepth - 1;
	{
		TConstArrayView<FNKSRIjk> Coords3 = EncoderSvh.Grids[NeckDepth]->ActiveGridCoords();
		if (Coords3.Num() == 0)
		{
			UE_LOG(LogNKSR, Error, TEXT("UnetForward: depth-%d grid is empty"), NeckDepth);
			return;
		}
		FIntVector MinB = Coords3[0], MaxB = Coords3[0];
		for (const FNKSRIjk& Ijk : Coords3)
		{
			MinB = FIntVector(FMath::Min(MinB.X, Ijk.X), FMath::Min(MinB.Y, Ijk.Y), FMath::Min(MinB.Z, Ijk.Z));
			MaxB = FIntVector(FMath::Max(MaxB.X, Ijk.X), FMath::Max(MaxB.Y, Ijk.Y), FMath::Max(MaxB.Z, Ijk.Z));
		}
		const int64 Nx = (int64)MaxB.X - MinB.X + 1, Ny = (int64)MaxB.Y - MinB.Y + 1, Nz = (int64)MaxB.Z - MinB.Z + 1;
		const int64 NumNeck = Nx * Ny * Nz;
		UE_LOG(LogNKSR, Log, TEXT("UnetForward: dense neck bbox [%d..%d]x[%d..%d]x[%d..%d] = %lld voxels"),
			MinB.X, MaxB.X, MinB.Y, MaxB.Y, MinB.Z, MaxB.Z, NumNeck);
		if (NumNeck > (int64)2e7)
		{
			UE_LOG(LogNKSR, Error, TEXT("UnetForward: dense neck of %lld voxels exceeds the 2e7 limit — aborting (input span too large for voxel size)"), NumNeck);
			return;
		}
		TArray<FNKSRIjk> NeckCoords;
		NeckCoords.Reserve((int32)NumNeck);
		for (int32 X = MinB.X; X <= MaxB.X; ++X) for (int32 Y = MinB.Y; Y <= MaxB.Y; ++Y) for (int32 Z = MinB.Z; Z <= MaxB.Z; ++Z) NeckCoords.Add(FNKSRIjk(X, Y, Z));
		OutDecoderTmpSvh.BuildFromGridCoords(NeckDepth, NeckCoords);
	}

	FNKSRMatrix DecMain;
	ZeroPad(EncFeats[NeckDepth], *EncoderSvh.Grids[NeckDepth], *OutDecoderTmpSvh.Grids[NeckDepth], DecMain);

	// --- decoder loop (it = 0..3, d = 3-it; head suffix j = it+1) ---
	TArray<bool> UpMask;   // over OutDecoderSvh.Grids[d+1] rows
	for (int32 It = 0; It < TreeDepth; ++It)
	{
		const int32 D = TreeDepth - 1 - It;
		const int32 J = It + 1;

		if (It > 0)
		{
			// Upsampling(2,'nearest'): tmp grid d = subdivided(dec grid d+1, up_mask); feat = parent value.
			const FNKSRIndexGrid& DecPrev = *OutDecoderSvh.Grids[D + 1];
			OutDecoderTmpSvh.BuildFromGrid(D, DecPrev.Subdivided(2, UpMask));
			const FNKSRIndexGrid& TmpG = *OutDecoderTmpSvh.Grids[D];
			FNKSRMatrix Up;
			DecPrev.Subdivide(DecMain, 2, TmpG, Up);
			FNKSRMatrix Skip;
			ZeroPad(EncFeats[D], *EncoderSvh.Grids[D], TmpG, Skip);
			FNKSRMatrix Cat;
			NKSRTensorOps::ConcatCols(Skip, Up, Cat);   // channel order: [encoder skip, upsampled decoder]
			DecMain = MoveTemp(Cat);
			const FString DecP = FString::Printf(TEXT("unet.decoders.Dec-%d"), J);
			ConvBlockGcr3(*Store, DecP + TEXT(".SingleConv1"), OutDecoderTmpSvh, D, DecMain);
			ConvBlockGcr3(*Store, DecP + TEXT(".SingleConv2"), OutDecoderTmpSvh, D, DecMain);
		}

		// struct / udf heads live on the tmp (pre-pruning) grid.
		HeadForward(*Store, FString::Printf(TEXT("unet.struct_heads.Struct-%d"), J), OutDecoderTmpSvh, D, DecMain, OutFeatures.Structure[D]);
		HeadForward(*Store, FString::Printf(TEXT("unet.udf_heads.UDF-%d"), J), OutDecoderTmpSvh, D, DecMain, OutFeatures.Udf[D]);

		// argmax structure decision (GC-6: ties -> smallest index). 0=NON_EXIST, 1=EXIST_STOP, 2=EXIST_CONTINUE.
		const FNKSRMatrix& St = OutFeatures.Structure[D];
		TArray<uint8> Decision;
		Decision.SetNumUninitialized(St.Rows);
		int32 NumExist = 0;
		for (int32 R = 0; R < St.Rows; ++R)
		{
			const float* Row = St.Row(R);
			uint8 Best = 0;
			if (Row[1] > Row[0]) Best = 1;
			if (Row[2] > Row[Best]) Best = 2;
			Decision[R] = Best;
			if (Best != 0) ++NumExist;
		}
		if (NumExist == 0) break;   // early stop: predicted structure empty

		// prune: dec grid d = tmp coords where decision != 0; features re-ordered onto it.
		const FNKSRIndexGrid& TmpG = *OutDecoderTmpSvh.Grids[D];
		TConstArrayView<FNKSRIjk> TmpCoords = TmpG.ActiveGridCoords();
		TArray<FNKSRIjk> DecCoords;
		DecCoords.Reserve(NumExist);
		for (int32 R = 0; R < TmpCoords.Num(); ++R) if (Decision[R] != 0) DecCoords.Add(TmpCoords[R]);
		OutDecoderSvh.BuildFromGridCoords(D, DecCoords);
		const FNKSRIndexGrid& DecG = *OutDecoderSvh.Grids[D];
		FNKSRMatrix Pruned;
		ZeroPad(DecMain, TmpG, DecG, Pruned);
		DecMain = MoveTemp(Pruned);

		TArray<bool> ContinueMask;
		ContinueMask.SetNumUninitialized(Decision.Num());
		for (int32 R = 0; R < Decision.Num(); ++R) ContinueMask[R] = (Decision[R] == 2);
		ZeroPadMask(ContinueMask, TmpG, DecG, UpMask);

		// normal / basis heads live on the dec (pruned) grid (normal only below adaptive_depth, spec B §5.3).
		const FString NormalP = FString::Printf(TEXT("unet.normal_heads.Normal-%d"), J);
		if (D < AdaptiveDepth_) HeadForward(*Store, NormalP, OutDecoderSvh, D, DecMain, OutFeatures.Normal[D]);
		BasisHeadForward(*Store, FString::Printf(TEXT("unet.basis_heads.Basis-%d"), J), OutDecoderSvh, D, DecMain, OutFeatures.Basis[D]);

		if (!UpMask.Contains(true)) break;   // early stop: next level certainly empty
	}
}

//======================================================================================================
// MLPFeatureInterpolator.interpolate (interpolator.py MLPWithGrad)
//======================================================================================================

void FNKSRNetwork::InterpolatorEvaluate(int32 Depth, TConstArrayView<FVector3f> Queries, const FNKSRIndexGrid& Grid,
                                        const FNKSRMatrix& Features, bool bGrad, FNKSRMatrix& OutTheta, FNKSRGradTensor* OutGradTheta) const
{
	OutTheta.SetZeroed(0, KernelDim);
	if (OutGradTheta) OutGradTheta->SetZeroed(0, KernelDim);
	if (!Store || Depth < 0 || Depth >= TreeDepth)
	{
		UE_LOG(LogNKSR, Error, TEXT("InterpolatorEvaluate: bad state (Store=%d, Depth=%d)"), Store != nullptr, Depth);
		return;
	}
	if (Queries.Num() == 0) return;
	if (Features.Rows != Grid.NumVoxels())
	{
		UE_LOG(LogNKSR, Error, TEXT("InterpolatorEvaluate: feature rows %d != grid voxels %d (depth %d)"), Features.Rows, Grid.NumVoxels(), Depth);
		return;
	}

	const bool bWantGrad = bGrad && OutGradTheta != nullptr;
	FNKSRMatrix X;
	FNKSRGradTensor Jac;
	Grid.SampleTrilinear(Queries, Features, X, bWantGrad ? &Jac : nullptr);   // v, J_v = d(sample)/d(world xyz)

	const FString Base = FString::Printf(TEXT("interpolators.%d.mlp.layers."), Depth);
	static const int32 LinearIdx[4] = { 0, 2, 4, 6 };
	for (int32 L = 0; L < 4; ++L)
	{
		const FNKSRMatrix& W = GetTensor(*Store, Base + FString::Printf(TEXT("%d.weight"), LinearIdx[L]));
		const FNKSRMatrix& B = GetTensor(*Store, Base + FString::Printf(TEXT("%d.bias"), LinearIdx[L]));
		FNKSRMatrix XNext;
		NKSRTensorOps::Linear(X, W, &B, XNext);
		X = MoveTemp(XNext);
		if (bWantGrad)
		{
			FNKSRGradTensor JNext;
			LinearJacobian(W, Jac, JNext);
			Jac = MoveTemp(JNext);
		}
		if (L < 3)
		{
			NKSRTensorOps::ReluInPlace(X);
			if (bWantGrad) ReluJacobianInPlace(X, Jac);
		}
	}
	OutTheta = MoveTemp(X);
	if (bWantGrad) *OutGradTheta = MoveTemp(Jac);
}

//======================================================================================================
// udf_decoder forward (encdec.py MultiscalePointDecoder, coords_depths=[2,3], aggregation='cat')
//======================================================================================================

void FNKSRNetwork::UdfDecoderForward(TConstArrayView<FVector3f> Xyz, const FNKSRSvh& Svh, const TArray<FNKSRMatrix>& UdfFeatures, TArray<float>& OutVals) const
{
	OutVals.Reset();
	if (!Store)
	{
		UE_LOG(LogNKSR, Error, TEXT("UdfDecoderForward: network not initialized"));
		return;
	}
	const int32 M = Xyz.Num();
	if (M == 0) return;

	// positional encoding p = (xyz % vs_d)/vs_d - 0.5 for d in {2,3} (torch '%' semantics, world coords, GC-1).
	FNKSRMatrix P;
	P.SetUninitialized(M, 6);
	static const int32 CoordsDepths[2] = { 2, 3 };
	for (int32 Ci = 0; Ci < 2; ++Ci)
	{
		double VsD, OriginD;
		Svh.GetGridVoxelSizeOrigin(CoordsDepths[Ci], VsD, OriginD);
		const float Vs = (float)VsD;
		for (int32 I = 0; I < M; ++I)
		{
			float* Row = P.Row(I) + Ci * 3;
			Row[0] = NKSRPosModF(Xyz[I].X, Vs) / Vs - 0.5f;
			Row[1] = NKSRPosModF(Xyz[I].Y, Vs) / Vs - 0.5f;
			Row[2] = NKSRPosModF(Xyz[I].Z, Vs) / Vs - 0.5f;
		}
	}

	// multiscale features: 4 depths x 16ch trilinear samples, missing levels contribute zeros (GC-5).
	FNKSRMatrix C;
	C.SetZeroed(M, UdfBranchDim * TreeDepth);
	for (int32 D = 0; D < TreeDepth; ++D)
	{
		const FNKSRIndexGrid* Grid = (Svh.Grids.IsValidIndex(D) && Svh.Grids[D]) ? Svh.Grids[D].Get() : nullptr;
		if (!Grid) continue;
		const FNKSRMatrix* Feat = UdfFeatures.IsValidIndex(D) ? &UdfFeatures[D] : nullptr;
		if (!Feat || Feat->Rows != Grid->NumVoxels() || Feat->Cols != UdfBranchDim)
		{
			UE_LOG(LogNKSR, Warning, TEXT("UdfDecoderForward: udf features at depth %d mismatch grid (%d voxels) — treated as zeros"), D, Grid->NumVoxels());
			continue;
		}
		FNKSRMatrix Ci;
		Grid->SampleTrilinear(Xyz, *Feat, Ci);
		for (int32 R = 0; R < M; ++R) FMemory::Memcpy(C.Row(R) + D * UdfBranchDim, Ci.Row(R), sizeof(float) * UdfBranchDim);
	}

	FNKSRMatrix Net;
	NKSRTensorOps::Linear(P, GetTensor(*Store, TEXT("udf_decoder.fc_p.weight")), &GetTensor(*Store, TEXT("udf_decoder.fc_p.bias")), Net);
	for (int32 I = 0; I < 2; ++I)
	{
		FNKSRMatrix Cc;
		NKSRTensorOps::Linear(C, GetTensor(*Store, FString::Printf(TEXT("udf_decoder.fc_c.%d.weight"), I)),
			&GetTensor(*Store, FString::Printf(TEXT("udf_decoder.fc_c.%d.bias"), I)), Cc);
		AddInPlace(Net, Cc);
		FNKSRMatrix NetNext;
		ResnetBlockFC(*Store, FString::Printf(TEXT("udf_decoder.blocks.%d"), I), Net, NetNext);
		Net = MoveTemp(NetNext);
	}
	NKSRTensorOps::ReluInPlace(Net);
	FNKSRMatrix Out;
	NKSRTensorOps::Linear(Net, GetTensor(*Store, TEXT("udf_decoder.fc_out.weight")), &GetTensor(*Store, TEXT("udf_decoder.fc_out.bias")), Out);

	OutVals.SetNumUninitialized(M);
	for (int32 I = 0; I < M; ++I) OutVals[I] = Out.At(I, 0);
}
