#pragma once
//
#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ComputeShaderMeshFill.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderSceneCapture.h"
#include "ComputeShaderShallowWater.h"
#include "CSLandscapeEditLayerBase.h"
#include "LandscapeExtra.h"
#include "Components/BoxComponent.h"
//
#include "ComputeShaderLandscape.generated.h"
//

class USplineComponent;
class ALandscape;

UENUM(BlueprintType)
enum class ETempLayerSourceMode : uint8
{
	ExternalRT UMETA(DisplayName = "External RT (Set from code/BP)"),
	FlatOffset UMETA(DisplayName = "Flat Height Offset"),
	ProceduralNoise UMETA(DisplayName = "Procedural Noise Generation")
};

UCLASS()
class PCGEDITORPROCESS_API ACSLandscape : public ACSLandscapeEditLayerBase
{
GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	bool bAffectHeightmap = true;

	/** Auto-created render targets — read from Blueprint, no need to assign manually. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_DebugView = nullptr;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_Result = nullptr;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_LandscapeData = nullptr;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LandscapeData|Realtime")
	UTextureRenderTarget2D* RT_RealtimeResult = nullptr;

	USceneComponent* SceneComponent;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LandscapeData")
	UBoxComponent* Box;

	FCSReadLandscapeData Orig_LandscapeData;

	FVector BoxMin = FVector::ZeroVector;
	FVector BoxMax = FVector::ZeroVector;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector LandscapeTexMinUV = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector LandscapeTexUVRange = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector MapMin = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FVector MapMax = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData")
	float BlurRange = .25;

	// --- Realtime procedural layer ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime")
	bool bRealtimeUpdate = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Source")
	ETempLayerSourceMode SourceMode = ETempLayerSourceMode::FlatOffset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Source",
		meta = (DisplayName = "Height Offset (cm)", EditCondition = "SourceMode == ETempLayerSourceMode::FlatOffset"))
	float HeightOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ProceduralNoise"))
	float NoiseFrequency = 0.005f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ProceduralNoise"))
	float NoiseAmplitude = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ProceduralNoise"))
	int32 NoiseOctaves = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Source",
		meta = (EditCondition = "SourceMode == ETempLayerSourceMode::ExternalRT"))
	UTextureRenderTarget2D* ExternalHeightRT = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Blend")
	ECSLandscapeBlendMode BlendMode = ECSLandscapeBlendMode::Additive;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Blend", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LayerAlpha = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Blend", meta = (DisplayName = "Falloff Width (cm)"))
	float FalloffWidth = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData|Realtime|Debug")
	bool bShowDebugView = false;

	UPROPERTY( BlueprintReadWrite, Category = "LandscapeData")
	UDynamicMeshComponent* VisMesh;

	ACSLandscape();

	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void InitRT();

	/** Ensure all RTs exist and match the given landscape data size. */
	void EnsureRTs(int32 SizeX, int32 SizeY);

	virtual bool IsParameterValidMult()
	{
		return Box != nullptr;
	}

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ReadLandscapeDataToTexture();

	/** Capture the Landscape under the box and immediately publish it to this actor's Edit Layer. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CopyLandscapeData();

	/** Bake the current RT_Result permanently into the Landscape base heightmap (box region only, surrounding
	 *  terrain preserved), then delete this actor's Edit Layer. Replaces the old Paste action. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void BakeLandscape();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Realtime")
	void SetExternalHeightRT(UTextureRenderTarget2D* InRT);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Realtime")
	void RefreshLayer();

	/** Generic "an input height RT deforms the terrain": routes HeightRT through this actor's
	 *  realtime external-RT blend so the landscape edit layer picks it up. Subclasses that produce
	 *  a height RT (e.g. the road) call this to affect the terrain. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ApplyHeightmapRTToLandscape(UTextureRenderTarget2D* HeightRT);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void BP_InitRT();

	/**
	 * Debug: export any RT as a UTexture2D asset in the current level's package folder.
	 * Overwrites existing asset with the same name.
	 * @param InRT          The render target to export.
	 * @param AssetName     Asset name (e.g. "RT_Result_Debug"). Saved next to the level.
	 */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Debug")
	void DebugExportRT(UTextureRenderTarget2D* InRT, const FString& AssetName = TEXT("RT_Debug"));

#if WITH_EDITOR
	// --- ILandscapeEditLayerRenderer interface (only the per-subclass parts) ---
	virtual bool RenderLayer(
		UE::Landscape::EditLayers::FRenderParams& RenderParams,
		UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder) override;

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

protected:
	/** Persistent copy of RT_Result, serialized with the actor so data survives level save/load. */
	UPROPERTY()
	UTexture2D* PersistentResult = nullptr;

	/** Only contribute to heightmap merge when enabled by the user. */
	virtual bool ShouldSupportHeightmap() const override { return bAffectHeightmap; }
	virtual bool IsLayerEnabledForMerge() const override { return bRealtimeUpdate || bHasResult; }

	/** Record Copy and realtime blends into the Landscape merge graph in dependency order. */
	void EnqueueLayerMerge(
		UTextureRenderTarget2D* InCombinedResult,
		UTextureRenderTarget2D* OutResult,
		const FIntPoint& Size,
		const FTransform& RenderAreaWorldTransform,
		bool bApplyCachedResult,
		bool bApplyRealtime,
		UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder);

	/** Save RT_Result contents into PersistentResult (UTexture2D, serialized with level). */
	void SaveResultToPersistent();

	/** Restore RT_Result from PersistentResult on level load. */
	void RestoreResultFromPersistent();

	FIntRect LastRenderAreaSectionRect;

	void EnsureRealtimeRTs(const FIntPoint& InSize);

public:
	virtual void PostLoad() override;
};


UCLASS()
class PCGEDITORPROCESS_API ACSLandscapeRiver : public ACSLandscape
{
GENERATED_BODY()
public:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LandscapeData")
	USplineComponent* SplineComponent;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_SplineRotateDist = nullptr;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_SplineGradientHeight = nullptr;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData", Meta=(Priority=1000))
	float TargetRiverWidth = 200;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LandscapeData", Meta=(Priority=1000))
	float RiverDepth = 200;
	
	ACSLandscapeRiver();

	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void InitRT() override; 

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ProjectLineToLandscape();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void RecenterSpline();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void GenerateRiverBed();
	
	void BoxMatchSpline();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void SimRiver(TSubclassOf<AActor> ActorClass, int32 SimIteration, FVector SourcePoint, float Size = .05);


};


