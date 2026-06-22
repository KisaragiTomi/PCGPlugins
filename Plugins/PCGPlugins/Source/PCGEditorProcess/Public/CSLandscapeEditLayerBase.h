#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ComputeShaderBasicFunction.h"
#include "LandscapeEditLayerRenderer.h"
#include "LandscapeEditLayerRendererState.h"
#include "LandscapeEditLayerTargetTypeState.h"
#include "LandscapeEditLayerTypes.h"
#include "LandscapeEditTypes.h"
#include "CSLandscapeEditLayerBase.generated.h"

class ALandscape;
class UTextureRenderTarget2D;

/**
 * Shared base for all CS-driven Landscape Edit Layer actors.
 *
 * Owns the unified Edit Layer hookup (a per-actor UCSLandscapeEditLayer slot tracked by
 * OwnedEditLayerGuid), the merge-pipeline renderer boilerplate, lifecycle cleanup, and the
 * common "read RT -> uint16 height -> SetHeightData" bake path.
 *
 * Subclasses only override RenderLayer() (their merge contribution) and the generation/blend
 * that fills their own render targets.
 */
UCLASS(Abstract)
class PCGEDITORPROCESS_API ACSLandscapeEditLayerBase : public AActor
#if CPP && WITH_EDITOR
	, public ILandscapeEditLayerRenderer
#endif
{
	GENERATED_BODY()

public:
	ACSLandscapeEditLayerBase();

	/** Request the owning Landscape to re-merge all edit layers. */
	UFUNCTION(BlueprintCallable, Category = "LandscapeEditLayer")
	void RequestLandscapeUpdate(bool bInUserTriggered = false);

	virtual void PostLoad() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

#if WITH_EDITOR
	// --- ILandscapeEditLayerRenderer interface (shared boilerplate) ---
	virtual FString GetEditLayerRendererDebugName() const override;

	virtual void GetRendererStateInfo(
		const UE::Landscape::EditLayers::FMergeContext* InMergeContext,
		UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutSupportedTargetTypeState,
		UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutEnabledTargetTypeState,
		TArray<UE::Landscape::EditLayers::FTargetLayerGroup>& OutTargetLayerGroups) const override;

	virtual TArray<UE::Landscape::EditLayers::FEditLayerRenderItem> GetRenderItems(
		const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const override;

	virtual UE::Landscape::EditLayers::ERenderFlags GetRenderFlags(
		const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const override;
#endif

protected:
	/** GUID of the Edit Layer this actor owns on the Landscape. */
	UPROPERTY()
	FGuid OwnedEditLayerGuid;

	/** True once this actor has a renderable merge contribution. Drives the merge enabled state. */
	bool bHasResult = false;

	friend class UCSLandscapeEditLayer;

	/** Find the first ALandscape in the world. */
	ALandscape* FindLandscape() const;

	/** Ensure this actor owns a UCSLandscapeEditLayer slot on the Landscape; create if missing. */
	void EnsureEditLayer();

	/** Remove this actor's owned Edit Layer from the Landscape. */
	void RemoveEditLayer();

	/**
	 * Bake a render target into the Landscape heightmap (persistent layer 0).
	 * Shared by every subclass Commit; differences are passed in.
	 *
	 * @param SourceRT                 RT whose Alpha channel holds the world-space height.
	 * @param LandscapeData            Region descriptor (TextureValidSize + ReadRange).
	 * @param bClearLayerFirst         Clear the target layer's heightmap before writing.
	 * @param bWriteAlphaBlendAndFlags Also write zeroed AlphaBlend/Flags channels.
	 */
	void BakeResultToLandscape(
		UTextureRenderTarget2D* SourceRT,
		const FCSReadLandscapeData& LandscapeData,
		bool bClearLayerFirst,
		bool bWriteAlphaBlendAndFlags);

	// --- Subclass customization hooks ---

	/** Whether this layer participates in heightmap merging at all. Default true. */
	virtual bool ShouldSupportHeightmap() const { return true; }

	/** Whether the merge contribution is currently enabled. Default mirrors bHasResult. */
	virtual bool IsLayerEnabledForMerge() const { return bHasResult; }
};