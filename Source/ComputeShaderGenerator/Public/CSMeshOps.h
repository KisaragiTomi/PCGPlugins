#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderMeshBoolean.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSMeshOps.generated.h"

class AActor;
class AComputeShaderMeshGenerator;
class UCSMesh;
class UDynamicMesh;
class UMaterialInterface;
class UStaticMesh;
struct FCSGpuMeshCPUData;
struct FCSMeshEditContext;

// -----------------------------------------------------------------------------
// Operator library over UCSMesh — the GPU counterpart of the GeometryScript
// UGeometryScriptLibrary_* function libraries.
//
// Every operator takes the mesh it edits as the first parameter and returns it, so calls
// chain in Blueprint the way GeometryScript nodes do. All of them go through
// UCSMesh::EditMeshSync, which registers the resident streams, runs the operator's RDG
// passes, and restores each stream's access state — an operator can therefore not leave the
// mesh in a state where it silently stops drawing.
//
// Everything here is synchronous. That is the existing contract for this subsystem (a
// render flush is what makes it safe for the game thread to hand raw pointers across), not
// an oversight; async operators would need handle-based resources and are out of scope.
// -----------------------------------------------------------------------------

/** Source options for CopyFromStaticMesh. */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshFromStaticMeshOptions
{
	GENERATED_BODY()

	/** LOD to read. Clamped to what the mesh actually has. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	int32 LODIndex = 0;

	/** Local-to-world transform baked into the uploaded positions. The resident set is world
	 *  space by contract, so a source placed in the level passes its component transform. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	FTransform Transform = FTransform::Identity;

	/** Reverse triangle winding while uploading. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bFlipWinding = false;

	/** Replace UCSMesh::Materials with the source mesh's material slots. Off keeps the
	 *  existing table, in which case the per-triangle ids still index it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bCopyMaterials = true;
};

/** Sink options for CopyToStaticMesh. */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshToStaticMeshOptions
{
	GENERATED_BODY()

	/** Empty with bTransient=false means "AutoResult folder next to OwnerActor's level". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	FString AssetPath;

	/** Build a throwaway mesh instead of an asset (no MeshDescription commit). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bTransient = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bReplaceExisting = true;

	/** Write the package to disk instead of leaving it dirty for a manual save. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bSaveToDisk = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bEnableNanite = false;

	/** Space to bake the world-space GPU data into. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	FTransform TargetTransform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bBakeToLocalSpace = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bRecomputeNormals = false;
};

/** Sink options for CopyToDynamicMesh (the "sink B" of the previous plan). */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshToDynamicMeshOptions
{
	GENERATED_BODY()

	/** Swap triangle winding at the DynamicMesh boundary. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bReverseOrientation = false;

	/** Drop zero-area triangles instead of letting them into the topology. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bSkipDegenerateTriangles = true;

	/** Ignore the GPU tangent basis and recompute per-vertex normals from the geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bRecomputeNormals = false;

	/** Carry the per-triangle material ids into the DynamicMesh material-id attribute. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bTransferMaterialIDs = true;

	/** Carry UV0 and vertex colours into their overlays. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bTransferUVs = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bTransferColors = true;

	/** Bake the world-space GPU data into TargetTransform's local space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bBakeToLocalSpace = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	FTransform TargetTransform = FTransform::Identity;
};

/** Scene-extraction options for AppendBoxSceneTriangles. */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshBoxSceneOptions
{
	GENERATED_BODY()

	/** World-space box to collect geometry from. Required: the operator no longer takes a
	 *  generator, so there are no actor bounds left to fall back to and an invalid box collects
	 *  nothing. AComputeShaderMeshGenerator::GetGeneratorBoundsWorldBox() is what the old
	 *  default resolved to, for callers that want the previous behaviour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	FBox QueryBox = FBox(ForceInit);

	/** Triangle ceiling for the extraction, which also sizes the transient soup. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh", meta = (ClampMin = "1"))
	int32 MaxTriangles = 500000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bIncludeLandscape = true;

	/** Only keep static meshes on actors carrying this tag. NAME_None keeps everything.
	 *  Landscape is exempt — the tag picks props, and dropping the ground with them would
	 *  leave the extraction floating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	FName RequiredActorTag;

	/** Filter geometry by distance to these points. Used to be read off the generator; it is a
	 *  per-call decision, so it is stated here now. Empty keeps everything in the box. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	TArray<FVector> ReferencePoints;

	/** > 0 with ReferencePoints set filters triangles by distance. Either half missing means
	 *  no filtering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	float ReferenceFilterDistance = 0.0f;

	/** Actor whose own meshes are skipped. A generator sitting inside its own query box passes
	 *  itself here so it does not extract the geometry it is about to replace. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	TObjectPtr<AActor> ExcludedActor = nullptr;

	/** Actors carrying any of these tags are skipped entirely. Defaulted to the generator's own
	 *  "UA" so an extraction driven from Blueprint keeps ignoring the same authoring props. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	TArray<FName> ExcludedActorTags = { TEXT("UA") };

	/** Static-mesh LOD to read. Clamped per mesh to what it actually has. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh", meta = (ClampMin = "0"))
	int32 LODIndex = 0;

	/** Keep one output material slot per (mesh, source slot) instead of deduping by material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bPreserveSourceMaterialSlots = true;

	/** Finish with ComputeWorldBoundsSync instead of widening the bounds by the whole query box.
	 *  The extraction's output size is GPU-decided, so without this the mesh's bounds are the box
	 *  that was searched — which for a sparse box is mostly empty space, and culling and shadow
	 *  quality both pay for it. Costs one GPU stall per call, so a caller appending many boxes in
	 *  a row should turn it off and run the operator once at the end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	bool bComputeExactBounds = true;
};

UCLASS()
class COMPUTESHADERGENERATOR_API UCSMeshOps : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// -------------------------------------------------------------------------
	// Creation
	// -------------------------------------------------------------------------

	/** Creates an empty GPU mesh with the given capacity. Outer null parks it on the
	 *  transient package, matching how UDynamicMesh objects are usually created. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Create", meta = (DefaultToSelf = "Outer"))
	static UCSMesh* AllocateGpuMesh(UObject* Outer, int32 VertexCapacity = 3, int32 IndexCapacity = 3);

	// -------------------------------------------------------------------------
	// Copy in / out
	// -------------------------------------------------------------------------

	/** Uploads a StaticMesh LOD into Target, replacing its contents. Reads the mesh's own
	 *  render buffers on the GPU — no CPU round-trip and no MeshDescription. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Copy")
	static UPARAM(DisplayName = "Target") UCSMesh* CopyFromStaticMesh(
		UCSMesh* Target, UStaticMesh* Source, const FCSMeshFromStaticMeshOptions& Options);

	/** Reads Target back and builds a StaticMesh from it. One blocking readback. Editor-only
	 *  for the asset path; bTransient works in any configuration. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Copy", meta = (DefaultToSelf = "Outer"))
	static UStaticMesh* CopyToStaticMesh(
		UCSMesh* Target, UObject* Outer, AActor* OwnerActor, const FCSMeshToStaticMeshOptions& Options);

	/** Reads Target back into a UDynamicMesh, carrying normals, UV0, colours and per-triangle
	 *  material ids. Passing a null TargetMesh creates one. One blocking readback. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Copy", meta = (DefaultToSelf = "Outer"))
	static UDynamicMesh* CopyToDynamicMesh(
		UCSMesh* Target, UDynamicMesh* TargetMesh, UObject* Outer, const FCSMeshToDynamicMeshOptions& Options);

	// -------------------------------------------------------------------------
	// Scene extraction
	// -------------------------------------------------------------------------

	/** Extracts the scene's triangles inside a box straight into Target's resident streams.
	 *  The extraction writes the mesh object, so nothing goes through a scene proxy. Appends to
	 *  whatever Target already holds.
	 *
	 *  Takes a world context rather than a generator: collecting a box of scene geometry is a
	 *  question about the world (see CSBoxSceneCollection), and every value that used to come
	 *  off the actor — bounds, reference points, excluded actor and tags, LOD — is a field of
	 *  Options now. Any UObject in the level works as the context; a generator still does. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Scene", meta = (WorldContext = "WorldContextObject"))
	static UPARAM(DisplayName = "Target") UCSMesh* AppendBoxSceneTriangles(
		UCSMesh* Target, UObject* WorldContextObject, const FCSMeshBoxSceneOptions& Options);

	/**
	 * A generator actor's own extraction policy as an option struct: its bounds box, reference
	 * points, excluded actor and tags, LOD and triangle ceiling — everything AppendBoxSceneTriangles
	 * stopped reading off the actor when it became world-context-based.
	 *
	 * Exists because that policy is otherwise unreachable: GetGeneratorBoundsWorldBox and
	 * FCSMeshGeneratorVoxelGridSettings::LODIndex are C++-only, so a Blueprint could not rebuild
	 * the struct by hand however many Make-Struct pins it wired. It is also the single copy the
	 * actor's own submit path uses, so the two cannot drift.
	 *
	 * MaxTrianglesOverride <= 0 keeps the actor's MaxTriangles. A null generator gives back the
	 * default struct, whose invalid QueryBox makes the extraction a no-op.
	 */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Scene")
	static FCSMeshBoxSceneOptions MakeGeneratorBoxSceneOptions(
		AComputeShaderMeshGenerator* Generator, float ReferenceFilterDistance = 200.0f, int32 MaxTrianglesOverride = 0);

	// -------------------------------------------------------------------------
	// Boolean / arrangement
	// -------------------------------------------------------------------------

	/**
	 * Runs the GPU arrangement + Boolean classification over the generator's box and puts the
	 * result into Target, replacing its contents.
	 *
	 * Where this differs from AComputeShaderMeshBoolean::BooleanBoxScene: that entry point is
	 * a one-shot that ends in a StaticMesh, so anything downstream (weld, transform, display,
	 * a second Boolean) meant exporting and re-importing. Here the result is a mesh object, so
	 * the rest of the chain stays on the GPU and saving is just another operator.
	 *
	 * No mesh data crosses the bus: the arrangement, classification, ray visibility AND the
	 * output attribute reconstruction (barycentric interpolation of the source UV / normal /
	 * tangent / colour onto the sub-triangles, plus the per-triangle material ids) all run as
	 * compute passes writing the resident streams. What still comes back is a ~27-uint status
	 * block, because the resident capacity is a CPU-side allocation and only the GPU knows how
	 * big the result is.
	 *
	 * The exception is VertexWeldDistance > 0, which still goes through the CPU snapshot path:
	 * the weld post-process removes duplicate triangles, and reproducing that on the GPU needs
	 * a global hash table. Blocking; editor-oriented.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Boolean")
	static UPARAM(DisplayName = "Target") UCSMesh* ApplyMeshBoolean(
		UCSMesh* Target, AComputeShaderMeshBoolean* Generator, ECSMeshBooleanOp Op, const FCSMeshBooleanOptions& Options);

	/** Stage A only: removes interpenetration without any inside/outside deletion. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Boolean")
	static UPARAM(DisplayName = "Target") UCSMesh* ApplyMeshArrangement(
		UCSMesh* Target, AComputeShaderMeshBoolean* Generator, const FCSMeshBooleanOptions& Options);

	/**
	 * Merges co-located corners onto a shared vertex, entirely on the GPU. Wraps the shared
	 * weld facility: it decides spatial equivalence, this rewrites the index buffer. Attribute
	 * merging is deliberately not done — the surviving vertex keeps its own attributes, which
	 * is what preserves UV/normal seams across a weld.
	 *
	 * WeldDistance <= 0 does nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Repair")
	static UPARAM(DisplayName = "Target") UCSMesh* WeldVertices(UCSMesh* Target, float WeldDistance = 0.01f);

	// -------------------------------------------------------------------------
	// In-place operators
	// -------------------------------------------------------------------------

	/** Applies a transform to positions and the tangent basis. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Transform")
	static UPARAM(DisplayName = "Target") UCSMesh* TransformMesh(UCSMesh* Target, const FTransform& Transform);

	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Transform")
	static UPARAM(DisplayName = "Target") UCSMesh* TranslateMesh(UCSMesh* Target, FVector Translation);

	/** Reverses winding and negates the stored normals. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Normals")
	static UPARAM(DisplayName = "Target") UCSMesh* FlipNormals(UCSMesh* Target);

	/** Sets every vertex colour to a constant. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Colors")
	static UPARAM(DisplayName = "Target") UCSMesh* SetVertexColors(UCSMesh* Target, FLinearColor Color);

	// -------------------------------------------------------------------------
	// Draw batches
	// -------------------------------------------------------------------------

	/**
	 * Sorts Target's triangles by material slot so each slot's triangles are contiguous in the
	 * index buffer, writes one DrawIndexedIndirect arg set per slot, and publishes the matching
	 * section table. This is what turns a mesh that carries per-triangle material ids into a mesh
	 * that actually draws with more than one material.
	 *
	 * A counting sort on the GPU (histogram / serial scan / scatter), so the triangle count stays
	 * GPU-decided throughout — only the slot count (UCSMesh::Materials) comes from the CPU. The
	 * index buffer and the per-triangle material-id stream are reordered together; positions and
	 * the other vertex attributes are untouched, because only the index buffer decides which
	 * triangle is which.
	 *
	 * One section per material slot, including slots no triangle uses: their arg set draws zero
	 * indices, which costs a no-op draw call but keeps section i, arg set i and material slot i
	 * the same index everywhere. Dropping the empty ones would mean reading the histogram back.
	 * A mesh with an empty material table publishes no sections at all, which is the same
	 * whole-mesh draw the one section it would otherwise get describes.
	 *
	 * Order inside a run is not reproducible between calls (the scatter hands out destinations
	 * with an atomic cursor) and nothing downstream depends on it.
	 *
	 * Blocking, like every operator here, and when the indirect-args buffer has to grow to fit the
	 * slots the buffers change identity, so bound proxies rebind. Safe to re-run: an already-sorted
	 * mesh comes back with the same runs.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Sections")
	static UPARAM(DisplayName = "Target") UCSMesh* BuildMaterialSections(UCSMesh* Target);

	// -------------------------------------------------------------------------
	// Bounds
	// -------------------------------------------------------------------------

	/**
	 * Reduces the resident positions on the GPU into an exact world-space AABB and stores it in
	 * the resident set, replacing whatever the operators had accumulated.
	 *
	 * Explicitly opt-in, and named Sync, because the result has to reach a CPU FBox: this blocks
	 * on the GPU (submit, wait for idle, read six uints). Operators that know their bounds on the
	 * CPU — every upload path — must keep doing that instead of calling this. What it is for is
	 * the operators whose output only the GPU knows, where the alternative is a bound as loose as
	 * the query box (see FCSMeshBoxSceneOptions::bComputeExactBounds).
	 *
	 * Leaves the existing bounds untouched when the mesh turns out to be empty or the readback
	 * fails, so a caller's conservative estimate is never replaced by an invalid box.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Bounds")
	static UPARAM(DisplayName = "Target") UCSMesh* ComputeWorldBoundsSync(UCSMesh* Target);

	// -------------------------------------------------------------------------
	// Shared pass helpers, for operators implemented in other translation units
	// -------------------------------------------------------------------------

	/** Writes exact counts (upload paths) plus the matching DrawIndexedIndirect args. Any
	 *  operator that changes the counts must end with this, or the draw keeps the old index
	 *  count; operators whose size only the GPU knows write both from their own kernel.
	 *  Invalidates the section table for the reason spelled out on InvalidateSections. */
	static void AddSetCountersPass(FCSMeshEditContext& Context, uint32 VertexCount, uint32 IndexCount);

	/**
	 * Drops the section table, from inside an edit. Mandatory for every operator that changes the
	 * triangle count or the order triangles sit in — and EditMeshSync does NOT do it for you.
	 *
	 * The hazard: a section table is only meaningful next to the arg sets it was built with, and
	 * the counter kernels (SetCountersCS, AdvanceCountersBySoupCS) write arg set 0 alone. Run any
	 * of them on a sectioned mesh and set 0 becomes a whole-mesh draw while sets 1..N-1 still
	 * describe the runs of a triangle layout that no longer exists: the mesh draws its first
	 * material over everything and then draws garbage on top. Nothing errors, and nobody looking
	 * at the symptom would suspect the section table.
	 *
	 * Dropping it restores the documented default (one batch over the whole mesh from arg set 0),
	 * which is exactly what set 0 now holds. Re-run BuildMaterialSections to get the batches back.
	 */
	static void InvalidateSections(FCSMeshEditContext& Context);

	/** Uploads a CPU mesh snapshot into Target, replacing its contents — the mirror of
	 *  UCSMesh::ReadbackMeshSync. A per-corner snapshot (what the Boolean produces, so welded
	 *  positions can still carry per-face UV/normal seams) is expanded into a soup, because
	 *  the resident streams are strictly per-vertex; the StaticMesh build merges identical
	 *  vertices again on the way out. Not a UFUNCTION: FCSGpuMeshCPUData is not reflected. */
	static bool CopyFromMeshSnapshot(UCSMesh* Target, const FCSGpuMeshCPUData& Snapshot);
};
