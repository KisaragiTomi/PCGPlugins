#include "CSDirectTriangleMeshComponent.h"
#include "CSDirectTriangleMeshSceneProxy.h"
#include "RHI.h"

UCSDirectTriangleMeshComponent::UCSDirectTriangleMeshComponent()
{
	// The triangle soup is world-space; render with an absolute identity transform so
	// local space == world space (positions are drawn 1:1, independent of the owner).
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);

	// GPU-Scene instance culling overrides custom indirect args in the Virtual Shadow Map
	// passes, so indirect-drawn meshes cannot cast VSM shadows (same limitation as roads).
	CastShadow = false;
}

void UCSDirectTriangleMeshComponent::SetTriangleSource(const FCSBoxScenePreparedData& InPrepared, uint32 InVertexCapacity, const FBox& InWorldBounds)
{
	PendingPrepared = InPrepared;
	PendingVertexCapacity = InVertexCapacity;
	LocalBounds = InWorldBounds; // absolute identity transform => local bounds are world bounds

	// Submission followed immediately by a save must see the new proxy. MarkRenderStateDirty
	// defers recreation until the game-thread end-of-frame update, which FlushRenderingCommands
	// alone does not process.
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

void UCSDirectTriangleMeshComponent::ClearTriangleSource()
{
	PendingPrepared = FCSBoxScenePreparedData();
	PendingVertexCapacity = 0;
	LocalBounds = FBox(ForceInit);
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

FPrimitiveSceneProxy* UCSDirectTriangleMeshComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;
	if (!PendingPrepared.IsValid() || !PendingPrepared.HasAnyTriangles() || PendingVertexCapacity == 0) return nullptr;
	return new FCSDirectTriangleMeshSceneProxy(this, PendingPrepared, PendingVertexCapacity);
}
