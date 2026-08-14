#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "ComputeShaderMeshBoolean.h"
#include "ComputeShaderMeshGenerator.h"

#include "Components/BoxComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// Boolean output parity: the GPU direct-write rebuild against the CPU snapshot rebuild.
//
// Why this test has to exist at all: RebuildGpuMeshFromCapture is an ALGORITHMIC port, not a
// change of ownership. The accept predicate and the barycentric attribute interpolation were
// re-implemented in HLSL against the CPU original. Every existing Boolean assertion (triangle
// counts, orientation, overflow flags) passes just as happily on drifted UVs, flipped tangents
// or a wrong normal on a near-degenerate fragment, so only an attribute-by-attribute comparison
// pins the port down.
//
// THE PIPELINE RUNS ONCE. This matters and is not an optimisation. The Boolean pipeline is not
// run-to-run deterministic: the source soup is filled by a CAS bump allocator
// (ExtractStaticMeshTrianglesCS), so a source triangle's soup index moves between runs, and the
// BSP orders each triangle's cuts by the *index* of the triangle that produced them
// (BSPCutOtherOwner) — so the cut application order, and with it the tessellation, moves too.
// Two runs of the same Boolean on the same scene return different triangle counts. Running the
// pipeline twice and comparing the two rebuilds therefore compares two different fragment sets,
// not two implementations, and any deviation it reports is meaningless.
//
// So: one run, captured (FCSMeshBooleanCapture), and both rebuilds are handed that same capture.
// The CPU arrays are readbacks of the very buffers the GPU rebuild reads, so the two halves are
// fed byte-identical input by construction, and every difference that survives is the port.
//
// The comparison is made on the RESIDENT representation of both results, not on the CPU
// snapshot: that is what a caller of ApplyMeshBoolean gets, it applies the same 8-bit
// quantisation to the tangent basis and the same BGRA quantisation to colours on both
// sides, and it is where a mistake would actually be visible.
//
// Triangles are matched by geometry rather than by index: both rebuilds allocate output slots
// differently (a sequential CPU loop against an atomic GPU cursor), so index i on one side is an
// unrelated triangle on the other. With one shared capture the positions are bit-identical, so
// the match is unambiguous.
// -----------------------------------------------------------------------------

namespace
{
// Unity builds share a TU, so file-local names carry a per-file prefix.

/** Tolerances, and why each one is what it is. */
namespace CSMeshBooleanParity_Tolerance
{
	// Positions are not recomputed by either path: both write the same float3 values out of the
	// arrangement's fragment soup (the CPU one merely takes a detour through a readback). The
	// only per-path decision is the corner swap, and both take it from the same predicate, so
	// this should be an exact match. The tolerance is here to report a drift, not to allow one.
	constexpr float Position = 1.0e-3f;      // cm

	// UV0 is the one attribute that stays in float32 all the way to the resident stream, so it
	// is where a barycentric-solve difference shows up undamped. The weights differ by the
	// float32 round-off of the solve — order 1e-6 relative — and the interpolation multiplies
	// that by the spread of the source triangle's UVs. Test geometry has UVs inside [0,1], so
	// the expected deviation is order 1e-6; 2e-4 leaves two orders of magnitude of headroom for
	// the dot/cross chain to accumulate in and still catches a real algorithmic divergence
	// (a wrong corner, an unclamped weight, a missing renormalisation) by orders of magnitude.
	constexpr float TexCoord = 2.0e-4f;

	// The tangent basis is stored as 8-bit SNORM, i.e. steps of 1/127. Both paths therefore
	// quantise the same float values through the same packer, and can only differ where a
	// component sits within float noise of a rounding boundary — at most one step.
	constexpr float TangentBasis = 1.1f / 127.0f;

	// Colours are stored as 8-bit BGRA: steps of 1/255, same argument.
	constexpr float Color = 1.1f / 255.0f;
}

/** One output triangle, flattened out of the resident representation for comparison. */
struct FCSMeshBooleanParityTriangle
{
	FVector3f Positions[3] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
	FVector2f TexCoords[3] = { FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector };
	FVector3f Normals[3] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
	FVector3f Tangents[3] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
	FVector4f Colors[3] = { FVector4f(0, 0, 0, 0), FVector4f(0, 0, 0, 0), FVector4f(0, 0, 0, 0) };
	float BinormalSigns[3] = { 1.0f, 1.0f, 1.0f };
	UMaterialInterface* Material = nullptr;
};

/**
 * Reads one resident stream back as raw uints.
 *
 * Needed for exactly one thing the mesh readback drops: the binormal sign. The readback
 * unpacks the packed tangent pair into two float3s (tangent, normal) and throws the .w byte
 * away — and the sign is precisely the thing that a "flipped tangent" bug moves, so it cannot
 * be left out of a parity comparison.
 */
bool CSMeshBooleanParity_ReadStreamUints(
	const UCSMesh* Mesh, ECSGpuStreamRole Role, uint8 Slot, TArray<uint32>& OutValues)
{
	OutValues.Reset();
	if (!IsInGameThread()) return false;

	const FCSMeshResident* Resident = Mesh ? Mesh->GetResidentPtr() : nullptr;
	if (!Resident) return false;
	const FCSMeshResident::FStream* Stream = Resident->FindStream(Role, Slot);
	if (!Stream || !Stream->Pooled.IsValid()) return false;

	const uint32 Units = FMath::Max(CSGpuMeshStreams::UnitsForCountSource(
		Stream->Desc.CountSource, FMath::Max(Resident->VertexCapacity, 1u), FMath::Max(Resident->IndexCapacity, 1u)), 1u);
	const uint32 NumUints = Units * Stream->Desc.ElementsPerUnit * Stream->Desc.BytesPerElement / uint32(sizeof(uint32));
	const uint32 Bytes = NumUints * uint32(sizeof(uint32));
	if (NumUints == 0) return false;

	FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("CSMeshBooleanParity.StreamReadback"));
	TRefCountPtr<FRDGPooledBuffer> Pooled = Stream->Pooled;
	ENQUEUE_RENDER_COMMAND(CSMeshBooleanParityEnqueueStream)(
		[Pooled, Readback, Bytes, Role](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMeshBooleanParity.ReadbackStream"));
			FRDGBufferRef StreamRDG = GraphBuilder.RegisterExternalBuffer(Pooled);
			AddEnqueueCopyPass(GraphBuilder, Readback, StreamRDG, Bytes);
			// Leaving a resident stream in RDG's SRVMask epilogue is illegal for index / indirect
			// use and simply stops the mesh drawing, so restore it even from a read-only pass.
			CSGpuMeshStreams::SetStreamAccessFinal(GraphBuilder, StreamRDG, Role);
			GraphBuilder.Execute();
		});
	FlushRenderingCommands();

	TArray<uint32> Values;
	Values.SetNumZeroed(int32(NumUints));
	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSMeshBooleanParityConsumeStream)(
		[Readback, &Values, Bytes, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			// The flush above only waits for the render thread; the copy itself is still in flight.
			if (!Readback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (Readback->IsReady() && Readback->GetGPUSizeBytes() >= Bytes)
			{
				if (const uint32* Raw = static_cast<const uint32*>(Readback->Lock(Bytes)))
				{
					FMemory::Memcpy(Values.GetData(), Raw, Bytes);
					Readback->Unlock();
					bRead = true;
				}
			}
			delete Readback;
		});
	FlushRenderingCommands();

	if (!bRead) return false;
	OutValues = MoveTemp(Values);
	return true;
}

/** Reads NumUints uints off the front of a pooled buffer. The capture's fragment buffers are not
 *  resident streams, so no per-role access state has to be restored — but nothing may consume
 *  them after this either, which is why the diagnostic below runs last. */
bool CSMeshBooleanParity_ReadPooledUints(
	const TRefCountPtr<FRDGPooledBuffer>& Pooled, uint32 NumUints, TArray<uint32>& OutValues)
{
	OutValues.Reset();
	if (!IsInGameThread() || !Pooled.IsValid() || NumUints == 0) return false;

	const uint32 Bytes = NumUints * uint32(sizeof(uint32));
	FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("CSMeshBooleanParity.CaptureReadback"));
	TRefCountPtr<FRDGPooledBuffer> Held = Pooled;
	ENQUEUE_RENDER_COMMAND(CSMeshBooleanParityEnqueueCapture)(
		[Held, Readback, Bytes](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMeshBooleanParity.ReadbackCapture"));
			FRDGBufferRef BufferRDG = GraphBuilder.RegisterExternalBuffer(Held);
			AddEnqueueCopyPass(GraphBuilder, Readback, BufferRDG, Bytes);
			GraphBuilder.Execute();
		});
	FlushRenderingCommands();

	TArray<uint32> Values;
	Values.SetNumZeroed(int32(NumUints));
	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSMeshBooleanParityConsumeCapture)(
		[Readback, &Values, Bytes, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			if (!Readback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (Readback->IsReady() && Readback->GetGPUSizeBytes() >= Bytes)
			{
				if (const uint32* Raw = static_cast<const uint32*>(Readback->Lock(Bytes)))
				{
					FMemory::Memcpy(Values.GetData(), Raw, Bytes);
					Readback->Unlock();
					bRead = true;
				}
			}
			delete Readback;
		});
	FlushRenderingCommands();

	if (!bRead) return false;
	OutValues = MoveTemp(Values);
	return true;
}

float CSMeshBooleanParity_AsFloat(uint32 Bits)
{
	float Value = 0.0f;
	FMemory::Memcpy(&Value, &Bits, sizeof(float));
	return Value;
}

uint32 CSMeshBooleanParity_AsUint(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(uint32));
	return Bits;
}

/** C++ transcription of the shader's MBOutCompensatedCross, so a disagreement can be attributed
 *  to the algorithm rather than to the HLSL compiler having folded the Dekker split away. */
float CSMeshBooleanParity_Determinant2(float A, float D, float B, float C)
{
	auto Split = [](float V, float& Hi, float& Lo)
	{
		Hi = CSMeshBooleanParity_AsFloat(CSMeshBooleanParity_AsUint(V) & 0xFFFFF000u);
		Lo = V - Hi;
	};
	auto TwoProduct = [&Split](float X, float Y, float& P, float& E)
	{
		P = X * Y;
		float XHi, XLo, YHi, YLo;
		Split(X, XHi, XLo);
		Split(Y, YHi, YLo);
		E = ((XHi * YHi - P) + XHi * YLo + XLo * YHi) + XLo * YLo;
	};
	float P1, E1, P2, E2;
	TwoProduct(A, D, P1, E1);
	TwoProduct(B, C, P2, E2);
	return (P1 - P2) + (E1 - E2);
}

FVector3f CSMeshBooleanParity_CompensatedCross(const FVector3f& U, const FVector3f& V)
{
	return FVector3f(
		CSMeshBooleanParity_Determinant2(U.Y, V.Z, U.Z, V.Y),
		CSMeshBooleanParity_Determinant2(U.Z, V.X, U.X, V.Z),
		CSMeshBooleanParity_Determinant2(U.X, V.Y, U.Y, V.X));
}

/** Corner-order-independent key for one triangle, so a GPU output triangle can be traced back to
 *  the fragment it came from even though the rebuild may have swapped corners 1 and 2. */
FString CSMeshBooleanParity_CornerSetKey(const FVector3f& A, const FVector3f& B, const FVector3f& C)
{
	TArray<FString> Parts;
	for (const FVector3f& P : { A, B, C })
		Parts.Add(FString::Printf(TEXT("%08x/%08x/%08x"),
			CSMeshBooleanParity_AsUint(P.X), CSMeshBooleanParity_AsUint(P.Y), CSMeshBooleanParity_AsUint(P.Z)));
	Parts.Sort();
	return FString::Join(Parts, TEXT("|"));
}

/**
 * Explains an accept-predicate disagreement, term by term, on the fragments both rebuilds saw.
 *
 * The counts can only diverge in the predicate — the capture guarantees identical input and the
 * attribute interpolation cannot add or remove a triangle. This walks the CPU predicate over the
 * captured fragments exactly as ComputeShaderMeshBoolean.cpp does, traces every GPU output
 * triangle back to the fragment it came from, and dumps both sides' view of the ones the CPU
 * rejected. It deliberately reports the term that fired (range / Stage B keep / non-finite /
 * degenerate area) rather than assuming which one it was.
 *
 * It also reports GPU triangles that trace back to NO fragment: that would mean the divergence is
 * not the predicate at all, and it is worth knowing rather than silently misattributing.
 */
void CSMeshBooleanParity_DiagnosePredicate(
	FAutomationTestBase& Test,
	const TCHAR* CaseName,
	const FCSMeshBooleanCapture& Capture,
	const TArray<FCSMeshBooleanParityTriangle>& GpuTriangles,
	int32 CpuTriangleCount)
{
	// Must match MB_SRC_MASK / MB_SRC_KEEP in MeshBoolean.usf and MeshBooleanSourceMask /
	// MeshBooleanSourceKeep in ComputeShaderMeshBoolean.cpp.
	constexpr uint32 SourceMask = 0x1fffffffu;
	constexpr uint32 SourceKeep = 0x80000000u;

	const uint32 FragmentCount = Capture.FragmentCount;
	TArray<uint32> Encoded;
	TArray<uint32> SoupRaw;
	if (!CSMeshBooleanParity_ReadPooledUints(Capture.FragmentSource, FragmentCount, Encoded)
		|| !CSMeshBooleanParity_ReadPooledUints(Capture.FragmentSoup, FragmentCount * 9u, SoupRaw))
	{
		Test.AddError(FString::Printf(TEXT("[%s] predicate diagnostic: could not read the captured fragment buffers back."), CaseName));
		return;
	}

	struct FFragmentVerdict
	{
		bool bAccepted = false;
		const TCHAR* Reason = TEXT("accepted");
		FVector3f Corners[3] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
		int32 Source = INDEX_NONE;
		double FacingSqDouble = 0.0;
		float FacingSqPlainFloat = 0.0f;
		float FacingSqCompensated = 0.0f;
	};

	TArray<FFragmentVerdict> Verdicts;
	Verdicts.SetNum(int32(FragmentCount));
	// Keyed by corner set, and a key can legitimately name several fragments (the arrangement can
	// emit coincident sub-triangles). Counting per key rather than mapping one-to-one is what makes
	// "the GPU emitted an extra fragment" distinguishable from "the GPU emitted one twice" — both
	// produce the same triangle-count gap and only one of them is a predicate disagreement.
	TMap<FString, TArray<int32>> KeyToFragments;
	int32 AcceptedByCpuPredicate = 0;

	for (uint32 Fragment = 0; Fragment < FragmentCount; ++Fragment)
	{
		FFragmentVerdict& Verdict = Verdicts[int32(Fragment)];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const uint32 Base = Fragment * 9u + uint32(Corner) * 3u;
			Verdict.Corners[Corner] = FVector3f(
				CSMeshBooleanParity_AsFloat(SoupRaw[int32(Base + 0)]),
				CSMeshBooleanParity_AsFloat(SoupRaw[int32(Base + 1)]),
				CSMeshBooleanParity_AsFloat(SoupRaw[int32(Base + 2)]));
		}
		KeyToFragments.FindOrAdd(
			CSMeshBooleanParity_CornerSetKey(Verdict.Corners[0], Verdict.Corners[1], Verdict.Corners[2]))
			.Add(int32(Fragment));

		// --- the CPU predicate, transcribed term by term ---
		const uint32 Word = Encoded[int32(Fragment)];
		Verdict.Source = int32(Word & SourceMask);

		const FVector3f Edge1 = Verdict.Corners[1] - Verdict.Corners[0];
		const FVector3f Edge2 = Verdict.Corners[2] - Verdict.Corners[0];
		const FVector3d Facing = FVector3d(Edge1).Cross(FVector3d(Edge2));
		Verdict.FacingSqDouble = Facing.SquaredLength();
		Verdict.FacingSqPlainFloat = FVector3f::CrossProduct(Edge1, Edge2).SizeSquared();
		Verdict.FacingSqCompensated = CSMeshBooleanParity_CompensatedCross(Edge1, Edge2).SizeSquared();

		if (Verdict.Source < 0 || Verdict.Source >= int32(Capture.SourceTriangleCount))
		{
			Verdict.Reason = TEXT("rejected: source out of range");
		}
		else if (Capture.bStageB && (Word & SourceKeep) == 0u)
		{
			Verdict.Reason = TEXT("rejected: Stage B did not mark KEEP");
		}
		else if (!FMath::IsFinite(Verdict.FacingSqDouble))
		{
			Verdict.Reason = TEXT("rejected: facing not finite");
		}
		else if (Verdict.FacingSqDouble < 1e-24)
		{
			Verdict.Reason = TEXT("rejected: degenerate area");
		}
		else
		{
			Verdict.bAccepted = true;
			++AcceptedByCpuPredicate;
		}
	}

	// If this fails the transcription above has drifted from the real CPU loop and nothing below
	// can be trusted — say so instead of reporting confident nonsense.
	Test.TestEqual(*FString::Printf(
		TEXT("[%s] the diagnostic's transcription of the CPU predicate reproduces the CPU rebuild's count"), CaseName),
		AcceptedByCpuPredicate, CpuTriangleCount);

	// How many GPU output triangles carry each corner set.
	TMap<FString, int32> GpuTrianglesPerKey;
	for (const FCSMeshBooleanParityTriangle& Triangle : GpuTriangles)
	{
		++GpuTrianglesPerKey.FindOrAdd(
			CSMeshBooleanParity_CornerSetKey(Triangle.Positions[0], Triangle.Positions[1], Triangle.Positions[2]));
	}

	int32 Untraceable = 0;
	int32 Duplicated = 0;
	int32 Disagreements = 0;
	constexpr int32 MaxDumped = 8;
	for (const TPair<FString, int32>& GpuKey : GpuTrianglesPerKey)
	{
		const TArray<int32>* Fragments = KeyToFragments.Find(GpuKey.Key);
		if (!Fragments) { Untraceable += GpuKey.Value; continue; }

		TArray<int32> RejectedFragments;
		int32 AcceptedHere = 0;
		for (int32 Fragment : *Fragments)
		{
			if (Verdicts[Fragment].bAccepted) ++AcceptedHere;
			else RejectedFragments.Add(Fragment);
		}

		int32 Excess = GpuKey.Value - AcceptedHere;
		if (Excess <= 0) continue;

		// The excess is explained by CPU-rejected fragments carrying the same corner set; anything
		// left over after those are accounted for is the GPU emitting one fragment more than once,
		// which is a different bug entirely.
		for (int32 Fragment : RejectedFragments)
		{
			if (Excess <= 0) break;
			--Excess;
			++Disagreements;
			if (Disagreements > MaxDumped) continue;
			const FFragmentVerdict& Verdict = Verdicts[Fragment];
			Test.AddError(FString::Printf(
				TEXT("[%s] predicate disagreement on fragment %d: the GPU emitted it, the CPU %s.\n")
				TEXT("    encoded source word = 0x%08x (source triangle %d, KEEP=%d, of %u source triangles, StageB=%d)\n")
				TEXT("    P0 = (%.9g, %.9g, %.9g)\n")
				TEXT("    P1 = (%.9g, %.9g, %.9g)\n")
				TEXT("    P2 = (%.9g, %.9g, %.9g)\n")
				TEXT("    edge lengths = %.9g / %.9g\n")
				TEXT("    facingSq: CPU double = %.17g | GPU-compensated (C++ transcription) = %.17g | plain float32 = %.17g | threshold = 1e-24"),
				CaseName, Fragment, Verdict.Reason,
				Encoded[Fragment], Verdict.Source, (Encoded[Fragment] & SourceKeep) != 0u ? 1 : 0,
				Capture.SourceTriangleCount, Capture.bStageB ? 1 : 0,
				Verdict.Corners[0].X, Verdict.Corners[0].Y, Verdict.Corners[0].Z,
				Verdict.Corners[1].X, Verdict.Corners[1].Y, Verdict.Corners[1].Z,
				Verdict.Corners[2].X, Verdict.Corners[2].Y, Verdict.Corners[2].Z,
				(Verdict.Corners[1] - Verdict.Corners[0]).Size(), (Verdict.Corners[2] - Verdict.Corners[0]).Size(),
				Verdict.FacingSqDouble, Verdict.FacingSqCompensated, Verdict.FacingSqPlainFloat));
		}
		// Whatever the rejected fragments could not account for is the GPU emitting one fragment
		// more than once — the same triangle-count gap, a completely different cause.
		Duplicated += Excess;
	}

	Test.AddInfo(FString::Printf(
		TEXT("[%s] predicate diagnostic: %u fragments captured; CPU predicate accepts %d, GPU emitted %d. Of the GPU's output, %d trace back to a CPU-REJECTED fragment, %d to NO fragment, %d are DUPLICATE emissions."),
		CaseName, FragmentCount, AcceptedByCpuPredicate, GpuTriangles.Num(), Disagreements, Untraceable, Duplicated));
	if (Untraceable > 0)
	{
		Test.AddError(FString::Printf(
			TEXT("[%s] %d GPU output triangles correspond to no captured fragment — the divergence is NOT the accept predicate."),
			CaseName, Untraceable));
	}
	if (Duplicated > 0)
	{
		Test.AddError(FString::Printf(
			TEXT("[%s] %d GPU output triangles are duplicate emissions of an accepted fragment — the divergence is NOT the accept predicate."),
			CaseName, Duplicated));
	}
}

/** Flattens a finished UCSMesh into per-triangle records. */
bool CSMeshBooleanParity_Collect(UCSMesh* Mesh, TArray<FCSMeshBooleanParityTriangle>& OutTriangles, FString& OutError)
{
	OutTriangles.Reset();
	if (!Mesh) { OutError = TEXT("null mesh"); return false; }

	FCSGpuMeshCPUData Data;
	if (!Mesh->ReadbackMeshSync(Data)) { OutError = TEXT("readback failed"); return false; }

	TArray<uint32> PackedTangents;
	if (!CSMeshBooleanParity_ReadStreamUints(Mesh, ECSGpuStreamRole::TangentBasis, 0, PackedTangents))
	{
		OutError = TEXT("tangent stream readback failed");
		return false;
	}

	const int32 TriangleCount = Data.Indices.Num() / 3;
	if (TriangleCount <= 0) { OutError = TEXT("no triangles"); return false; }

	// Both paths land as a per-corner soup with an identity index buffer (the CPU one because
	// CopyFromMeshSnapshot expands a per-corner snapshot, the GPU one because the emit kernel
	// writes one vertex per corner). Anything else means the layout contract moved.
	for (int32 Corner = 0; Corner < Data.Indices.Num(); ++Corner)
	{
		if (Data.Indices[Corner] == uint32(Corner)) continue;
		OutError = FString::Printf(TEXT("index buffer is not an identity soup at corner %d (=%u)"), Corner, Data.Indices[Corner]);
		return false;
	}

	OutTriangles.SetNum(TriangleCount);
	for (int32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
	{
		FCSMeshBooleanParityTriangle& Out = OutTriangles[Triangle];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const int32 Vertex = Triangle * 3 + Corner;
			if (Data.Positions.IsValidIndex(Vertex)) Out.Positions[Corner] = Data.Positions[Vertex];
			if (Data.TexCoords().IsValidIndex(Vertex)) Out.TexCoords[Corner] = Data.TexCoords()[Vertex];
			if (Data.Normals.IsValidIndex(Vertex)) Out.Normals[Corner] = Data.Normals[Vertex];
			if (Data.Tangents.IsValidIndex(Vertex)) Out.Tangents[Corner] = Data.Tangents[Vertex];
			if (Data.Colors.IsValidIndex(Vertex)) Out.Colors[Corner] = Data.Colors[Vertex];
			// The sign rides in the high byte of the packed normal, as a signed 8-bit SNORM.
			const int32 PackedIndex = Vertex * 2 + 1;
			if (PackedTangents.IsValidIndex(PackedIndex))
			{
				const int8 SignByte = int8((PackedTangents[PackedIndex] >> 24) & 0xffu);
				Out.BinormalSigns[Corner] = SignByte < 0 ? -1.0f : 1.0f;
			}
		}
		// Compare the material each triangle RESOLVES to, not its slot index: the two paths
		// number their slots differently on purpose (the CPU one deduplicates in first-use
		// order, the GPU one lays the extraction registry out flat plus one empty slot).
		const int32 Slot = Data.TriangleMaterialSlots.IsValidIndex(Triangle) ? Data.TriangleMaterialSlots[Triangle] : 0;
		Out.Material = Mesh->Materials.IsValidIndex(Slot) ? Mesh->Materials[Slot].Get() : nullptr;
	}
	return true;
}

/** Largest corner-to-corner distance between two triangles, corner order preserved. */
float CSMeshBooleanParity_CornerDistance(const FCSMeshBooleanParityTriangle& A, const FCSMeshBooleanParityTriangle& B)
{
	float Worst = 0.0f;
	for (int32 Corner = 0; Corner < 3; ++Corner)
		Worst = FMath::Max(Worst, (A.Positions[Corner] - B.Positions[Corner]).Size());
	return Worst;
}

/** The measured worst case of every comparison, so a failure reports numbers rather than a verdict. */
struct FCSMeshBooleanParityReport
{
	int32 Matched = 0;
	int32 Unmatched = 0;
	float WorstPosition = 0.0f;
	float WorstTexCoord = 0.0f;
	float WorstNormal = 0.0f;
	float WorstTangent = 0.0f;
	float WorstColor = 0.0f;
	int32 BinormalSignMismatches = 0;
	int32 MaterialMismatches = 0;
};

void CSMeshBooleanParity_Compare(
	const TArray<FCSMeshBooleanParityTriangle>& Cpu,
	const TArray<FCSMeshBooleanParityTriangle>& Gpu,
	FCSMeshBooleanParityReport& Report)
{
	TArray<bool> Claimed;
	Claimed.Init(false, Gpu.Num());

	for (const FCSMeshBooleanParityTriangle& CpuTriangle : Cpu)
	{
		int32 Best = INDEX_NONE;
		float BestDistance = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < Gpu.Num(); ++Index)
		{
			if (Claimed[Index]) continue;
			const float Distance = CSMeshBooleanParity_CornerDistance(CpuTriangle, Gpu[Index]);
			if (Distance < BestDistance) { BestDistance = Distance; Best = Index; }
		}
		if (Best == INDEX_NONE || BestDistance > CSMeshBooleanParity_Tolerance::Position)
		{
			++Report.Unmatched;
			Report.WorstPosition = FMath::Max(Report.WorstPosition, BestDistance == TNumericLimits<float>::Max() ? 0.0f : BestDistance);
			continue;
		}

		Claimed[Best] = true;
		++Report.Matched;
		const FCSMeshBooleanParityTriangle& GpuTriangle = Gpu[Best];
		Report.WorstPosition = FMath::Max(Report.WorstPosition, BestDistance);
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Report.WorstTexCoord = FMath::Max(Report.WorstTexCoord,
				(CpuTriangle.TexCoords[Corner] - GpuTriangle.TexCoords[Corner]).GetAbsMax());
			Report.WorstNormal = FMath::Max(Report.WorstNormal,
				(CpuTriangle.Normals[Corner] - GpuTriangle.Normals[Corner]).GetAbsMax());
			Report.WorstTangent = FMath::Max(Report.WorstTangent,
				(CpuTriangle.Tangents[Corner] - GpuTriangle.Tangents[Corner]).GetAbsMax());
			const FVector4f ColorDelta = CpuTriangle.Colors[Corner] - GpuTriangle.Colors[Corner];
			Report.WorstColor = FMath::Max(Report.WorstColor, FMath::Max(
				FMath::Max(FMath::Abs(ColorDelta.X), FMath::Abs(ColorDelta.Y)),
				FMath::Max(FMath::Abs(ColorDelta.Z), FMath::Abs(ColorDelta.W))));
			if (CpuTriangle.BinormalSigns[Corner] != GpuTriangle.BinormalSigns[Corner]) ++Report.BinormalSignMismatches;
		}
		if (CpuTriangle.Material != GpuTriangle.Material) ++Report.MaterialMismatches;
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSMeshBooleanGpuParityTest,
	"PCGPlugins.ComputeShaderGenerator.MeshBoolean.GpuParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSMeshBooleanGpuParityTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh"), CubeMesh)) return false;

	// Two interpenetrating cubes, offset on two axes so the intersection is a real polygon on
	// several faces rather than a single shared plane: that is what makes the arrangement
	// produce sub-triangles whose corners are strictly INSIDE their source triangle, which is
	// the only case where the barycentric rebuild does any work at all. Two disjoint boxes
	// would pass this test with the interpolation deleted.
	AStaticMeshActor* FirstCube = World->SpawnActor<AStaticMeshActor>();
	AStaticMeshActor* SecondCube = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("First cube actor"), FirstCube)) return false;
	if (!TestNotNull(TEXT("Second cube actor"), SecondCube)) return false;
	FirstCube->SetActorTransform(FTransform(FVector(0.0, 0.0, 0.0)));
	FirstCube->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
	SecondCube->SetActorTransform(FTransform(FRotator(0.0, 20.0, 0.0), FVector(60.0, 45.0, 30.0)));
	SecondCube->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);

	AComputeShaderMeshBoolean* Generator = World->SpawnActor<AComputeShaderMeshBoolean>();
	if (!TestNotNull(TEXT("Mesh boolean generator"), Generator)) return false;
	Generator->SetActorTransform(FTransform(FVector(30.0, 22.5, 15.0)));
	Generator->GeneratorBounds->SetBoxExtent(FVector(300.0));
	// Bound every buffer the pipeline sizes from this: the default authoring ceiling is 20M,
	// which is a lot of VRAM to allocate for 24 triangles.
	Generator->MaxTriangles = 4096;
	Generator->bReadLandscape = false;
	// The GPU path does not implement welding (duplicate-triangle removal needs a global hash
	// table); leaving it off is what keeps both paths on the same algorithm here.
	Generator->VertexWeldDistance = 0.0f;
	World->UpdateWorldComponents(true, false);
	FlushRenderingCommands();

	const FCSMeshBooleanOptions Options = Generator->MakeBooleanOptions();
	if (!TestTrue(TEXT("Welding is off, so both paths run the same algorithm"), Options.VertexWeldDistance <= 0.0f)) return false;

	// Both classification regimes: ArrangementOnly never runs Stage B (every fragment survives,
	// the MB_SRC_KEEP filter is disabled on both sides), Union does. The keep filter is shared
	// code on the GPU side and a separate branch on the CPU side, so it needs exercising both ways.
	// MinTriangles is a premise check, not a result check: ArrangementOnly deletes nothing, so
	// two interpenetrating cubes MUST come out as more than their 24 source triangles — if they
	// do not, nothing was cut and every tolerance below would pass on a rebuild that merely
	// copied corner attributes across. Union deletes faces, so only "produced something" is safe
	// to assert there.
	struct FCase { ECSMeshBooleanOp Op; const TCHAR* Name; int32 MinTriangles; };
	const FCase Cases[] = {
		{ ECSMeshBooleanOp::ArrangementOnly, TEXT("ArrangementOnly"), 25 },
		{ ECSMeshBooleanOp::Union, TEXT("Union"), 1 },
	};

	for (const FCase& Case : Cases)
	{
		// --- ONE pipeline run. The capture is the shared input both rebuilds consume; see the
		//     file header for why a second run would invalidate the whole comparison.
		FCSMeshBooleanCapture Capture;
		FCSGpuMeshCPUData Snapshot;
		TArray<UMaterialInterface*> SnapshotMaterials;
		if (!TestTrue(*FString::Printf(TEXT("[%s] the Boolean pipeline produced a result"), Case.Name),
			Generator->RunBooleanToSnapshot(Case.Op, Options, Snapshot, SnapshotMaterials, &Capture)))
		{
			return false;
		}
		if (!TestTrue(*FString::Printf(TEXT("[%s] the run handed back a usable capture"), Case.Name),
			Capture.IsValid()))
		{
			return false;
		}

		// --- reference half: the CPU rebuild's output, uploaded exactly as ApplyMeshBoolean used
		//     to. RunBooleanToSnapshot already produced this from the captured fragments.
		UCSMesh* CpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		if (!TestNotNull(TEXT("CPU-reference mesh object"), CpuMesh)) return false;
		CpuMesh->Materials.Reset(SnapshotMaterials.Num());
		for (UMaterialInterface* Material : SnapshotMaterials) CpuMesh->Materials.Add(Material);
		if (!TestTrue(*FString::Printf(TEXT("[%s] snapshot upload"), Case.Name),
			UCSMeshOps::CopyFromMeshSnapshot(CpuMesh, Snapshot)))
		{
			return false;
		}

		// --- the half being tested: the GPU rebuild, on those same fragments
		UCSMesh* GpuMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		if (!TestNotNull(TEXT("GPU mesh object"), GpuMesh)) return false;
		if (!TestTrue(*FString::Printf(TEXT("[%s] GPU rebuild produced a result"), Case.Name),
			Generator->RebuildGpuMeshFromCapture(Capture, GpuMesh)))
		{
			return false;
		}

		TArray<FCSMeshBooleanParityTriangle> GpuTriangles;
		TArray<FCSMeshBooleanParityTriangle> CpuTriangles;
		FString GpuError;
		FString CpuError;
		const bool bGpuCollected = CSMeshBooleanParity_Collect(GpuMesh, GpuTriangles, GpuError);
		const bool bCpuCollected = CSMeshBooleanParity_Collect(CpuMesh, CpuTriangles, CpuError);
		if (!TestTrue(*FString::Printf(TEXT("[%s] GPU result collected (%s)"), Case.Name, *GpuError), bGpuCollected)) return false;
		if (!TestTrue(*FString::Printf(TEXT("[%s] CPU result collected (%s)"), Case.Name, *CpuError), bCpuCollected)) return false;

		TestTrue(*FString::Printf(TEXT("[%s] the arrangement produced at least %d triangles (got %d)"),
			Case.Name, Case.MinTriangles, GpuTriangles.Num()), GpuTriangles.Num() >= Case.MinTriangles);

		// Both rebuilds saw the same fragments, so this is now a statement about the ACCEPT
		// PREDICATE alone — the source-range test, the Stage B keep filter and the degenerate-area
		// rejection — with the attribute interpolation not yet involved. A mismatch here localises
		// to the predicate; a mismatch below localises to the interpolation.
		TestEqual(*FString::Printf(TEXT("[%s] both rebuilds accept the same number of fragments"), Case.Name),
			GpuTriangles.Num(), CpuTriangles.Num());
		// The GPU counting kernel runs the predicate a second time to size the resident
		// allocation. If it disagreed with the emit kernel the mesh would be written past — or
		// short of — its capacity, so it has to agree with the CPU count too.
		TestEqual(*FString::Printf(TEXT("[%s] the GPU accept-count kernel agrees with the CPU rebuild"), Case.Name),
			int32(Capture.OutputTriangleCount), CpuTriangles.Num());

		// Only the predicate can move these counts, so when they differ, say exactly which
		// fragments and which term — rather than leaving the next reader to infer it. Runs last
		// because it reads the captured buffers back, and nothing consumes them afterwards.
		if (GpuTriangles.Num() != CpuTriangles.Num())
		{
			CSMeshBooleanParity_DiagnosePredicate(*this, Case.Name, Capture, GpuTriangles, CpuTriangles.Num());
		}

		FCSMeshBooleanParityReport Report;
		CSMeshBooleanParity_Compare(CpuTriangles, GpuTriangles, Report);

		AddInfo(FString::Printf(
			TEXT("[%s] parity over %d triangles: matched=%d unmatched=%d | worst position=%.3e cm (tol %.1e), UV=%.3e (tol %.1e), normal=%.3e (tol %.1e), tangent=%.3e (tol %.1e), colour=%.3e (tol %.1e) | binormal-sign mismatches=%d, material mismatches=%d"),
			Case.Name, CpuTriangles.Num(), Report.Matched, Report.Unmatched,
			Report.WorstPosition, CSMeshBooleanParity_Tolerance::Position,
			Report.WorstTexCoord, CSMeshBooleanParity_Tolerance::TexCoord,
			Report.WorstNormal, CSMeshBooleanParity_Tolerance::TangentBasis,
			Report.WorstTangent, CSMeshBooleanParity_Tolerance::TangentBasis,
			Report.WorstColor, CSMeshBooleanParity_Tolerance::Color,
			Report.BinormalSignMismatches, Report.MaterialMismatches));

		TestEqual(*FString::Printf(TEXT("[%s] every CPU triangle has a GPU counterpart"), Case.Name), Report.Unmatched, 0);
		TestTrue(*FString::Printf(TEXT("[%s] positions agree (worst %.3e cm > %.1e)"),
			Case.Name, Report.WorstPosition, CSMeshBooleanParity_Tolerance::Position),
			Report.WorstPosition <= CSMeshBooleanParity_Tolerance::Position);
		TestTrue(*FString::Printf(TEXT("[%s] UV0 agrees (worst %.3e > %.1e)"),
			Case.Name, Report.WorstTexCoord, CSMeshBooleanParity_Tolerance::TexCoord),
			Report.WorstTexCoord <= CSMeshBooleanParity_Tolerance::TexCoord);
		TestTrue(*FString::Printf(TEXT("[%s] normals agree (worst %.3e > %.1e)"),
			Case.Name, Report.WorstNormal, CSMeshBooleanParity_Tolerance::TangentBasis),
			Report.WorstNormal <= CSMeshBooleanParity_Tolerance::TangentBasis);
		TestTrue(*FString::Printf(TEXT("[%s] tangents agree (worst %.3e > %.1e)"),
			Case.Name, Report.WorstTangent, CSMeshBooleanParity_Tolerance::TangentBasis),
			Report.WorstTangent <= CSMeshBooleanParity_Tolerance::TangentBasis);
		TestTrue(*FString::Printf(TEXT("[%s] vertex colours agree (worst %.3e > %.1e)"),
			Case.Name, Report.WorstColor, CSMeshBooleanParity_Tolerance::Color),
			Report.WorstColor <= CSMeshBooleanParity_Tolerance::Color);
		TestEqual(*FString::Printf(TEXT("[%s] binormal signs agree"), Case.Name), Report.BinormalSignMismatches, 0);
		TestEqual(*FString::Printf(TEXT("[%s] every triangle resolves to the same material"), Case.Name),
			Report.MaterialMismatches, 0);

		// The material-id stream must be populated deliberately: the buffer comes from a pool, so
		// an unwritten id is the previous tenant's bytes reinterpreted as a slot. Every id the
		// GPU path writes has to index the table it published — including the trailing empty slot
		// that CS_NO_MATERIAL_ID lands in.
		TestTrue(*FString::Printf(TEXT("[%s] the GPU path published a material table"), Case.Name),
			GpuMesh->Materials.Num() > 0);
		bool bAllSlotsInRange = true;
		{
			FCSGpuMeshCPUData GpuData;
			if (GpuMesh->ReadbackMeshSync(GpuData))
			{
				for (int32 Slot : GpuData.TriangleMaterialSlots)
					bAllSlotsInRange &= GpuMesh->Materials.IsValidIndex(Slot);
			}
			else bAllSlotsInRange = false;
		}
		TestTrue(*FString::Printf(TEXT("[%s] every per-triangle material id indexes the published table"), Case.Name),
			bAllSlotsInRange);

		// Bounds come from an explicit GPU reduction on this path; a bound the size of the
		// 600 cm query box would mean the reduction never ran.
		const FBox GpuBounds = GpuMesh->GetWorldBoundsApprox();
		TestTrue(*FString::Printf(TEXT("[%s] the GPU path published tight world bounds (size %s)"),
			Case.Name, *GpuBounds.GetSize().ToString()),
			GpuBounds.IsValid && GpuBounds.GetSize().GetMax() < 400.0);
	}

	// The end-to-end entry point, as a smoke check only. It builds the capture itself, and a field
	// it forgot to fill would otherwise reach production untested — but its output cannot be
	// compared against anything here, because it is a SECOND pipeline run and would come back with
	// a different tessellation. So: it ran, it produced geometry, it published a material table.
	{
		UCSMesh* EndToEndMesh = UCSMeshOps::AllocateGpuMesh(World, 3, 3);
		if (TestNotNull(TEXT("End-to-end mesh object"), EndToEndMesh))
		{
			TestTrue(TEXT("RunBooleanToGpuMesh builds its own capture and drives the rebuild"),
				Generator->RunBooleanToGpuMesh(ECSMeshBooleanOp::Union, Options, EndToEndMesh));
			TestTrue(TEXT("End-to-end run produced geometry"), EndToEndMesh->GetTriangleCountSync() > 0);
			TestTrue(TEXT("End-to-end run published a material table"), EndToEndMesh->Materials.Num() > 0);
		}
	}

	FirstCube->Destroy();
	SecondCube->Destroy();
	Generator->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
