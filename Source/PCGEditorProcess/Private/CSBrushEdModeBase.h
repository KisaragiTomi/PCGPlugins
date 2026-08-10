#pragma once

#include "CoreMinimal.h"
#include "EdMode.h"

class AActor;
class FEditorViewportClient;
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

	/** Hands a finished stroke to the target. Called once on mouse-up, never during the drag. */
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) = 0;

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

private:
	void CreateBrushComponent();
	void DestroyBrushComponent();
	void UpdateBrushComponent(FEditorViewportClient* ViewportClient);
	bool UpdateBrushTraceFromMouse(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 MouseX, int32 MouseY);
	bool TraceBrushRay(FEditorViewportClient* ViewportClient, const FVector& RayOrigin, const FVector& RayDirection);
	bool TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const;
	void BeginStroke();
	void UpdateStroke();
	void CommitStroke();
	void SamplePendingPoints();
	bool IsCandidatePointAllowed(const FVector& Location, float MinSpacing) const;
	bool IsTooCloseToPending(const FVector& Location, float MinSpacingSq) const;
	void GetRandomVectorInBrush(float BrushRadius, FVector& OutStart, FVector& OutEnd) const;
	void DrawPendingPreview() const;

	TObjectPtr<UStaticMeshComponent> SphereBrushComponent;
	TObjectPtr<UMaterialInstanceDynamic> BrushMID;

	bool bBrushTraceValid = false;
	bool bStrokeActive = false;
	FVector BrushLocation = FVector::ZeroVector;
	FVector BrushNormal = FVector::UpVector;
	FVector BrushTraceDirection = FVector::ForwardVector;
	TArray<FCSBrushSample> PendingSamples;
};
