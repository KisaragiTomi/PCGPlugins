#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "LandscapeEditLayerRenderer.h"
#include "LandscapeEditLayerRendererState.h"
#include "LandscapeEditLayerTargetTypeState.h"
#include "LandscapeEditLayerTypes.h"
#include "LandscapeEditTypes.h"
#include "Components/BoxComponent.h"
#include "ComputeShaderLandscapeTempLayer.generated.h"

class ALandscape;

UENUM(BlueprintType)
enum class ETempLayerBlendMode : uint8
{
	Additive     UMETA(DisplayName = "Additive (Height += Delta * Alpha)"),
	AlphaLerp    UMETA(DisplayName = "Alpha Lerp (Lerp toward generated height)"),
	Override     UMETA(DisplayName = "Override (Replace within alpha mask)"),
	Subtract     UMETA(DisplayName = "Subtract (Height -= Delta * Alpha)"),
	Multiply     UMETA(DisplayName = "Multiply (Height *= Generated * Alpha + (1-Alpha))")
};

UENUM(BlueprintType)
enum class ETempLayerSourceMode : uint8
{
	ExternalRT   UMETA(DisplayName = "External RT (Set from code/BP)"),
	FlatOffset   UMETA(DisplayName = "Flat Height Offset"),
	ProceduralNoise UMETA(DisplayName = "Procedural Noise Generation")
};

/**
 * Non-destructive CS-driven terrain modification layer.
 * Produces temporary changes through the Edit Layer Merge pipeline.
 * 
 * The modification is NEVER written to the underlying Landscape heightmap.
 * Remove or disable this actor to instantly revert the terrain change.
 */
UCLASS(Blueprintable, meta = (DisplayName = "CS Temp Landscape Layer"))
class PCGEDITORPROCESS_API ACSLandscapeTempLayer : public AActor
#if CPP && WITH_EDITOR
	, public ILandscapeEditLayerRenderer
#endif
{
	GENERATED_BODY()

public:
	ACSLandscapeTempLayer();

	// --- Source Configuration ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Source")
	ETempLayerSourceMode SourceMode = ETempLayerSourceMode::FlatOffset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Source",
		meta = (DisplayName = "Height Offset (cm)", EditCondition = "SourceMode == ETempLayerSourceMode::FlatOffset"))
	float HeightOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ProceduralNoise"))
	float NoiseFrequency = 0.005f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ProceduralNoise"))
	float NoiseAmplitude = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ProceduralNoise"))
	int32 NoiseOctaves = 6;

	/** External RT supplying per-pixel height delta. R channel = height delta (world units). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ExternalRT"))
	UTextureRenderTarget2D* ExternalHeightRT = nullptr;

	// --- Blend Settings ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Blend")
	ETempLayerBlendMode BlendMode = ETempLayerBlendMode::Additive;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Blend", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LayerAlpha = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Blend",
		meta = (DisplayName = "Falloff Width (cm)"))
	float FalloffWidth = 500.0f;

	// --- Debug ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Debug")
	UTextureRenderTarget2D* RT_DebugView = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TempLayer|Debug")
	bool bShowDebugView = false;

	// --- API ---

	UFUNCTION(BlueprintCallable, Category = "TempLayer")
	void SetExternalHeightRT(UTextureRenderTarget2D* InRT);

	UFUNCTION(BlueprintCallable, Category = "TempLayer")
	void RefreshLayer();

	/** Permanently bake the current temp layer result into the Landscape heightmap and destroy this actor. */
	UFUNCTION(BlueprintCallable, Category = "TempLayer")
	void CommitToLandscape();

	UFUNCTION(BlueprintCallable, Category = "TempLayer")
	void RequestLandscapeUpdate(bool bInUserTriggered = false);

	// --- Edit Layer Renderer Interface ---

#if WITH_EDITOR
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

	virtual bool RenderLayer(
		UE::Landscape::EditLayers::FRenderParams& RenderParams,
		UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder) override;
#endif

	virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	UPROPERTY()
	USceneComponent* SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TempLayer")
	UBoxComponent* BrushBox;

	UPROPERTY(Transient)
	UTextureRenderTarget2D* RT_InternalResult = nullptr;

	UPROPERTY()
	FGuid OwnedEditLayerGuid;

	friend class UCSLandscapeEditLayer;

	bool bHasResult = false;

	ALandscape* FindLandscape() const;
	void EnsureRTs(const FIntPoint& InSize);
	void RunBlendCS(UTextureRenderTarget2D* InCombinedResult, UTextureRenderTarget2D* OutResult, const FIntPoint& Size);
};
