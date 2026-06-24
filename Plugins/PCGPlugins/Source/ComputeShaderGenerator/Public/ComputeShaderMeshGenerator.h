#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphBuilder.h"
#include "UDynamicMesh.h"
#include "ComputeShaderDebugParams.h"
#include "ComputeShaderMeshGenerator.generated.h"

class AActor;
class ALandscape;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;

class AComputeShaderMeshGenerator;

DECLARE_MULTICAST_DELEGATE_OneParam(FCSInstanceBrushEditorRequest, AComputeShaderMeshGenerator*);

// -----------------------------------------------------------------------------
// Core Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType, meta = (DisplayName = "CS Triangle Mesh Data"))
struct COMPUTESHADERGENERATOR_API FCSTriangleMeshData
{
	GENERATED_BODY()
public:
	// GPU readback 后的 compact vertex buffer。xyz 是顶点位置。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Vertices;

	// 有效 vertex 数。小于 0 时使用 Vertices.Num()。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 VertexCount = -1;

	// 可选 index buffer。每 3 个 index 组成一个 triangle。
	// 如果为空，则 Vertices 会按 triangle soup 解释：0/1/2, 3/4/5, ...
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<int32> Indices;

	// 有效 index 数。小于 0 时使用 Indices.Num()。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 IndexCount = -1;

	// 可选 vertex normal。若 bRecomputeNormals 为 true，下游 DynamicMesh 可忽略它并重算法线。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> VertexNormals;
};

USTRUCT(BlueprintType, meta = (DisplayName = "CS Static Mesh Triangle Request"))
struct COMPUTESHADERGENERATOR_API FCSStaticMeshTriangleRequest
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	UStaticMesh* StaticMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 LODIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FTransform LocalToWorld = FTransform::Identity;

	// 可选包围盒。有效时作为粗筛；无效时不按 Bounds 筛选。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FBox WorldBounds = FBox(ForceInit);

	// 生成 Request 的来源 Actor。用于在 RDG 三角形提取阶段排除自身或指定 Tag 的 Actor。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	AActor* SourceActor = nullptr;
};

USTRUCT(BlueprintType, meta = (DisplayName = "CS Surface Voxel Data"))
struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelData
{
	GENERATED_BODY()
public:
	// GPU 生成的 surface voxel 中心点。每个 voxel 只表示一个表面面片，不生成封闭 cube。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Positions;

	// 与 Positions 一一对应的表面法线；用于后续生成开放 mesh 的面朝向。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Normals;

	// 有效 voxel 数。小于 0 时使用 Positions.Num()。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 VoxelCount = -1;

	// 生成 voxel 时使用的 cell size，后续转 mesh 时可作为默认面片大小。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	float VoxelSize = 0.0f;

	// 体素整数网格坐标，与 Positions 一一对应（-1 索引为无效）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FIntVector> Cells;

	// 面积加权质心（target position），与 Positions 一一对应。用于更精确的表面匹配。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> TargetPositions;

	// 体素网格的世界空间原点（与 Cells 坐标系对应）。
	// Cell (cx, cy, cz) 的世界空间中心 = VoxelOrigin + (Cell + 0.5) * VoxelSize。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FVector VoxelOrigin = FVector::ZeroVector;
};

// game-thread 预备好的盒内场景三角形数据：static mesh 已 resolve 出渲染资源引用，
// landscape 已在 game thread 完成 CPU 提取。可安全捕获进 render 线程 lambda，再交给
// AddPreparedBoxSceneTrianglesToRDG 消费。内部用 PImpl 隐藏 .cpp-only 的 resolved 类型。
struct FCSBoxScenePreparedDataImpl;

struct COMPUTESHADERGENERATOR_API FCSBoxScenePreparedData
{
	TSharedPtr<FCSBoxScenePreparedDataImpl, ESPMode::ThreadSafe> Impl;

	bool IsValid() const { return Impl.IsValid(); }
	bool HasAnyTriangles() const;
};

struct COMPUTESHADERGENERATOR_API FCSStaticMeshTriangleRDGOutput
{
	// Triangle soup: 每个 triangle 占 3 个 float4 vertex，vertex.w = 1。
	FRDGBufferRef TriangleVertices = nullptr;
	FRDGBufferUAVRef TriangleVerticesUAV = nullptr;
	FRDGBufferSRVRef TriangleVerticesSRV = nullptr;

	// 与 TriangleVertices 一一对应；每个 vertex 存 triangle normal，normal.w = 0。
	FRDGBufferRef TriangleNormals = nullptr;
	FRDGBufferUAVRef TriangleNormalsUAV = nullptr;
	FRDGBufferSRVRef TriangleNormalsSRV = nullptr;

	// Counter[0] = 实际写入的 triangle 数；有效 vertex 数 = Counter[0] * 3。
	FRDGBufferRef TriangleCounter = nullptr;
	FRDGBufferUAVRef TriangleCounterUAV = nullptr;
	FRDGBufferSRVRef TriangleCounterSRV = nullptr;

	uint32 MaxTriangles = 0;
	uint32 MaxVertices = 0;

	// 保持外部 RHI SRV 引用直到 GraphBuilder.Execute()，避免 RDG pass 执行前被释放。
	TArray<FShaderResourceViewRHIRef> ReferencedIndexBufferSRVs;
};

struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelRDGOutput
{
	// Surface voxel center，xyz 是 voxel 中心，w = 1。
	FRDGBufferRef VoxelPositions = nullptr;
	FRDGBufferUAVRef VoxelPositionsUAV = nullptr;
	FRDGBufferSRVRef VoxelPositionsSRV = nullptr;

	// Surface voxel normal，xyz 是法线，w = 0。
	FRDGBufferRef VoxelNormals = nullptr;
	FRDGBufferUAVRef VoxelNormalsUAV = nullptr;
	FRDGBufferSRVRef VoxelNormalsSRV = nullptr;

	// Counter[0] = 实际写入的 voxel 数。
	FRDGBufferRef VoxelCounter = nullptr;
	FRDGBufferUAVRef VoxelCounterUAV = nullptr;
	FRDGBufferSRVRef VoxelCounterSRV = nullptr;

	// GPU 端去重用 hash slots。
	FRDGBufferRef VoxelHashSlots = nullptr;
	FRDGBufferUAVRef VoxelHashSlotsUAV = nullptr;

	FRDGBufferRef VoxelHashIndices = nullptr;
	FRDGBufferUAVRef VoxelHashIndicesUAV = nullptr;

	FRDGBufferRef VoxelNormalSums = nullptr;
	FRDGBufferUAVRef VoxelNormalSumsUAV = nullptr;
	FRDGBufferSRVRef VoxelNormalSumsSRV = nullptr;

	FRDGBufferRef VoxelNormalCounts = nullptr;
	FRDGBufferUAVRef VoxelNormalCountsUAV = nullptr;
	FRDGBufferSRVRef VoxelNormalCountsSRV = nullptr;

	// Ported from ResinRattan: target-position accumulation (clipped centroid)
	FRDGBufferRef VoxelTargetPositions = nullptr;
	FRDGBufferUAVRef VoxelTargetPositionsUAV = nullptr;
	FRDGBufferSRVRef VoxelTargetPositionsSRV = nullptr;

	FRDGBufferRef VoxelTargetOffsetSums = nullptr;
	FRDGBufferUAVRef VoxelTargetOffsetSumsUAV = nullptr;

	FRDGBufferRef VoxelTargetWeightSums = nullptr;
	FRDGBufferUAVRef VoxelTargetWeightSumsUAV = nullptr;

	// Integer grid cell per voxel, required for spatial blur neighbour lookup.
	FRDGBufferRef VoxelCells = nullptr;
	FRDGBufferUAVRef VoxelCellsUAV = nullptr;

	// Blur output buffers (read when blur is enabled).
	FRDGBufferRef BlurredVoxelNormals = nullptr;
	FRDGBufferUAVRef BlurredVoxelNormalsUAV = nullptr;
	FRDGBufferSRVRef BlurredVoxelNormalsSRV = nullptr;

	FRDGBufferRef BlurredVoxelTargetPositions = nullptr;
	FRDGBufferUAVRef BlurredVoxelTargetPositionsUAV = nullptr;
	FRDGBufferSRVRef BlurredVoxelTargetPositionsSRV = nullptr;

	uint32 MaxVoxels = 0;
	uint32 HashSlotCount = 0;
	float VoxelSize = 0.0f;
	FVector VoxelOrigin = FVector::ZeroVector;
};

// -----------------------------------------------------------------------------
// Core System - Generated Data Cache
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorTriangleTextureDataHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 VertexCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 TriangleCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 IndexCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	float ReferenceFilterDistance = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FBox SourceWorldBounds = FBox(ForceInit);

	// One RGBA32f texel per triangle-soup vertex. xyz = world position, w = 1.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT = nullptr;

	// One RGBA32f texel per triangle-soup vertex. xyz = normal, w = 0.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT = nullptr;

	// Small metadata texture. Pixel 0 = counts/filter, pixels 1-3 = bounds/dimensions.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> TriangleMetaRT = nullptr;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorSurfaceVoxelTextureDataHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 VoxelCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	float VoxelSize = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FVector VoxelOrigin = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FBox SourceWorldBounds = FBox(ForceInit);

	// One RGBA32f texel per sampled surface voxel. xyz = voxel center, w = 1.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelPositionRT = nullptr;

	// One RGBA32f texel per sampled surface voxel. xyz = blended normal, w = 0.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelNormalRT = nullptr;

	// One RGBA32f texel per sampled surface voxel. xyz = weighted surface target, w = 1.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelTargetRT = nullptr;

	// One RGBA32f texel per sampled surface voxel. xyz = integer voxel cell encoded as floats, w = 0.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelCellRT = nullptr;

	// Small metadata texture. Pixel 0 = counts/size, pixels 1-4 = origin/bounds/dimensions.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT = nullptr;
};

// -----------------------------------------------------------------------------
// Dirty Cache Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorVoxelGridSettings
{
	GENERATED_BODY()

	float VoxelSize = 100.0f;

	float ActivationRadius = 200.0f;

	int32 MaxActiveVoxels = 4096;

	int32 MaxTrianglesPerVoxel = 256;

	int32 LODIndex = 0;

	float BoundsTolerance = 1.0f;

	int32 MaxCacheTextureDimension = 4096;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorTriangleCacheRequest
{
	GENERATED_BODY()

	FName RequestId = NAME_None;

	bool bForceFullRebuild = false;

	float ActivationRadiusOverride = 0.0f;

	bool bPersistentInterest = true;

	TArray<FVector> CachedReferencePoints;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorTriangleCacheHandle
{
	GENERATED_BODY()

	bool bValid = false;

	int32 CacheGeneration = 0;

	FBox CachedWorldBounds = FBox(ForceInit);

	FIntVector GridSize = FIntVector::ZeroValue;

	float VoxelSize = 0.0f;

	int32 ActiveVoxelCount = 0;

	int32 DirtyVoxelCount = 0;

	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT = nullptr;

	TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT = nullptr;

	TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT = nullptr;
};

USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorVoxelCacheState
{
	GENERATED_BODY()

	FBox CachedWorldBounds = FBox(ForceInit);
	FIntVector GridSize = FIntVector::ZeroValue;
	float CachedVoxelSize = 0.0f;
	int32 CachedMaxActiveVoxels = 0;
	int32 CachedMaxTrianglesPerVoxel = 0;
	int32 CachedLODIndex = 0;
	int32 CachedMaxTextureDimension = 0;
	uint32 CacheGeneration = 0;

	TSet<FIntVector> ActiveCells;
	TSet<FIntVector> CellsToActivate;
	TSet<FIntVector> CellsToDeactivate;
	TSet<FIntVector> DirtyCells;
	TMap<FIntVector, int32> CellToPage;
	TArray<int32> FreePages;
};

// -----------------------------------------------------------------------------
// Brush Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSInstancePaintComponentSlot
{
	GENERATED_BODY()

	TObjectPtr<UStaticMesh> Mesh = nullptr;

	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component = nullptr;
};

UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API AComputeShaderMeshGenerator : public AActor
{
	GENERATED_BODY()

public:
	/** Creates the generator actor, scene root, bounds component, and DynamicMesh rendering defaults. */
	AComputeShaderMeshGenerator(const FObjectInitializer& ObjectInitializer);

	/** Returns the DynamicMeshComponent owned by this actor. */
	UDynamicMeshComponent* GetDynamicMeshComponent() const { return DynamicMeshComponent; }

	// -------------------------------------------------------------------------
	// Core System
	// -------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UDynamicMeshComponent> DynamicMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UBoxComponent> GeneratorBounds;



	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	FCSMeshGeneratorVoxelGridSettings VoxelGridSettings;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Reference Filter")
	TArray<FVector> ReferencePoints;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Scene Filter")
	TArray<FName> ExcludedActorTags = { TEXT("UA"), TEXT("UN") };

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 MaxTriangles = 20000000;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 MaxVoxels = 2000000;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "0.001"))
	float QuadScale = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh")
	float NormalOffsetScale = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1.0"))
	float DynamicMeshCullBoundsScale = 10.0f;

	// -------------------------------------------------------------------------
	// Surface Voxel Blur — ResinRattan port
	// -------------------------------------------------------------------------

	/** Number of 3D mean-filter iterations applied after voxelization. 0 = disabled. */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "0"))
	int32 SurfaceVoxelBlurIterations = 0;

	/** Neighbourhood radius for the 3D mean filter. 1 = 3x3x3 Moore neighbourhood. */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 SurfaceVoxelBlurRadius = 1;

	// -------------------------------------------------------------------------
	// Dirty Cache System
	// -------------------------------------------------------------------------

	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT;

	TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT;

	TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT;

	// -------------------------------------------------------------------------
	// Core System - Generated Data Cache
	// -------------------------------------------------------------------------

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorTriangleTextureDataHandle LastTriangleTextureData;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorSurfaceVoxelTextureDataHandle LastSurfaceVoxelTextureData;

	// -------------------------------------------------------------------------
	// Debug System
	// -------------------------------------------------------------------------

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data|Debug")
	FCSSurfaceVoxelData LastSurfaceVoxelData;

	/** Triangle surface data used by the CPU/BVH vine visualization path.
	 *  Filled by GenerateVines(). */
	FCSTriangleMeshData CachedSurfaceTriangles;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Mesh|Debug")
	int64 GeneratorTimeCode = -1;

	// -------------------------------------------------------------------------
	// Brush System
	// -------------------------------------------------------------------------

	static FCSInstanceBrushEditorRequest OnInstanceBrushEditorRequest;

	TObjectPtr<UStaticMesh> InstanceBrushMesh = nullptr;

	float InstanceBrushRadius = 500.0f;

	int32 InstanceBrushSamplesPerMouseMove = 16;

	float InstanceBrushMinSpacing = 100.0f;

	float InstanceBrushTraceRadius = 0.0f;

	float InstanceBrushPreviewPointSize = 8.0f;

	float InstanceBrushPreviewLifetime = 0.1f;

	bool bInstanceBrushAlignToNormal = true;

	bool bInstanceBrushUseGeneratorBounds = true;

	bool bInstanceBrushExitAfterCommit = false;

	FVector2D InstanceBrushUniformScaleRange = FVector2D(1.0f, 1.0f);

	float InstanceBrushRandomYawDegrees = 360.0f;

	TArray<FCSInstancePaintComponentSlot> PaintedInstanceComponents;

	/** Opens the editor-side instance brush tool for this generator. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Mesh Generator|Instance Brush", meta = (DevelopmentOnly))
	void StartInstanceBrush();

	/** Finds or creates the HISM component used to store painted instances for the given mesh. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Instance Brush")
	UHierarchicalInstancedStaticMeshComponent* GetOrCreatePaintComponent(UStaticMesh* Mesh);

	/** Returns the existing painted-instance component for a mesh, or nullptr if none exists. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Instance Brush")
	UHierarchicalInstancedStaticMeshComponent* FindPaintComponent(UStaticMesh* Mesh) const;

	/** Appends world-space instance transforms to the paint component for Mesh and returns the added count. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Instance Brush")
	int32 CommitPaintInstances(const TArray<FTransform>& WorldTransforms, UStaticMesh* Mesh);

	/** Tests whether a brush placement point is inside the generator bounds when bounds filtering is enabled. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Instance Brush")
	bool IsInstanceBrushPointAllowed(const FVector& WorldPosition) const;

	// -------------------------------------------------------------------------
	// Core System - Scene Extraction and Mesh Output
	// -------------------------------------------------------------------------

	void BuildBoxSceneTriangleRequests(UWorld* World,
		const FBox& QueryBox,
		TArray<FCSStaticMeshTriangleRequest>& OutRequests);

	/** [game thread] 用 FLandscapeComponentDataInterface 在 CPU 端把 QueryBox 内的 landscape 高度场
	 *  提取成 triangle-soup（世界坐标，已按上朝向定向），暂存进 OutTriangleData，供后续 RDG 流程作为
	 *  initial triangle 上传。必须在 game thread 调用（CDI 构造强制 game thread）。普通 static mesh 不走
	 *  这里，仍用本 class 的 GPU resolve 流程。InReferencePoints 非空且 InReferenceFilterDistance > 0 时，
	 *  按到参考点的距离做 CPU 粗筛；否则保留盒内全部三角形。MaxTriangles == 0 时直接返回空。 */
	static void BuildBoxSceneLandscapeTriangles(UWorld* World,
		const FBox& QueryBox,
		const TArray<FVector>& InReferencePoints,
		float InReferenceFilterDistance,
		int32 MaxTriangles,
		FCSTriangleMeshData& OutTriangleData);

	/** Extended overload with OBB filtering and actor-tag culling. When WorldToLocalBoxTransform
	 *  is non-null each triangle is additionally tested against the OBB defined by
	 *  (*WorldToLocalBoxTransform, *LocalBoxExtent). RequiredActorTag != NAME_None restricts to
	 *  landscape proxies that carry that tag. bSortComponentsByDistance sorts components closest
	 *  to the box center first (useful when MaxTriangles may truncate results). */
	static void BuildBoxSceneLandscapeTriangles(UWorld* World,
		const FBox& QueryBox,
		const TArray<FVector>& InReferencePoints,
		float InReferenceFilterDistance,
		int32 MaxTriangles,
		FCSTriangleMeshData& OutTriangleData,
		const FTransform* WorldToLocalBoxTransform,
		const FVector* LocalBoxExtent,
		FName RequiredActorTag = NAME_None,
		bool bSortComponentsByDistance = true);

	/** [game thread] 枚举 QueryBox 内的 static mesh + landscape，完成 static mesh 渲染资源 resolve
	 *  与 landscape CPU 三角形提取，返回可安全捕获进 render 线程 lambda 的预备数据。
	 *  必须在 game thread 调用（触碰 UObject / FLandscapeComponentDataInterface）。
	 *  RequiredActorTag != NAME_None 时，仅保留带该 Tag 的 Actor 的 static mesh（landscape 始终包含）。 */
	FCSBoxScenePreparedData PrepareBoxSceneTriangles(
		UWorld* World,
		const FBox& QueryBox,
		int32 InMaxTriangles = -1,
		const TArray<FVector>& InReferencePoints = TArray<FVector>(),
		float InReferenceFilterDistance = 0.0f,
		FName RequiredActorTag = NAME_None);

	/** [render thread] 消费 PrepareBoxSceneTriangles 的预备数据，在 GraphBuilder 上建出 triangle-soup
	 *  buffer。只做 RHI/RDG 操作，不触碰 UObject，可安全在 ENQUEUE_RENDER_COMMAND lambda 内调用。 */
	FCSStaticMeshTriangleRDGOutput AddPreparedBoxSceneTrianglesToRDG(
		FRDGBuilder& GraphBuilder,
		FRHICommandListImmediate& RHICmdList,
		const FCSBoxScenePreparedData& Prepared,
		const TCHAR* DebugName = TEXT("CS.BoxSceneTriangles"));

	/** Reads back box-scene triangles into a CPU FCSTriangleMeshData by dispatching
	 *  AddBoxSceneTrianglesToRDG on the render thread and copying the GPU triangle-soup buffer back.
	 *  ReferenceFilterDistance <= 0 (or empty ReferencePoints) keeps all triangles in the generator
	 *  bounds; otherwise triangles are GPU-filtered by distance to this actor's ReferencePoints.
	 *  Also refreshes LastTriangleTextureData. Blocks via FlushRenderingCommands. */
	FCSTriangleMeshData GetBoxSceneTrianglesFromGPUFiltered(float ReferenceFilterDistance = 200.0f);

	/** Rasterizes a GPU triangle soup into a 2D heightmap via top-down orthographic projection.
	 *  Output format matches SceneCapture depth: texel.x = CameraHeight - WorldZ.
	 *  Runs entirely within the supplied FRDGBuilder; must be called on the render thread. */
	void RasterizeTriangleSoupToHeightmapRDG(
		FRDGBuilder& GraphBuilder,
		const FCSStaticMeshTriangleRDGOutput& TriangleOutput,
		FRDGTextureRef OutputHeightmap,
		const FBox& WorldBounds,
		float CameraHeight);

	/** Converts an ALandscape::RenderHeightmap G16 output into the depth format (CameraHeight - WorldZ)
	 *  and merges it into an existing OutputHeightmap using min (higher terrain wins).
	 *  Runs entirely within the supplied FRDGBuilder; must be called on the render thread. */
	void ConvertLandscapeHeightmapToDepthRDG(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef LandscapeG16Texture,
		FRDGTextureRef OutputHeightmap,
		float CameraHeight,
		float LandscapeScaleZ,
		float LandscapeOriginZ);

	/** Captures the landscape heightmap using GeneratorBounds as the capture area.
	 *  bOutputWorldHeight=true  → RGBA16f with RGB=Normal, A=WorldZ (cm)
	 *  bOutputWorldHeight=false → Depth from CameraHeight (R channel)
	 *  If OutRT is null, auto-creates a temporary RT and draws DrawDebugPoint.
	 *  Iterates ALL ALandscape actors; supports multi-landscape merge and World Partition.
	 *  @param OutRT Output render target (null for debug mode)
	 *  @param bOutputWorldHeight true=Normal+WorldZ, false=Depth */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Heightmap")
	bool CaptureLandscapeHeightmap(UTextureRenderTarget2D* OutRT, bool bOutputWorldHeight = true);

	/** Explicit-parameter overload for CaptureLandscapeHeightmap(Depth mode).
	 *  Iterates ALL ALandscape actors and min-merges (highest terrain wins). */
	bool CaptureLandscapeHeightmapToDepth(
		FVector WorldCenter,
		float CaptureExtent,
		float CameraHeight,
		UTextureRenderTarget2D* OutDepthRT);

	/** Converts an ALandscape::RenderHeightmap G16 output into Normal+Height format
	 *  (RGBA: Normal.XYZ, WorldHeight_cm) via finite-difference normals.
	 *  When bMergeByMaxZ is true, only overwrites texels where the new worldZ exceeds the
	 *  existing .w value — used to composite multiple landscapes (output must be pre-cleared
	 *  with .w = -large for correct results).
	 *  Runs entirely within the supplied FRDGBuilder; must be called on the render thread. */
	void ConvertLandscapeHeightmapToNormalHeightRDG(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef LandscapeG16Texture,
		FRDGTextureRef OutputNormalHeight,
		float LandscapeScaleZ,
		float LandscapeOriginZ,
		FVector2f TexelWorldSize,
		bool bMergeByMaxZ = false);

	/** Explicit-parameter overload for CaptureLandscapeHeightmap(WorldHeight mode). */
	bool CaptureLandscapeHeightmapGPU(
		FVector WorldCenter,
		float CaptureExtent,
		UTextureRenderTarget2D* OutNormalHeightRT);

	/** GPU triangle extraction from landscape heightmap.
	 *  Renders the landscape heightmap in GeneratorBounds, then a compute shader converts
	 *  each texel into 2 triangles (6 world-space vertices) in a StructuredBuffer.
	 *  Returns readback vertex data as FCSTriangleMeshData.
	 *  @param TextureSize Resolution of the intermediate heightmap (default 128) */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Heightmap")
	FCSTriangleMeshData CaptureLandscapeTrianglesGPU(int32 TextureSize = 128);

	/** Static utility: renders a landscape heightmap via ALandscape::RenderHeightmap (GPU)
	 *  and converts to Normal+Height (RGB=Normal, A=WorldHeight_cm) in the given RT.
	 *  Does NOT require an AComputeShaderMeshGenerator instance.
	 *  @param Landscape The landscape actor to capture
	 *  @param WorldCenter Center of the capture area
	 *  @param WorldExtentXY Half-size of the capture area (only XY used)
	 *  @param OutNormalHeightRT Output render target (RGBA16f/RGBA32f, bCanCreateUAV=true) */
	static bool RenderLandscapeToNormalHeightRT(
		ALandscape* Landscape,
		FVector WorldCenter,
		FVector WorldExtentXY,
		UTextureRenderTarget2D* OutNormalHeightRT);

	/** Converts the latest bounded scene surface voxels into an open quad-strip DynamicMesh. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	UDynamicMesh* SurfaceVoxelsToOpenDynamicMesh(float VoxelSize = 10.0f,
		bool bReverseOrientation = false,
		bool bRecomputeNormals = false);

	/** Converts bounded scene surface voxels into a VDB-style meshed surface DynamicMesh. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	UDynamicMesh* SurfaceVoxelsToVDBMesh(float VoxelSize = 10.0f,
		float RadiusMult = 2.0f,
		bool bRecomputeNormals = true);

	/** Builds a render-facing DynamicMesh from collected scene triangles.
	 *  If ReferenceFilterDistance is 0 or ReferencePoints is empty, returns all triangles
	 *  within the box; otherwise filters triangles by distance to reference points.
	 *  Keep bReverseOrientation=true by default: downstream vine/BVH output relies on
	 *  this DynamicMesh-facing winding even though the source triangle data is already normalized. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	UDynamicMesh* GetBoxSceneTrianglesFilteredToDynamicMesh(float ReferenceFilterDistance = 200.0f,
		bool bReverseOrientation = true,
		bool bSkipDegenerateTriangles = true,
		bool bRecomputeNormals = true);

	/** Voxelizes filtered scene triangles and outputs world-space positions and normals.
	 *  If ReferenceFilterDistance is 0 or ReferencePoints is empty, voxelizes all triangles
	 *  within the box; otherwise only voxelizes triangles near reference points. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	void GetBoxSceneFilteredSurfaceVoxels(float VoxelSize,
		float ReferenceFilterDistance,
		TArray<FVector>& OutPositions,
		TArray<FVector>& OutNormals);

	/** Synchronously voxelizes the bounded scene surface and reads the voxels back to the CPU by
	 *  running the RDG surface-voxel pass on the render thread and blocking via FlushRenderingCommands.
	 *  Keeps all triangles within the generator bounds (no reference-point filtering). Refreshes the
	 *  cached LastSurfaceVoxelData / LastSurfaceVoxelTextureData and returns the sanitized voxel data. */
	FCSSurfaceVoxelData ReadbackBoxSceneSurfaceVoxelsSync(float VoxelSize, const TCHAR* DebugName = nullptr);

	/** Returns the handle for the most recently stored triangle texture data. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorTriangleTextureDataHandle GetLastTriangleTextureData() const { return LastTriangleTextureData; }

	/** Returns the handle for the most recently stored surface-voxel texture data. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorSurfaceVoxelTextureDataHandle GetLastSurfaceVoxelTextureData() const { return LastSurfaceVoxelTextureData; }

	/** Rebuilds triangle data for the generator bounds and stores it in transient render targets. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorTriangleTextureDataHandle UpdateBoxSceneTriangleTextureData(float ReferenceFilterDistance = 200.0f);

	/** Rebuilds surface-voxel data for the generator bounds and stores it in transient render targets. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorSurfaceVoxelTextureDataHandle UpdateBoxSceneSurfaceVoxelTextureData(float VoxelSize = 10.0f);

	/** Releases transient generated-data render targets and invalidates the cached handles. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data")
	void ClearGeneratedDataTextureCache();

	// -------------------------------------------------------------------------
	// Debug System
	// -------------------------------------------------------------------------

	/** Builds an isolated debug quad at each surface voxel to visualize voxel placement and orientation. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh|Debug")
	UDynamicMesh* SurfaceVoxelsToIsolatedQuadsDebug(float VoxelSize = 10.0f,
		bool bReverseOrientation = false);

	/** Draws debug direction lines and optional points from the last cached surface-voxel data. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugLastSurfaceVoxelDirections(
		const FCSDebugLastVoxelDirectionOptions& Options = FCSDebugLastVoxelDirectionOptions()) const;

	/** Regenerates bounded scene surface voxels and draws their normals as debug direction lines. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugBoxSceneSurfaceVoxelDirections(
		const FCSDebugBoxVoxelDirectionOptions& Options);

	/** Draws active cache voxel cells, optionally limited to one request and including the cache bounds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugActiveVoxels(
		const FCSDebugActiveVoxelOptions& Options = FCSDebugActiveVoxelOptions()) const;

	/** Spawns a temporary ADynamicMeshActor at this actor's location,
	 *  converts CachedSurfaceTriangles into a DynamicMesh,
	 *  and destroys the actor after LifetimeSeconds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly, DisplayName = "Spawn Debug Surface Triangles DynamicMesh Actor"))
	void SpawnDebugSurfaceTrianglesDynamicMeshActor(float LifetimeSeconds = 10.0f);

	// -------------------------------------------------------------------------
	// Core System - Dynamic Mesh Helpers
	// -------------------------------------------------------------------------

	/** Replaces the actor's generated DynamicMesh and refreshes render/culling bounds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	bool SetGeneratedDynamicMesh(UDynamicMesh* NewMesh, float BoundsScale = -1.0f);

	/** Updates DynamicMeshComponent culling settings after geometry or bounds-scale changes. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	void RefreshDynamicMeshComponentCullingBounds(float BoundsScale = -1.0f);

	// -------------------------------------------------------------------------
	// Dirty Cache System
	// -------------------------------------------------------------------------

	/** Ensures the triangle cache covers the request's active cells and refreshes dirty pages as needed. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCache(const FCSMeshGeneratorTriangleCacheRequest& Request);

	/** Ensures a default bounds-based cache request using the current GeneratorBounds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(
		FName RequestId,
		bool bForceFullRebuild = false);

	/** Ensures a bounds-based cache request using an explicit box center and extent. */
	virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(
		FName RequestId,
		const FVector& BoxCenter,
		const FVector& BoxExtent,
		bool bForceFullRebuild = false);

	/** Refreshes the default GeneratorBounds-based cache request. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void UpdateMeshGeneratorCacheByBox(bool bForceFullRebuild = false);

	/** Removes a persistent cache interest request and frees cells no longer needed by any request. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void ReleaseTriangleCacheRequest(FName RequestId);

	/** Clears all cache requests, active cells, dirty state, and GPU cache render targets. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void ClearMeshGeneratorCache();

	/** Marks every currently active voxel page dirty so the next update rewrites cached triangle data. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void MarkAllActiveVoxelsDirty();

	/** Returns a lightweight summary of the current triangle-cache state. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Triangle Cache")
	FCSMeshGeneratorTriangleCacheHandle GetTriangleCacheHandle() const;

	/** Returns the world-space bounds currently covered by the triangle cache. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Triangle Cache")
	FBox GetCachedWorldBounds() const { return CacheState.CachedWorldBounds; }

protected:
	// -------------------------------------------------------------------------
	// Core System - Lifecycle
	// -------------------------------------------------------------------------

	/** Draws a bounded number of debug directions and optional points from matched position/direction arrays. */
	int32 DrawDebugDirectionArray(
		const TArray<FVector>& Positions,
		const TArray<FVector>& Directions,
		float DirectionLength = 100.0f,
		FLinearColor DirectionColor = FLinearColor::Blue,
		float Duration = 5.0f,
		float Thickness = 2.0f,
		bool bPersistentLines = false,
		bool bDrawPoints = true,
		FLinearColor PointColor = FLinearColor::Yellow,
		float PointSize = 8.0f,
		int32 MaxDirectionsToDraw = 0) const;

	/** Keeps construction-time component/render settings synchronized when actor properties change in editor. */
	virtual void OnConstruction(const FTransform& Transform) override;
	/** Refreshes component/render settings after all components have been registered. */
	virtual void PostRegisterAllComponents() override;
	/** Releases transient GPU resources when the actor leaves play. */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// -------------------------------------------------------------------------
	// Dirty Cache System - Internals
	// -------------------------------------------------------------------------

	/** Returns true when cache settings or bounds changed enough to require recreating all cache resources. */
	virtual bool DoesInputRequireFullRebuild(const FBox& InputWorldBounds) const;
	/** Reinitializes cache state, render targets, and page mappings for the supplied world bounds. */
	virtual void RebuildCacheResources(const FBox& InputWorldBounds);
	/** Builds active cache cells around this actor's ReferencePoints using ActivationRadius. */
	virtual void BuildActiveCellsFromReferencePoints(float ActivationRadius, TSet<FIntVector>& OutCells) const;
	/** Builds active cache cells around the supplied reference points using ActivationRadius. */
	virtual void BuildActiveCellsFromReferencePoints(const TArray<FVector>& InReferencePoints, float ActivationRadius, TSet<FIntVector>& OutCells) const;
	/** Combines all persistent request cell sets into one active-cell set. */
	virtual void BuildUnionActiveCells(TSet<FIntVector>& OutCells) const;
	/** Computes cells to activate/deactivate by comparing NewActiveCells against the current cache state. */
	virtual void DiffActiveCells(const TSet<FIntVector>& NewActiveCells);
	/** Assigns free cache texture pages to newly active cells. */
	virtual void AllocatePagesForCells(const TSet<FIntVector>& Cells);
	/** Frees cache texture pages for cells that are no longer active. */
	virtual void ReleasePagesForCells(const TSet<FIntVector>& Cells);
	/** Queues render-thread compute work to rewrite triangle-cache pages marked dirty. */
	virtual void DispatchDirtyVoxelTriangleCacheUpdate();

	/** Returns the current GeneratorBounds component as a valid world-space box when possible. */
	FBox GetGeneratorBoundsWorldBox() const;
	/** Computes the integer voxel-grid dimensions needed to cover InputWorldBounds. */
	FIntVector ComputeGridSize(const FBox& InputWorldBounds) const;
	/** Converts a world position into a clamped cache voxel key. */
	FIntVector WorldPositionToCell(FVector WorldPosition) const;
	/** Returns the world-space bounds for a single cache voxel cell. */
	FBox GetCellWorldBounds(const FIntVector& Cell) const;
	/** Releases transient render targets used by the triangle cache. */
	void ReleaseCacheResources();
	/** Clears runtime cache state and optionally removes persistent cache requests. */
	void ResetCacheRuntime(bool bClearRequests);
	/** Populates the free-page stack for all available cache pages. */
	void InitializeFreePages();
	/** Allocates UAV-capable render targets sized for cache metadata, triangle vertices, and triangle normals. */
	void CreateCacheRenderTargets();
	/** Stores CPU triangle data into generated-data texture targets and updates LastTriangleTextureData. */
	void StoreTriangleTextureData(const FCSTriangleMeshData& TriangleData, float ReferenceFilterDistance, FBox SourceWorldBounds = FBox(ForceInit));
	/** Stores CPU surface-voxel data into generated-data texture targets and updates LastSurfaceVoxelTextureData. */
	void StoreSurfaceVoxelTextureData(const FCSSurfaceVoxelData& SurfaceVoxelData, FVector VoxelOrigin);
	/** Releases triangle generated-data textures and invalidates the triangle data handle. */
	void ClearTriangleTextureData();
	/** Releases surface-voxel generated-data textures and invalidates the surface-voxel data handle. */
	void ClearSurfaceVoxelTextureData();
	/** Gets or allocates a transient generated-data render target with the requested size. */
	UTextureRenderTarget2D* GetOrCreateGeneratedDataRenderTarget(TObjectPtr<UTextureRenderTarget2D>& RenderTarget, const TCHAR* BaseName, int32 Width, int32 Height);
	/** Rebuilds per-request active-cell sets from LastRequests after a cache resource rebuild. */
	void RebuildRequestActiveCellsFromLastRequests();
	/** Checks whether triangle-cache render targets exist and can be written by compute shaders. */
	bool HasValidCacheResources() const;
	/** Compares two bounds using VoxelGridSettings.BoundsTolerance for cache reuse decisions. */
	bool AreBoundsCompatible(const FBox& A, const FBox& B) const;
	/** Converts NAME_None into the class default cache request id. */
	FName NormalizeRequestId(FName RequestId) const;

	FCSMeshGeneratorVoxelCacheState CacheState;

	TMap<FName, TSet<FIntVector>> RequestActiveCells;
	TMap<FName, FCSMeshGeneratorTriangleCacheRequest> LastRequests;
};
