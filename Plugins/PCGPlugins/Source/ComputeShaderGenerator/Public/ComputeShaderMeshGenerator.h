#pragma once

#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphBuilder.h"
#include "UDynamicMesh.h"
#include "ComputeShaderMeshGenerator.generated.h"

class AActor;
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

	// Small metadata texture. Pixel 0 = counts/size, pixels 1-3 = origin/bounds/dimensions.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT = nullptr;
};

// -----------------------------------------------------------------------------
// Dirty Cache Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorVoxelKey
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Voxel")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Voxel")
	int32 Y = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Voxel")
	int32 Z = 0;

	FCSMeshGeneratorVoxelKey() = default;
	FCSMeshGeneratorVoxelKey(int32 InX, int32 InY, int32 InZ)
		: X(InX), Y(InY), Z(InZ)
	{
	}

	explicit FCSMeshGeneratorVoxelKey(const FIntVector& InCell)
		: X(InCell.X), Y(InCell.Y), Z(InCell.Z)
	{
	}

	FIntVector ToIntVector() const
	{
		return FIntVector(X, Y, Z);
	}

	bool operator==(const FCSMeshGeneratorVoxelKey& Other) const
	{
		return X == Other.X && Y == Other.Y && Z == Other.Z;
	}
};

FORCEINLINE uint32 GetTypeHash(const FCSMeshGeneratorVoxelKey& Key)
{
	return HashCombine(HashCombine(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y)), ::GetTypeHash(Key.Z));
}

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorVoxelGridSettings
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Voxel")
	float VoxelSize = 100.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Voxel")
	float ActivationRadius = 200.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Voxel")
	int32 MaxActiveVoxels = 4096;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	int32 MaxTrianglesPerVoxel = 256;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	int32 LODIndex = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	float BoundsTolerance = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	int32 MaxCacheTextureDimension = 4096;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorTriangleCacheRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Triangle Cache")
	FName RequestId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Triangle Cache")
	bool bForceFullRebuild = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Triangle Cache")
	float ActivationRadiusOverride = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Triangle Cache")
	bool bPersistentInterest = true;

	TArray<FVector> CachedReferencePoints;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorTriangleCacheHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	int32 CacheGeneration = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	FBox CachedWorldBounds = FBox(ForceInit);

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	FIntVector GridSize = FIntVector::ZeroValue;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	float VoxelSize = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	int32 ActiveVoxelCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	int32 DirtyVoxelCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
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

	TSet<FCSMeshGeneratorVoxelKey> ActiveCells;
	TSet<FCSMeshGeneratorVoxelKey> CellsToActivate;
	TSet<FCSMeshGeneratorVoxelKey> CellsToDeactivate;
	TSet<FCSMeshGeneratorVoxelKey> DirtyCells;
	TMap<FCSMeshGeneratorVoxelKey, int32> CellToPage;
	TArray<int32> FreePages;
};

// -----------------------------------------------------------------------------
// Brush Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSInstancePaintComponentSlot
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Instance Brush")
	TObjectPtr<UStaticMesh> Mesh = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Instance Brush")
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component = nullptr;
};

UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API AComputeShaderMeshGenerator : public ADynamicMeshActor
{
	GENERATED_BODY()

public:
	/** Creates the generator actor, scene root, bounds component, and DynamicMesh rendering defaults. */
	AComputeShaderMeshGenerator(const FObjectInitializer& ObjectInitializer);

	// -------------------------------------------------------------------------
	// Core System
	// -------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UBoxComponent> GeneratorBounds;



	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	FCSMeshGeneratorVoxelGridSettings VoxelGridSettings;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Reference Filter")
	TArray<FVector> ReferencePoints;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Scene Filter")
	FName ExcludedActorTag = TEXT("UA");

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 MaxTriangles = 20000000;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 MaxVoxels = 2000000;

	// Per-triangle surface voxelization budget. This is intended to limit how many
	// voxel cells one large triangle may scan when VoxelSize is small; MaxVoxels is
	// the separate total output capacity.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 MaxVoxelCellsPerTriangle = 4096;

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

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
	TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Mesh|Debug")
	int64 GeneratorTimeCode = -1;

	// -------------------------------------------------------------------------
	// Brush System
	// -------------------------------------------------------------------------

	static FCSInstanceBrushEditorRequest OnInstanceBrushEditorRequest;

	UPROPERTY( BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush")
	TObjectPtr<UStaticMesh> InstanceBrushMesh = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "1.0", UIMin = "1.0"))
	float InstanceBrushRadius = 500.0f;

	UPROPERTY( BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "1"))
	int32 InstanceBrushSamplesPerMouseMove = 16;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float InstanceBrushMinSpacing = 100.0f;

	UPROPERTY( BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float InstanceBrushTraceRadius = 0.0f;

	UPROPERTY( BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "0.1", UIMin = "0.1"))
	float InstanceBrushPreviewPointSize = 8.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "0.01", UIMin = "0.01"))
	float InstanceBrushPreviewLifetime = 0.1f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush")
	bool bInstanceBrushAlignToNormal = true;

	UPROPERTY( BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush")
	bool bInstanceBrushUseGeneratorBounds = true;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush")
	bool bInstanceBrushExitAfterCommit = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "0.001", UIMin = "0.001"))
	FVector2D InstanceBrushUniformScaleRange = FVector2D(1.0f, 1.0f);

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float InstanceBrushRandomYawDegrees = 360.0f;

	UPROPERTY( BlueprintReadOnly, Category = "CS Mesh Generator|Instance Brush")
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

	/** Builds static-mesh triangle extraction requests from explicit actors, optionally culling by expanded reference bounds. */
	void BuildActorSceneTriangleRequests(TArray<AActor*> InActors,
		TArray<FCSStaticMeshTriangleRequest>& OutRequests,
		float BoundsExpand = 0.0f);

	/** Builds static-mesh triangle extraction requests for actors intersecting QueryBox in World. */
	void BuildBoxSceneTriangleRequests(UWorld* World,
		const FBox& QueryBox,
		TArray<FCSStaticMeshTriangleRequest>& OutRequests);

	/** Adds RDG compute passes that extract scene static-mesh triangles into GPU buffers. */
	FCSStaticMeshTriangleRDGOutput AddStaticMeshTrianglesToRDG(
		FRDGBuilder& GraphBuilder,
		FRHICommandListImmediate& RHICmdList,
		const TArray<FCSStaticMeshTriangleRequest>& Requests,
		float ReferenceFilterDistance = 200.0f,
		const TCHAR* DebugName = TEXT("CS.StaticMeshTriangles"),
		bool bNaniteOnlyFallbackMesh = true);

	/** Adds RDG compute passes that voxelize extracted triangle surfaces into GPU voxel buffers.
	 *  Includes centroid-based target positions and optional spatial blur pass. */
	FCSSurfaceVoxelRDGOutput AddTriangleSurfaceVoxelsToRDG(
		FRDGBuilder& GraphBuilder,
		const FCSStaticMeshTriangleRDGOutput& TriangleOutput,
		FVector VoxelOrigin,
		float VoxelSize = 10.0f,
		int32 HashSlotCount = 0,
		int32 BlurIterations = 0,
		int32 BlurRadius = 1,
		const TCHAR* DebugName = TEXT("CS.SurfaceVoxels"));


	/** Reads all scene triangles inside the generator bounds back from the GPU without reference-point filtering. */
	FCSTriangleMeshData GetBoxSceneTrianglesFromGPU();

	/** Reads scene triangles inside the generator bounds back from the GPU, optionally filtering near ReferencePoints. */
	FCSTriangleMeshData GetBoxSceneTrianglesFromGPUFiltered(float ReferenceFilterDistance = 200.0f);

	/** Voxelizes all scene triangles inside the generator bounds and reads positions/normals back from the GPU. */
	FCSSurfaceVoxelData GetBoxSceneSurfaceVoxelsFromGPU(float VoxelSize = 10.0f);

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

	/** Returns the handle for the most recently stored triangle texture data. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorTriangleTextureDataHandle GetLastTriangleTextureData() const;

	/** Returns the handle for the most recently stored surface-voxel texture data. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorSurfaceVoxelTextureDataHandle GetLastSurfaceVoxelTextureData() const;

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
		float DirectionLength = 0.0f,
		FLinearColor DirectionColor = FLinearColor::Blue,
		float Duration = 5.0f,
		float Thickness = 2.0f,
		bool bPersistentLines = false,
		bool bDrawPoints = true,
		FLinearColor PointColor = FLinearColor::Yellow,
		float PointSize = 8.0f,
		int32 MaxDirectionsToDraw = 0) const;

	/** Draws debug arrows and optional points from the last cached surface-voxel data. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Debug Last Surface Voxel Arrows"))
	int32 DrawDebugLastSurfaceVoxelArrows(
		float ArrowLength = 0.0f,
		FLinearColor ArrowColor = FLinearColor::Blue,
		float Duration = 5.0f,
		float Thickness = 2.0f,
		bool bPersistentLines = false,
		bool bDrawPoints = true,
		FLinearColor PointColor = FLinearColor::Yellow,
		float PointSize = 8.0f,
		int32 MaxArrowsToDraw = 0) const;

	/** Regenerates bounded scene surface voxels and draws their normals as debug direction lines. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugBoxSceneSurfaceVoxelDirections(
		float VoxelSize = 10.0f,
		float DirectionLength = 0.0f,
		FLinearColor DirectionColor = FLinearColor::Blue,
		float Duration = 5.0f,
		float Thickness = 2.0f,
		bool bPersistentLines = false,
		bool bDrawPoints = true,
		FLinearColor PointColor = FLinearColor::Yellow,
		float PointSize = 8.0f,
		int32 MaxDirectionsToDraw = 0);

	/** Regenerates bounded scene surface voxels and draws their normals as debug arrows. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Debug Box Scene Surface Voxel Arrows"))
	int32 DrawDebugBoxSceneSurfaceVoxelArrows(
		float VoxelSize = 10.0f,
		float ArrowLength = 0.0f,
		FLinearColor ArrowColor = FLinearColor::Blue,
		float Duration = 5.0f,
		float Thickness = 2.0f,
		bool bPersistentLines = false,
		bool bDrawPoints = true,
		FLinearColor PointColor = FLinearColor::Yellow,
		float PointSize = 8.0f,
		int32 MaxArrowsToDraw = 0);

	/** Draws active cache voxel cells, optionally limited to one request and including the cache bounds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugActiveVoxels(
		FName RequestId = NAME_None,
		FLinearColor DebugColor = FLinearColor::Green,
		float Duration = 5.0f,
		float Thickness = 2.0f,
		bool bPersistentLines = false,
		bool bDrawCacheBounds = true,
		int32 MaxVoxelsToDraw = 0) const;

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
	FBox GetCachedWorldBounds() const;

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
	virtual void BuildActiveCellsFromReferencePoints(float ActivationRadius, TSet<FCSMeshGeneratorVoxelKey>& OutCells) const;
	/** Builds active cache cells around the supplied reference points using ActivationRadius. */
	virtual void BuildActiveCellsFromReferencePoints(const TArray<FVector>& InReferencePoints, float ActivationRadius, TSet<FCSMeshGeneratorVoxelKey>& OutCells) const;
	/** Combines all persistent request cell sets into one active-cell set. */
	virtual void BuildUnionActiveCells(TSet<FCSMeshGeneratorVoxelKey>& OutCells) const;
	/** Computes cells to activate/deactivate by comparing NewActiveCells against the current cache state. */
	virtual void DiffActiveCells(const TSet<FCSMeshGeneratorVoxelKey>& NewActiveCells);
	/** Assigns free cache texture pages to newly active cells. */
	virtual void AllocatePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells);
	/** Frees cache texture pages for cells that are no longer active. */
	virtual void ReleasePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells);
	/** Queues render-thread compute work to rewrite triangle-cache pages marked dirty. */
	virtual void DispatchDirtyVoxelTriangleCacheUpdate();

	/** Returns the current GeneratorBounds component as a valid world-space box when possible. */
	FBox GetGeneratorBoundsWorldBox() const;
	/** Computes the integer voxel-grid dimensions needed to cover InputWorldBounds. */
	FIntVector ComputeGridSize(const FBox& InputWorldBounds) const;
	/** Converts a world position into a clamped cache voxel key. */
	FCSMeshGeneratorVoxelKey WorldPositionToCell(FVector WorldPosition) const;
	/** Returns the world-space bounds for a single cache voxel cell. */
	FBox GetCellWorldBounds(const FCSMeshGeneratorVoxelKey& Cell) const;
	/** Releases transient render targets used by the triangle cache. */
	void ReleaseCacheResources();
	/** Clears runtime cache state and optionally removes persistent cache requests. */
	void ResetCacheRuntime(bool bClearRequests);
	/** Populates the free-page stack for all available cache pages. */
	void InitializeFreePages();
	/** Allocates UAV-capable render targets sized for cache metadata, triangle vertices, and triangle normals. */
	void CreateCacheRenderTargets();
	/** Stores CPU triangle data into generated-data texture targets and updates LastTriangleTextureData. */
	void StoreTriangleTextureData(const FCSTriangleMeshData& TriangleData, float ReferenceFilterDistance);
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

	UPROPERTY(Transient)
	FCSMeshGeneratorVoxelCacheState CacheState;

	TMap<FName, TSet<FCSMeshGeneratorVoxelKey>> RequestActiveCells;
	TMap<FName, FCSMeshGeneratorTriangleCacheRequest> LastRequests;
};
