#pragma once

#include "CoreMinimal.h"
#include "CSLandscapeEditLayerBase.h"
#include "ComputeShaderBasicFunction.h"
#include "Components/BoxComponent.h"
#include "ComputeShaderLandscapeLayer.generated.h"

class UMaterialInterface;
class UTextureRenderTarget2D;
class UTexture2D;

UCLASS()
class PCGEDITORPROCESS_API ACSLandscapeLayer : public ACSLandscapeEditLayerBase
{
	GENERATED_BODY()
public:
	ACSLandscapeLayer();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	FName LayerName = TEXT("PCG_TempLayer");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	ECSLandscapeBlendMode BlendMode = ECSLandscapeBlendMode::Alpha;

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

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData",
		meta = (DisplayName = "Original Landscape Data (immutable)"))
	UTextureRenderTarget2D* RT_OrigLandscapeData;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_LayerAlpha;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_LayerGeneratedHeight;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_LayerBlendResult;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	UTextureRenderTarget2D* RT_DebugView;

	// Not BlueprintType — used internally for data passing
	FCSReadLandscapeData Orig_LandscapeData;

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

#if WITH_EDITOR
	// --- ILandscapeEditLayerRenderer interface (only the per-subclass parts) ---
	virtual bool RenderLayer(
		UE::Landscape::EditLayers::FRenderParams& RenderParams,
		UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder) override;

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
