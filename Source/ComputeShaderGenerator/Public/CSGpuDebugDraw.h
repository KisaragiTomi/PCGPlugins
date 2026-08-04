#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshTypes.h"
#include "LocalVertexFactory.h"

class FMeshElementCollector;
class FPrimitiveSceneProxy;
class FSceneView;
class UMaterialInterface;

/**
 * A debug position stream: three floats per vertex (matching the standard mesh Position
 * stream) plus the typed SRV the local vertex factory needs.
 *
 * The SRV is not optional. On every platform with manual vertex fetch (SM5+), the shader
 * reads positions through FLocalVertexFactoryUniformShaderParameters::VertexFetch_PositionBuffer
 * as Buffer<float> indexed at VertexId*3 — the vertex stream is not used at all, and a null
 * SRV trips the uniform-buffer resource validation outright.
 */
struct FCSGpuDebugPositionStream
{
	FCSPooledVertexBuffer Buffer;
	FShaderResourceViewRHIRef SRV;
};

/**
 * A retained surface-voxel set as RDG handles: one position and one normal per voxel, plus a
 * GPU-written counter whose [0] is the valid voxel count. Capacity is what the buffers hold;
 * MaxItems clamps how many are drawn. The count is never read back — it reaches the draw
 * through AddIndirectArgsPass.
 */
struct FCSGpuDebugVoxelSource
{
	FRDGBufferRef Positions = nullptr; // Buffer<float4>
	FRDGBufferRef Normals = nullptr;   // Buffer<float4>
	FRDGBufferRef Counter = nullptr;   // Buffer<uint>
	uint32 Capacity = 0;
	uint32 MaxItems = 0;

	bool IsValid() const { return Positions && Normals && Counter && Capacity > 0; }
};

/**
 * Shared statics for GPU-resident debug geometry.
 *
 * SCOPE — GPU-RESIDENT SOURCES ONLY. If the data you want to visualise already lives on the
 * CPU, use the engine's immediate-mode debug drawing instead: DrawDebugPoint / DrawDebugLine /
 * DrawDebugDirectionalArrow / DrawDebugBox. Those are one call, they honour line thickness and
 * point size, and the world's line batcher already batches them. Pushing CPU-side debug
 * geometry through this class buys nothing and throws those knobs away. What belongs here is
 * geometry whose vertices — and usually whose count — only ever exist in GPU memory, where
 * reading it back just to call DrawDebugLine would be the actual waste.
 *
 * Every such visual in this plugin (surface-voxel directions / isolated quads, vine center
 * lines, ...) is built the same way: a pooled position stream + one or more pooled index
 * buffers, a position-only FLocalVertexFactory over them, optional 5-uint indirect args
 * written by a compute pass, and a coloured one-frame material proxy submitted through
 * FCSGpuMeshSceneProxy::SubmitGpuBufferDraw. Nothing is ever mapped or read back.
 *
 * Only the compute pass that fills the buffers is per-site; everything else lives here so a
 * new debug visual is "allocate + bind + submit", not another copy of the resource plumbing.
 * All functions are render-thread only.
 */
class COMPUTESHADERGENERATOR_API FCSGpuDebugDraw
{
public:
	FCSGpuDebugDraw() = delete;

	/** Engine debug mesh material, falling back to the default surface material. */
	static UMaterialInterface* GetDebugMaterial();

	/** Allocate a pooled 3-floats-per-vertex position buffer (VertexBuffer usage), its
	 *  manual-fetch SRV, and init the wrapper. Compute passes write it as RWBuffer<float>. */
	static void AllocatePositionStream(FRHICommandListBase& RHICmdList, FCSGpuDebugPositionStream& OutStream,
		uint32 NumVertices, const TCHAR* DebugName);

	/** Allocate a pooled uint index buffer (IndexBuffer usage) and init its wrapper. */
	static void AllocateIndexBuffer(FRHICommandListBase& RHICmdList, FCSPooledIndexBuffer& OutBuffer,
		uint32 NumIndices, const TCHAR* DebugName);

	/** Allocate a 5-uint DrawIndexedIndirect args buffer. */
	static TRefCountPtr<FRDGPooledBuffer> AllocateIndirectArgs(const TCHAR* DebugName);

	/** Bind a position-only vertex factory over a debug position stream and init it. */
	static void BindPositionOnlyVertexFactory(FRHICommandListBase& RHICmdList, FLocalVertexFactory& VertexFactory,
		FCSGpuDebugPositionStream& PositionStream);

	/** Release a wrapper and drop its pooled reference. Call after the vertex factory is released. */
	static void ReleasePositionStream(FCSGpuDebugPositionStream& Stream);
	static void ReleaseIndexBuffer(FCSPooledIndexBuffer& Buffer);

	// -------------------------------------------------------------------------
	// Shapes — one entry per GPU debug primitive, the counterpart to the engine's
	// DrawDebugLine / DrawDebugPoint / DrawDebugBox family. Each records compute passes that
	// fill caller-owned buffers in the shared debug layout (float triples + uint indices), so
	// a proxy only has to allocate, call one of these, and submit. Render thread only.
	// -------------------------------------------------------------------------

	/** One line per voxel, from its centre along its normal, plus one point index per voxel
	 *  centre (drawn or not is the caller's choice). Sizes: 2 vertices / 2 line indices /
	 *  1 point index per voxel. */
	static void AddVoxelDirectionsPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
		const FCSGpuDebugVoxelSource& Source, float DirectionLength,
		FRDGBufferRef OutPositions, FRDGBufferRef OutLineIndices, FRDGBufferRef OutPointIndices);

	/** One normal-facing quad per voxel. Sizes: 4 vertices / 6 indices per voxel. */
	static void AddVoxelQuadsPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
		const FCSGpuDebugVoxelSource& Source, float VoxelSize, float QuadScale, float NormalOffsetScale,
		bool bReverseOrientation, FRDGBufferRef OutPositions, FRDGBufferRef OutIndices);

	/** One line per (start, end) vertex-index pair, for a producer that already knows its own
	 *  connectivity. Pairs are int4 with the endpoints in xy. Sizes: 2 indices per segment. */
	static void AddLineIndicesPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
		FRDGBufferRef IndexPairs, uint32 NumSegments, FRDGBufferRef OutIndices);

	/** Unpack a structured float4 position source into a debug position stream's float triples.
	 *  Lets a producer that already has float4 positions feed the shared debug layout. */
	static void AddPositionUnpackPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
		FRDGBufferRef SourceFloat4, uint32 NumVertices, FRDGBufferRef DestFloat3);

	/** Turn a GPU-written item counter into DrawIndexedIndirect args. IndicesPerItem is 2 for
	 *  the direction lines, 6 for the quads, 1 for the centre points. */
	static void AddIndirectArgsPass(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel,
		FRDGBufferRef Counter, uint32 Capacity, uint32 MaxItems, uint32 IndicesPerItem, FRDGBufferRef OutArgs);

	/** Allocate a one-frame coloured debug material proxy and submit the indexed draw for every
	 *  visible view. Pass IndirectArgsBuffer for a GPU-decided count (NumPrimitives is then
	 *  ignored); leave it null for a CPU-known count. Debug geometry never casts shadows. */
	static void SubmitColoredDraw(
		const FPrimitiveSceneProxy& SceneProxy,
		const TArray<const FSceneView*>& Views,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector,
		const FVertexFactory& VertexFactory,
		const FIndexBuffer& IndexBuffer,
		EPrimitiveType PrimitiveType,
		const FLinearColor& Color,
		uint32 NumPrimitives,
		uint32 MaxVertexIndex,
		FRHIBuffer* IndirectArgsBuffer = nullptr,
		uint32 IndirectArgsOffset = 0);
};
