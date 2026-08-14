#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshTypes.h"
#include "RenderGraphResources.h"
#include "UObject/Object.h"
#include "CSMesh.generated.h"

class FRDGBuilder;
class UMaterialInterface;

// -----------------------------------------------------------------------------
// GPU-resident mesh object.
//
// UCSMesh is to the GPU-mesh base what UDynamicMesh is to FDynamicMesh3: a UObject
// shell that can be held, passed around, and chained through operators, with the
// actual geometry living in FCSMeshResident. The relationship to the abstract render
// base is the same one the engine has between UDynamicMesh and UBaseDynamicMeshComponent
// — the object owns data, the component only draws it. Do not confuse this with
// UCSMeshAsset (ComputeShaderSceneCapture.h), which is an unrelated MeshFill data asset.
//
// What is deliberately NOT copied from UDynamicMesh: FDynamicMesh3-style topology
// editing. There is no half-edge structure on the GPU, so EditMesh(lambda)'s per-element
// random access has no counterpart. The counterpart here is "an operator is a sequence
// of RDG passes writing the resident streams", and every one of them goes through the
// single EditMeshSync entry point so the change event can never be missed.
//
// There is exactly one other way in, for the one caller shape EditMeshSync cannot serve: a
// per-frame render pass, already inside the renderer's graph, which can neither build its own
// graph nor block on a flush. That is FCSMeshRenderThreadEdit, and it shares EditMeshSync's
// registration and access-state restoration rather than repeating them — see its comment.
// -----------------------------------------------------------------------------

/**
 * One draw batch of the mesh.
 *
 * Sections are parallel to the DrawIndexedIndirect arg sets in the IndirectArgs stream:
 * section i draws with arg set i, and nothing else establishes that pairing, which is why a
 * table longer than the arg buffer is refused instead of clamped.
 *
 * An EMPTY table means "one batch over the whole mesh, from arg set 0". That is what every
 * mesh did before sections existed and is still the default, so a mesh that never goes
 * through the section builder keeps drawing exactly as it did.
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshSection
{
	GENERATED_BODY()

	/** Index into UCSMesh::Materials. An out-of-range id is only warned about, not rejected:
	 *  the sort usually runs before the caller has finished filling the material table. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh")
	int32 MaterialIndex = 0;

	/** Where the run sits, for tooling and debugging only — INDEX_NONE when the sort left the
	 *  counts on the GPU, which is the normal case. The draw always takes its counts from the
	 *  arg set; if it took them from here, a table that outlived its args would still draw. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS GpuMesh")
	int32 FirstTriangle = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS GpuMesh")
	int32 TriangleCount = INDEX_NONE;
};

/**
 * Render-thread-side retained buffer set. Ownership lives here; a scene proxy only
 * borrows it. Buffers must be released on the render thread, which is why UCSMesh holds
 * this behind a TSharedPtr it hands to ENQUEUE_RENDER_COMMAND on destruction.
 */
struct COMPUTESHADERGENERATOR_API FCSMeshResident
{
	struct FStream
	{
		FCSGpuStreamDesc Desc;                 // reuses the existing stream description
		TRefCountPtr<FRDGPooledBuffer> Pooled; // retained, survives across frames
	};

	/** Standard triangle set (8 streams) by default; extra streams may be appended. */
	TArray<FStream> Streams;

	/** DrawIndexedIndirect arg sets the IndirectArgs stream is sized for. 1 is the single
	 *  whole-mesh draw; a per-LOD consumer declares one per LOD and the section builder one
	 *  per section. Kept in step with the IndirectArgs descriptor by every path that changes
	 *  either, so a consumer can read the draw count without decoding the descriptor. */
	uint32 NumIndirectDraws = 1;

	/** Draw batches, parallel to the arg sets (section i draws with arg set i). Empty means one
	 *  batch over the whole mesh, which is the default and what every mesh that never meets the
	 *  section builder keeps doing.
	 *
	 *  Written on the render thread inside an edit (like WorldBounds), read on the game thread
	 *  after that edit's flush. Dropped by MarkBuffersChanged(): a table left pointing at
	 *  regenerated — and therefore zeroed — arg sets draws garbage or nothing, and neither
	 *  symptom leads anyone back to the stale table. */
	TArray<FCSMeshSection> Sections;

	/** Allocated capacity. The *actual* counts are GPU-decided and live in the MeshCounters
	 *  stream — this is the ceiling the operators must not exceed. */
	uint32 VertexCapacity = 0;
	uint32 IndexCapacity = 0;

	/** Conservative world-space bounds readable from the game thread. Operators that move
	 *  or add geometry are responsible for widening it. */
	FBox WorldBounds = FBox(ForceInit);

	/** Incremented on every completed edit. Consumers (render proxies, spatial handles)
	 *  compare against it to decide whether their cached view is stale. */
	uint32 Generation = 0;

	/** Incremented only when the buffers themselves change identity (allocate / grow /
	 *  release). A consumer that merely borrows the buffers can ignore a content edit but
	 *  must rebind when this moves, or it keeps drawing from buffers that no longer exist. */
	uint32 AllocationGeneration = 0;

	/** Counts an operator could state exactly (uploads); INDEX_NONE when only the GPU knows
	 *  (scene extraction, boolean). Written on the render thread inside an edit, read on the
	 *  game thread after that edit's flush. */
	int32 KnownVertexCount = INDEX_NONE;
	int32 KnownIndexCount = INDEX_NONE;

	/** Registers the standard triangle streams: the seven the render base uses, plus a
	 *  per-triangle material-id stream, and with vertex colours included in the readback
	 *  set. Records InNumIndirectDraws as the mesh's draw count. Replaces any previous
	 *  declaration. Does not allocate; call AllocateBuffers() after setting the capacities. */
	void AddStandardTriangleStreams(uint32 InNumIndirectDraws = 1);

	/** Appends one stream beyond the standard set. A stream is addressed by its (Role,
	 *  TexCoordIndex) pair and Find() returns the first match, so a duplicate pair would
	 *  silently shadow the stream it collides with — that is refused and logged instead.
	 *  Note the standard resident set already owns AuxVertex slot 0 (the material ids).
	 *
	 *  Declaration-time only: on an already-allocated mesh the new stream has no buffer, which
	 *  makes IsAllocated() false and every edit fail. Go through UCSMesh::SetStreamLayoutSync,
	 *  which reallocates. */
	bool AddStream(const FCSGpuStreamDesc& Desc);

	/** Allocates a pooled buffer for every registered stream from the current capacities.
	 *  Render thread only. Existing buffers are dropped without copying — UCSMesh's capacity
	 *  and layout entry points are what preserve contents across a reallocation. */
	void AllocateBuffers();

	/** Drops every pooled buffer. Render thread only. */
	void ReleaseBuffers();

	/** The buffers changed identity: bumps AllocationGeneration and drops the section table.
	 *  Every path that allocates or frees ends here, so neither can be forgotten — a consumer
	 *  that missed the generation bump keeps drawing from freed buffers, and a section table
	 *  that outlived its arg sets draws garbage. */
	void MarkBuffersChanged();

	bool IsAllocated() const;

	/** Index into Streams, or INDEX_NONE. (Role, TexCoordIndex) is how every consumer of this
	 *  set addresses a stream. */
	int32 FindStreamIndex(ECSGpuStreamRole Role, uint8 Index = 0) const;

	const FStream* FindStream(ECSGpuStreamRole Role, uint8 Index = 0) const;

	/** Total VRAM held by the allocated streams. Derived from the descriptors and capacities
	 *  rather than queried, so it is safe to call from the game thread. */
	int64 GetAllocatedBytes() const;

	/** What the declared stream set would cost at those capacities, allocated or not. The
	 *  pre-flight twin of GetAllocatedBytes: the budget has to be consulted before the
	 *  allocation exists, not after. Game-thread safe for the same reason. */
	int64 GetRequiredBytes(uint32 InVertexCapacity, uint32 InIndexCapacity) const;
};

/**
 * A mesh's declared stream set: the standard resident streams plus whatever a consumer needs
 * on top of them.
 *
 * The standard extras (per-triangle material ids, vertex-colour readback) are deliberately not
 * configurable here. Every operator in CSMeshOps requires the material-id stream, so a resident
 * set declared without it would not fail at declaration time — it would fail at the first
 * operator, which is much further from the mistake.
 */
struct FCSMeshStreamLayout
{
	/** DrawIndexedIndirect arg sets to size the IndirectArgs stream for: one per LOD for the
	 *  instanced path, one per section for the section builder. 1 keeps the single whole-mesh
	 *  draw every mesh has by default. */
	uint32 NumIndirectDraws = 1;

	/** Appended after the standard set, in order. Each must use a (Role, TexCoordIndex) pair no
	 *  standard stream already holds; AuxVertex slot 0 is the material-id stream. */
	TArray<FCSGpuStreamDesc> ExtraStreams;
};

/**
 * One already-declared stream's new element count, for UCSMesh::ResizeStreamsSync.
 *
 * The stream is named the way every other consumer of this set names one — by its (Role, SlotIndex)
 * pair, which is what FCSMeshResident::FindStream and FCSMeshEditContext::Find key on. ElementCount
 * is the whole count: only Fixed-CountSource streams can be resized this way, so nothing multiplies
 * it by a capacity afterwards.
 */
struct FCSMeshStreamResize
{
	ECSGpuStreamRole Role = ECSGpuStreamRole::AuxVertex;
	uint8 SlotIndex = 0;
	uint32 ElementCount = 0;
};

/**
 * One edit's view of the resident set: every stream already registered into the graph.
 *
 * Operators receive this instead of the raw FRDGBuilder so that (a) they never register an
 * external buffer themselves and (b) the per-role final access state is restored by the
 * framework rather than by each operator remembering to. A stream left in RDG's default
 * epilogue state (SRVMask) is illegal for index / indirect use, and the symptom — "the
 * component stopped drawing after some operator ran" — is nearly impossible to trace back.
 */
struct COMPUTESHADERGENERATOR_API FCSMeshEditContext
{
	/**
	 * Which of the two sanctioned edit paths built this context. Two things depend on it, and
	 * neither can be decided per operator, because both follow from who owns the graph.
	 */
	enum class EKind : uint8
	{
		/** UCSMesh::EditMeshSync. The edit owns the FRDGBuilder and executes it, with the game
		 *  thread blocked on the flush until it has: RDG's epilogue is therefore the last thing
		 *  that happens before anything can read the streams, and that same flush is what fences
		 *  the resident set's game-thread-read fields. */
		OwnedGraph,
		/** FCSMeshRenderThreadEdit. The edit shares a graph somebody else owns and executes.
		 *  Nothing is fenced, and the passes that read these streams — the draw itself — are later
		 *  passes IN that graph, so an epilogue transition would land after them. */
		BorrowedGraph,
	};

	FCSMeshEditContext(FRDGBuilder& InGraphBuilder, FCSMeshResident& InResident, EKind InKind = EKind::OwnedGraph);

	FRDGBuilder& GraphBuilder;
	FCSMeshResident& Resident;

	/** Parallel to Resident.Streams. */
	TArray<FRDGBufferRef> StreamBuffers;

	EKind GetKind() const { return Kind; }

	/**
	 * Restores every registered stream's per-role final access state: the single implementation of
	 * the rule this whole layer exists to enforce. Called for you — by EditMeshSync at the end of
	 * the edit and by ~FCSMeshRenderThreadEdit — and never by an operator.
	 *
	 * How it applies the state is the one thing EKind changes. An owned graph asks RDG for it in
	 * the epilogue (SetBufferAccessFinal); a borrowed graph takes the transition immediately
	 * (UseExternalAccessMode), because the draw that reads these streams is a later pass in that
	 * same graph and an epilogue transition would reach it after the fact — which is to say never.
	 */
	void RestoreStreamAccess();

	FRDGBufferRef Find(ECSGpuStreamRole Role, uint8 Index = 0) const;
	FRDGBufferRef FindBySemantic(ECSGpuMeshSemantic Semantic) const;

	FRDGBufferRef Positions() const { return Find(ECSGpuStreamRole::Position); }
	FRDGBufferRef Tangents() const { return Find(ECSGpuStreamRole::TangentBasis); }
	FRDGBufferRef TexCoords() const { return Find(ECSGpuStreamRole::TexCoord); }
	FRDGBufferRef Colors() const { return Find(ECSGpuStreamRole::Color); }
	FRDGBufferRef Indices() const { return Find(ECSGpuStreamRole::Index); }
	FRDGBufferRef IndirectArgs() const { return Find(ECSGpuStreamRole::IndirectArgs); }
	FRDGBufferRef Counters() const { return Find(ECSGpuStreamRole::MeshCounters); }
	FRDGBufferRef MaterialIds() const { return FindBySemantic(ECSGpuMeshSemantic::MaterialId); }

	/** Record counts the operator knows exactly. Leave alone when only the GPU knows.
	 *
	 *  Ignored, with a warning, on a BorrowedGraph edit: these are read from the game thread and
	 *  EditMeshSync's flush is the only thing that fences that read. A per-frame render pass
	 *  publishing them would hand the game thread the result of a graph nobody has executed yet. */
	void SetKnownCounts(int32 VertexCount, int32 IndexCount);

	/** Marks the counts unknown; the next GetTriangleCountSync() will read the GPU. Ignored on a
	 *  BorrowedGraph edit for the same reason as SetKnownCounts. */
	void InvalidateKnownCounts();

	/** Holds a non-RDG view alive until the graph has executed. SHADER_PARAMETER_SRV does not
	 *  take a reference, and the graph runs after the operator lambda has returned — so an
	 *  SRV created on the operator's stack would be dead by the time the pass reads it. */
	void KeepAliveResource(const FShaderResourceViewRHIRef& View);

private:
	TArray<FShaderResourceViewRHIRef> KeepAliveViews;
	EKind Kind = EKind::OwnedGraph;
};

/**
 * The second sanctioned way to write the resident streams: an edit inside a graph the CALLER owns,
 * on the render thread.
 *
 * UCSMesh::EditMeshSync is the first and covers everything the game thread drives — it builds its
 * own FRDGBuilder, runs the operator, restores the access states, executes and blocks. A per-frame
 * render pass can do none of that: it is already inside the renderer's graph, which it must neither
 * execute nor flush. Without this the only way out was to write the streams directly and restore
 * the access states by hand, which is the same load-bearing rule maintained in two places — and the
 * copy that drifts is the one nothing reports, because a stream left in RDG's default epilogue
 * (SRVMask) is illegal for index / indirect use and simply stops working.
 *
 * Scope it around the passes and let it close before the caller executes:
 *
 *     FCSMeshRenderThreadEdit Edit(GraphBuilder, *Resident);
 *     FRDGBufferRef Args = Edit->IndirectArgs();
 *     ... add passes ...
 *                                  // ~FCSMeshRenderThreadEdit restores every stream's access state
 *
 * What it must not touch is the resident set's game-thread-read state. KnownVertexCount /
 * KnownIndexCount are refused by the context; Sections, WorldBounds, Generation and the
 * OnMeshChanged broadcast are simply not its business — nothing fences a render-thread write of
 * them against the game thread reading them, and no consumer expects any of them to move per frame.
 * A render-thread edit changes what is IN the buffers, never what the object says about them.
 */
struct COMPUTESHADERGENERATOR_API FCSMeshRenderThreadEdit
{
	FCSMeshRenderThreadEdit(FRDGBuilder& InGraphBuilder, FCSMeshResident& InResident);
	~FCSMeshRenderThreadEdit();

	// Non-copyable: two scopes over one edit would restore the access states twice, the first time
	// while passes that still write those streams are yet to be added.
	FCSMeshRenderThreadEdit(const FCSMeshRenderThreadEdit&) = delete;
	FCSMeshRenderThreadEdit& operator=(const FCSMeshRenderThreadEdit&) = delete;

	FCSMeshEditContext& Get() { return Context; }
	FCSMeshEditContext& operator*() { return Context; }
	FCSMeshEditContext* operator->() { return &Context; }

private:
	FCSMeshEditContext Context;
};

using FCSMeshResidentRef = TSharedPtr<FCSMeshResident, ESPMode::ThreadSafe>;

/**
 * Readback lifted off the scene proxy.
 *
 * The counter read and the descriptor-driven stream loop used to act on a live
 * FCSGpuMeshSceneProxy, which meant a mesh that was not currently being rendered could not
 * be read back or saved at all. Both now act on a plain FCSMeshResident; the proxy path
 * forwards a view of its own streams, so UCSGpuMeshComponent::ReadbackMeshSync behaves
 * exactly as before while UCSMesh gets the same capability with no component involved.
 */
namespace CSMeshReadback
{
	/** Reads the two-uint MeshCounters stream. Game thread; blocks on the GPU. */
	COMPUTESHADERGENERATOR_API bool ReadCountersSync(
		const FCSMeshResident& Resident, uint32& OutVertexCount, uint32& OutIndexCount);

	/** Reads every readback stream into a CPU snapshot. Game thread; blocks on the GPU.
	 *  Does not fill OutMeshData.Materials — the resident set carries ids, not materials. */
	COMPUTESHADERGENERATOR_API bool ReadbackResidentSync(
		const FCSMeshResident& Resident, FCSGpuMeshCPUData& OutMeshData);
}

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCSMeshChanged, UCSMesh*);

/**
 * GPU-resident mesh object. Holds the retained buffer set, a material table, and the change
 * event render components listen to. Every mutation goes through EditMeshSync.
 *
 * Transient by default: GPU data does not survive a level reload, so nothing here is
 * serialized except the material table (which must be a UPROPERTY — the resident set is not
 * reflected, so the object shell is the only thing keeping those materials alive) and the
 * memory-policy knobs below, which are settings rather than data.
 */
UCLASS(BlueprintType)
class COMPUTESHADERGENERATOR_API UCSMesh : public UObject
{
	GENERATED_BODY()

public:
	UCSMesh();

	/** Material table indexed by the per-triangle material-id stream.
	 *
	 *  Writing an element directly (Materials[i] = X) changes what the mesh *should* draw with
	 *  but fires no change event, so a bound render component keeps its already-resolved batch
	 *  materials and goes on drawing the old one. Element assignment on a TArray UPROPERTY
	 *  cannot be intercepted, so the notification has to be the caller's move: go through
	 *  SetMaterial, or call NotifyMaterialsChanged after a batch of direct writes. */
	UPROPERTY(BlueprintReadWrite, Category = "CS GpuMesh")
	TArray<TObjectPtr<UMaterialInterface>> Materials;

	/** Assigns one slot and broadcasts. Grows the table when Index is past the end, so a producer
	 *  can fill slots in any order. Negative Index is ignored. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	void SetMaterial(int32 Index, UMaterialInterface* Material);

	/** Announces that Materials was edited in place. The escape hatch for callers that write the
	 *  array directly — including Blueprint, which can assign the whole array in one node. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	void NotifyMaterialsChanged();

	// -------------------------------------------------------------------------
	// Memory policy
	//
	// The pool already refuses to cache more VRAM than the device can spare, but every direct
	// AllocateSync / EnsureCapacitySync went around that gate entirely. These settings drive
	// the same pre-flight for the object itself, in the shape AComputeShaderMeshGenerator
	// established for the box-scene pipelines.
	// -------------------------------------------------------------------------

	/** Pre-flights every allocation against the device's free VRAM. Off allocates blind, which
	 *  on an over-committed device means a driver-level failure instead of a refusal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh|Memory Budget")
	bool bCheckGpuMemoryBudget = true;

	/** Fraction of the free VRAM one mesh may claim. The rest covers RDG pooling, the transient
	 *  buffers the operators allocate on top of the resident set, and driver overhead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh|Memory Budget", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float GpuMemoryBudgetSafetyRatio = 0.7f;

	/** How much slack ShrinkCapacitySync tolerates before it reallocates: 0.5 means "shrink
	 *  once the allocation is more than 1.5x what the mesh needs". This is the hysteresis that
	 *  keeps a mesh whose size oscillates — a road rebuilt a few hundred vertices shorter, then
	 *  longer again — from reallocating and copying itself on every single rebuild. Zero turns
	 *  that off and shrinks on any surplus at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GpuMesh|Memory Budget", meta = (ClampMin = "0.0"))
	float ShrinkSlackRatio = 0.5f;

	//~ UObject interface
	virtual void BeginDestroy() override;

	// -------------------------------------------------------------------------
	// Object-level state
	// -------------------------------------------------------------------------

	/** Empties the mesh while keeping the allocation, as UDynamicMesh::Reset does: the
	 *  counters go to zero, the indirect args draw nothing, and the section table goes with
	 *  them (it describes geometry that no longer exists). Blocks (render flush). */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	void Reset();

	/** Frees the GPU buffers outright. The object stays usable; the next operator that
	 *  needs capacity reallocates. Blocks (render flush). */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	void ReleaseSync();

	/** Game-thread approximation: true when nothing is allocated, or when the last operator
	 *  stated a zero count. Never touches the GPU, so it can be wrong right after an operator
	 *  whose output size only the GPU knows — use GetTriangleCountSync() when it matters. */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	bool IsEmpty() const;

	/** Reads the GPU-decided triangle count back. This is a full GPU stall (flush + readback),
	 *  which is why the name says Sync; do not call it per frame. Returns 0 when unallocated. */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	int32 GetTriangleCountSync();

	/** Same stall as GetTriangleCountSync, reporting both counts. */
	bool GetCountsSync(uint32& OutVertexCount, uint32& OutIndexCount);

	/** Conservative world bounds maintained by the operators. No GPU access. */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	FBox GetWorldBoundsApprox() const;

	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	int32 GetVertexCapacity() const;

	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	int32 GetIndexCapacity() const;

	/** Bumped by every completed edit; render consumers use it to detect staleness. */
	uint32 GetGeneration() const;

	// -------------------------------------------------------------------------
	// Editing
	// -------------------------------------------------------------------------

	/**
	 * The game thread's mutation entry point. Runs EditFunc on the render thread inside one
	 * FRDGBuilder with every resident stream already registered, restores the per-role
	 * access states, executes, then broadcasts OnMeshChanged on the game thread.
	 *
	 * Synchronous, matching the existing contract for this subsystem (a flush fence is what
	 * makes it safe for the game thread to hand raw pointers to the render thread). Game
	 * thread only. Returns false when the mesh has no allocation to edit.
	 *
	 * A render pass already inside somebody else's graph cannot use this — it would have to
	 * flush the thread it is running on. FCSMeshRenderThreadEdit is that caller's entry point,
	 * over the same registration and the same access-state restoration.
	 */
	bool EditMeshSync(TFunctionRef<void(FCSMeshEditContext&)> EditFunc);

	/**
	 * Guarantees at least this much capacity, reallocating and copying the existing contents
	 * across when it has to grow. Capacity is explicit here in a way it never is for a CPU
	 * mesh: the buffers are fixed-size and the counts inside them are decided by the GPU.
	 * Never shrinks — see ShrinkCapacitySync for that. Game thread only; blocks.
	 *
	 * Returns false when the mesh does not end up with the requested capacity, which today
	 * means the VRAM pre-flight refused it (bCheckGpuMemoryBudget). A refusal leaves the
	 * previous allocation untouched and intact: an allocation is refused outright rather than
	 * clamped to what fits, because a clamped capacity is not a smaller mesh — it is the mesh
	 * the caller asked for with its tail silently cut off by whichever operator hit the ceiling.
	 * Callers that can degrade (fewer triangles, a smaller query box) need to know they must.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	bool EnsureCapacitySync(int32 InVertexCapacity, int32 InIndexCapacity);

	/**
	 * The counterpart to EnsureCapacitySync: hands back capacity a regenerated mesh no longer
	 * needs. Roads and vines are rebuilt over and over, and with only a grow path their buffers
	 * ratchet up to the largest version they ever had and stay there for the session.
	 *
	 * Never shrinks below the live counts, and the surviving contents are copied across exactly
	 * as the grow path copies them, so this is not destructive: the request is a floor, not a
	 * truncation. When only the GPU knows the counts (KnownVertexCount / KnownIndexCount are
	 * INDEX_NONE after a scene extraction or a Boolean) bAllowCounterReadback decides between
	 * paying for a counter readback and refusing the shrink. It defaults to reading back,
	 * because the meshes worth shrinking are precisely the ones whose counts are GPU-decided,
	 * and a two-uint readback is small next to the reallocation it gates. Passing false makes
	 * an unknown-count mesh refuse instead — never truncate on a guess.
	 *
	 * Nothing happens unless the surplus exceeds ShrinkSlackRatio; see that property.
	 *
	 * Game thread only; blocks. Returns true only when the buffers were actually reallocated,
	 * which is also exactly when AllocationGeneration moved and bound proxies must rebind.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	bool ShrinkCapacitySync(int32 InVertexCapacity, int32 InIndexCapacity, bool bAllowCounterReadback = true);

	// -------------------------------------------------------------------------
	// Stream layout and draw batches
	// -------------------------------------------------------------------------

	/**
	 * Declares the whole stream set: the standard resident streams, any extra streams on top,
	 * and how many DrawIndexedIndirect arg sets the IndirectArgs stream carries.
	 *
	 * Works before the first allocation (the declaration is simply remembered, and the next
	 * allocation builds from it) and on an allocated mesh, which reallocates at the current
	 * capacities and copies every stream that survives the change — streams the layout adds
	 * start with whatever the buffer pool last left in them, the same as on a fresh allocation.
	 * Either way the buffers change identity, so AllocationGeneration moves and the section
	 * table is dropped.
	 *
	 * Not a UFUNCTION: FCSGpuStreamDesc is not reflected. Game thread only; blocks. Returns
	 * false, with nothing changed at all, when the layout is rejected (see FCSMeshResident::
	 * AddStream) or when the VRAM pre-flight refuses the reallocation.
	 */
	bool SetStreamLayoutSync(const FCSMeshStreamLayout& Layout);

	/**
	 * Resizes streams the layout already declares, and only those. Each named stream gets a new
	 * buffer at the new element count; every other stream keeps the buffer — and therefore the
	 * contents — it already had.
	 *
	 * The general twin of EnsureIndirectDrawCapacitySync, and the reason a consumer whose extra
	 * streams are sized from a live count no longer has to re-declare its whole layout to change
	 * one of them: SetStreamLayoutSync reallocates and copies EVERY resident stream, base-mesh
	 * geometry included, which for a per-instance buffer that grows with every AddInstance means
	 * the entire mesh is copied so that one aux buffer can get bigger.
	 *
	 * A resized stream comes back ZEROED. Its previous contents are deliberately not carried
	 * across, for two reasons that point the same way: a stream sized from a live count is indexed
	 * by that count (the instanced leaf's visible-instance regions start at Lod * InstanceCapacity),
	 * so surviving bytes would survive at offsets that no longer mean what they meant; and the
	 * streams worth resizing at all are the ones a pass rewrites in full before anything reads
	 * them, which makes a copy pure cost. Zeroed rather than left alone because a buffer straight
	 * out of the pool holds the previous tenant's bytes, and a consumer reading those has no way
	 * to tell that is what it is reading.
	 *
	 * The resize is EXACT — no hysteresis, no rounding up. A stream left larger than asked for
	 * would put the buffer and the CPU-side stride constants that index it out of step, which is
	 * the class of mistake FCSGpuInstancedGpuLayout exists to prevent. Hysteresis belongs to the
	 * caller, which is also the only place that knows how its counts move.
	 *
	 * Only Fixed-CountSource streams qualify. For a PerVertex / PerIndex / PerTriangle stream
	 * ElementsPerUnit is the per-unit stride — a format constant the vertex factory and the
	 * readback both decode by — and its size follows the mesh capacity, so EnsureCapacitySync /
	 * ShrinkCapacitySync are what resize it. IndirectArgs and MeshCounters are refused too: both
	 * are Fixed but both keep bookkeeping outside the descriptor (NumIndirectDraws, which SetSections
	 * is checked against; "two uints", which the counter readback copies verbatim).
	 *
	 * Batched because each call is its own render round trip, and a consumer whose extras all
	 * derive from one number changes all of them at once.
	 *
	 * Buffers change identity, so AllocationGeneration moves and the section table is dropped;
	 * a bound proxy must rebind. Not a UFUNCTION: ECSGpuStreamRole is not reflected. Game thread
	 * only; blocks. Returns false with NOTHING changed when any entry is rejected or the VRAM
	 * pre-flight refuses the total — validated up front, because half an applied resize leaves some
	 * streams sized for the new count and some for the old, and the constants that index them can
	 * only be right for one of the two.
	 */
	bool ResizeStreamsSync(const TArray<FCSMeshStreamResize>& Resizes);

	/** One stream. Same contract as the batch form; prefer that one for streams that change
	 *  together, since each call here costs its own render flush. */
	bool ResizeStreamSync(ECSGpuStreamRole Role, uint8 SlotIndex, int32 NewElementCount);

	/**
	 * Grows only the IndirectArgs stream, to at least NumArgSets DrawIndexedIndirect arg sets.
	 * Just that one buffer is reallocated; the geometry streams and their contents are left
	 * completely alone, which is what lets a section builder run after the geometry is final.
	 *
	 * The new buffer is zeroed before the existing arg sets are copied back into it, so a set
	 * nobody has written yet draws nothing instead of drawing whatever the pool's previous
	 * tenant left in those five uints. Never shrinks. AllocationGeneration moves (the args
	 * buffer a bound proxy holds is gone) and the section table is dropped, so the builder's
	 * order is: grow, fill the args, then SetSections.
	 *
	 * Game thread only; blocks. Returns true when the stream holds that many sets afterwards;
	 * false for an absurd count, since a set count that came out of an unclamped GPU counter
	 * should not be able to ask for gigabytes of arg buffer.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	bool EnsureIndirectDrawCapacitySync(int32 NumArgSets);

	/** Arg sets the IndirectArgs stream currently holds. 1 is the whole-mesh draw. */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	int32 GetIndirectDrawCount() const;

	/** The draw batches. Empty means one batch over the whole mesh — see FCSMeshSection. */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh")
	TArray<FCSMeshSection> GetSections() const;

	/**
	 * Publishes the table produced by the material sort. Refused, with nothing changed, when it
	 * is longer than the IndirectArgs stream's arg-set count: section i draws from arg set i,
	 * so a section past the end would issue a draw out of an arg set that does not exist. Call
	 * EnsureIndirectDrawCapacitySync first.
	 *
	 * An empty table restores the single whole-mesh batch. Broadcasts OnMeshChanged, since the
	 * batch list is part of what a render consumer draws. Game thread only.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh")
	bool SetSections(const TArray<FCSMeshSection>& InSections);

	/** Reads the whole mesh back to a CPU snapshot. One blocking readback; editor/save path
	 *  only. Unlike the component-based readback this needs no registered component and no
	 *  live scene proxy — an unrendered UCSMesh can still be saved. */
	bool ReadbackMeshSync(FCSGpuMeshCPUData& OutMeshData) const;

	/** Fired after every completed edit (game thread). */
	FOnCSMeshChanged OnMeshChanged;

	// -------------------------------------------------------------------------
	// Resident access (render side / operator implementations)
	// -------------------------------------------------------------------------

	const FCSMeshResidentRef& GetResident() const { return Resident; }
	FCSMeshResident* GetResidentPtr() const { return Resident.Get(); }

private:
	/** Allocates the declared stream set at the given capacity, discarding any previous one.
	 *  False means the VRAM pre-flight refused it and nothing was allocated. */
	bool AllocateSync(uint32 InVertexCapacity, uint32 InIndexCapacity);

	/** VRAM pre-flight for an allocation of RequestedBytes. False means "do not allocate".
	 *  The peak it actually tests is RequestedBytes plus whatever is already allocated, because
	 *  a reallocation holds both buffer sets at once while it copies between them. A device
	 *  that cannot report its VRAM at all is not a reason to refuse — that case proceeds with a
	 *  log, matching CSGpuMemoryBudget's BudgetUnknown verdict and the pool's refusal to evict
	 *  on a guess. Never prompts: allocations happen inside operators, and a modal dialog per
	 *  operator is not a thing the callers of this API can survive. */
	bool ConfirmGpuMemoryBudget(int64 RequestedBytes, const TCHAR* OperationName) const;

	FCSMeshResidentRef Resident;
};
