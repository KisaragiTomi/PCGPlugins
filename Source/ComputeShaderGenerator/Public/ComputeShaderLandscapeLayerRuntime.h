#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderLandscapeLayerRuntime.generated.h"

class UMaterialInterface;
class UTextureRenderTarget2D;

UENUM(BlueprintType)
enum class ERuntimeLayerBlendMode : uint8
{
	Alpha         UMETA(DisplayName = "Alpha Lerp"),
	Additive      UMETA(DisplayName = "Additive"),
	Subtract      UMETA(DisplayName = "Subtract"),
	Multiply      UMETA(DisplayName = "Multiply"),
	MaterialDrive UMETA(DisplayName = "Material Driven (Per-pixel blend)")
};

UENUM(BlueprintType)
enum class ERuntimeLayerGenerationMode : uint8
{
	Noise          UMETA(DisplayName = "Procedural Noise"),
	NoiseErosion   UMETA(DisplayName = "Noise + Thermal Erosion"),
	RiverBed       UMETA(DisplayName = "River Bed Carving"),
	CustomHeight   UMETA(DisplayName = "Custom Height Field (from RT)")
};

USTRUCT(BlueprintType)
struct FRuntimeLayerSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	FName LayerName = TEXT("RuntimeLayer");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	ERuntimeLayerBlendMode BlendMode = ERuntimeLayerBlendMode::Alpha;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings")
	UMaterialInterface* LayerMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|MaterialBlend")
	UTexture2D* MaterialBlendTexture = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|MaterialBlend",
		meta = (DisplayName = "Alpha Multiplier"))
	float GlobalAlpha = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation")
	float NoiseFrequency = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation")
	float NoiseAmplitude = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation")
	float FalloffWidth = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation")
	int32 ErosionIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation")
	float ErosionStrength = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings|Generation")
	ERuntimeLayerGenerationMode GenerationMode = ERuntimeLayerGenerationMode::Noise;
};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSRuntimeLandscapeLayer : public ACSRangeGenerator
{
	GENERATED_BODY()
public:
	ACSRuntimeLandscapeLayer();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LayerSettings",
		meta = (DisplayName = "Layer Settings"))
	FRuntimeLayerSettings LayerSettings;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData",
		meta = (DisplayName = "Original Landscape Data (read-only after ReadLandscapeData)"))
	UTextureRenderTarget2D* RT_OrigLandscapeData;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData",
		meta = (DisplayName = "Generated Layer Alpha (R) + Height (A) RT"))
	UTextureRenderTarget2D* RT_LayerAlphaHeight;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData",
		meta = (DisplayName = "Blended Result RT"))
	UTextureRenderTarget2D* RT_BlendResult;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData",
		meta = (DisplayName = "Debug View RT"))
	UTextureRenderTarget2D* RT_DebugView;

	FCSReadLandscapeData Orig_LandscapeData;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	FVector LandscapeTexMinUV = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	FVector LandscapeTexUVRange = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	FVector MapMin = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	FVector MapMax = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	FGuid LandscapeGuid;

	UPROPERTY(BlueprintReadWrite, Category = "RuntimeData")
	bool bInitialized = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime|Landscape",
		meta = (DisplayName = "Target Landscape Actor (auto-find if none)"))
	ALandscape* TargetLandscape;

	/** When false, ApplyToLandscape() is skipped in the pipeline. Call CommitToLandscape() explicitly to permanently write. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime|Landscape",
		meta = (DisplayName = "Auto Commit (permanently write on apply)"))
	bool bAutoCommit = false;

	UPROPERTY(BlueprintReadOnly, Category = "Runtime|Landscape")
	bool bHasRuntimeResult = false;

public:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	void InitRuntimeRT();

	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	void ReadLandscapeData();

	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	void GenerateLayerData();

	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	void BlendLayer();

	/** If bAutoCommit is true, permanently writes to landscape. Otherwise just marks result as ready. */
	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	void ApplyToLandscape();

	/** Force-commit the current result to the landscape heightmap regardless of bAutoCommit. */
	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	void CommitToLandscape();

	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	void FullRuntimePipeline();

	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	ALandscape* FindOrCreateLandscape();

	UFUNCTION(BlueprintCallable, Category = "RuntimeLandscapeLayer")
	static FName GetBlendModeName(ERuntimeLayerBlendMode Mode);

protected:
	bool FindLandscape();
	void ReadRuntimeLandscapeData(FCSReadLandscapeData& LandscapeData, FVector Center, FVector Extent, int32 ExtentPlus = 1);
	void GenerateLayerData_Noise(float InNoiseFrequency, float InNoiseAmplitude);
	void GenerateLayerData_Erosion(int32 Iterations, float Strength);
};

USTRUCT(BlueprintType)
struct FLandscapeLayerBlendResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Result")
	UTextureRenderTarget2D* BlendResult = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Result")
	UTextureRenderTarget2D* AlphaMask = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Result")
	FVector2D ValidUVRange = FVector2D::Zero();

	UPROPERTY(BlueprintReadOnly, Category = "Result")
	FBox WorldBounds = FBox(EForceInit::ForceInitToZero);
};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSRuntimeLandscapeLayerManager : public AActor
{
	GENERATED_BODY()
public:
	ACSRuntimeLandscapeLayerManager();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ManagerSettings")
	ALandscape* TargetLandscape;

	UPROPERTY(BlueprintReadWrite, Category = "ManagerSettings")
	TArray<ACSRuntimeLandscapeLayer*> ActiveLayers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ManagerSettings",
		meta = (DisplayName = "Auto-find Landscape on BeginPlay"))
	bool bAutoFindLandscape = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ManagerSettings",
		meta = (DisplayName = "Auto-run pipeline on BeginPlay"))
	bool bAutoRunOnBeginPlay = false;

	UFUNCTION(BlueprintCallable, Category = "LayerManager")
	void AddLayer(ACSRuntimeLandscapeLayer* Layer);

	UFUNCTION(BlueprintCallable, Category = "LayerManager")
	void RemoveLayer(ACSRuntimeLandscapeLayer* Layer);

	UFUNCTION(BlueprintCallable, Category = "LayerManager")
	void RunAllLayers();

	UFUNCTION(BlueprintCallable, Category = "LayerManager")
	void ClearAllLayers();

	UFUNCTION(BlueprintCallable, Category = "LayerManager")
	FLandscapeLayerBlendResult BlendMultipleLayers(const TArray<FLandscapeLayerBlendResult>& InLayerResults, ERuntimeLayerBlendMode FinalBlendMode);

protected:
	virtual void BeginPlay() override;
};
