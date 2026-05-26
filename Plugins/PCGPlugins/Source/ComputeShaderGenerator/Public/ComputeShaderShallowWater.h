#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderGenerateHepler.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/DecalComponent.h"
#include "RenderGraphBuilder.h"
#include "RHIGPUReadback.h"
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
class COMPUTESHADERGENERATOR_API ACSShallowWaterCapture : public AActor
{
	GENERATED_BODY()
public:
	ACSShallowWaterCapture();
	virtual void Tick(float DeltaTime) override;
	virtual bool ShouldTickIfViewportsOnly() const override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_DebugView;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_VelocityHeight;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultVelHeight;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultDepthWet;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_Source;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SceneDepth;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SmoothHeight;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	int32 SWUniqueID = -99999;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	bool CleanDepthWet = true;


	USceneComponent* SceneComponent;
	UBoxComponent* Box;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	USceneCaptureComponent2D* CaptureSceneDepth;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UStaticMesh* DebugMesh;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UStaticMeshComponent* ReusltMesh;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=1000))
	bool bUseBakedResultMesh = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(EditCondition="bUseBakedResultMesh", Priority=1000))
	UStaticMesh* BakedResultMesh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=999))
	UStaticMesh* SimulationPreviewMesh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=999))
	UMaterialInterface* SimulationWaterMaterial = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=999))
	UMaterialInterface* SimulationDecalMaterial = nullptr;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UHierarchicalInstancedStaticMeshComponent* SimVisHISM;
	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UDecalComponent* CausticsDecal;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* WaterMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* DecalMaterial;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* VisWaterMaterial;
	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* VisDecalMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	bool CloseBound = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000, ClampMin="1", UIMin="1"))
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float WorldPixelSize = 40;
	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float SeaLevel = -1000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	FName SWCaptureTag = FName("CSSW");

	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	FName SWTag = FName("CSSW_Bake");

	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float MaxHeight = 10000;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float Scale3DZ = 100;
	float TextureSize = 256;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float CaptureSize = 2000;

	virtual void PostLoad() override;
	virtual void BeginPlay() override;
	virtual void PreSave(FObjectPreSaveContext ObjectSaveContext) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void OnConstruction(const FTransform& Transform) override;
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	bool CheckAndCreateTexture_SWSourcePoint()
	{
		if (RT_SceneDepth == nullptr) RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor::Black, true, false);
		CaptureSceneDepth->TextureTarget = RT_SceneDepth;
		if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_VelocityHeight == nullptr) RT_VelocityHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_ResultVelHeight == nullptr) RT_ResultVelHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_Source == nullptr) RT_Source = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(MaxHeight, 0, 0, 0), true, false);
		if (RT_SmoothHeight == nullptr) RT_SmoothHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA32f,FLinearColor(-9999, -9999, -9999, -9999), true, false);

		if (RT_ResultDepthWet == nullptr)
		{
			RT_ResultDepthWet = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(-9999, -9999, -9999, -9999), true, false);
			UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultDepthWet,  FLinearColor(-9999, -9999, -9999, -9999));
			CleanDepthWet = true;
		}
		else
		{
			if (CleanDepthWet)
			{
				UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_ResultDepthWet,  FLinearColor(-9999, -9999, -9999, -9999));
				CleanDepthWet = false;
			}
		}
		SetMaterialParameter();
		return true;
	}
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ConstructionComponent();
	
	UFUNCTION(BlueprintPure, Category = "SWParameter")
	int32 GetWaterfallExpansionCount() const
	{
		constexpr int32 LUT[] = { 5, 7, 10 };
		const int32 Idx = FMath::Clamp(static_cast<int32>(WaterfallExpansionIterations), 0, 2);
		return LUT[Idx];
	}

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ShallowWaterSolverSoucePoint(int32 InIteration);


	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void SetHeight();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void HeightSmooth();
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void Clean();
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CleanDepthWet_Construct();
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	TArray<FVector4> GetSources();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CleanupAttachedActors();
	
	UFUNCTION(BlueprintNativeEvent)
	void SetMaterialParameter();
	virtual void SetMaterialParameter_Implementation();
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CaptureSceneDepthNow();

	UPROPERTY()
	UTextureRenderTarget2D* RT_TileMask = nullptr;
	int32 CachedActiveTileCount = 0;
	TArray<uint8> CachedTileBits;
	TArray<int32> ISMTileSlots;
	static constexpr int32 ReadbackBufferCount = 2;
	FRHIGPUTextureReadback* TileMaskReadback[ReadbackBufferCount] = {};
	int32 TileMaskReadbackWriteIdx = 0;
	int32 TileMaskReadbackWidth = 0;
	int32 TileMaskReadbackHeight = 0;
	int32 TileMaskReadbackCopyWidth[ReadbackBufferCount] = {};
	int32 TileMaskReadbackCopyHeight[ReadbackBufferCount] = {};
	int32 TileMaskReadbackGeneration[ReadbackBufferCount] = {};

	FRHIGPUTextureReadback* ResultReadback[ReadbackBufferCount] = {};
	int32 ResultReadbackWriteIdx = 0;
	int32 ResultReadbackCopyWidth[ReadbackBufferCount] = {};
	int32 ResultReadbackCopyHeight[ReadbackBufferCount] = {};
	int32 ResultReadbackGeneration[ReadbackBufferCount] = {};
	int32 SolverReadbackGeneration = 1;
	TArray<FFloat16Color> CachedResultPixels;
	int32 CachedResultWidth = 0;
	int32 CachedResultHeight = 0;

	FTimerHandle ConstructionDebounceHandle;
	uint64 LastSolverFrameNumber = 0;

	UPROPERTY(Transient, EditAnywhere, BlueprintReadWrite, Category = "Debug|RenderDoc", Meta=(Priority=999))
	bool bCaptureNextSolverFrame = false;

	UFUNCTION(BlueprintCallable, Category = "Debug|RenderDoc")
	void RequestRenderDocCapture();

	UFUNCTION(BlueprintCallable, Category = "Debug|RenderDoc")
	void ShallowWaterSolverSoucePointWithCapture(int32 InIteration);

	UFUNCTION(BlueprintNativeEvent, Category = "ComputeShader")
	void OnSolverStarted();
	virtual void OnSolverStarted_Implementation();

	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	FVector2D SimUVCenter;
	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	float SimUVSize = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	float SimUVInvSize = 0.f;

	UFUNCTION(BlueprintPure, Category = "SimVis")
	void GetSimUVParams(FVector2D& OutCenter, float& OutSize, float& OutInvSize) const
	{
		OutCenter = SimUVCenter;
		OutSize = SimUVSize;
		OutInvSize = SimUVInvSize;
	}

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader", Meta=(ClampMin="0", UIMin="0"))
	void StartSolver(float TimerRate = 0.0f);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void StopSolver();

	DECLARE_DELEGATE_OneParam(FOnBakeResultMesh, ACSShallowWaterCapture*);
	static FOnBakeResultMesh OnBakeResultMeshDelegate;

	UFUNCTION(CallInEditor, Category = "SWParameter", Meta=(DevelopmentOnly))
	void BakeResultMesh();

	UFUNCTION(BlueprintNativeEvent, Category = "ComputeShader|Bake")
	void OnBakeComplete();
	virtual void OnBakeComplete_Implementation();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	void UseBakedResultMesh(UStaticMesh* InBakedMesh, UMaterialInterface* InWaterMaterial, UMaterialInterface* InDecalMaterial);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	void UseSimulationResultMesh();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader", Meta=(ClampMin="1", UIMin="1"))
	void ToggleSimVisualization(int32 SimIterationsPerFrame = 1);

	UPROPERTY(Transient, BlueprintReadOnly, Category = "ComputeShader")
	bool bSimVisActive = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|DebugViewPlane")
	UMaterialInterface* DebugViewPlaneMaterial;

	UFUNCTION(BlueprintCallable, Category = "Debug|DebugViewPlane")
	void ShowDebugViewPlane(float Duration = 5.0f);

private:
	void ClearSolverTimer();
	bool IsSolverTimerActive() const;
	void ScheduleSolverTimerTick();
	void HandleSolverTimerTick(int32 ExpectedSolverReadbackGeneration);
	void StopSimulationRuntime(bool bResetVisualization);
	void UpdateSimulationPreviewMesh();
	bool EnsureSimVisHISMReady();
	void ResetSimVisTiles();
	void ResetSolverReadbackState(bool bAdvanceGeneration, bool bClearCachedResult);

	TWeakObjectPtr<AActor> DebugViewPlaneActor;
	FTimerHandle DebugViewPlaneTimerHandle;
	FTimerHandle SolverTimerHandle;
	float SolverTimerRate = 0.0f;
};




UCLASS()
class COMPUTESHADERGENERATOR_API ACSSHallowWaterSource : public AActor
{
	GENERATED_BODY()
public:
	ACSSHallowWaterSource();
	
};
