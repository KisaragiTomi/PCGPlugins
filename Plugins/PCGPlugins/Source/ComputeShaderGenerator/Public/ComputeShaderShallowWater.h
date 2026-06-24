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
#include "RHIGPUReadback.h"
#include "TimerManager.h"
#include "Components/SceneCaptureComponent2D.h"

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

	UPROPERTY(Transient, NonTransactional, EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_DebugView;
	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_VelocityHeight;
	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultVelHeight;
	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultDepthWet;
	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_Source;
	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_VoxelTerrain;
	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SmoothHeight;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	int32 SWUniqueID = -99999;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	bool CleanDepthWet = true;


	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UStaticMesh* DebugMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Debug")
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
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debug")
	UHierarchicalInstancedStaticMeshComponent* SimVisHISM;
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Debug")
	UDecalComponent* CausticsDecal;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* WaterMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* DecalMaterial;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* VisWaterMaterial;
	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* VisDecalMaterial;
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
	virtual void Destroyed() override;
	virtual void BeginDestroy() override;
	virtual bool ShouldTickIfViewportsOnly() const override;
	virtual void Tick(float DeltaTime) override;

	virtual void OnConstruction(const FTransform& Transform) override;
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ConstructionComponent();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	bool CheckAndCreateTexture_SWSourcePoint()
	{
		if (!CanRunShallowWaterGPUWork(TEXT("CheckAndCreateTexture_SWSourcePoint"))) return false;

		const int32 SafeTextureSize = ResolveTextureSize();
		TextureSize = SafeTextureSize;

		if (RT_VoxelTerrain == nullptr) RT_VoxelTerrain = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor(MaxHeight + 9000, 0, 0, 1), false, true);
		if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_VelocityHeight == nullptr) RT_VelocityHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_ResultVelHeight == nullptr) RT_ResultVelHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_Source == nullptr) RT_Source = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(MaxHeight, 0, 0, 0), true, false);
		if (RT_SmoothHeight == nullptr) RT_SmoothHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f,FLinearColor(-9999, -9999, -9999, -9999), true, false);
		if (!RT_VoxelTerrain || !RT_DebugView || !RT_VelocityHeight || !RT_ResultVelHeight || !RT_Source || !RT_SmoothHeight) return false;

		if (RT_ResultDepthWet == nullptr)
		{
			RT_ResultDepthWet = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(-9999, -9999, -9999, -9999), true, false);
			if (!RT_ResultDepthWet) return false;
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

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void ReleaseTransientRenderResources();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	int32 VoxelMaxSceneTriangles = 2000000000;

	// Tiled voxel build: per-column fixed run capacity. The persistent Runs buffer is sized
	// ColumnCount * VoxelMaxRunsPerColumn (no prefix sum). Columns with more vertical runs than
	// this are truncated. 16 covers typical terrain (a few solid spans per column); raise it for
	// highly layered geometry (caves, overhangs) at the cost of more VRAM.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000, ClampMin="1", ClampMax="256"))
	int32 VoxelMaxRunsPerColumn = 16;

	// Tiled voxel build: target VRAM budget (MB) for the transient per-tile occupancy scratch.
	// The XY tile size is chosen adaptively so one tile's dense bit grid (TileW*TileH*GridZ bits)
	// fits this budget. Smaller budget -> more, smaller tiles -> lower peak VRAM, more dispatches.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000, ClampMin="1", ClampMax="2048"))
	int32 VoxelTileBudgetMB = 64;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	float MaxWaterRisePerFrame = 40.0f;
	// When rebuilding the terrain height map, the search starts at the most recent water surface and
	// expands outward (up and down). Voxels more than this many cm ABOVE the water surface are ignored.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	float VoxelMaxAboveWaterSurface = 50.0f;

	// During initial height map build, voxels more than this many cm BELOW the lowest source point
	// are rejected. Prevents underground cavities from being selected as terrain surface.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	float VoxelMaxBelowWaterSurface = 500.0f;

	// Terrain voxelization ignores scene triangles whose world Z is higher than the highest source
	// point plus this margin (cm). Keeps source-overhead geometry (ceilings, overhangs) out of the
	// terrain. Only the build step uses it; with no sources the cap is disabled.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter|Voxel", Meta=(Priority=1000))
	float VoxelMaxAboveSourceZ = 0.0f;

	// Persistent SPARSE terrain voxel grid built from box-overlapping geometry at StartSolver.
	// XY is a full grid; Z is variable-length runs per column (CSR layout):
	//   VoxelColumnRunStartBuffer[col] : offset into VoxelRunsBuffer
	//   VoxelColumnRunCountBuffer[col] : number of runs in the column
	//   VoxelRunsBuffer[r]             : packed run (low16 = zStart cell, high16 = zEnd cell)
	// Plus per-XY coverage for the 2/3 fill test. Non-UObject GPU resources,
	// lifetime = StartSolver -> StopSolver/teardown.
	TRefCountPtr<FRDGPooledBuffer> VoxelColumnRunStartBuffer;
	TRefCountPtr<FRDGPooledBuffer> VoxelColumnRunCountBuffer;
	TRefCountPtr<FRDGPooledBuffer> VoxelRunsBuffer;
	TRefCountPtr<FRDGPooledBuffer> VoxelCoverageBuffer;
	uint32 VoxelTotalRunCount = 0;
	FIntVector VoxelGridSize = FIntVector::ZeroValue;
	float VoxelBoxExtentXY = 0.0f;
	float VoxelCellSizeXY = 0.0f;
	float VoxelGridMinWorldZ = 0.0f;
	float VoxelCellSizeZ = 0.0f;
	bool bVoxelGridValid = false;
	bool bVoxelHeightMapInitialized = false;

	// Builds the persistent terrain voxel grid from geometry inside the simulation box.
	void BuildTerrainVoxelGrid();
	// Releases the persistent terrain voxel grid GPU resources.
	void ReleaseTerrainVoxelGrid();

	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	TObjectPtr<USceneCaptureComponent2D> CaptureSceneDepth;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SceneDepth = nullptr;

	UPROPERTY(Transient, NonTransactional)
	UTextureRenderTarget2D* RT_TileMask = nullptr;

	// Static visualization instance grid. Built once at construction time (BuildSimVisInstanceGrid),
	// covering the whole CaptureSize x CaptureSize fluid box. Instances are NOT added/removed during
	// simulation; dry tiles simply render nothing through the water material. These describe the grid
	// layout so bake can map each instance back to its footprint in the result texture.
	int32 SimVisGridCountX = 0;
	int32 SimVisGridCountY = 0;
	float SimVisTileWorldSize = 0.0f;

	static constexpr int32 ReadbackBufferCount = 2;

	FRHIGPUTextureReadback* TileMaskReadback[ReadbackBufferCount] = {};
	int32 TileMaskReadbackWriteIdx = 0;
	int32 TileMaskReadbackCopyWidth[ReadbackBufferCount] = {};
	int32 TileMaskReadbackCopyHeight[ReadbackBufferCount] = {};
	int32 TileMaskReadbackGeneration[ReadbackBufferCount] = {};
	int32 TileMaskReadbackWidth = 0;
	int32 TileMaskReadbackHeight = 0;
	TArray<uint8> CachedTileBits;

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
	void StartSolver(float TimerRate = 0.0f,
		UPARAM(meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "8")) int32 InIteration = 1);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void StopSolver();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void RequestRenderDocCapture();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ShallowWaterSolverSoucePointWithCapture(int32 InIteration);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void ToggleSimVisualization(int32 SimIterationsPerFrame = 1);

	DECLARE_DELEGATE_OneParam(FOnBakeResultMesh, ACSShallowWaterCapture*);
	static FOnBakeResultMesh OnBakeResultMeshDelegate;

	UFUNCTION(BlueprintCallable, Category = "SWParameter", Meta=(DevelopmentOnly))
	void BakeResultMesh();

	UFUNCTION(BlueprintNativeEvent, Category = "ComputeShader|Bake")
	void OnBakeComplete();
	virtual void OnBakeComplete_Implementation();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	void UseBakedResultMesh(UStaticMesh* InBakedMesh, UMaterialInterface* InWaterMaterial, UMaterialInterface* InDecalMaterial);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	void UseSimulationResultMesh();

	UPROPERTY(Transient, BlueprintReadOnly, Category = "ComputeShader")
	bool bSimVisActive = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|DebugViewPlane")
	UMaterialInterface* DebugViewPlaneMaterial;

	UFUNCTION(BlueprintCallable, Category = "Debug|DebugViewPlane")
	void ShowDebugViewPlane(float Duration = 5.0f);

	// Reads back the persistent run-length terrain voxel grid and draws one debug line per vertical
	// run (a long voxel becomes a single full-height line). Requires a valid voxel grid (StartSolver
	// must have built it). Duration is the debug line lifetime in seconds.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader|Debug", Meta=(ClampMin="0", UIMin="0"))
	void VisualizeVoxelRuns(float Duration = 5.0f);

	// One-shot debug: builds the terrain voxel grid using the solver's GPU pipeline (VoxelFill →
	// CountRuns → ScanBlocks → ScanBlockSums → AddOffsets → EmitRuns), reads back the sparse
	// CSR run-length buffers, then draws THICK vertical debug lines per voxel column. Each run
	// is drawn as a single thick cylinder-like line. Duration controls debug-line lifetime.
	// Thickness is in world units; defaults to CellSizeXY (the full XY width of each voxel column).
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader|Debug", Meta=(ClampMin="0", UIMin="0"))
	void DrawDebugVoxelGrid(float Duration = 10.0f, float Thickness = -1.0f);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader|Debug")
	void DrawDebugHeightMapPoints(float Duration = 10.0f, int32 Stride = 8, float PointSize = 12.0f, bool bUseLiquidSearch = false);

private:
	void ClearSolverTimer();
	bool IsSolverTimerActive() const;
	void ScheduleSolverTimerTick();
	void HandleSolverTimerTick(int32 ExpectedSolverReadbackGeneration);
	void StopSimulationRuntime(bool bResetVisualization);
	void UpdateSimulationPreviewMesh();
	bool EnsureSimVisHISMReady();
	// Builds the full static visualization instance grid covering the whole fluid box.
	// Called once at construction; instances persist for the actor's lifetime.
	void BuildSimVisInstanceGrid();
	void ResetSimVisTiles();
	void ResetSolverReadbackState(bool bAdvanceGeneration, bool bClearCachedResult);
	void WaitForPendingShallowWaterRendering(const TCHAR* Context) const;
	bool IsShallowWaterConstructionBlocked() const;
	bool CanRunShallowWaterGPUWork(const TCHAR* Context) const;
	int32 ResolveTextureSize() const;
	void ReleaseShallowWaterTransientResources(const TCHAR* Context);

	UE_DEPRECATED(5.7, "Use CaptureSceneDepthFromTriangles instead")
	void CaptureSceneDepthNow();

	void CaptureSceneDepthFromTriangles();

	TArray<int32> ISMTileSlots;
	int32 CachedActiveTileCount = 0;

	TWeakObjectPtr<AActor> DebugViewPlaneActor;
	FTimerHandle DebugViewPlaneTimerHandle;
	FTimerHandle SolverTimerHandle;
	float SolverTimerRate = 0.0f;
	int32 SolverIterationsPerFrame = 1;
	bool bSWConstructionGuardActive = false;
	bool bSWConstructionWorkPending = false;
	bool bCaptureNextSolverFrame = false;
};




UCLASS()
class COMPUTESHADERGENERATOR_API ACSSHallowWaterSource : public AActor
{
	GENERATED_BODY()
public:
	ACSSHallowWaterSource();
	
};
