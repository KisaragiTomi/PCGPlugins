#pragma once

#include "CoreMinimal.h"

class AActor;
class UMaterialInterface;
class UWorld;

/**
 * Collection of the scene geometry inside a world-space box.
 *
 * This is the scene-query counterpart of CSGpuTriangleUtilities, and it is stateless for the
 * same reason: nothing about "which triangles are in this box" belongs to a particular
 * generator actor, so requiring one only means a caller must spawn an actor to ask a question
 * about the world. Every value the collection used to read off AComputeShaderMeshGenerator —
 * the query box, the reference points and their filter distance, the excluded actor and tags,
 * the required tag, the source LOD, landscape inclusion and the triangle cap — is now an
 * explicit field of FCSBoxSceneCollectOptions. Keeping that boundary stops generator-specific
 * policy (which actor to skip, which LOD the voxel grid happens to want) from being an
 * invisible input to everyone else's extraction.
 *
 * Where it differs from CSGpuTriangleUtilities, honestly: this is not RDG work. It iterates
 * the world's UStaticMeshComponents, reads component material overrides, resolves render
 * resources, pulls Nanite source geometry out of editor MeshDescriptions and walks the
 * landscape height field on the CPU. All of that is UObject and scene state, so
 * CollectBoxSceneTriangles is game-thread only.
 *
 * The result is the hand-off across that boundary: FCSBoxScenePreparedData is an immutable
 * shared snapshot holding only ref-counted render resources and plain data, so it may be
 * captured by value into an ENQUEUE_RENDER_COMMAND lambda. The render thread turns it into
 * buffers via AComputeShaderMeshGenerator::AddPreparedBoxSceneTrianglesToRDG, which touches
 * no UObject.
 */

// game-thread 预备好的盒内场景三角形数据：static mesh 已 resolve 出渲染资源引用，
// landscape 已在 game thread 完成 CPU 提取。可安全捕获进 render 线程 lambda，再交给
// AddPreparedBoxSceneTrianglesToRDG 消费。内部用 PImpl 隐藏 .cpp-only 的 resolved 类型。
struct FCSBoxScenePreparedDataImpl;

struct COMPUTESHADERGENERATOR_API FCSBoxScenePreparedData
{
	TSharedPtr<FCSBoxScenePreparedDataImpl, ESPMode::ThreadSafe> Impl;

	bool IsValid() const { return Impl.IsValid(); }
	bool HasAnyTriangles() const;

	// 去重后的材质表：soup 材质 buffer 里的 id 索引进本表。CS_NO_MATERIAL_ID 表示无材质（如地形）。
	int32 GetMaterialRegistryNum() const;
	UMaterialInterface* GetMaterialByRegistryIndex(int32 Index) const;
};

/**
 * Everything the collection used to take from the generator actor, made explicit.
 *
 * The defaults are the neutral answer — "collect what is in the box" — deliberately not the
 * generator's own defaults. An actor-shaped caller overwrites them wholesale through
 * AComputeShaderMeshGenerator::MakeBoxSceneCollectOptions so its serialized properties keep
 * deciding; a caller that only has a world still gets a working extraction instead of one
 * silently governed by some other actor's settings.
 */
struct COMPUTESHADERGENERATOR_API FCSBoxSceneCollectOptions
{
	/** World-space box to collect from. Invalid collects nothing: there is no actor left to
	 *  fall back to for bounds, and guessing one would extract a box the caller never asked for. */
	FBox QueryBox = FBox(ForceInit);

	/** Triangle ceiling for the extraction, which also sizes the transient soup. Clamped to >= 1.
	 *  Matches FCSMeshBoxSceneOptions so the two entry points agree when neither is told otherwise. */
	int32 MaxTriangles = 500000;

	/** Filter geometry by distance to these points. Empty (or a filter distance of 0) keeps
	 *  everything in the box — the two are checked together, so points without a distance,
	 *  or a distance without points, both mean "no filtering". */
	TArray<FVector> ReferencePoints;

	float ReferenceFilterDistance = 0.0f;

	/** Only keep static meshes on actors carrying at least one of these tags. Empty keeps
	 *  everything; entries that are NAME_None never match.
	 *  Landscape is deliberately exempt: the tags select which props to extract, and dropping
	 *  the ground with them would leave the geometry floating. */
	TArray<FName> RequiredActorTags;

	/** Actor whose own meshes are skipped. A generator sitting inside its own query box would
	 *  otherwise extract the geometry it is about to replace. */
	const AActor* ExcludedActor = nullptr;

	/** Actors carrying any of these tags are skipped entirely. */
	TArray<FName> ExcludedActorTags;

	/** Static-mesh LOD to read. Clamped per mesh to what it actually has. */
	int32 LODIndex = 0;

	bool bIncludeLandscape = true;

	/** Pull Nanite meshes from their editor MeshDescription instead of the render fallback.
	 *  Off means a Nanite source contributes its coarse fallback mesh, which is usually not
	 *  what a geometry operation wants but is the only option outside the editor. */
	bool bUseMeshDescriptionSourceTriangles = true;

	/** true keeps one registry entry per source (mesh, material slot), so a mesh with five
	 *  slots yields five output slots even when they share a material or are all unassigned.
	 *  false dedupes by material pointer only, giving the most compact list but losing the
	 *  source slot layout - every empty slot merges into one. */
	bool bPreserveSourceMaterialSlots = true;
};

namespace CSBoxSceneCollection
{
	/**
	 * [game thread] Enumerates the static meshes and landscape inside Options.QueryBox, resolves
	 * static-mesh render resources and extracts landscape triangles on the CPU.
	 *
	 * Must run on the game thread — it touches UObjects and FLandscapeComponentDataInterface.
	 * Returns data that is safe to capture into a render-thread lambda; an invalid world or box
	 * yields a prepared block whose IsValid() is false rather than an assert, because "nothing
	 * in range" is a normal outcome for a box query.
	 */
	COMPUTESHADERGENERATOR_API FCSBoxScenePreparedData CollectBoxSceneTriangles(
		UWorld* World,
		const FCSBoxSceneCollectOptions& Options);
}
