#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

/**
 * GPU weld + repair of a Boolean result (Shaders/Private/MeshBooleanRepair.usf).
 *
 * Runs on the pipeline's fragment soup after Stage B, before the GPU output rebuild, and
 * turns the classified fragments into a mesh whose vertices are shared: a salted weld over
 * every fragment, hole restoration from the fragments Stage B deleted, duplicate removal,
 * slit-crack fills, and a nudge that gives zero-area triangles a real area. Only the output
 * policy lives here; the weld itself is the shared CSGpuTriangleUtilities facility.
 *
 * The CPU snapshot path keeps its own weld post-process (Stage 13/14) and does none of this,
 * so the two paths deliberately disagree whenever VertexWeldDistance > 0.
 */
namespace MeshBooleanRepair
{
	/** Statistics block layout; mirrors MB_REPAIR_STAT_* in MeshBooleanRepair.usf. */
	enum EStat : uint32
	{
		/** [0, 4) is the weld's own block, see CSGpuTriangleUtilities::EVertexWeldStat. */
		StatWeldOrphans = 0,
		StatWeldUnresolved,
		StatWeldChainLinks,
		StatWeldRepresentatives,
		StatValid,
		StatLive,
		StatCollapsed,
		StatRestored,
		StatDuplicate,
		StatOutputFragments,
		StatBoundary,
		StatSlitLoops,
		StatFills,
		StatFillOverflow,
		StatOpen,
		StatNudged,
		StatStillDegenerate,
		StatInverted,
		StatOutputTotal,
		StatCount
	};

	struct FSettings
	{
		float WeldDistance = 0.0f;
		FVector3f GridOrigin = FVector3f::ZeroVector;
		/** Stage B ran: fragments carry keep marks and restoration has something to restore. */
		bool bStageB = false;
		bool bRestoreHoles = true;
		int32 RestoreRounds = 16;
		float RestoreMaxBendRadians = 0.0f;
		/** Slit fills and the zero-area nudge. */
		bool bFillSlits = true;
		float SlitTolerance = 0.0f;
		float NudgeDistance = 0.0f;
	};

	/**
	 * The pipeline's Stage B fast-winding field, kept alive past the pipeline graph so that
	 * restoration can check a candidate the way Stage B classified it.
	 */
	struct FWindingField
	{
		/** LBVH nodes (2 float4 each) and order-1 multipoles (5 float4 each) over the source soup. */
		TRefCountPtr<FRDGPooledBuffer> Topology;
		TRefCountPtr<FRDGPooledBuffer> Multipoles;
		/** Triangles the field was built over (the soup capacity, padding included). */
		uint32 TriangleCount = 0;
		float BetaSq = 1.0f;
		/** Stage B's iso threshold on |winding|. */
		float Threshold = 0.5f;
		/** Distance of the two probes from a sample point, along the source normal. */
		float SampleOffset = 0.1f;
	};

	struct FInputs
	{
		/** Pipeline output: 3 float3 per fragment, and its encoded source (MB_SRC_* bits). */
		FRDGBufferRef FragmentSoup = nullptr;
		FRDGBufferRef FragmentSource = nullptr;
		/** Source triangle soup, 3 float4 per triangle. */
		FRDGBufferRef SourceVertices = nullptr;
		uint32 FragmentCount = 0;
		uint32 SourceTriangleCount = 0;
		/** Registered FWindingField buffers; restoration is skipped without them. */
		FRDGBufferRef WindingTopology = nullptr;
		FRDGBufferRef WindingMultipoles = nullptr;
		FWindingField Winding;
	};

	/** What the repaired emit reads, plus the statistics block (StatCount uint32). */
	struct FOutputs
	{
		FRDGBufferRef Representatives = nullptr;
		FRDGBufferRef Flags = nullptr;
		FRDGBufferRef Claims = nullptr;
		FRDGBufferRef LoopSlots = nullptr;
		FRDGBufferRef LoopOffsets = nullptr;
		FRDGBufferRef Fills = nullptr;
		FRDGBufferRef Stats = nullptr;
	};

	/** Fill-triangle capacity for a fragment count; the emit indexes fills below it. */
	uint32 FillCapacity(uint32 FragmentCount);

	FOutputs AddRepairPasses(FRDGBuilder& GraphBuilder, const FInputs& Inputs, const FSettings& Settings);
}
