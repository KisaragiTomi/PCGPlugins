#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderGenerateHepler.h"
#include "ComputeShaderMeshGenerator.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/DecalComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "TimerManager.h"

#define CSSW_VELOCITY_CLAMP 4

class FObjectPreSaveContext;

#include "ComputeShaderShallowWater.generated.h"

UENUM(BlueprintType)
enum class EWaterfallExpansion : uint8
{
	Expansion_5  = 0 UMETA(DisplayName = "5"),
	Expansion_7  = 1 UMETA(DisplayName = "7"),
	Expansion_10 = 2 UMETA(DisplayName = "10"),
	MAX          = 3 UMETA(Hidden),
};



UCLASS(HideCategories=(Replication), meta=(PrioritizeCategories="SWParameter"))
class COMPUTESHADERGENERATOR_API ACSShallowWaterCapture : public AComputeShaderMeshGenerator
{
	GENERATED_BODY()
public:
	ACSShallowWaterCapture(const FObjectInitializer& ObjectInitializer);

	// -------------------------------------------------------------------------
	// Components
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UStaticMeshComponent* ReusltMesh;

	UPROPERTY(Transient, BlueprintReadWrite, DuplicateTransient)
	UHierarchicalInstancedStaticMeshComponent* SimVisHISM;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UDecalComponent* CausticsDecal;

	// -------------------------------------------------------------------------
	// Simulation Parameters
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* WaterMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* DecalMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	bool CloseBound = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000, ClampMin="1", ClampMax="32", UIMin="1", UIMax="8"))
	int32 Iteration = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	int32 HeightSmoothIteration = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	EWaterfallExpansion WaterfallExpansionIterations = EWaterfallExpansion::Expansion_10;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float DT = .1;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float Friction = 0.005;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float AdvectFoam = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float FoamFadeSpeed = 0.001;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter",
		Meta=(Priority=1000, ClampMin="0", UIMin="0",
			  ToolTip="Max height gap (cm) before a cell is clamped to its neighbours. 0 = disabled."))
	float SpikeClampHeight = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float MaxWaterRisePerFrame = 40.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float WorldPixelSize = 40;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float SeaLevel = -1000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	FName SWCaptureTag = FName("CSSW");

	// -------------------------------------------------------------------------
	// Capture Parameters
	// -------------------------------------------------------------------------

	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	FName SWTag = FName("CSSW_Bake");

	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float MaxHeight = 10000;

	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float Scale3DZ = 100;

	int32 TextureSize = 256;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float CaptureSize = 10000;

	// -------------------------------------------------------------------------
	// Bake System
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=1000))
	bool bUseBakedResultMesh = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(EditCondition="bUseBakedResultMesh", Priority=1000))
	UStaticMesh* BakedResultMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=999))
	UStaticMesh* SimulationPreviewMesh = nullptr;

	// -------------------------------------------------------------------------
	// Simulation Runtime (Transient)
	// -------------------------------------------------------------------------

	UPROPERTY(Transient, NonTransactional, EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_DebugView;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_VelocityHeight;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultVelHeight;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultDepthWet;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SmoothHeight;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SceneDepth = nullptr;

	UPROPERTY(Transient, NonTransactional)
	UTextureRenderTarget2D* RT_TileMask = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	FVector2D SimUVCenter;

	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	float SimUVSize = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	float SimUVInvSize = 0.f;

	FTimerHandle ConstructionDebounceHandle;
	int32 SolverGeneration = 1;
	uint64 LastSolverFrameNumber = 0;

	// -------------------------------------------------------------------------
	// Debug
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	int32 SWUniqueID = -99999;

	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UStaticMesh* DebugMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|DebugViewPlane")
	UMaterialInterface* DebugViewPlaneMaterial;

	// -------------------------------------------------------------------------
	// Public Functions
	// -------------------------------------------------------------------------

	virtual void PostLoad() override;
	virtual void BeginPlay() override;
	virtual void PreSave(FObjectPreSaveContext ObjectSaveContext) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
	virtual void BeginDestroy() override;
	virtual void OnConstruction(const FTransform& Transform) override;

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ConstructionComponent();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	bool CheckAndCreateTexture_SWSourcePoint()
	{
		if (!GetWorld() || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) return false;

		const int32 SafeTextureSize = FMath::RoundUpToPowerOfTwo(
			FMath::Max(16, FMath::CeilToInt32(CaptureSize / FMath::Max(WorldPixelSize, 1.0f))));
		TextureSize = SafeTextureSize;

		if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_VelocityHeight == nullptr) RT_VelocityHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_ResultVelHeight == nullptr) RT_ResultVelHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_SmoothHeight == nullptr) RT_SmoothHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f,FLinearColor(-9999, -9999, -9999, -9999), true, false);
		if (!RT_DebugView || !RT_VelocityHeight || !RT_ResultVelHeight || !RT_SmoothHeight) return false;

		if (RT_ResultDepthWet == nullptr)
		{
			RT_ResultDepthWet = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(-9999, -9999, -9999, -9999), true, false);
			if (!RT_ResultDepthWet) return false;
		}
		SetMaterialParameter();
		return true;
	}

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ShallowWaterSolverSoucePoint(int32 InIteration);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CaptureAll();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void SetHeight();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void HeightSmooth();

	UFUNCTION(BlueprintNativeEvent)
	void SetMaterialParameter();
	virtual void SetMaterialParameter_Implementation();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void ReleaseTransientRenderResources();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void Clean();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader", Meta=(ClampMin="0", UIMin="0"))
	void StartSolver(float TimerRate = 0.0f,
		UPARAM(meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "8")) int32 InIteration = 1);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void StopSolver();

	UFUNCTION(BlueprintNativeEvent, Category = "ComputeShader")
	void OnSolverStarted();
	virtual void OnSolverStarted_Implementation();

	DECLARE_DELEGATE_OneParam(FOnBakeResultMesh, ACSShallowWaterCapture*);
	static FOnBakeResultMesh OnBakeResultMeshDelegate;

	UFUNCTION(BlueprintCallable, Category = "SWParameter", Meta=(DevelopmentOnly))
	void BakeResultMesh();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	void BrowseBakedAssets();

	UFUNCTION(BlueprintNativeEvent, Category = "ComputeShader|Bake")
	void OnBakeComplete();
	virtual void OnBakeComplete_Implementation();
	
	UFUNCTION(BlueprintCallable, Category = "Debug|DebugViewPlane")
	void ShowDebugViewPlane(float Duration = 5.0f);

private:
	void ClearSolverTimer();
	void ScheduleSolverTimerTick();
	void StopSimulationRuntime();
	void ResetSolverState(bool bAdvanceGeneration);
	bool CleanRenderTargets();
	void RefreshConstructionLayout();
	void RebuildSimulationVisualization();
	void ShowBakedResult();
	void UpdateSimUV();
	void EnsureTileMask(int32 Width, int32 Height);
	void EnsureRTSizes();

	TWeakObjectPtr<AActor> DebugViewPlaneActor;
	FTimerHandle DebugViewPlaneTimerHandle;
	FTimerHandle SolverTimerHandle;
	float SolverTimerRate = 0.0f;
};




UCLASS()
class COMPUTESHADERGENERATOR_API ACSSHallowWaterSource : public AActor
{
	GENERATED_BODY()
};
