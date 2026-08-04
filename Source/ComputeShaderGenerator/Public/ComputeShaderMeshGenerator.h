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
#include "CSGpuMemoryBudget.h"
#include "CSGpuTriangleUtilities.h"
#include "ComputeShaderMeshGenerator.generated.h"

class AActor;
class ALandscape;
class UCSDirectTriangleMeshComponent;
class UCSMeshGeneratorDebugComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

class AComputeShaderMeshGenerator;

DECLARE_MULTICAST_DELEGATE_OneParam(FCSInstanceBrushEditorRequest, AComputeShaderMeshGenerator*);

// Triangle-soup 鏉愯川 id 鐨?鏃犳潗璐?鍝ㄥ叺鍊硷紙濡傚湴褰?CPU 涓夎褰級銆俿oup 鏉愯川 buffer 浼氶娓呮垚璇ュ€硷紝
// GPU extract 鍙 static mesh 涓夎褰㈠啓鍏ョ湡瀹?registry id锛屽洜姝ゅ湴褰?鏈啓鍏ョ殑涓夎淇濇寔鏃犳潗璐ㄣ€?
inline constexpr uint32 CS_NO_MATERIAL_ID = 0xFFFFFFFFu;

// -----------------------------------------------------------------------------
// Core Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType, meta = (DisplayName = "CS Triangle Mesh Data"))
struct COMPUTESHADERGENERATOR_API FCSTriangleMeshData
{
	GENERATED_BODY()
public:
	// GPU readback 鍚庣殑 compact vertex buffer銆倄yz 鏄《鐐逛綅缃€?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Vertices;

	// 鏈夋晥 vertex 鏁般€傚皬浜?0 鏃朵娇鐢?Vertices.Num()銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 VertexCount = -1;

	// 鍙€?index buffer銆傛瘡 3 涓?index 缁勬垚涓€涓?triangle銆?
	// 濡傛灉涓虹┖锛屽垯 Vertices 浼氭寜 triangle soup 瑙ｉ噴锛?/1/2, 3/4/5, ...
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<int32> Indices;

	// 鏈夋晥 index 鏁般€傚皬浜?0 鏃朵娇鐢?Indices.Num()銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 IndexCount = -1;

	// 鍙€?vertex normal銆傝嫢 bRecomputeNormals 涓?true锛屼笅娓?DynamicMesh 鍙拷鐣ュ畠骞堕噸绠楁硶绾裤€?
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

	// 鍙€夊寘鍥寸洅銆傛湁鏁堟椂浣滀负绮楃瓫锛涙棤鏁堟椂涓嶆寜 Bounds 绛涢€夈€?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FBox WorldBounds = FBox(ForceInit);

	// 鐢熸垚 Request 鐨勬潵婧?Actor銆傜敤浜庡湪 RDG 涓夎褰㈡彁鍙栭樁娈垫帓闄よ嚜韬垨鎸囧畾 Tag 鐨?Actor銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	AActor* SourceActor = nullptr;

	// Optional originating component. Used to preserve component-instance painted vertex colors.
	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh")
	TObjectPtr<UStaticMeshComponent> SourceComponent = nullptr;

	// 鏉ユ簮 component 鐨勬潗璐ㄦЫ锛坥verride-aware锛屾潵鑷?Component->GetMaterial(i)锛夈€?
	// FStaticMeshSection.MaterialIndex 绱㈠紩杩涙湰鏁扮粍锛岀敤浜庢妸姣忎釜涓夎褰㈡槧灏勫洖鍏舵簮鏉愯川銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<TObjectPtr<UMaterialInterface>> MaterialSlots;
};

USTRUCT(BlueprintType, meta = (DisplayName = "CS Surface Voxel Data"))
struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelData
{
	GENERATED_BODY()
public:
	// GPU 鐢熸垚鐨?surface voxel 涓績鐐广€傛瘡涓?voxel 鍙〃绀轰竴涓〃闈㈤潰鐗囷紝涓嶇敓鎴愬皝闂?cube銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Positions;

	// 涓?Positions 涓€涓€瀵瑰簲鐨勮〃闈㈡硶绾匡紱鐢ㄤ簬鍚庣画鐢熸垚寮€鏀?mesh 鐨勯潰鏈濆悜銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Normals;

	// 鏈夋晥 voxel 鏁般€傚皬浜?0 鏃朵娇鐢?Positions.Num()銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 VoxelCount = -1;

	// 鐢熸垚 voxel 鏃朵娇鐢ㄧ殑 cell size锛屽悗缁浆 mesh 鏃跺彲浣滀负榛樿闈㈢墖澶у皬銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	float VoxelSize = 0.0f;

	// 浣撶礌鏁存暟缃戞牸鍧愭爣锛屼笌 Positions 涓€涓€瀵瑰簲锛?1 绱㈠紩涓烘棤鏁堬級銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FIntVector> Cells;

	// 闈㈢Н鍔犳潈璐ㄥ績锛坱arget position锛夛紝涓?Positions 涓€涓€瀵瑰簲銆傜敤浜庢洿绮剧‘鐨勮〃闈㈠尮閰嶃€?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> TargetPositions;

	// 浣撶礌缃戞牸鐨勪笘鐣岀┖闂村師鐐癸紙涓?Cells 鍧愭爣绯诲搴旓級銆?
	// Cell (cx, cy, cz) 鐨勪笘鐣岀┖闂翠腑蹇?= VoxelOrigin + (Cell + 0.5) * VoxelSize銆?
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FVector VoxelOrigin = FVector::ZeroVector;
};

// game-thread 棰勫濂界殑鐩掑唴鍦烘櫙涓夎褰㈡暟鎹細static mesh 宸?resolve 鍑烘覆鏌撹祫婧愬紩鐢紝
// landscape 宸插湪 game thread 瀹屾垚 CPU 鎻愬彇銆傚彲瀹夊叏鎹曡幏杩?render 绾跨▼ lambda锛屽啀浜ょ粰
// AddPreparedBoxSceneTrianglesToRDG 娑堣垂銆傚唴閮ㄧ敤 PImpl 闅愯棌 .cpp-only 鐨?resolved 绫诲瀷銆?
struct FCSBoxScenePreparedDataImpl;

struct COMPUTESHADERGENERATOR_API FCSBoxScenePreparedData
{
	TSharedPtr<FCSBoxScenePreparedDataImpl, ESPMode::ThreadSafe> Impl;

	bool IsValid() const { return Impl.IsValid(); }
	bool HasAnyTriangles() const;

	// 鍘婚噸鍚庣殑鏉愯川琛細soup 鏉愯川 buffer 閲岀殑 id 绱㈠紩杩涙湰琛ㄣ€侰S_NO_MATERIAL_ID 琛ㄧず鏃犳潗璐紙濡傚湴褰級銆?
	int32 GetMaterialRegistryNum() const;
	UMaterialInterface* GetMaterialByRegistryIndex(int32 Index) const;
};

struct COMPUTESHADERGENERATOR_API FCSStaticMeshTriangleRDGOutput
{
	// Triangle soup: 姣忎釜 triangle 鍗?3 涓?float4 vertex锛寁ertex.w = 1銆?
	FRDGBufferRef TriangleVertices = nullptr;
	FRDGBufferUAVRef TriangleVerticesUAV = nullptr;
	FRDGBufferSRVRef TriangleVerticesSRV = nullptr;

	// 涓?TriangleVertices 涓€涓€瀵瑰簲锛涙瘡涓?vertex 瀛?triangle normal锛宯ormal.w = 0銆?
	FRDGBufferRef TriangleNormals = nullptr;
	FRDGBufferUAVRef TriangleNormalsUAV = nullptr;
	FRDGBufferSRVRef TriangleNormalsSRV = nullptr;

	// Counter[0] = 瀹為檯鍐欏叆鐨?triangle 鏁帮紱鏈夋晥 vertex 鏁?= Counter[0] * 3銆?
	FRDGBufferRef TriangleCounter = nullptr;
	FRDGBufferUAVRef TriangleCounterUAV = nullptr;
	FRDGBufferSRVRef TriangleCounterSRV = nullptr;

	// 姣忎釜 triangle 涓€涓?uint 鏉愯川 id锛堜笌 TriangleVertices 骞宠锛屾寜 triangle 鑰岄潪 vertex 绱㈠紩锛夈€?
	// 鍊间负 FCSBoxScenePreparedData 鏉愯川琛ㄤ笅鏍囷紱CS_NO_MATERIAL_ID 琛ㄧず鏃犳潗璐紙濡傚湴褰級銆?
	// 鐢?GPU 鍦ㄤ笌椤剁偣鐩稿悓鐨?atomic slot 涓婂啓鍏ワ紝鏁?readback 鍚庡彲鎸?soup 涓夎搴忓彿鍙栧洖鏉愯川銆?
	FRDGBufferRef TriangleMaterialIds = nullptr;
	FRDGBufferUAVRef TriangleMaterialIdsUAV = nullptr;
	FRDGBufferSRVRef TriangleMaterialIdsSRV = nullptr;

	// 涓?TriangleVertices 涓€涓€瀵瑰簲锛堟寜 vertex 绱㈠紩锛? per triangle锛夛細姣忛《鐐?UV0锛坒loat2锛夈€?
	// 鐢?GPU extract 鍦ㄤ笌椤剁偣鐩稿悓鐨?atomic slot 鍐欏叆锛涙棤 UV 鐨勬簮锛堝湴褰?鏈粦瀹氾級淇濇寔 (0,0)銆?
	// 按通道交错存放：UV[Corner * NumUVChannels + Channel]。通道数取自源模型，
	// 源只有 1 条 UV 时就是 1，退化成原来的逐角点单 UV 布局。
	FRDGBufferRef TriangleUVs = nullptr;
	FRDGBufferUAVRef TriangleUVsUAV = nullptr;
	FRDGBufferSRVRef TriangleUVsSRV = nullptr;
	int32 NumUVChannels = 1;

	// Per-corner source vertex attributes, parallel to TriangleVertices.
	FRDGBufferRef TriangleColors = nullptr;
	FRDGBufferUAVRef TriangleColorsUAV = nullptr;
	FRDGBufferSRVRef TriangleColorsSRV = nullptr;
	FRDGBufferRef TriangleTangents = nullptr;
	FRDGBufferUAVRef TriangleTangentsUAV = nullptr;
	FRDGBufferSRVRef TriangleTangentsSRV = nullptr;
	FRDGBufferRef TriangleBiTangents = nullptr;
	FRDGBufferUAVRef TriangleBiTangentsUAV = nullptr;
	FRDGBufferSRVRef TriangleBiTangentsSRV = nullptr;

	uint32 MaxTriangles = 0;
	uint32 MaxVertices = 0;

	// 淇濇寔澶栭儴 RHI SRV 寮曠敤鐩村埌 GraphBuilder.Execute()锛岄伩鍏?RDG pass 鎵ц鍓嶈閲婃斁銆?
	TArray<FShaderResourceViewRHIRef> ReferencedIndexBufferSRVs;
};

struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelRDGOutput
{
	// Surface voxel center锛寈yz 鏄?voxel 涓績锛寃 = 1銆?
	FRDGBufferRef VoxelPositions = nullptr;
	FRDGBufferUAVRef VoxelPositionsUAV = nullptr;
	FRDGBufferSRVRef VoxelPositionsSRV = nullptr;

	// Surface voxel normal锛寈yz 鏄硶绾匡紝w = 0銆?
	FRDGBufferRef VoxelNormals = nullptr;
	FRDGBufferUAVRef VoxelNormalsUAV = nullptr;
	FRDGBufferSRVRef VoxelNormalsSRV = nullptr;

	// Counter[0] = 瀹為檯鍐欏叆鐨?voxel 鏁般€?
	FRDGBufferRef VoxelCounter = nullptr;
	FRDGBufferUAVRef VoxelCounterUAV = nullptr;
	FRDGBufferSRVRef VoxelCounterSRV = nullptr;

	// GPU 绔幓閲嶇敤 hash slots銆?
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

// GPU-resident surface voxels retained across the game/render boundary. The surface
// voxelizer (GetBoxSceneFilteredSurfaceVoxels) already builds these on the GPU as
// pooled buffers; retaining them here lets a consumer register them directly
// (RegisterExternalBuffer) instead of the readback -> repack -> re-upload round-trip.
// Layout matches what the vine VVVoxel projection consumes: float4 positions/normals/
// target-positions, int4 cells, uint hash slots. Counts/params come with them so no
// per-consume readback is needed. IsValid() means the buffers are usable.
struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelGPUBuffers
{
	TRefCountPtr<FRDGPooledBuffer> Positions;       // float4 (xyz center, w=1)
	TRefCountPtr<FRDGPooledBuffer> Normals;         // float4 (xyz normal, w=0), blur-resolved
	TRefCountPtr<FRDGPooledBuffer> TargetPositions; // float4 (xyz clipped centroid, w=1), blur-resolved
	TRefCountPtr<FRDGPooledBuffer> Cells;           // int4 (grid xyz, w=0)
	TRefCountPtr<FRDGPooledBuffer> Counter;         // uint2 ([0]=valid count, [1]=dropped count)
	// Two-buffer cell hash (producer format, StaticMeshPointSampler.usf): HashSlots[slot] =
	// HashCell(cell)+1 (key), HashIndices[slot] = voxelIndex+1. Probe (Hash+Probe*1103515245)%Count.
	TRefCountPtr<FRDGPooledBuffer> HashSlots;        // uint keys
	TRefCountPtr<FRDGPooledBuffer> HashIndices;      // uint voxelIndex+1 parallel to HashSlots
	int32 VoxelCapacity = 0;
	int32 VoxelCount = 0;
	uint32 HashSlotCount = 0;
	float VoxelSize = 0.0f;
	FVector VoxelOrigin = FVector::ZeroVector;
	FBox WorldBounds = FBox(ForceInit);
	bool IsValid() const { return Positions.IsValid() && Normals.IsValid() && TargetPositions.IsValid() && Cells.IsValid() && Counter.IsValid() && VoxelCapacity > 0; }
	void Reset() { *this = FCSSurfaceVoxelGPUBuffers(); }
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

	/** Delegating default constructor so subclasses can keep plain default constructors. */
	AComputeShaderMeshGenerator();

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

	/** Draws GPU triangle soup submitted via SubmitBoxSceneTrianglesToRenderPipeline directly through
	 *  the render pipeline (no readback, no DynamicMesh). Uses an absolute (world-origin) transform so
	 *  the world-space triangle positions render 1:1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UCSDirectTriangleMeshComponent> DirectMeshComponent;

	/** GPU-only visualization for surface voxels and cache cells. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Debug")
	TObjectPtr<UCSMeshGeneratorDebugComponent> MeshGeneratorDebugComponent;

	/** Dedicated GPU triangle component used by the surface-triangle debug entry point. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Debug")
	TObjectPtr<UCSDirectTriangleMeshComponent> DebugTriangleComponent;



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

	// -------------------------------------------------------------------------
	// GPU memory budget
	//
	// MaxTriangles above is a fixed authoring limit; it says nothing about the machine the
	// generator actually runs on. These settings drive the shared pre-flight check in
	// CSGpuMemoryBudget, which sizes the limit from the adapter's live free VRAM instead.
	// -------------------------------------------------------------------------

	/** Runs the shared VRAM pre-flight check before box-scene GPU pipelines start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget")
	bool bCheckGpuMemoryBudget = true;

	/** Fraction of the free VRAM a single operation is allowed to claim. The rest covers RDG
	 *  pooling, fragmentation and driver overhead, so values near 1.0 will crash before the
	 *  check ever fires. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float GpuMemoryBudgetSafetyRatio = 0.7f;

	/** Asks for confirmation when the estimate exceeds the budget. False aborts (or proceeds,
	 *  per bProceedWhenGpuMemoryBudgetUnattended) without showing a dialog. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget")
	bool bPromptWhenExceedingGpuMemoryBudget = true;

	/** What to do when the budget is exceeded but no dialog can be shown (commandlet, unattended
	 *  run, or a non-game thread caller). Default aborts, so batch jobs fail loudly instead of
	 *  taking the machine down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget")
	bool bProceedWhenGpuMemoryBudgetUnattended = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "0.001"))
	float QuadScale = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh")
	float NormalOffsetScale = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1.0"))
	float DynamicMeshCullBoundsScale = 10.0f;

	// -------------------------------------------------------------------------
	// Surface Voxel Blur 鈥?ResinRattan port
	// -------------------------------------------------------------------------

	/** Number of 3D mean-filter iterations applied after voxelization. 0 = disabled. */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "0"))
	int32 SurfaceVoxelBlurIterations = 0;

	/** Neighbourhood radius for the 3D mean filter. 1 = 3x3x3 Moore neighbourhood. */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 SurfaceVoxelBlurRadius = 1;

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

	// GPU-resident mirror of LastSurfaceVoxelData: the same voxel buffers the readback
	// sources, kept pooled so a consumer can register them directly instead of the
	// readback -> repack -> re-upload round-trip. Populated alongside LastSurfaceVoxelData
	// by GetBoxSceneFilteredSurfaceVoxels. Not a UPROPERTY (holds render resources).
	FCSSurfaceVoxelGPUBuffers LastSurfaceVoxelGPUBuffers;

	/** Triangle surface data used by the CPU/BVH vine visualization path.
	 *  Filled by GenerateVines(). */
	FCSTriangleMeshData CachedSurfaceTriangles;

	/** 结果资产名尾部的稳定编号（YYMMDDHHMM）。-1 表示尚未生成，首次保存时由
	 *  EnsureGeneratorTimeCode() 赋值并随 actor 存盘，之后每次运行都复用。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Mesh|Debug")
	int64 GeneratorTimeCode = -1;

	// -------------------------------------------------------------------------
	// Core System - Scene Extraction and Mesh Output
	// -------------------------------------------------------------------------

	void BuildBoxSceneTriangleRequests(UWorld* World,
		const FBox& QueryBox,
		TArray<FCSStaticMeshTriangleRequest>& OutRequests);

	/** [game thread] 鐢?FLandscapeComponentDataInterface 鍦?CPU 绔妸 QueryBox 鍐呯殑 landscape 楂樺害鍦?
	 *  鎻愬彇鎴?triangle-soup锛堜笘鐣屽潗鏍囷紝宸叉寜涓婃湞鍚戝畾鍚戯級锛屾殏瀛樿繘 OutTriangleData锛屼緵鍚庣画 RDG 娴佺▼浣滀负
	 *  initial triangle 涓婁紶銆傚繀椤诲湪 game thread 璋冪敤锛圕DI 鏋勯€犲己鍒?game thread锛夈€傛櫘閫?static mesh 涓嶈蛋
	 *  杩欓噷锛屼粛鐢ㄦ湰 class 鐨?GPU resolve 娴佺▼銆侷nReferencePoints 闈炵┖涓?InReferenceFilterDistance > 0 鏃讹紝
	 *  鎸夊埌鍙傝€冪偣鐨勮窛绂诲仛 CPU 绮楃瓫锛涘惁鍒欎繚鐣欑洅鍐呭叏閮ㄤ笁瑙掑舰銆侻axTriangles == 0 鏃剁洿鎺ヨ繑鍥炵┖銆?*/
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

	/** [game thread] 鏋氫妇 QueryBox 鍐呯殑 static mesh + landscape锛屽畬鎴?static mesh 娓叉煋璧勬簮 resolve
	 *  涓?landscape CPU 涓夎褰㈡彁鍙栵紝杩斿洖鍙畨鍏ㄦ崟鑾疯繘 render 绾跨▼ lambda 鐨勯澶囨暟鎹€?
	 *  蹇呴』鍦?game thread 璋冪敤锛堣Е纰?UObject / FLandscapeComponentDataInterface锛夈€?
	 *  RequiredActorTag != NAME_None 鏃讹紝浠呬繚鐣欏甫璇?Tag 鐨?Actor 鐨?static mesh锛坙andscape 濮嬬粓鍖呭惈锛夈€?*/
	FCSBoxScenePreparedData PrepareBoxSceneTriangles(
		UWorld* World,
		const FBox& QueryBox,
		int32 InMaxTriangles = -1,
		const TArray<FVector>& InReferencePoints = TArray<FVector>(),
		float InReferenceFilterDistance = 0.0f,
		FName RequiredActorTag = NAME_None,
		bool bIncludeLandscape = true,
		bool bUseMeshDescriptionSourceTriangles = true,
		// true keeps one registry entry per source (mesh, material slot), so a mesh with five
		// slots yields five output slots even when they share a material or are all unassigned.
		// false dedupes by material pointer only, giving the most compact list but losing the
		// source slot layout - every empty slot merges into one.
		bool bPreserveSourceMaterialSlots = true);

	/** [render thread] 娑堣垂 PrepareBoxSceneTriangles 鐨勯澶囨暟鎹紝鍦?GraphBuilder 涓婂缓鍑?triangle-soup
	 *  buffer銆傚彧鍋?RHI/RDG 鎿嶄綔锛屼笉瑙︾ UObject锛屽彲瀹夊叏鍦?ENQUEUE_RENDER_COMMAND lambda 鍐呰皟鐢ㄣ€?*/
	static FCSStaticMeshTriangleRDGOutput AddPreparedBoxSceneTrianglesToRDG(
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
	 *  bOutputWorldHeight=true  鈫?RGBA16f with RGB=Normal, A=WorldZ (cm)
	 *  bOutputWorldHeight=false 鈫?Depth from CameraHeight (R channel)
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
	 *  existing .w value 鈥?used to composite multiple landscapes (output must be pre-cleared
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

	/** Generic: rasterize an indexed GPU triangle mesh into OutHeightmap via top-down orthographic
	 *  projection (texel.x = CameraHeight - WorldZ). Extracts a triangle soup from the position/index
	 *  buffers (FExtractStaticMeshTrianglesCS) then runs RasterizeTriangleSoupToHeightmapRDG. Adds
	 *  passes to GraphBuilder; the caller executes. Must be called on the render thread.
	 *  @param PositionSRV      Buffer<float> SRV, xyz per vertex (stride 3 floats).
	 *  @param IndexSRV         Buffer<uint>  SRV, triangle-list indices.
	 *  @param TriangleCapacity Number of triangles to process (index_count / 3); extra degenerate
	 *                          triangles from unused capacity rasterize to nothing.
	 *  @param LocalToWorld     Transforms the (local-space) positions to world.
	 *  @param OutHeightmap     UAV-capable float heightmap (RDG-registered). */
	void RasterizeIndexedMeshToHeightmapRDG(
		FRDGBuilder& GraphBuilder,
		FRHIShaderResourceView* PositionSRV,
		FRHIShaderResourceView* IndexSRV,
		uint32 TriangleCapacity,
		const FMatrix44f& LocalToWorld,
		FRDGTextureRef OutHeightmap,
		const FBox& WorldBounds,
		float CameraHeight);

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

	/** Builds and retains bounded scene surface voxels entirely on the GPU. The valid count remains
	 *  in FCSSurfaceVoxelGPUBuffers::Counter and is consumed by downstream compute/indirect draws. */
	bool PrepareBoxSceneSurfaceVoxelsGPU(float VoxelSize, float ReferenceFilterDistance = 0.0f);

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

	/** Draws an isolated quad at each surface voxel directly from retained GPU buffers. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh|Debug")
	bool SurfaceVoxelsToIsolatedQuadsDebug(float VoxelSize = 10.0f,
		bool bReverseOrientation = false);

	/** Draws debug direction lines and optional points from the last retained GPU surface voxels. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugLastSurfaceVoxelDirections(
		const FCSDebugLastVoxelDirectionOptions& Options = FCSDebugLastVoxelDirectionOptions());

	/** Regenerates bounded scene surface voxels and draws their normals as debug direction lines. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugBoxSceneSurfaceVoxelDirections(
		const FCSDebugBoxVoxelDirectionOptions& Options);

	/** Extracts and draws bounded scene surface triangles through a dedicated GPU component. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly, DisplayName = "Draw GPU Surface Triangles"))
	void SpawnDebugSurfaceTrianglesDynamicMeshActor(float LifetimeSeconds = 10.0f);

	/** Clears all GPU-only MeshGenerator debug visualization. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly))
	void ClearMeshGeneratorGPUDebug();

	// -------------------------------------------------------------------------
	// Core System - Dynamic Mesh Helpers
	// -------------------------------------------------------------------------

	/** Replaces the actor's generated DynamicMesh and refreshes render/culling bounds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	bool SetGeneratedDynamicMesh(UDynamicMesh* NewMesh, float BoundsScale = -1.0f);

	// -------------------------------------------------------------------------
	// Core System - Direct GPU Render (no readback, no DynamicMesh)
	// -------------------------------------------------------------------------

	/** Directly submits the bounded scene surface triangles to the render pipeline: extracts the
	 *  GPU triangle soup for GeneratorBounds and draws it every frame through a custom scene proxy
	 *  (UCSDirectTriangleMeshComponent / FCSDirectTriangleMeshSceneProxy), with vertex/index data
	 *  living only on the GPU 閳?no CPU readback and no UDynamicMesh. Material is applied on the draw
	 *  (null keeps the component's current material). MaxDirectTriangles bounds the persistent GPU
	 *  buffers (the actual count is discovered on the GPU and never read back, so this cap sizes the
	 *  allocation). ReferenceFilterDistance filters by distance to ReferencePoints when > 0 and
	 *  ReferencePoints is non-empty. Returns false when there is no world/bounds/geometry. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	bool SubmitBoxSceneTrianglesToRenderPipeline(UMaterialInterface* Material = nullptr,
		int32 MaxDirectTriangles = 500000,
		float ReferenceFilterDistance = 0.0f);

	/** Returns the direct-render component that draws the submitted GPU triangle soup. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Mesh")
	UCSDirectTriangleMeshComponent* GetDirectMeshComponent() const { return DirectMeshComponent; }

	/** Saves the current direct GPU mesh as a StaticMesh asset. Editor only; returns null otherwise. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	UStaticMesh* SaveDirectGPUMeshToStaticMesh(
		const FString& AssetPathAndName,
		bool bReplaceExistingAsset = true,
		bool bSaveAsset = true,
		bool bConvertToActorLocalSpace = true);

	/** Updates DynamicMeshComponent culling settings after geometry or bounds-scale changes. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	void RefreshDynamicMeshComponentCullingBounds(float BoundsScale = -1.0f);

	// -------------------------------------------------------------------------
	// Core System - Result Asset Naming
	// -------------------------------------------------------------------------
	//
	// 结果资产的命名策略集中在这里，来自 CSSW 的烘焙流程：每个 actor 只认一个稳定编号，
	// 名字里不带每次运行的时间戳，因此同一个 actor 反复运行始终写同一个资产 —— 覆盖旧模型，
	// 而不是在 content browser 里堆出一串只差时间戳的副本。

	/** 返回本 actor 的稳定编号，首次调用时按当前时间生成一次并记入 GeneratorTimeCode。
	 *  懒生成而不是在构造函数里生成：构造期赋值会让 CDO 也带上一个编号，与 CDO 同值的实例不会被
	 *  delta 序列化，重新打开关卡后编号就变了 —— 编号一变，重跑就写出新资产而不是覆盖旧的。 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	int64 EnsureGeneratorTimeCode();

	/** 结果资产所在文件夹：<关卡所在目录>/<GetResultAssetFolderName()>（关卡 /Game/Maps/L_Foo
	 *  -> /Game/Maps/AutoResult）。关卡没有内容路径（未存盘的 /Temp 地图）时返回空串。 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	FString GetResultAssetFolderPath() const;

	/** 结果资产完整路径 <文件夹>/SM_<基名>_<编号><NameSuffix>。同一个 actor 编号恒定，重跑即覆盖。
	 *  只有一个 actor 要同时产出多份互不覆盖的结果时才需要传 NameSuffix。
	 *  关卡没有内容路径时返回空串，调用方应据此退回 transient 或显式路径。 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	FString BuildResultAssetPath(const FString& NameSuffix = TEXT(""));

protected:
	// -------------------------------------------------------------------------
	// Result asset naming policy (子类覆写点)
	// -------------------------------------------------------------------------

	/** 结果资产文件夹名，落在关卡同级。 */
	virtual FString GetResultAssetFolderName() const { return TEXT("AutoResult"); }

	/** 结果资产名里 SM_ 之后、编号之前的部分。默认用 actor 名（已是合法的包内对象名）。 */
	virtual FString GetResultAssetBaseName() const;

	/** 结果资产名尾部的稳定编号。子类若已有自己的持久化编号（如 CSSW 的 SWUniqueID），
	 *  覆写此函数即可沿用，已烘好的资产名不会变。 */
	virtual FString GetResultAssetUniqueTag();


	// -------------------------------------------------------------------------
	// Shared GPU triangle-soup algorithms
	// -------------------------------------------------------------------------

	/**
	 * Protected convenience entry point for derived generators that need spatial queries.
	 *
	 * The implementation is delegated to the stateless CSGpuTriangleUtilities module:
	 * inheritance provides discoverability to subclasses, while the actor remains free of
	 * render-resource ownership and Boolean-specific policy.
	 */
	static CSGpuTriangleUtilities::FTriangleLBVH AddTriangleLBVHToRDG(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef TriangleSoupSRV,
		int32 TriangleCount,
		int32 SortElementCount,
		const FVector3f& AabbMin,
		const FVector3f& InvExtent);

	/**
	 * Builds fast-winding multipoles for a shared LBVH. The base class exposes only
	 * geometric preprocessing; derived classes retain their own iso thresholds and
	 * sampling rules because those values describe algorithm policy.
	 */
	static FRDGBufferRef AddFastWindingToRDG(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef TriangleSoupSRV,
		const CSGpuTriangleUtilities::FTriangleLBVH& LBVH,
		int32 TriangleCount);

	/**
	 * Finds positional weld representatives on the GPU. Mesh-attribute merging and
	 * topology cleanup remain with the derived producer because different outputs have
	 * different seam, material, and winding requirements.
	 */
	static FRDGBufferRef AddVertexWeldToRDG(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef OutputTriangleSoup,
		FRDGBufferRef OutputTriangleCounter,
		int32 OutputTriangleCapacity,
		int32 SourceTriangleCapacity,
		const FVector3f& GridOrigin,
		float WeldDistance,
		FRDGBufferSRVRef TriangleFilter = nullptr,
		uint32 TriangleFilterMask = 0u);

	/**
	 * Pre-flight VRAM check for a box-scene pipeline, using this actor's budget settings.
	 *
	 * The cost model is the caller's: only the derived generator knows which buffers its own
	 * pass will allocate and at what multiplier. Everything device-dependent - free VRAM,
	 * the triangle estimate, the confirmation policy - is delegated to CSGpuMemoryBudget so
	 * every generator answers the question the same way.
	 *
	 * Must be called on the game thread, before any extraction work. Returns true to proceed.
	 */
	bool ConfirmGpuMemoryBudgetForBoxScene(
		const TCHAR* OperationName,
		const FBox& QueryBox,
		const CSGpuMemoryBudget::FTriangleSoupCostModel& Cost,
		bool bIncludeLandscape) const;

	// -------------------------------------------------------------------------
	// Core System - Lifecycle
	// -------------------------------------------------------------------------
	/** Shared surface-voxel producer. CPU output/readback is performed only when bReadbackToCPU is true. */
	void BuildBoxSceneFilteredSurfaceVoxels(float VoxelSize,
		float ReferenceFilterDistance,
		TArray<FVector>& OutPositions,
		TArray<FVector>& OutNormals,
		bool bReadbackToCPU);

	/** Keeps construction-time component/render settings synchronized when actor properties change in editor. */
	virtual void OnConstruction(const FTransform& Transform) override;
	/** Refreshes component/render settings after all components have been registered. */
	virtual void PostRegisterAllComponents() override;
	/** Releases transient GPU resources when the actor leaves play. */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Clears the dedicated triangle debug component after its requested lifetime. */
	void ClearDebugTriangleComponent();

	/** Returns the current GeneratorBounds component as a valid world-space box when possible. */
	FBox GetGeneratorBoundsWorldBox() const;
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

	FTimerHandle DebugTriangleClearTimerHandle;
};
