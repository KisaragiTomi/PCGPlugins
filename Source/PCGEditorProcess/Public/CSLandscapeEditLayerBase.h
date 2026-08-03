#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ComputeShaderMeshGenerator.h"
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
class PCGEDITORPROCESS_API ACSLandscapeEditLayerBase : public AComputeShaderMeshGenerator
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

	/** Opacity of this actor's Edit Layer heightmap contribution, mapped to the Landscape layer Alpha
	 *  (-1..1). Drive it from the Construction Script via SetEditLayerAlpha(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeEditLayer",
		meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float EditLayerAlpha = 1.0f;

	/** Set EditLayerAlpha and push it to the owning Landscape edit layer immediately (Blueprint-callable). */
	UFUNCTION(BlueprintCallable, Category = "LandscapeEditLayer")
	void SetEditLayerAlpha(float InAlpha);

	virtual void PostActorCreated() override;
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

	/** Push EditLayerAlpha to the owning Landscape edit layer (no-op until the layer exists). */
	void ApplyEditLayerAlpha();

	/**
	 * Bake a render target into the Landscape heightmap (persistent layer 0).
	 * Shared by every subclass Commit; differences are passed in.
	 *
	 * @param SourceRT                 RT holding the world-space height (channel selected by bHeightInAlpha).
	 * @param LandscapeData            Region descriptor (TextureValidSize + ReadRange).
	 * @param bClearLayerFirst         Clear the target layer's heightmap before writing.
	 * @param bWriteAlphaBlendAndFlags Also write zeroed AlphaBlend/Flags channels.
	 * @param bHeightInAlpha           true: height in Alpha (legacy). false: height in Red (Copy/merge convention).
	 */
	void BakeResultToLandscape(
		UTextureRenderTarget2D* SourceRT,
		const FCSReadLandscapeData& LandscapeData,
		bool bClearLayerFirst,
		bool bWriteAlphaBlendAndFlags,
		bool bHeightInAlpha = true);

	/** Bake a merge-pipeline texture whose landscape uint16 height is packed in normalized RG. */
	void BakePackedHeightToLandscape(UTextureRenderTarget2D* SourceRT, const FIntRect& SectionRect);

	// --- Subclass customization hooks ---

	/** Whether this layer participates in heightmap merging at all. Default true. */
	virtual bool ShouldSupportHeightmap() const { return true; }

	/** Whether the merge contribution is currently enabled. Default mirrors bHasResult. */
	virtual bool IsLayerEnabledForMerge() const { return bHasResult; }
};
