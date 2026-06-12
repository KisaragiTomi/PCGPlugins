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
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LandscapeData")
	UTextureRenderTarget2D* RT_CopyLandscapeData = nullptr;

	USceneComponent* SceneComponent;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LandscapeData")
	UBoxComponent* Box;

	FCSReadLandscapeData Orig_LandscapeData;

	FCSReadLandscapeData Copy_LandscapeData;

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

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CopyLandscapeData();

	/** Non-destructive preview: computes the paste blend into RT_Result without writing to landscape. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void PasteLandscapeData();

	/** Permanently bake the current RT_Result into the Landscape heightmap. */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CommitToLandscape();

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
#endif

protected:
	/** Persistent copy of RT_Result, serialized with the actor so data survives level save/load. */
	UPROPERTY()
	UTexture2D* PersistentResult = nullptr;

	/** Only contribute to heightmap merge when enabled by the user. */
	virtual bool ShouldSupportHeightmap() const override { return bAffectHeightmap; }

	/** Blend CS result into the Merge pipeline's scratch RT (called from RenderLayer). */
	void ApplyResultToCombined(UTextureRenderTarget2D* InCombinedResult, UTextureRenderTarget2D* OutResult, const FIntPoint& Size);

	/** Save RT_Result contents into PersistentResult (UTexture2D, serialized with level). */
	void SaveResultToPersistent();

	/** Restore RT_Result from PersistentResult on level load. */
	void RestoreResultFromPersistent();

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


