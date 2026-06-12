#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderLandscape.h"
#include "LandscapeEditLayerRenderer.h"
#include "LandscapeEditLayerRendererState.h"
#include "LandscapeEditLayerTargetTypeState.h"
#include "LandscapeEditLayerTypes.h"
#include "LandscapeEditTypes.h"
#include "ComputeShaderLandscapeLayer.generated.h"

class UMaterialInterface;

UENUM(BlueprintType)
enum class ELandscapeLayerBlendMode : uint8
{
	Alpha         UMETA(DisplayName = "Alpha Lerp (Generated * Alpha + Normal * (1 - Alpha))"),
	Override      UMETA(DisplayName = "Override (Replace Normal with Generated)"),
	Additive      UMETA(DisplayName = "Additive (Normal + Generated * Alpha)"),
	Subtract      UMETA(DisplayName = "Subtract (Normal - Generated * Alpha)"),
	Multiply      UMETA(DisplayName = "Multiply (Normal * (Generated * Alpha + (1 - Alpha)))"),
	MaterialDrive UMETA(DisplayName = "Material Driven (Per-pixel blend from material texture)")
};

UCLASS()
class PCGEDITORPROCESS_API ACSLandscapeLayer : public AActor
#if CPP && WITH_EDITOR
	, public ILandscapeEditLayerRenderer
#endif
{
	GENERATED_BODY()
public:
	ACSLandscapeLayer();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	FName LayerName = TEXT("PCG_TempLayer");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	ELandscapeLayerBlendMode BlendMode = ELandscapeLayerBlendMode::Alpha;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	UMaterialInterface* LayerMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|MaterialBlend",
		meta = (DisplayName = "Alpha Texture (R=BlendFactor, G=HeightMod, B=NormalStrength)"))
	UTexture2D* MaterialBlendTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|MaterialBlend",
		meta = (DisplayName = "Alpha Multiplier (Global intensity control)"))
	float GlobalAlpha = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|MaterialBlend",
		meta = (DisplayName = "Height Modifier Multiplier"))
	float HeightModStrength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|MaterialBlend",
		meta = (DisplayName = "Normal Strength (0=flat, 1=full generated normal)"))
	float NormalStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation",
		meta = (DisplayName = "Falloff Width at brush edge (world units)"))
	float FalloffWidth = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation",
		meta = (DisplayName = "Noise Frequency"))
	float NoiseFrequency = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation",
		meta = (DisplayName = "Noise Amplitude (max height displacement)"))
	float NoiseAmplitude = 500.0f;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_LayerAlpha;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_LayerGeneratedHeight;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_LayerBlendResult;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_DebugView;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	FGuid LayerGuid;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	int32 LayerIndex = -1;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	bool bLayerCreated = false;

	// Not BlueprintType — used internally for data passing
	FReadLandscapeData Orig_LandscapeData;

	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	FVector LandscapeTexMinUV = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	FVector LandscapeTexUVRange = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	FVector MapMin = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	FVector MapMax = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	FVector NormalStrengthDebug = FVector(1, 0, 1);

protected:
	USceneComponent* SceneComponent;
	UBoxComponent* Box;

	UPROPERTY()
	FGuid OwnedEditLayerGuid;

	friend class UCSLandscapeEditLayer;

public:
	virtual void OnConstruction(const FTransform& Transform) override;

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void InitRT();

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void ReadLandscapeDataToTexture();

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void GenerateLayerAlphaAndHeight();

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void BlendLayerWithAlpha();

	/** Non-destructive: generates the blend result but does NOT write to landscape. Call CommitToLandscape() to permanently apply. */
	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void ApplyBlendedLayerToLandscape();

	/** Permanently bake the current blended result into the Landscape heightmap. */
	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void CommitToLandscape();

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void CreateLandscapeLayer();

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void RemoveLandscapeLayer();

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void FullPipeline();

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	static UTextureRenderTarget2D* CreateBlankAlphaTexture(UObject* WorldContextObject, int32 SizeX, int32 SizeY, FLinearColor ClearColor = FLinearColor::Black);

	UFUNCTION(BlueprintCallable, Category = "LandscapeLayer")
	void RequestLandscapeUpdate(bool bInUserTriggered = false);

#if WITH_EDITOR
	// --- ILandscapeEditLayerRenderer interface ---
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

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	bool bHasLayerResult = false;
	ALandscape* FindLandscape() const;
};
