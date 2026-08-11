#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"

class AActor;
class FEditorViewportClient;
class FSceneViewStateInterface;
class FViewport;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** One traced brush sample, before the leaf mode turns it into whatever it stores. */
struct FCSBrushSample
{
	FVector Location = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
};

/** Per-stroke knobs, read off the leaf's own target actor each time they are needed. */
struct FCSBrushSettings
{
	float Radius = 500.0f;
	float TraceRadius = 0.0f;
	float MinSpacing = 100.0f;
	int32 SamplesPerMouseMove = 16;
	float PreviewPointSize = 8.0f;
	float PreviewLifetime = 0.1f;
	FColor PreviewColor = FColor::Cyan;
	bool bExitAfterCommit = false;

	/** Drop hits whose surface faces the same way the ray travels, i.e. the ray struck a back face
	 *  from inside the geometry. The brush sphere is a volume: near its rim a sample ray starts
	 *  level with the surface and can enter the mesh, and this is what keeps the resulting hit off
	 *  the inside wall. No slope restriction — placement is orientation-agnostic by design. */
	bool bRejectBackFaces = true;
};

/**
 * The brush interaction shared by every actor-owned paint tool in this plugin.
 *
 * Everything about *how you paint* lives here: the foliage-style brush sphere, the viewport ray
 * to surface trace, the disc sampling inside the brush, spacing rejection, the drag-accumulates /
 * mouse-up-commits / Esc-cancels stroke lifecycle, and the DrawDebugPoint preview. None of it is
 * specific to what a sample eventually becomes.
 *
 * A leaf mode supplies only what differs: which actor is being painted, that actor's brush
 * settings, and what to do with a finished stroke. Committing is the only place a leaf touches
 * its target's data, so a stroke never costs more than a few traces regardless of what the leaf
 * ultimately builds.
 *
 * A leaf that does not sample on the CPU at all overrides SamplePendingPoints and leaves the
 * pending set empty; the stroke lifecycle, the brush sphere and the preview are unaffected.
 */
class FCSBrushEdModeBase : public FEdMode
{
public:
	FCSBrushEdModeBase();
	virtual ~FCSBrushEdModeBase() override;

	//~ FEdMode interface
	virtual void Enter() override;
	virtual void Exit() override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	/** Names the concrete mode, not the base, so a GC report points at the leaf that is active. */
	virtual FString GetReferencerName() const override { return GetID().ToString(); }
	virtual bool MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y) override;
	virtual bool CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y) override;
	virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event) override;
	virtual void Tick(FEditorViewportClient* ViewportClient, float DeltaTime) override;
	virtual bool UsesTransformWidget() const override { return false; }
	virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override { return false; }
	virtual bool ShouldDrawWidget() const override { return false; }
	virtual EAxisList::Type GetWidgetAxisToDraw(UE::Widget::EWidgetMode InWidgetMode) const override { return EAxisList::None; }

protected:
	// -------------------------------------------------------------------------
	// Leaf contract
	// -------------------------------------------------------------------------

	/** The actor being painted. Null disables the brush entirely. */
	virtual AActor* GetBrushTargetActor() const = 0;

	/** Brush knobs for this stroke, normally forwarded from the target actor's properties. */
	virtual FCSBrushSettings GetBrushSettings() const = 0;

	/** Hands a finished stroke to the target. Called once on mouse-up, never during the drag.
	 *  Samples is what SamplePendingPoints collected, so a leaf that samples on the GPU gets an
	 *  empty array and commits whatever its own pass already wrote. */
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) = 0;

	/** Collects this update's samples. The default traces the world and fills the pending set;
	 *  called once when the stroke starts and again on every mouse-move while it is held. */
	virtual void SamplePendingPoints();

	/** Drops the leaf's target. Called from Exit so a deactivated mode holds nothing of the level. */
	virtual void ClearBrushTarget() = 0;

	/** Spacing check against what the target already holds. Default: no committed set to check. */
	virtual bool IsTooCloseToCommitted(const FVector& Location, float MinSpacingSq) const { return false; }

	/** Per-placement veto, e.g. a bounds restriction on the target. */
	virtual bool IsPointAllowed(const FVector& Location) const { return true; }

	/** Preconditions beyond having a target, e.g. the instance brush needing a mesh assigned. */
	virtual bool IsReadyToPaint() const { return true; }

	/** Leaves call this from their SetTargetActor to drop any stroke aimed at the old target. */
	void ResetBrushState();

	void CancelStroke();
	void ExitTemporaryMode();

	/** Where the cursor ray met a surface, and whether that hit is current. The sample disc — CPU
	 *  or GPU — is centred there. */
	bool IsBrushTraceValid() const { return bBrushTraceValid; }
	const FVector& GetBrushLocation() const { return BrushLocation; }

	/** Identifies the viewport the brush is being dragged in. A leaf that samples off the rendered
	 *  frame needs it to pick the right one out of the several the editor renders. */
	FSceneViewStateInterface* GetBrushViewState() const { return BrushViewState; }

private:
	void CreateBrushComponent();
	void DestroyBrushComponent();
	void UpdateBrushComponent(FEditorViewportClient* ViewportClient);
	bool UpdateBrushTraceFromMouse(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 MouseX, int32 MouseY);
	bool TraceBrushRay(FEditorViewportClient* ViewportClient, const FVector& RayOrigin, const FVector& RayDirection);
	bool TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const;
	bool IsSurfacePaintable(const FHitResult& Hit, const FVector& TraceDirection, const FCSBrushSettings& Settings) const;
	void BeginStroke();
	void UpdateStroke();
	void CommitStroke();
	bool IsCandidatePointAllowed(const FVector& Location, float MinSpacing) const;
	bool IsTooCloseToPending(const FVector& Location, float MinSpacingSq) const;
	void GetRandomVectorInBrush(float BrushRadius, FVector& OutStart, FVector& OutEnd) const;
	void DrawPendingPreview() const;

	TObjectPtr<UStaticMeshComponent> SphereBrushComponent;
	TObjectPtr<UMaterialInstanceDynamic> BrushMID;

	bool bBrushTraceValid = false;
	bool bStrokeActive = false;
	FSceneViewStateInterface* BrushViewState = nullptr;
	FVector BrushLocation = FVector::ZeroVector;
	FVector BrushNormal = FVector::UpVector;
	FVector BrushTraceDirection = FVector::ForwardVector;
	TArray<FCSBrushSample> PendingSamples;
};
