// -----------------------------------------------------------------------------
// AMeshGeneratorBrushCache 实现
//
// 原先整段住在 ComputeShaderMeshGenerator.cpp（该文件一度 6677 行）；2026-08 整理
// 归位到本文件。包含：实例笔刷（StartInstanceBrush / paint 组件）、体素分页三角形
// 缓存（EnsureTriangleCache* 系列，协议见 Docs/ComputeShaderMeshGeneratorVoxelTriangleCache.md）、
// 脏体素增量更新 dispatch，以及仅本 TU 使用的 2 个缓存 shader。
// -----------------------------------------------------------------------------

#include "MeshGeneratorBrushCache.h"
#include "MeshGeneratorInternal.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"
#include "UObject/Package.h"

using namespace CSMeshGenInternal;

FCSInstanceBrushEditorRequest AMeshGeneratorBrushCache::OnInstanceBrushEditorRequest;


// -----------------------------------------------------------------------------
// Dirty Cache System - Shaders
// -----------------------------------------------------------------------------

class FClearDirtyVoxelCacheCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearDirtyVoxelCacheCS);
	SHADER_USE_PARAMETER_STRUCT(FClearDirtyVoxelCacheCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, DirtyVoxelPages)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VoxelMetaTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleVertexTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleNormalTexture)
		SHADER_PARAMETER(uint32, DirtyVoxelCount)
		SHADER_PARAMETER(uint32, CacheGeneration)
		SHADER_PARAMETER(uint32, MaxTrianglesPerVoxel)
		SHADER_PARAMETER(uint32, MetaTextureWidth)
		SHADER_PARAMETER(uint32, MetaTextureHeight)
		SHADER_PARAMETER(uint32, TriangleVertexTextureWidth)
		SHADER_PARAMETER(uint32, TriangleVertexTextureHeight)
		SHADER_PARAMETER(uint32, TriangleNormalTextureWidth)
		SHADER_PARAMETER(uint32, TriangleNormalTextureHeight)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FClearDirtyVoxelCacheCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ClearDirtyVoxelCacheCS", SF_Compute);

class FScatterTrianglesToVoxelCacheCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScatterTrianglesToVoxelCacheCS);
	SHADER_USE_PARAMETER_STRUCT(FScatterTrianglesToVoxelCacheCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceTriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, DirtyVoxelPages)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VoxelMetaTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleVertexTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TriangleNormalTexture)
		SHADER_PARAMETER(uint32, SurfaceTriangleCount)
		SHADER_PARAMETER(uint32, DirtyVoxelCount)
		SHADER_PARAMETER(uint32, CacheGeneration)
		SHADER_PARAMETER(uint32, GridSizeX)
		SHADER_PARAMETER(uint32, GridSizeY)
		SHADER_PARAMETER(uint32, GridSizeZ)
		SHADER_PARAMETER(uint32, MaxTrianglesPerVoxel)
		SHADER_PARAMETER(uint32, MetaTextureWidth)
		SHADER_PARAMETER(uint32, MetaTextureHeight)
		SHADER_PARAMETER(uint32, TriangleVertexTextureWidth)
		SHADER_PARAMETER(uint32, TriangleVertexTextureHeight)
		SHADER_PARAMETER(uint32, TriangleNormalTextureWidth)
		SHADER_PARAMETER(uint32, TriangleNormalTextureHeight)
		SHADER_PARAMETER(FVector3f, CacheWorldMin)
		SHADER_PARAMETER(float, CacheVoxelSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 64);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScatterTrianglesToVoxelCacheCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ScatterTrianglesToVoxelCacheCS", SF_Compute);


namespace
{
constexpr float CSGeneratorMinVoxelSize = 1.0e-3f;
const FName CSGeneratorDefaultRequestId(TEXT("Default"));


void ReleaseRTAndNull(TObjectPtr<UTextureRenderTarget2D>& RT)
{
	if (RT) { RT->ReleaseResource(); RT = nullptr; }
}

void InitCacheRT(UTextureRenderTarget2D* RT, int32 Width, int32 Height)
{
	if (!RT) { return; }
	RT->RenderTargetFormat = RTF_RGBA32f;
	RT->ClearColor = FLinearColor::Black;
	RT->bCanCreateUAV = true;
	RT->InitAutoFormat(Width, Height);
	RT->UpdateResourceImmediate(true);
}

bool IsValidCacheRT(const UTextureRenderTarget2D* RT)
{
	return RT && RT->bCanCreateUAV && RT->SizeX > 0 && RT->SizeY > 0;
}

struct FCSMeshGeneratorDirtyVoxelPage
{
	uint32 X = 0;
	uint32 Y = 0;
	uint32 Z = 0;
	uint32 PageIndex = 0;
};

static_assert(sizeof(FCSMeshGeneratorDirtyVoxelPage) == sizeof(uint32) * 4, "Dirty voxel page buffer must match shader uint4 layout.");

} // namespace

void AMeshGeneratorBrushCache::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearMeshGeneratorCache();
	Super::EndPlay(EndPlayReason);
}

// -----------------------------------------------------------------------------
// Brush System
// -----------------------------------------------------------------------------

void AMeshGeneratorBrushCache::StartInstanceBrush()
{
#if WITH_EDITOR
	if (!InstanceBrushMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::StartInstanceBrush] InstanceBrushMesh is not set. Actor=%s"),
			*GetNameSafe(this));
	}

	if (OnInstanceBrushEditorRequest.IsBound())
	{
		OnInstanceBrushEditorRequest.Broadcast(this);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::StartInstanceBrush] No editor brush handler is registered. Actor=%s"),
			*GetNameSafe(this));
	}
#endif
}

UHierarchicalInstancedStaticMeshComponent* AMeshGeneratorBrushCache::FindPaintComponent(UStaticMesh* Mesh) const
{
	if (!Mesh)
	{
		return nullptr;
	}

	for (const FCSInstancePaintComponentSlot& Slot : PaintedInstanceComponents)
	{
		if (Slot.Mesh == Mesh && Slot.Component)
		{
			return Slot.Component;
		}
	}

	return nullptr;
}

UHierarchicalInstancedStaticMeshComponent* AMeshGeneratorBrushCache::GetOrCreatePaintComponent(UStaticMesh* Mesh)
{
	if (!Mesh)
	{
		return nullptr;
	}

	if (UHierarchicalInstancedStaticMeshComponent* ExistingComponent = FindPaintComponent(Mesh))
	{
		return ExistingComponent;
	}

	const FName BaseName(*FString::Printf(TEXT("PaintedInstances_%s"), *Mesh->GetName()));
	const FName ComponentName = MakeUniqueObjectName(this, UHierarchicalInstancedStaticMeshComponent::StaticClass(), BaseName);
	UHierarchicalInstancedStaticMeshComponent* NewComponent = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, ComponentName, RF_Transactional);
	if (!NewComponent)
	{
		return nullptr;
	}

	NewComponent->SetStaticMesh(Mesh);
	NewComponent->SetMobility(EComponentMobility::Static);
	NewComponent->SetupAttachment(SceneRoot ? SceneRoot.Get() : GetRootComponent());
	AddInstanceComponent(NewComponent);
	NewComponent->RegisterComponent();

	FCSInstancePaintComponentSlot& NewSlot = PaintedInstanceComponents.AddDefaulted_GetRef();
	NewSlot.Mesh = Mesh;
	NewSlot.Component = NewComponent;

#if WITH_EDITOR
	MarkPackageDirty();
#endif

	return NewComponent;
}

int32 AMeshGeneratorBrushCache::CommitPaintInstances(const TArray<FTransform>& WorldTransforms, UStaticMesh* Mesh)
{
	if (!Mesh || WorldTransforms.IsEmpty())
	{
		return 0;
	}

	UHierarchicalInstancedStaticMeshComponent* PaintComponent = GetOrCreatePaintComponent(Mesh);
	if (!PaintComponent)
	{
		return 0;
	}

	const int32 PreviousInstanceCount = PaintComponent->GetInstanceCount();
	PaintComponent->AddInstances(WorldTransforms, false, true, false);
	PaintComponent->MarkRenderStateDirty();

#if WITH_EDITOR
	MarkPackageDirty();
#endif

	return PaintComponent->GetInstanceCount() - PreviousInstanceCount;
}

bool AMeshGeneratorBrushCache::IsInstanceBrushPointAllowed(const FVector& WorldPosition) const
{
	if (!bInstanceBrushUseGeneratorBounds)
	{
		return true;
	}

	const FBox Bounds = GetGeneratorBoundsWorldBox();
	return Bounds.IsValid && Bounds.IsInsideOrOn(WorldPosition);
}

// -----------------------------------------------------------------------------
// Dirty Cache System - Public API
// -----------------------------------------------------------------------------

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::EnsureTriangleCache(const FCSMeshGeneratorTriangleCacheRequest& Request)
{
	FCSMeshGeneratorTriangleCacheRequest NormalizedRequest = Request;
	NormalizedRequest.RequestId = NormalizeRequestId(Request.RequestId);
	NormalizedRequest.CachedReferencePoints = ReferencePoints;

	const FBox InputWorldBounds = GetGeneratorBoundsWorldBox();
	if (!InputWorldBounds.IsValid || !FMath::IsFinite(VoxelGridSettings.VoxelSize) || VoxelGridSettings.VoxelSize <= CSGeneratorMinVoxelSize)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::EnsureTriangleCache] Invalid bounds or voxel size. Actor=%s Request=%s"),
			*GetNameSafe(this),
			*NormalizedRequest.RequestId.ToString());
		return GetTriangleCacheHandle();
	}

	const bool bNeedsFullRebuild = NormalizedRequest.bForceFullRebuild || DoesInputRequireFullRebuild(InputWorldBounds) || !HasValidCacheResources();
	if (bNeedsFullRebuild)
	{
		ResetCacheRuntime(false);
		RebuildCacheResources(InputWorldBounds);
		++CacheState.CacheGeneration;
		RebuildRequestActiveCellsFromLastRequests();
	}

	if (!HasValidCacheResources())
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::EnsureTriangleCache] Cache resources are invalid after rebuild. Actor=%s Request=%s"),
			*GetNameSafe(this),
			*NormalizedRequest.RequestId.ToString());
		return GetTriangleCacheHandle();
	}

	TSet<FCSMeshGeneratorVoxelKey> RequestCells;
	const float RequestActivationRadius = NormalizedRequest.ActivationRadiusOverride > 0.0f
		? NormalizedRequest.ActivationRadiusOverride
		: VoxelGridSettings.ActivationRadius;
	BuildActiveCellsFromReferencePoints(NormalizedRequest.CachedReferencePoints, RequestActivationRadius, RequestCells);

	const FName SafeRequestId = NormalizedRequest.RequestId;
	const bool bPersistentInterest = NormalizedRequest.bPersistentInterest;
	if (bPersistentInterest)
	{
		RequestActiveCells.FindOrAdd(SafeRequestId) = MoveTemp(RequestCells);
		LastRequests.FindOrAdd(SafeRequestId) = MoveTemp(NormalizedRequest);
	}
	else
	{
		RequestActiveCells.FindOrAdd(SafeRequestId) = RequestCells;
	}

	TSet<FCSMeshGeneratorVoxelKey> NewUnionActiveCells;
	BuildUnionActiveCells(NewUnionActiveCells);

	DiffActiveCells(NewUnionActiveCells);
	ReleasePagesForCells(CacheState.CellsToDeactivate);
	AllocatePagesForCells(CacheState.CellsToActivate);
	CacheState.DirtyCells.Append(CacheState.CellsToActivate);

	DispatchDirtyVoxelTriangleCacheUpdate();

	CacheState.ActiveCells = MoveTemp(NewUnionActiveCells);

	if (!bPersistentInterest)
	{
		RequestActiveCells.Remove(SafeRequestId);
	}

	return GetTriangleCacheHandle();
}

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::EnsureTriangleCacheByBox(
	FName RequestId,
	bool bForceFullRebuild)
{
	FCSMeshGeneratorTriangleCacheRequest Request;
	Request.RequestId = RequestId;
	Request.bForceFullRebuild = bForceFullRebuild;
	return EnsureTriangleCache(Request);
}

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::EnsureTriangleCacheByBox(
	FName RequestId,
	const FVector& BoxCenter,
	const FVector& BoxExtent,
	bool bForceFullRebuild)
{
	if (GeneratorBounds)
	{
		GeneratorBounds->SetWorldLocation(BoxCenter);

		const FVector SafeWorldExtent(
			FMath::Max(0.0, BoxExtent.X),
			FMath::Max(0.0, BoxExtent.Y),
			FMath::Max(0.0, BoxExtent.Z));
		const FVector ComponentScale = GeneratorBounds->GetComponentTransform().GetScale3D().GetAbs();
		const FVector SafeComponentScale(
			FMath::Max(UE_KINDA_SMALL_NUMBER, ComponentScale.X),
			FMath::Max(UE_KINDA_SMALL_NUMBER, ComponentScale.Y),
			FMath::Max(UE_KINDA_SMALL_NUMBER, ComponentScale.Z));
		GeneratorBounds->SetBoxExtent(
			FVector(
				SafeWorldExtent.X / SafeComponentScale.X,
				SafeWorldExtent.Y / SafeComponentScale.Y,
				SafeWorldExtent.Z / SafeComponentScale.Z),
			true);
	}

	return EnsureTriangleCacheByBox(RequestId, bForceFullRebuild);
}

void AMeshGeneratorBrushCache::UpdateMeshGeneratorCacheByBox(
	bool bForceFullRebuild)
{
	EnsureTriangleCacheByBox(CSGeneratorDefaultRequestId, bForceFullRebuild);
}

void AMeshGeneratorBrushCache::ReleaseTriangleCacheRequest(FName RequestId)
{
	const FName SafeRequestId = NormalizeRequestId(RequestId);
	if (!RequestActiveCells.Contains(SafeRequestId) && !LastRequests.Contains(SafeRequestId))
	{
		return;
	}

	RequestActiveCells.Remove(SafeRequestId);
	LastRequests.Remove(SafeRequestId);

	TSet<FCSMeshGeneratorVoxelKey> NewUnionActiveCells;
	BuildUnionActiveCells(NewUnionActiveCells);

	DiffActiveCells(NewUnionActiveCells);
	ReleasePagesForCells(CacheState.CellsToDeactivate);
	CacheState.ActiveCells = MoveTemp(NewUnionActiveCells);
}

void AMeshGeneratorBrushCache::ClearMeshGeneratorCache()
{
	ResetCacheRuntime(true);
	++CacheState.CacheGeneration;
}

void AMeshGeneratorBrushCache::MarkAllActiveVoxelsDirty()
{
	CacheState.DirtyCells.Append(CacheState.ActiveCells);
}

FCSMeshGeneratorTriangleCacheHandle AMeshGeneratorBrushCache::GetTriangleCacheHandle() const
{
	FCSMeshGeneratorTriangleCacheHandle Handle;
	Handle.bValid = HasValidCacheResources() && CacheState.CachedWorldBounds.IsValid;
	Handle.CacheGeneration = int32(FMath::Min<uint32>(CacheState.CacheGeneration, uint32(TNumericLimits<int32>::Max())));
	Handle.CachedWorldBounds = CacheState.CachedWorldBounds;
	Handle.GridSize = CacheState.GridSize;
	Handle.VoxelSize = CacheState.CachedVoxelSize;
	Handle.ActiveVoxelCount = CacheState.ActiveCells.Num();
	Handle.DirtyVoxelCount = CacheState.DirtyCells.Num();
	Handle.VoxelMetaRT = VoxelMetaRT;
	Handle.TriangleVertexRT = TriangleVertexRT;
	Handle.TriangleNormalRT = TriangleNormalRT;
	return Handle;
}

// -----------------------------------------------------------------------------
// Debug System
// -----------------------------------------------------------------------------

int32 AMeshGeneratorBrushCache::DrawDebugActiveVoxels(
	const FCSDebugActiveVoxelOptions& Options) const
{
	UWorld* World = GetWorld();
	if (!World || !CacheState.CachedWorldBounds.IsValid || CacheState.CachedVoxelSize <= CSGeneratorMinVoxelSize)
	{
		return 0;
	}

	const TSet<FCSMeshGeneratorVoxelKey>* CellsToDraw = &CacheState.ActiveCells;
	if (!Options.RequestId.IsNone())
	{
		CellsToDraw = RequestActiveCells.Find(NormalizeRequestId(Options.RequestId));
		if (!CellsToDraw)
		{
			return 0;
		}
	}

	if (CellsToDraw->IsEmpty())
	{
		return 0;
	}

	const float SafeDuration = FMath::Max(0.0f, Options.Duration);
	const float SafeThickness = FMath::Max(0.0f, Options.Thickness);
	const int32 DrawLimit = Options.MaxVoxelsToDraw > 0 ? Options.MaxVoxelsToDraw : TNumericLimits<int32>::Max();
	const FColor LineColor = Options.DebugColor.ToFColor(true);

	int32 DrawnCount = 0;
	for (const FCSMeshGeneratorVoxelKey& Cell : *CellsToDraw)
	{
		if (DrawnCount >= DrawLimit)
		{
			break;
		}

		const FBox CellBounds = GetCellWorldBounds(Cell);
		if (!CellBounds.IsValid)
		{
			continue;
		}

		DrawDebugBox(
			World,
			CellBounds.GetCenter(),
			CellBounds.GetExtent(),
			LineColor,
			Options.bPersistentLines,
			SafeDuration,
			0,
			SafeThickness);
		++DrawnCount;
	}

	if (Options.bDrawCacheBounds)
	{
		DrawDebugBox(
			World,
			CacheState.CachedWorldBounds.GetCenter(),
			CacheState.CachedWorldBounds.GetExtent(),
			FColor::White,
			Options.bPersistentLines,
			SafeDuration,
			0,
			FMath::Max(1.0f, SafeThickness));
	}

	return DrawnCount;
}

// -----------------------------------------------------------------------------
// Dirty Cache System - Internals
// -----------------------------------------------------------------------------

bool AMeshGeneratorBrushCache::DoesInputRequireFullRebuild(const FBox& InputWorldBounds) const
{
	const float SafeVoxelSize = FMath::Max(VoxelGridSettings.VoxelSize, CSGeneratorMinVoxelSize);
	const int32 SafeMaxActiveVoxels = FMath::Max(1, VoxelGridSettings.MaxActiveVoxels);
	const int32 SafeMaxTrianglesPerVoxel = FMath::Max(1, VoxelGridSettings.MaxTrianglesPerVoxel);
	const int32 SafeMaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);

	if (!CacheState.CachedWorldBounds.IsValid)
	{
		return true;
	}

	if (!AreBoundsCompatible(CacheState.CachedWorldBounds, InputWorldBounds))
	{
		return true;
	}

	if (!FMath::IsNearlyEqual(CacheState.CachedVoxelSize, SafeVoxelSize, KINDA_SMALL_NUMBER))
	{
		return true;
	}

	if (CacheState.GridSize != ComputeGridSize(InputWorldBounds))
	{
		return true;
	}

	if (CacheState.CachedMaxActiveVoxels != SafeMaxActiveVoxels ||
		CacheState.CachedMaxTrianglesPerVoxel != SafeMaxTrianglesPerVoxel ||
		CacheState.CachedLODIndex != VoxelGridSettings.LODIndex ||
		CacheState.CachedMaxTextureDimension != SafeMaxTextureDimension)
	{
		return true;
	}

	return false;
}

void AMeshGeneratorBrushCache::RebuildCacheResources(const FBox& InputWorldBounds)
{
	CacheState.CachedWorldBounds = InputWorldBounds;
	CacheState.CachedVoxelSize = FMath::Max(VoxelGridSettings.VoxelSize, CSGeneratorMinVoxelSize);
	CacheState.GridSize = ComputeGridSize(InputWorldBounds);
	CacheState.CachedMaxActiveVoxels = FMath::Max(1, VoxelGridSettings.MaxActiveVoxels);
	CacheState.CachedMaxTrianglesPerVoxel = FMath::Max(1, VoxelGridSettings.MaxTrianglesPerVoxel);
	CacheState.CachedLODIndex = VoxelGridSettings.LODIndex;
	CacheState.CachedMaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);

	InitializeFreePages();
	CreateCacheRenderTargets();
}

void AMeshGeneratorBrushCache::BuildActiveCellsFromReferencePoints(
	float ActivationRadius,
	TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
	BuildActiveCellsFromReferencePoints(ReferencePoints, ActivationRadius, OutCells);
}

void AMeshGeneratorBrushCache::BuildActiveCellsFromReferencePoints(
	const TArray<FVector>& InReferencePoints,
	float ActivationRadius,
	TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
	OutCells.Reset();
	if (!CacheState.CachedWorldBounds.IsValid || CacheState.GridSize.X <= 0 || CacheState.GridSize.Y <= 0 || CacheState.GridSize.Z <= 0)
	{
		return;
	}

	const float SafeVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const float SafeActivationRadius = FMath::Max(ActivationRadius, 0.0f);
	const int32 RadiusInCells = FMath::Max(0, FMath::CeilToInt(SafeActivationRadius / SafeVoxelSize));
	const double ActivationRadiusSq = double(SafeActivationRadius) * double(SafeActivationRadius);

	for (const FVector& Point : InReferencePoints)
	{
		if (!IsFiniteVector(Point) || !CacheState.CachedWorldBounds.IsInsideOrOn(Point))
		{
			continue;
		}

		const FIntVector CenterCell = WorldPositionToCell(Point);
		for (int32 Z = CenterCell.Z - RadiusInCells; Z <= CenterCell.Z + RadiusInCells; ++Z)
		{
			if (Z < 0 || Z >= CacheState.GridSize.Z)
			{
				continue;
			}

			for (int32 Y = CenterCell.Y - RadiusInCells; Y <= CenterCell.Y + RadiusInCells; ++Y)
			{
				if (Y < 0 || Y >= CacheState.GridSize.Y)
				{
					continue;
				}

				for (int32 X = CenterCell.X - RadiusInCells; X <= CenterCell.X + RadiusInCells; ++X)
				{
					if (X < 0 || X >= CacheState.GridSize.X)
					{
						continue;
					}

					const FCSMeshGeneratorVoxelKey Cell(X, Y, Z);
					const FBox CellBounds = GetCellWorldBounds(Cell);
					const FVector ClosestPoint = CellBounds.GetClosestPointTo(Point);
					const double DistSq = FVector::DistSquared(ClosestPoint, Point);
					if (RadiusInCells == 0 || DistSq <= ActivationRadiusSq)
					{
						OutCells.Add(Cell);
						if (OutCells.Num() >= FMath::Max(1, VoxelGridSettings.MaxActiveVoxels))
						{
							return;
						}
					}
				}
			}
		}
	}
}

void AMeshGeneratorBrushCache::BuildUnionActiveCells(TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
	OutCells.Reset();
	const int32 MaxActiveVoxels = FMath::Max(1, VoxelGridSettings.MaxActiveVoxels);
	for (const TPair<FName, TSet<FCSMeshGeneratorVoxelKey>>& Pair : RequestActiveCells)
	{
		for (const FCSMeshGeneratorVoxelKey& Cell : Pair.Value)
		{
			OutCells.Add(Cell);
			if (OutCells.Num() >= MaxActiveVoxels)
			{
				return;
			}
		}
	}
}

void AMeshGeneratorBrushCache::DiffActiveCells(const TSet<FCSMeshGeneratorVoxelKey>& NewActiveCells)
{
	CacheState.CellsToActivate.Reset();
	CacheState.CellsToDeactivate.Reset();
	CacheState.DirtyCells.Reset();

	for (const FCSMeshGeneratorVoxelKey& Cell : NewActiveCells)
	{
		if (!CacheState.ActiveCells.Contains(Cell))
		{
			CacheState.CellsToActivate.Add(Cell);
		}
	}

	for (const FCSMeshGeneratorVoxelKey& Cell : CacheState.ActiveCells)
	{
		if (!NewActiveCells.Contains(Cell))
		{
			CacheState.CellsToDeactivate.Add(Cell);
		}
	}
}

void AMeshGeneratorBrushCache::AllocatePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells)
{
	for (const FCSMeshGeneratorVoxelKey& Cell : Cells)
	{
		if (CacheState.CellToPage.Contains(Cell))
		{
			continue;
		}

		if (CacheState.FreePages.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator] Triangle cache page capacity exhausted. Actor=%s MaxActiveVoxels=%d"),
				*GetNameSafe(this),
				CacheState.CachedMaxActiveVoxels);
			break;
		}

		const int32 PageIndex = CacheState.FreePages.Pop(EAllowShrinking::No);
		CacheState.CellToPage.Add(Cell, PageIndex);
	}
}

void AMeshGeneratorBrushCache::ReleasePagesForCells(const TSet<FCSMeshGeneratorVoxelKey>& Cells)
{
	for (const FCSMeshGeneratorVoxelKey& Cell : Cells)
	{
		int32 PageIndex = INDEX_NONE;
		if (CacheState.CellToPage.RemoveAndCopyValue(Cell, PageIndex) && PageIndex != INDEX_NONE)
		{
			if (!CacheState.FreePages.Contains(PageIndex))
			{
				CacheState.FreePages.Add(PageIndex);
			}
		}
	}
}

void AMeshGeneratorBrushCache::DispatchDirtyVoxelTriangleCacheUpdate()
{
	if (CacheState.DirtyCells.IsEmpty())
	{
		return;
	}

	FBox DirtyWorldBounds(ForceInit);
	for (const FCSMeshGeneratorVoxelKey& DirtyCell : CacheState.DirtyCells)
	{
		DirtyWorldBounds += GetCellWorldBounds(DirtyCell);
	}

	if (!DirtyWorldBounds.IsValid)
	{
		CacheState.DirtyCells.Reset();
		return;
	}

	DirtyWorldBounds = DirtyWorldBounds.ExpandBy(FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize) * 0.5f);

	TArray<FCSStaticMeshTriangleRequest> SceneTriangleRequests;
	BuildBoxSceneTriangleRequestsInternal(
		GetWorld(),
		DirtyWorldBounds,
		CacheState.CachedLODIndex,
		SceneTriangleRequests);

	TArray<FVector> CacheReferencePoints;
	for (const TPair<FName, FCSMeshGeneratorTriangleCacheRequest>& Pair : LastRequests)
	{
		CacheReferencePoints.Append(Pair.Value.CachedReferencePoints);
	}

	const int32 DirtyCellCount = CacheState.DirtyCells.Num();
	TArray<FCSMeshGeneratorDirtyVoxelPage> DirtyPageData;
	DirtyPageData.Reserve(DirtyCellCount);
	for (const FCSMeshGeneratorVoxelKey& DirtyCell : CacheState.DirtyCells)
	{
		const int32* PageIndex = CacheState.CellToPage.Find(DirtyCell);
		if (!PageIndex || *PageIndex < 0)
		{
			continue;
		}

		FCSMeshGeneratorDirtyVoxelPage& DirtyPage = DirtyPageData.AddDefaulted_GetRef();
		DirtyPage.X = uint32(FMath::Max(0, DirtyCell.X));
		DirtyPage.Y = uint32(FMath::Max(0, DirtyCell.Y));
		DirtyPage.Z = uint32(FMath::Max(0, DirtyCell.Z));
		DirtyPage.PageIndex = uint32(*PageIndex);
	}

	if (DirtyPageData.IsEmpty() || !VoxelMetaRT || !TriangleVertexRT || !TriangleNormalRT)
	{
		CacheState.DirtyCells.Reset();
		return;
	}
	const int32 DirtyPageCountForLog = DirtyPageData.Num();

	const int64 RequestedTriangleCapacity = int64(FMath::Max(1, DirtyCellCount)) * int64(FMath::Max(1, CacheState.CachedMaxTrianglesPerVoxel));
	const int32 MaxTrianglesForDirtyCells = int32(FMath::Clamp<int64>(RequestedTriangleCapacity, 1, int64(TNumericLimits<int32>::Max())));
	const float ReferenceFilterDistance = FMath::Max(0.0f, VoxelGridSettings.ActivationRadius);
	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		SceneTriangleRequests,
		this,
		ExcludedActorTags,
		true,
		ResolvedRequests);

	FCSTriangleMeshData LandscapeTriangleData;
	BuildBoxSceneLandscapeTrianglesInternal(
		GetWorld(),
		DirtyWorldBounds,
		CacheReferencePoints,
		ReferenceFilterDistance,
		MaxTrianglesForDirtyCells,
		LandscapeTriangleData);

	const uint32 CacheGeneration = CacheState.CacheGeneration;
	const FIntVector GridSize = CacheState.GridSize;
	const FVector CacheWorldMin = CacheState.CachedWorldBounds.Min;
	const float CachedVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const uint32 MaxTrianglesPerVoxel = uint32(FMath::Max(1, CacheState.CachedMaxTrianglesPerVoxel));
	const uint32 MetaTextureWidth = uint32(FMath::Max(1, VoxelMetaRT->SizeX));
	const uint32 MetaTextureHeight = uint32(FMath::Max(1, VoxelMetaRT->SizeY));
	const uint32 TriangleVertexTextureWidth = uint32(FMath::Max(1, TriangleVertexRT->SizeX));
	const uint32 TriangleVertexTextureHeight = uint32(FMath::Max(1, TriangleVertexRT->SizeY));
	const uint32 TriangleNormalTextureWidth = uint32(FMath::Max(1, TriangleNormalRT->SizeX));
	const uint32 TriangleNormalTextureHeight = uint32(FMath::Max(1, TriangleNormalRT->SizeY));
	FTextureRenderTargetResource* VoxelMetaResource = VoxelMetaRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* TriangleVertexResource = TriangleVertexRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* TriangleNormalResource = TriangleNormalRT->GameThread_GetRenderTargetResource();

	ENQUEUE_RENDER_COMMAND(CSMeshGeneratorUpdateDirtyVoxelTriangleCache)(
		[ResolvedRequests = MoveTemp(ResolvedRequests),
		 TotalStaticMeshTriangleCount,
		 ReferencePoints = MoveTemp(CacheReferencePoints),
		 LandscapeTriangleData = MoveTemp(LandscapeTriangleData),
		 DirtyPageData = MoveTemp(DirtyPageData),
		 VoxelMetaResource,
		 TriangleVertexResource,
		 TriangleNormalResource,
		 ReferenceFilterDistance,
		 MaxTrianglesForDirtyCells,
		 CacheGeneration,
		 GridSize,
		 CacheWorldMin,
		 CachedVoxelSize,
		 MaxTrianglesPerVoxel,
		 MetaTextureWidth,
		 MetaTextureHeight,
		 TriangleVertexTextureWidth,
		 TriangleVertexTextureHeight,
		 TriangleNormalTextureWidth,
		 TriangleNormalTextureHeight](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef VoxelMetaTexture = RegisterRenderTargetTexture(GraphBuilder, VoxelMetaResource, TEXT("CS.MeshGenerator.VoxelMetaRT"));
			FRDGTextureRef TriangleVertexTexture = RegisterRenderTargetTexture(GraphBuilder, TriangleVertexResource, TEXT("CS.MeshGenerator.TriangleVertexRT"));
			FRDGTextureRef TriangleNormalTexture = RegisterRenderTargetTexture(GraphBuilder, TriangleNormalResource, TEXT("CS.MeshGenerator.TriangleNormalRT"));
			if (!VoxelMetaTexture || !TriangleVertexTexture || !TriangleNormalTexture || DirtyPageData.IsEmpty())
			{
				GraphBuilder.Execute();
				return;
			}

			FRDGTextureUAVRef VoxelMetaUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(VoxelMetaTexture));
			FRDGTextureUAVRef TriangleVertexUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(TriangleVertexTexture));
			FRDGTextureUAVRef TriangleNormalUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(TriangleNormalTexture));

			const uint32 DirtyPageCount = uint32(DirtyPageData.Num());
			FRDGBufferRef DirtyPageBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FCSMeshGeneratorDirtyVoxelPage), DirtyPageData.Num()),
				TEXT("CS.MeshGenerator.DirtyVoxelPages"));
			FCSMeshGeneratorDirtyVoxelPage* DirtyPageUploadData = GraphBuilder.AllocPODArray<FCSMeshGeneratorDirtyVoxelPage>(DirtyPageData.Num());
			FMemory::Memcpy(DirtyPageUploadData, DirtyPageData.GetData(), DirtyPageData.Num() * sizeof(FCSMeshGeneratorDirtyVoxelPage));
			GraphBuilder.QueueBufferUpload(DirtyPageBuffer, DirtyPageUploadData, DirtyPageData.Num() * sizeof(FCSMeshGeneratorDirtyVoxelPage));
			FRDGBufferSRVRef DirtyPageSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DirtyPageBuffer, PF_R32G32B32A32_UINT));

			TShaderMapRef<FClearDirtyVoxelCacheCS> ClearShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FClearDirtyVoxelCacheCS::FParameters* ClearParameters = GraphBuilder.AllocParameters<FClearDirtyVoxelCacheCS::FParameters>();
			ClearParameters->DirtyVoxelPages = DirtyPageSRV;
			ClearParameters->RW_VoxelMetaTexture = VoxelMetaUAV;
			ClearParameters->RW_TriangleVertexTexture = TriangleVertexUAV;
			ClearParameters->RW_TriangleNormalTexture = TriangleNormalUAV;
			ClearParameters->DirtyVoxelCount = DirtyPageCount;
			ClearParameters->CacheGeneration = CacheGeneration;
			ClearParameters->MaxTrianglesPerVoxel = MaxTrianglesPerVoxel;
			ClearParameters->MetaTextureWidth = MetaTextureWidth;
			ClearParameters->MetaTextureHeight = MetaTextureHeight;
			ClearParameters->TriangleVertexTextureWidth = TriangleVertexTextureWidth;
			ClearParameters->TriangleVertexTextureHeight = TriangleVertexTextureHeight;
			ClearParameters->TriangleNormalTextureWidth = TriangleNormalTextureWidth;
			ClearParameters->TriangleNormalTextureHeight = TriangleNormalTextureHeight;
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("CS.MeshGenerator.ClearDirtyVoxelCache"),
				ClearParameters,
				ERDGPassFlags::Compute,
				[ClearParameters, ClearShader, DirtyPageCount](FRHIComputeCommandList& InRHICmdList)
				{
					FComputeShaderUtils::Dispatch(InRHICmdList, ClearShader, *ClearParameters, FComputeShaderUtils::GetGroupCount(FIntVector(int32(DirtyPageCount), 1, 1), 64));
				});

			const FCSTriangleMeshData* InitialTriangleData = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0 ? &LandscapeTriangleData : nullptr;
			if (!ResolvedRequests.IsEmpty() || InitialTriangleData)
			{
				FCSStaticMeshTriangleRDGOutput TriangleOutput = AddResolvedStaticMeshTrianglesToRDGInternal(
					GraphBuilder,
					RHICmdList,
					ResolvedRequests,
					TotalStaticMeshTriangleCount,
					ReferencePoints,
					ReferenceFilterDistance,
					MaxTrianglesForDirtyCells,
					InitialTriangleData,
					TEXT("CS.MeshGenerator.DirtyStaticMeshTriangles"));

				if (TriangleOutput.TriangleVertices && TriangleOutput.TriangleNormals && TriangleOutput.TriangleCounter && TriangleOutput.MaxTriangles > 0)
				{
					TShaderMapRef<FScatterTrianglesToVoxelCacheCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FScatterTrianglesToVoxelCacheCS::FParameters* ScatterParameters = GraphBuilder.AllocParameters<FScatterTrianglesToVoxelCacheCS::FParameters>();
					ScatterParameters->TriangleVertices = TriangleOutput.TriangleVerticesSRV
						? TriangleOutput.TriangleVerticesSRV
						: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleVertices, PF_A32B32G32R32F));
					ScatterParameters->TriangleNormals = TriangleOutput.TriangleNormalsSRV
						? TriangleOutput.TriangleNormalsSRV
						: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleNormals, PF_A32B32G32R32F));
					ScatterParameters->SurfaceTriangleCounter = TriangleOutput.TriangleCounterSRV
						? TriangleOutput.TriangleCounterSRV
						: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriangleOutput.TriangleCounter, PF_R32_UINT));
					ScatterParameters->DirtyVoxelPages = DirtyPageSRV;
					ScatterParameters->RW_VoxelMetaTexture = VoxelMetaUAV;
					ScatterParameters->RW_TriangleVertexTexture = TriangleVertexUAV;
					ScatterParameters->RW_TriangleNormalTexture = TriangleNormalUAV;
					ScatterParameters->SurfaceTriangleCount = TriangleOutput.MaxTriangles;
					ScatterParameters->DirtyVoxelCount = DirtyPageCount;
					ScatterParameters->CacheGeneration = CacheGeneration;
					ScatterParameters->GridSizeX = uint32(FMath::Max(0, GridSize.X));
					ScatterParameters->GridSizeY = uint32(FMath::Max(0, GridSize.Y));
					ScatterParameters->GridSizeZ = uint32(FMath::Max(0, GridSize.Z));
					ScatterParameters->MaxTrianglesPerVoxel = MaxTrianglesPerVoxel;
					ScatterParameters->MetaTextureWidth = MetaTextureWidth;
					ScatterParameters->MetaTextureHeight = MetaTextureHeight;
					ScatterParameters->TriangleVertexTextureWidth = TriangleVertexTextureWidth;
					ScatterParameters->TriangleVertexTextureHeight = TriangleVertexTextureHeight;
					ScatterParameters->TriangleNormalTextureWidth = TriangleNormalTextureWidth;
					ScatterParameters->TriangleNormalTextureHeight = TriangleNormalTextureHeight;
					ScatterParameters->CacheWorldMin = FVector3f(CacheWorldMin);
					ScatterParameters->CacheVoxelSize = CachedVoxelSize;
					GraphBuilder.AddPass(
						RDG_EVENT_NAME("CS.MeshGenerator.ScatterTrianglesToVoxelCache"),
						ScatterParameters,
						ERDGPassFlags::Compute,
						[ScatterParameters, ScatterShader, DirtyPageCount](FRHIComputeCommandList& InRHICmdList)
						{
							FComputeShaderUtils::Dispatch(InRHICmdList, ScatterShader, *ScatterParameters, FComputeShaderUtils::GetGroupCount(FIntVector(int32(DirtyPageCount), 1, 1), 64));
						});
				}
			}

			GraphBuilder.Execute();
		});

	UE_LOG(LogTemp, Verbose, TEXT("[AComputeShaderMeshGenerator] Dirty voxel triangle cache update queued. Actor=%s Dirty=%d Pages=%d Generation=%u"),
		*GetNameSafe(this),
		DirtyCellCount,
		DirtyPageCountForLog,
		CacheState.CacheGeneration);

	CacheState.DirtyCells.Reset();
}


FIntVector AMeshGeneratorBrushCache::ComputeGridSize(const FBox& InputWorldBounds) const
{
	if (!InputWorldBounds.IsValid)
	{
		return FIntVector::ZeroValue;
	}

	const FVector BoundsSize = InputWorldBounds.GetSize();
	const float SafeVoxelSize = FMath::Max(VoxelGridSettings.VoxelSize, CSGeneratorMinVoxelSize);
	return FIntVector(
		FMath::Max(1, FMath::CeilToInt(BoundsSize.X / SafeVoxelSize)),
		FMath::Max(1, FMath::CeilToInt(BoundsSize.Y / SafeVoxelSize)),
		FMath::Max(1, FMath::CeilToInt(BoundsSize.Z / SafeVoxelSize)));
}

FCSMeshGeneratorVoxelKey AMeshGeneratorBrushCache::WorldPositionToCell(FVector WorldPosition) const
{
	const float SafeVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const FVector Local = (WorldPosition - CacheState.CachedWorldBounds.Min) / SafeVoxelSize;
	const FIntVector RawCell(
		FMath::FloorToInt(Local.X),
		FMath::FloorToInt(Local.Y),
		FMath::FloorToInt(Local.Z));

	return FCSMeshGeneratorVoxelKey(
		FMath::Clamp(RawCell.X, 0, FMath::Max(0, CacheState.GridSize.X - 1)),
		FMath::Clamp(RawCell.Y, 0, FMath::Max(0, CacheState.GridSize.Y - 1)),
		FMath::Clamp(RawCell.Z, 0, FMath::Max(0, CacheState.GridSize.Z - 1)));
}

FBox AMeshGeneratorBrushCache::GetCellWorldBounds(const FCSMeshGeneratorVoxelKey& Cell) const
{
	const float SafeVoxelSize = FMath::Max(CacheState.CachedVoxelSize, CSGeneratorMinVoxelSize);
	const FVector Min = CacheState.CachedWorldBounds.Min + FVector(Cell.X, Cell.Y, Cell.Z) * SafeVoxelSize;
	return FBox(Min, Min + FVector(SafeVoxelSize));
}

void AMeshGeneratorBrushCache::ReleaseCacheResources()
{
	ReleaseRTAndNull(VoxelMetaRT);
	ReleaseRTAndNull(TriangleVertexRT);
	ReleaseRTAndNull(TriangleNormalRT);
}

void AMeshGeneratorBrushCache::ResetCacheRuntime(bool bClearRequests)
{
	ReleaseCacheResources();

	CacheState.CachedWorldBounds = FBox(ForceInit);
	CacheState.GridSize = FIntVector::ZeroValue;
	CacheState.CachedVoxelSize = 0.0f;
	CacheState.CachedMaxActiveVoxels = 0;
	CacheState.CachedMaxTrianglesPerVoxel = 0;
	CacheState.CachedLODIndex = 0;
	CacheState.CachedMaxTextureDimension = 0;
	CacheState.ActiveCells.Empty();
	CacheState.CellsToActivate.Empty();
	CacheState.CellsToDeactivate.Empty();
	CacheState.DirtyCells.Empty();
	CacheState.CellToPage.Empty();
	CacheState.FreePages.Empty();

	if (bClearRequests)
	{
		RequestActiveCells.Empty();
		LastRequests.Empty();
	}
	else
	{
		RequestActiveCells.Empty();
	}
}

void AMeshGeneratorBrushCache::InitializeFreePages()
{
	CacheState.FreePages.Reset();
	CacheState.FreePages.Reserve(CacheState.CachedMaxActiveVoxels);
	for (int32 PageIndex = CacheState.CachedMaxActiveVoxels - 1; PageIndex >= 0; --PageIndex)
	{
		CacheState.FreePages.Add(PageIndex);
	}
}

void AMeshGeneratorBrushCache::CreateCacheRenderTargets()
{
	ReleaseCacheResources();

	const int32 MaxActiveVoxels = FMath::Max(1, CacheState.CachedMaxActiveVoxels);
	const int32 MaxTrianglesPerVoxel = FMath::Max(1, CacheState.CachedMaxTrianglesPerVoxel);
	const int32 MaxDimension = FMath::Max(CSGeneratorMinTextureDimension, CacheState.CachedMaxTextureDimension);
	const int64 TotalTriangleSlots = int64(MaxActiveVoxels) * int64(MaxTrianglesPerVoxel);
	const int64 TotalVertexPixels = TotalTriangleSlots * 3ll;
	const int64 TotalNormalPixels = TotalTriangleSlots;

	// 与 generated-data 纹理共用同一套线性尺寸公式（此前此处手写内联了 3 份同样的公式）。
	const FIntPoint MetaSize = GetLinearDataTextureSize(MaxActiveVoxels, MaxDimension);
	const FIntPoint VertexSize = GetLinearDataTextureSize(TotalVertexPixels, MaxDimension);
	const FIntPoint NormalSize = GetLinearDataTextureSize(TotalNormalPixels, MaxDimension);

	auto NewCacheRT = [this](const TCHAR* BaseName)
	{
		return NewObject<UTextureRenderTarget2D>(
			this,
			MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), BaseName),
			RF_Transient);
	};
	VoxelMetaRT = NewCacheRT(TEXT("CSMeshGenerator_VoxelMetaRT"));
	TriangleVertexRT = NewCacheRT(TEXT("CSMeshGenerator_TriangleVertexRT"));
	TriangleNormalRT = NewCacheRT(TEXT("CSMeshGenerator_TriangleNormalRT"));

	InitCacheRT(VoxelMetaRT, MetaSize.X, MetaSize.Y);
	InitCacheRT(TriangleVertexRT, VertexSize.X, VertexSize.Y);
	InitCacheRT(TriangleNormalRT, NormalSize.X, NormalSize.Y);
}

void AMeshGeneratorBrushCache::RebuildRequestActiveCellsFromLastRequests()
{
	RequestActiveCells.Empty();
	for (const TPair<FName, FCSMeshGeneratorTriangleCacheRequest>& Pair : LastRequests)
	{
		const FCSMeshGeneratorTriangleCacheRequest& Request = Pair.Value;
		if (!Request.bPersistentInterest)
		{
			continue;
		}

		const float RequestActivationRadius = Request.ActivationRadiusOverride > 0.0f
			? Request.ActivationRadiusOverride
			: VoxelGridSettings.ActivationRadius;

		TSet<FCSMeshGeneratorVoxelKey> RebuiltCells;
		BuildActiveCellsFromReferencePoints(Request.CachedReferencePoints, RequestActivationRadius, RebuiltCells);
		RequestActiveCells.Add(Pair.Key, MoveTemp(RebuiltCells));
	}
}

bool AMeshGeneratorBrushCache::HasValidCacheResources() const
{
	return IsValidCacheRT(VoxelMetaRT) && IsValidCacheRT(TriangleVertexRT) && IsValidCacheRT(TriangleNormalRT);
}

bool AMeshGeneratorBrushCache::AreBoundsCompatible(const FBox& A, const FBox& B) const
{
	if (!A.IsValid || !B.IsValid)
	{
		return false;
	}

	const float Tolerance = FMath::Max(0.0f, VoxelGridSettings.BoundsTolerance);
	const double ToleranceSq = double(Tolerance) * double(Tolerance);
	return FVector::DistSquared(A.GetCenter(), B.GetCenter()) <= ToleranceSq
		&& FVector::DistSquared(A.GetExtent(), B.GetExtent()) <= ToleranceSq;
}

FName AMeshGeneratorBrushCache::NormalizeRequestId(FName RequestId) const
{
	return RequestId.IsNone() ? CSGeneratorDefaultRequestId : RequestId;
}
