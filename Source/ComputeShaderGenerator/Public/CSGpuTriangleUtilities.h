#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

/**
 * GPU triangle-soup algorithms shared by mesh-generator actors.
 *
 * These operations intentionally live in a stateless RDG utility instead of an actor:
 * their inputs and outputs are render-graph buffers, and none of them needs UObject or
 * scene state. Keeping that boundary prevents Boolean-specific lifetime and policy from
 * leaking into other generators while still allowing the base generator to expose small
 * protected convenience wrappers.
 */
namespace CSGpuTriangleUtilities
{
	/** GPU-resident linear BVH over a triangle soup (three float4 values per triangle). */
	struct FTriangleLBVH
	{
		/** Two float4 values per node: AABB.xyz plus child/triangle index in w. */
		FRDGBufferRef Nodes = nullptr;

		/** Morton-sorted source-triangle indices; leaf order maps through this buffer. */
		FRDGBufferRef Payload = nullptr;
	};

	/**
	 * Builds a Karras LBVH entirely inside the supplied render graph.
	 *
	 * LBVH construction is common spatial infrastructure: Boolean intersection, ray
	 * visibility, winding evaluation, and future proximity queries can all consume the
	 * same topology. It therefore must not depend on AComputeShaderMeshBoolean.
	 */
	COMPUTESHADERGENERATOR_API FTriangleLBVH AddTriangleLBVHBuildPasses(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef TriangleSoupSRV,
		int32 TriangleCount,
		int32 SortElementCount,
		const FVector3f& AabbMin,
		const FVector3f& InvExtent);

	/**
	 * Adds order-1 fast-winding multipole construction for an existing triangle LBVH.
	 *
	 * This function builds the geometric field only. Decisions such as the Boolean iso
	 * threshold and two-sided sample offset remain with the caller because they are
	 * classification policy, not winding infrastructure.
	 */
	COMPUTESHADERGENERATOR_API FRDGBufferRef AddFastWindingMultipolePasses(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef TriangleSoupSRV,
		const FTriangleLBVH& LBVH,
		int32 TriangleCount);

	/**
	 * Computes a representative corner for every output position within WeldDistance.
	 *
	 * Only spatial equivalence is produced here. Attribute merging, degenerate removal,
	 * duplicate-triangle removal, and source-winding restoration are output-policy
	 * decisions and intentionally stay in the mesh producer.
	 *
	 * TriangleFilter optionally restricts welding to triangles whose filter word has any
	 * TriangleFilterMask bit set, indexed by triangle. Producers that leave discarded
	 * triangles resident in the soup must supply it: each hash bucket keeps only its
	 * lowest corner index, so a discarded corner with a lower index would shadow the live
	 * merge partner and silently reduce welding. Passing nullptr welds every corner.
	 */
	COMPUTESHADERGENERATOR_API FRDGBufferRef AddVertexWeldPasses(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef OutputTriangleSoup,
		FRDGBufferRef OutputTriangleCounter,
		int32 OutputTriangleCapacity,
		int32 SourceTriangleCapacity,
		const FVector3f& GridOrigin,
		float WeldDistance,
		FRDGBufferSRVRef TriangleFilter = nullptr,
		uint32 TriangleFilterMask = 0u);
}
