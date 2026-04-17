#pragma once

#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

#ifndef NUM_THREADS_PER_GROUP_DIMENSION_Z
#define NUM_THREADS_PER_GROUP_DIMENSION_Z 1
#endif

/**
 * Reusable compact-tile sparse dispatch helper.
 *
 * Provides:
 *   - FCompactTileBuffers: buffer handles for tile compact dispatch
 *   - CreateCompactTileBuffers(): allocate RDG buffers
 *   - CreateFullScreenCompactTileBuffers(): allocate + fill with all tiles (no sparse)
 *   - FSparseTileDispatchCS: standalone shader for FinalizeCompact pass
 *   - AddFinalizeCompactPass(): schedule the finalize pass
 *
 * The CompactActiveTiles pass itself should be implemented in the domain shader
 * because the activity check is domain-specific.
 */

// ─────────────── Buffer handle struct ───────────────

struct FCompactTileBuffers
{
	FRDGBufferRef TileCoordsBuffer = nullptr;
	FRDGBufferUAVRef TileCoordsUAV = nullptr;
	FRDGBufferSRVRef TileCoordsSRV = nullptr;

	FRDGBufferRef CounterBuffer = nullptr;
	FRDGBufferUAVRef CounterUAV = nullptr;
	FRDGBufferSRVRef CounterSRV = nullptr;

	FRDGBufferRef IndirectArgsBuffer = nullptr;
	FRDGBufferUAVRef IndirectArgsUAV = nullptr;

	uint32 MaxTileCount = 0;
};

// ─────────────── Buffer creation ───────────────

inline FCompactTileBuffers CreateCompactTileBuffers(
	FRDGBuilder& GraphBuilder,
	uint32 TextureWidth,
	uint32 TextureHeight,
	uint32 TileSizeX,
	uint32 TileSizeY)
{
	FCompactTileBuffers Out;

	const uint32 TilesX = FMath::Max((TextureWidth + TileSizeX - 1) / TileSizeX, 1u);
	const uint32 TilesY = FMath::Max((TextureHeight + TileSizeY - 1) / TileSizeY, 1u);
	Out.MaxTileCount = TilesX * TilesY;

	Out.TileCoordsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Out.MaxTileCount),
		TEXT("STD.CompactTileCoords"));
	Out.TileCoordsUAV = GraphBuilder.CreateUAV(Out.TileCoordsBuffer, PF_R32_UINT);
	Out.TileCoordsSRV = GraphBuilder.CreateSRV(Out.TileCoordsBuffer, PF_R32_UINT);

	Out.CounterBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
		TEXT("STD.CompactCounter"));
	Out.CounterUAV = GraphBuilder.CreateUAV(Out.CounterBuffer, PF_R32_UINT);
	Out.CounterSRV = GraphBuilder.CreateSRV(Out.CounterBuffer, PF_R32_UINT);

	Out.IndirectArgsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1),
		TEXT("STD.CompactIndirectArgs"));
	Out.IndirectArgsUAV = GraphBuilder.CreateUAV(Out.IndirectArgsBuffer, PF_R32_UINT);

	return Out;
}

inline void ResetCompactCounter(FRDGBuilder& GraphBuilder, const FCompactTileBuffers& Buffers)
{
	static const uint32 Zero = 0;
	GraphBuilder.QueueBufferUpload(Buffers.CounterBuffer, &Zero, sizeof(Zero));
}

/**
 * Create compact tile buffers pre-filled with ALL tiles (full-screen coverage).
 * Used when you need compact-tile-compatible dispatch but want to process every tile.
 */
inline FCompactTileBuffers CreateFullScreenCompactTileBuffers(
	FRDGBuilder& GraphBuilder,
	uint32 TextureWidth,
	uint32 TextureHeight,
	uint32 TileSizeX,
	uint32 TileSizeY)
{
	FCompactTileBuffers Out = CreateCompactTileBuffers(GraphBuilder, TextureWidth, TextureHeight, TileSizeX, TileSizeY);

	const uint32 TilesX = FMath::Max((TextureWidth + TileSizeX - 1) / TileSizeX, 1u);
	const uint32 TilesY = FMath::Max((TextureHeight + TileSizeY - 1) / TileSizeY, 1u);
	const uint32 TotalTiles = TilesX * TilesY;

	TArray<uint32> AllTileCoords;
	AllTileCoords.SetNum(TotalTiles);
	for (uint32 ty = 0; ty < TilesY; ty++)
	{
		for (uint32 tx = 0; tx < TilesX; tx++)
		{
			AllTileCoords[ty * TilesX + tx] = (ty << 16) | tx;
		}
	}
	GraphBuilder.QueueBufferUpload(Out.TileCoordsBuffer, AllTileCoords.GetData(), TotalTiles * sizeof(uint32));
	GraphBuilder.QueueBufferUpload(Out.CounterBuffer, &TotalTiles, sizeof(TotalTiles));

	const FRHIDispatchIndirectParameters FullArgs = { TotalTiles, 1u, 1u };
	GraphBuilder.QueueBufferUpload(Out.IndirectArgsBuffer, &FullArgs, sizeof(FullArgs));

	return Out;
}

// ─────────────── Parameter binding helpers ───────────────

/**
 * Bind compact tile buffers to shader parameters (UAV + SRV + IndirectArgs).
 * Expects the parameter struct to have:
 *   RWB_CompactTileCoords, RWB_CompactCounter, RWB_CompactIndirectArgs (UAV)
 *   B_CompactTileCoords, B_CompactCounter (SRV)
 *   CompactIndirectArgs (RDG_BUFFER_ACCESS)
 */
template<typename FParameterStruct>
void BindCompactTileBuffers(FParameterStruct* Params, const FCompactTileBuffers& Buffers)
{
	Params->RWB_CompactTileCoords = Buffers.TileCoordsUAV;
	Params->RWB_CompactCounter = Buffers.CounterUAV;
	Params->RWB_CompactIndirectArgs = Buffers.IndirectArgsUAV;
	Params->B_CompactTileCoords = Buffers.TileCoordsSRV;
	Params->B_CompactCounter = Buffers.CounterSRV;
	Params->CompactIndirectArgs = Buffers.IndirectArgsBuffer;
}

/**
 * Null out compact UAVs for passes that only READ from compact buffers (sim passes).
 * Prevents RDG UAV+SRV conflict on the same resource.
 */
template<typename FParameterStruct>
void NullifyCompactTileUAVs(FParameterStruct* Params)
{
	Params->RWB_CompactTileCoords = nullptr;
	Params->RWB_CompactCounter = nullptr;
	Params->RWB_CompactIndirectArgs = nullptr;
}

/**
 * Null out ALL compact tile bindings (for passes that don't use compact tiles at all).
 */
template<typename FParameterStruct>
void NullifyAllCompactTileBindings(FParameterStruct* Params)
{
	Params->RWB_CompactTileCoords = nullptr;
	Params->RWB_CompactCounter = nullptr;
	Params->RWB_CompactIndirectArgs = nullptr;
	Params->B_CompactTileCoords = nullptr;
	Params->B_CompactCounter = nullptr;
	Params->CompactIndirectArgs = nullptr;
}
