// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Curves/CurveLinearColor.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "MeshGeneratorBrushCache.h"
#include "ComputeShaderDebugParams.h"
// PendingSurfaceVoxelInputs 是按值持有的成员，必须是完整类型，不能只前向声明。
#include "CSSurfaceVoxelPasses.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

#include "GeometryEditorActor.generated.h"

class AStaticMeshActor;
class UCSMesh;
class UVineMeshComponent;

USTRUCT(BlueprintType, meta = (DisplayName = "SC Options"))
struct GEOMETRYSCRIPTEXTRAEDITOR_API FSpaceColonizationOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Iteration = 55;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Activetime = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 ExtentPlus = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float RandGrow = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float Seed = .5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float BackGrowRange = .8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float VoxelSize = 2.5f;

	// SpaceColonization-specific parameters
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 BackGrowCount = 3;
	// Fork Tapering starts shrinking the branch once it backtracks past the Nth non-primary
	// fork (1 = the first fork, 2 = the second, ...). Forks before the Nth are passed through
	// at full scale; from the Nth fork onward the retained ancestor points (BackGrowCount of
	// them) taper continuously down toward the end scale and may cross further forks without
	// resetting or stopping the taper.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1"))
	int32 ForkTaperForkOrdinal = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	float InfluenceRadius = 200.0f;

};

USTRUCT(BlueprintType, meta = (DisplayName = "VV Options"))
struct GEOMETRYSCRIPTEXTRAEDITOR_API FVV
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CurlNoiseScale = 2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CurlNoiseFre = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float PerlinNoiseScale = 11;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float PerlinNoiseFre = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float ResampleLength = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float LineScale = 0.1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CircleScale = 0.2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float MergeDistMult = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float VinesOffset = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UVScaleInfluence = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float UVScaleFloor = 0.08f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float UVScalePower = 1.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.000001"))
	float UVLengthScale = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UCurveLinearColor* CurveControl = nullptr;

	// --- moved from FVisVineParameters ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 GenerateVineVoxelNormalBlurIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUPostProjectionSmoothIterations = 3;

	// Half-width (in path points) of each post-projection smoothing pass. Larger values round
	// sharp folds harder per iteration; 1 reproduces the legacy immediate-neighbor smoothing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1"))
	int32 VisVineGPUPostProjectionSmoothKernelRadius = 4;

	// Light radius-1 (immediate prev/next only) smoothing passes applied after the wide-kernel
	// smoothing, as a final local cleanup.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUPostProjectionSmallSmoothIterations = 2;

	// How strongly the local corner angle modulates the path smoothing. The angle between the
	// incoming/outgoing segments at each point drives the strength: a smaller angle (sharper fold)
	// smooths harder, while near-straight points are left almost untouched. 0 == legacy uniform
	// smoothing (every interior point fully averaged); 1 == full angle-driven modulation.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0", ClampMax = "1"))
	float VisVineGPUPostProjectionSmoothAngleStrength = 1.0f;

	// Before the final tangent smoothing, redistribute each vine's surface points to uniform
	// arc-length spacing along the post-projection surface polyline. Point count is preserved.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	bool bVisVineGPUResampleSurfaceEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUNoiseIterations = 10;

	// Pass C 扫掠截面（Tube）的圆周段数：每个路径点环上的截面顶点数。3 = 三角形（原始行为），
	// 数值越大截面越接近圆。仅影响 Tube 截面，Plane 截面始终为 2 点。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "3"))
	int32 VisVineGPUTubeSegments = 3;

	// 整批藤蔓（全部源共用）的路径点上限，同时也是 GPU buffer 的分配容量。真实点数只有 GPU
	// 知道，所以下游一律按这个上限分配、按 GPU 计数绘制；调大只是多占显存，不改变结果。
	// 每源容量取 min(TargetCount × (SC_MAX_BACKTRACK+1), 本值 / 源数量)，所以在目标数很少时
	// 仍按更紧的理论界分配，不会浪费。
	//
	// 默认值必须覆盖“常见源数 × 理论界”，否则每源份额会低于理论界，EmitSpaceColonizationLinesCS
	// 会在回溯途中撞上容量直接 break —— 藤蔓被悄悄截短，表现为莫名其妙变稀变短。
	// 参考：980 个 target 的理论界是 980×101 ≈ 99K，1M 可以喂满 10 个源；旧实现每源独立按理论界
	// 分配（硬顶 4M），不除源数，所以这里给低了就是纯回归。被截断时会打 Warning 并给出建议值。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1024"))
	int32 MaxVinePointCount = 1048576;

};

USTRUCT()
struct FVineLinePointScaleData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<float> Values;
};

// VisVine debug parameter structs moved to ComputeShaderDebugParams.h

// CPU-prepped inputs for the space-colonization solve. The solve is recorded into the vine mesh
// RDG graph and its output never leaves the GPU, so this bundle is the whole hand-off.
// Defined in GeometryEditorActor.cpp.
struct FVineFusedSCInputs;

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API AVineContainer : public AMeshGeneratorBrushCache
{
	GENERATED_BODY()

public:
	AVineContainer(const FObjectInitializer& ObjectInitializer);
	virtual void PostLoad() override;
	virtual void PostRegisterAllComponents() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ---- References ----

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* GrowTarget;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* TubeVineSource;

	// The vine's renderer: binds VineGeometry and draws it through an indirect draw. It builds nothing —
	// VisVineGPUInternal writes the geometry into VineGeometry and then binds it here. Kept at an
	// identity world transform (the vine renders in world space).
	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UVineMeshComponent* VineGpuMesh;

	// Single source of truth for the vine surface material. It used to live on slot 0 of a
	// UDynamicMeshComponent this actor owned; that component is gone, so the assignment lives on the
	// actor and is pushed down (never read back) to two places at once — VineGpuMesh->MeshMaterial,
	// which the draw uses, and VineGeometry's slot 0, which is what Save Mesh bakes into the asset. See
	// ApplyVineMaterialToLeaf. Leaving it empty is fine: the renderer falls back to the engine
	// default surface material.
	//
	// MeshMaterial is EditAnywhere on the inherited component, so it *looks* authorable there. It is
	// not: every load and every generation overwrites it from here, and a value typed onto the
	// component is neither saved nor honoured.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GrowReference")
	TObjectPtr<UMaterialInterface> VineMaterial;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TargetType;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TubeType;

	// ---- Options ----

	UPROPERTY(BlueprintReadWrite, Category = "Options")
	FVV VV;

	UPROPERTY(BlueprintReadWrite, Category = "Options")
	FSpaceColonizationOptions SC;

	// ---- Debug Options ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineGPUProjectionDebugOptions GPUProjectionDebug;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineSurfaceVoxelDebugOptions SurfaceVoxelDebug;

	// ---- Transient State ----

	FBox InstanceBound;

	// The vine geometry itself: the retained GPU streams the build writes and VineGpuMesh draws.
	// Owned here rather than by the component because this actor, not a render state, decides when a
	// vine exists — that is exactly what survives a proxy recreation now. Transient because GPU data
	// does not survive a level reload; the property exists to hold the object against GC, not to
	// serialize it (EnsureVineGeometry is what brings a vine back after a load).
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> VineGeometry;

	UPROPERTY(Transient)
	TObjectPtr<AActor> DebugVineSplineActor;

	// ---- Core Operations ----

	/** Idempotent "the vine I own must exist" entry point: runs a generation only when there is no
	 *  vine geometry, and returns whether there is one by the time the call is over. Safe to call
	 *  from any lifecycle event and any number of times — the whole point is that this actor, not a
	 *  render-state recreation, decides when the vine is generated.
	 *
	 *  Nothing about a vine is serialized except its sources and targets, so restoring one means
	 *  re-running the whole generation (voxels -> space colonization -> mesh); there is no cached
	 *  bundle to re-push. Now that UVineMeshComponent draws a UCSMesh, a render-state recreation is
	 *  a rebind, which makes this the ONLY thing that regenerates the vine. */
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool EnsureVineGeometry();

	/** 藤蔓生成的唯一入口：表面体素 -> 空间殖民求解 -> concat -> 建网格，全部合并进 VineGeometry
	 *  那一次 EditMeshSync 的 RDG 图。以前这里是 4 张图外加一次 FlushRenderingCommands，而藤蔓
	 *  这条路根本不回读数据，那次阻塞是白等的。
	 *  包围盒由本函数从当前 source/target 现算，同时决定体素化范围：蓝图侧拿不到 transform 列表，
	 *  让它自己拼 FBox 只会拼出一个和体素化范围对不上的盒子。 */
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool GenerateVineGPU();

	/** Drops everything a generation produced — the cached lines and surface triangles, the debug
	 *  spline actor, and the vine geometry itself, which is unbound and released rather than merely
	 *  emptied. Releasing is what keeps EnsureVineGeometry()'s contract: a retained mesh would answer
	 *  "there is already a vine" and refuse to regenerate for the rest of this actor's life. */
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void Clean();

	// ---- Foliage ----

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void RebuildDisplayInstancesFromTransformArrays();

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ImportFoliageToTransformArray(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ExportTransformArrayToFoliage(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void FetchFoliage();

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void RevertFoliage();

	// ---- Vine Actions ----

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void SaveStaticmesh();

protected:
	//~ AComputeShaderMeshGenerator interface
	/** 沿用"标签优先"的资产基名（基类默认用 GetName()），保证已烘好的资产名不变。 */
	virtual FString GetResultAssetBaseName() const override;

public:

	// ---- SpaceColonization ----

	// Non-reflected worker (FVineFusedSCInputs isn't a USTRUCT): prepares the CPU side of the
	// fused space-colonization solve. Dispatches nothing; the passes are recorded into the vine
	// mesh graph by the build operator, inside VineGeometry's UCSMesh::EditMeshSync.
	bool PrepareVineFusedSCInputs(const TArray<FTransform>& SourceTransforms, const TArray<FTransform>& TargetTransforms, FVineFusedSCInputs& OutInputs);

	// ---- Debug ----

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug")
	void ClearDebugVineSplineActor();

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Debug Vine Surface Voxel Arrows"))
	int32 DrawDebugVineSurfaceVoxelArrows(float Duration = 5.0f, bool bUseCachedVoxels = false);


private:

	/** Queues one EnsureVineGeometry() for the next engine tick. See the rationale on the
	 *  definition for why the restore is hooked where it is and why it is not run inline. */
	void ScheduleEnsureVineGeometry();

	/** Latched at the first schedule and never cleared: the automatic restore is one shot per actor
	 *  lifetime. Re-arming it on every re-registration would stack tickers, and — because a
	 *  generation that keeps failing leaves the geometry absent — would retry the whole solve every
	 *  frame. Anything past the first attempt is the owner's call (GenerateVineGPU /
	 *  EnsureVineGeometry are both public). */
	bool bVineGeometryRestoreAttempted = false;

	// GenerateVineGPU 备好、等着交给构建那张图的体素输入。不是 UPROPERTY：里面是 PIMPL 持有的
	// 三角形请求数组，且只在一次生成内有效 —— VisVineGPUInternal 会把它 MoveTemp 进 build
	// bundle，消费后即失效，所以每次 GenerateVineGPU 都重新准备。
	FCSSurfaceVoxelPassInputs PendingSurfaceVoxelInputs;

	// One-shot data upgrade for packages saved while this actor still owned a UDynamicMeshComponent.
	// That component used to hold the vine surface material on slot 0, so dropping it would silently
	// strand the artist's assignment. See MigrateLegacyVineMaterial() for how the removed subobject
	// is still read back.
	void MigrateLegacyVineMaterial();

	/** Pushes VineMaterial down to the GPU leaf, the one direction the assignment ever travels. */
	void ApplyVineMaterialToLeaf();

	// Set when MigrateLegacyVineMaterial() actually moved a material. MarkPackageDirty is refused
	// while a package is loading, so the dirty flag has to be re-issued from
	// PostRegisterAllComponents — that is what turns the upgrade into a save the user can commit.
	bool bPendingLegacyVineMaterialDirty = false;
};
