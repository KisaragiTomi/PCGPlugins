#include "CSNaniteCutHLODBuilder.h"

#include "CSGpuMeshComponent.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshVisibilityCull.h"
#include "CSStaticMeshAssetSink.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "RHIGlobals.h"
#include "UObject/Package.h"
#include "WorldPartition/HLOD/HLODHashBuilder.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSNaniteCutHLOD, Log, All);

#if WITH_EDITOR

namespace
{
/** 可见性剔除只压实了三角形，没人引用的顶点还留在原处：建 MeshDescription 之前丢掉，免得资产里存一堆孤立顶点。 */
void CSNaniteCutHLOD_DropUnreferencedVertices(FCSGpuMeshCPUData& MeshData)
{
	if (MeshData.AttrLayout != FCSGpuMeshCPUData::EAttrLayout::PerVertex) return;
	const int32 NumVertices = MeshData.Positions.Num();
	TBitArray<> Used(false, NumVertices);
	for (uint32 Index : MeshData.Indices)
	{
		if (int32(Index) >= NumVertices) return;   // 越界索引原样交给后面去报错
		Used[int32(Index)] = true;
	}
	TArray<int32> Remap;
	Remap.SetNumUninitialized(NumVertices);
	int32 NumUsed = 0;
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex) Remap[Vertex] = Used[Vertex] ? NumUsed++ : INDEX_NONE;
	if (NumUsed == NumVertices) return;

	// Remap[V] <= V，原地往前搬是安全的。
	auto Compact = [&Remap, NumVertices, NumUsed](auto& Array)
	{
		if (Array.Num() != NumVertices) return;
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
			if (Remap[Vertex] != INDEX_NONE) Array[Remap[Vertex]] = Array[Vertex];
		Array.SetNum(NumUsed);
	};
	Compact(MeshData.Positions);
	Compact(MeshData.Normals);
	Compact(MeshData.Tangents);
	for (TArray<FVector2f>& Channel : MeshData.TexCoordChannels) Compact(Channel);
	Compact(MeshData.Colors);
	Compact(MeshData.BinormalSigns);
	for (uint32& Index : MeshData.Indices) Index = uint32(Remap[int32(Index)]);
}
}

void UCSNaniteCutHLODBuilderSettings::ComputeHLODHash(FHLODHashBuilder& InHashBuilder) const
{
	// 抽取算法本身改了输出时加一，逼已有的 HLOD 重建。2：加了外部可见性剔除。
	constexpr uint32 CSNaniteCutHLODVersion = 2;
	InHashBuilder.HashField(CSNaniteCutHLODVersion, TEXT("CSNaniteCutHLODVersion"));
	InHashBuilder.HashField(PixelError, TEXT("PixelError"));
	InHashBuilder.HashField(ReferenceScreenWidth, TEXT("ReferenceScreenWidth"));
	InHashBuilder.HashField(ReferenceHorizontalFOV, TEXT("ReferenceHorizontalFOV"));
	InHashBuilder.HashField(bEnableNaniteOutput, TEXT("bEnableNaniteOutput"));
	InHashBuilder.HashField(uint8(IncompletePolicy), TEXT("IncompletePolicy"));
	InHashBuilder.HashField(ExcludeTag, TEXT("ExcludeTag"));
	InHashBuilder.HashField(bCullHidden, TEXT("bCullHidden"));
	InHashBuilder.HashField(CullOptions.NumDirections, TEXT("CullNumDirections"));
	InHashBuilder.HashField(CullOptions.MinElevationDegrees, TEXT("CullMinElevation"));
	InHashBuilder.HashField(CullOptions.MaxElevationDegrees, TEXT("CullMaxElevation"));
	InHashBuilder.HashField(CullOptions.Resolution, TEXT("CullResolution"));
	InHashBuilder.HashField(CullOptions.bOrthographicViews, TEXT("CullOrthographicViews"));
	InHashBuilder.HashField(CullOptions.bKeepNeighbors, TEXT("CullKeepNeighbors"));
}

bool UCSNaniteCutHLODBuilder::RequiresWarmup() const
{
	// 引擎的约定（HLODBuilder.h:136）：产出 Nanite 网格的构建器要预热，否则切换的头几帧是粗网格。
	const UCSNaniteCutHLODBuilderSettings* Settings = Cast<const UCSNaniteCutHLODBuilderSettings>(HLODBuilderSettings);
	return Settings && Settings->bEnableNaniteOutput;
}

TSubclassOf<UHLODBuilderSettings> UCSNaniteCutHLODBuilder::GetSettingsClass() const
{
	return UCSNaniteCutHLODBuilderSettings::StaticClass();
}

TArray<UActorComponent*> UCSNaniteCutHLODBuilder::Build(const FHLODBuildContext& InHLODBuildContext, const TArray<UActorComponent*>& InSourceComponents) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UCSNaniteCutHLODBuilder::Build);

	const UCSNaniteCutHLODBuilderSettings* Settings = Cast<const UCSNaniteCutHLODBuilderSettings>(HLODBuilderSettings);
	if (!Settings) Settings = GetDefault<UCSNaniteCutHLODBuilderSettings>();

	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		UE_LOG(LogCSNaniteCutHLOD, Error, TEXT("[CSNaniteCutHLOD] Nanite cut extraction runs on the GPU and needs a real RHI; no HLOD mesh was built for %s."), *InHLODBuildContext.AssetsBaseName);
		return {};
	}

	const float WorldCutError = UCSNaniteCutOps::CutErrorForScreenError(
		float(InHLODBuildContext.MinVisibleDistance), Settings->PixelError, Settings->ReferenceScreenWidth, Settings->ReferenceHorizontalFOV);

	// 小物体归 foliage 管、带排除标签的物体，都不进 HLOD（HLOD 层按说也不该收它们，这里兜一层）。
	TArray<FCSNaniteCutSource> Sources;
	int32 NumExcluded = 0;
	int32 NumFoliageSkipped = 0;
	int32 NumTexCoordSets = 1;
	UCSNaniteCutOps::MakeSourcesFromComponents(FilterComponents<UStaticMeshComponent>(InSourceComponents), WorldCutError,
		Settings->ExcludeTag, Sources, NumExcluded, NumFoliageSkipped, NumTexCoordSets);
	if (NumExcluded > 0 || NumFoliageSkipped > 0)
	{
		UE_LOG(LogCSNaniteCutHLOD, Log, TEXT("[CSNaniteCutHLOD] %s: skipped %d component(s) tagged '%s' and %d foliage component(s)."),
			*InHLODBuildContext.AssetsBaseName, NumExcluded, *Settings->ExcludeTag.ToString(), NumFoliageSkipped);
	}
	if (Sources.IsEmpty()) return {};

	// Nanite 最多存 4 组 UV（NANITE_MAX_UVS）；按源网格实际用到的组数声明，多出来的显存谁都不付。
	UCSMesh* Merged = NewObject<UCSMesh>(GetTransientPackage());
	NumTexCoordSets = FMath::Clamp(NumTexCoordSets, 1, 4);
	if (NumTexCoordSets > 1) UCSMeshOps::EnsureTexCoordSets(Merged, NumTexCoordSets);

	FCSNaniteCutOptions Options;
	Options.bAppend = false;
	Options.IncompletePolicy = Settings->IncompletePolicy;
	Options.bRequestMissingPages = Settings->bRequestMissingPages;
	FCSNaniteCutResult Result;
	UCSNaniteCutOps::AppendNaniteCuts(Merged, Sources, Options, Result);

	int32 NumByStatus[int32(ECSNaniteCutStatus::Failed) + 1] = {};
	for (int32 Index = 0; Index < Result.Sources.Num(); ++Index)
	{
		const FCSNaniteCutSourceResult& SourceResult = Result.Sources[Index];
		++NumByStatus[int32(SourceResult.Status)];
		if (SourceResult.Status != ECSNaniteCutStatus::Complete)
		{
			UE_LOG(LogCSNaniteCutHLOD, Warning, TEXT("[CSNaniteCutHLOD] %s: %s -> %s (forced clusters %d, voxel clusters %d)"),
				*InHLODBuildContext.AssetsBaseName, *GetNameSafe(Sources[Index].Mesh),
				*StaticEnum<ECSNaniteCutStatus>()->GetNameStringByValue(int64(SourceResult.Status)),
				SourceResult.ForcedClusters, SourceResult.VoxelClusters);
		}
	}
	UE_LOG(LogCSNaniteCutHLOD, Log, TEXT("[CSNaniteCutHLOD] %s: %d source(s), CutError %.2f cm at %.0f cm; complete %d, incomplete %d, skipped %d, not ready %d, not Nanite %d, unsupported %d, failed %d; %d triangles."),
		*InHLODBuildContext.AssetsBaseName, Sources.Num(), WorldCutError, InHLODBuildContext.MinVisibleDistance,
		NumByStatus[int32(ECSNaniteCutStatus::Complete)], NumByStatus[int32(ECSNaniteCutStatus::Incomplete)],
		NumByStatus[int32(ECSNaniteCutStatus::Skipped)], NumByStatus[int32(ECSNaniteCutStatus::NotReady)],
		NumByStatus[int32(ECSNaniteCutStatus::NotNanite)], NumByStatus[int32(ECSNaniteCutStatus::Unsupported)],
		NumByStatus[int32(ECSNaniteCutStatus::Failed)], Result.WrittenTriangles);

	if (Result.WrittenTriangles <= 0)
	{
		Merged->ReleaseDeferred();
		return {};
	}

	// HLOD 只在 MinVisibleDistance 以外显示：从那一圈视点都看不见的三角形（被整个挡住的物体、穿插的内部面、
	// 封闭房间与山洞深处）永远画不出来。
	if (Settings->bCullHidden)
	{
		FCSMeshVisibilityCullResult CullResult;
		UCSMeshVisibilityOps::CullHiddenTriangles(Merged, InHLODBuildContext.MinVisibleDistance, Settings->CullOptions, CullResult);
		UE_LOG(LogCSNaniteCutHLOD, Log, TEXT("[CSNaniteCutHLOD] %s: hidden-triangle cull %d -> %d (%d visible, %d views)."),
			*InHLODBuildContext.AssetsBaseName, CullResult.TrianglesBefore, CullResult.TrianglesAfter, CullResult.TrianglesVisible, CullResult.NumViews);
	}

	FCSGpuMeshCPUData MeshData;
	const bool bReadBack = Merged->ReadbackMeshSync(MeshData);
	TArray<UMaterialInterface*> Materials;
	for (const TObjectPtr<UMaterialInterface>& Material : Merged->Materials) Materials.Add(Material.Get());
	Merged->ReleaseDeferred();
	if (!bReadBack)
	{
		UE_LOG(LogCSNaniteCutHLOD, Warning, TEXT("[CSNaniteCutHLOD] %s: readback of the merged cut failed."), *InHLODBuildContext.AssetsBaseName);
		return {};
	}

	CSNaniteCutHLOD_DropUnreferencedVertices(MeshData);

	// 以 HLOD 包围盒中心为枢轴烘到局部空间（引擎 MeshMerge 构建器同样把组件放在合并枢轴上）。
	// 提交 MeshDescription 的完整构建会按属性相等合并顶点，cluster 边界上重复的顶点顺带焊掉。
	const FVector Pivot = InHLODBuildContext.WorldPosition;
	FMeshDescription Description;
	if (!UCSGpuMeshComponent::BuildGpuMeshDescription(MeshData, FTransform(Pivot), /*bConvertToActorLocalSpace*/ true, Description))
	{
		UE_LOG(LogCSNaniteCutHLOD, Warning, TEXT("[CSNaniteCutHLOD] %s: building the mesh description failed."), *InHLODBuildContext.AssetsBaseName);
		return {};
	}

	UObject* Outer = InHLODBuildContext.AssetsOuter ? InHLODBuildContext.AssetsOuter : GetTransientPackage();
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Outer,
		MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(*(InHLODBuildContext.AssetsBaseName + TEXT("_NaniteCut")))));
	if (!CSStaticMeshAsset::PopulateFromDescription(StaticMesh, Description, Materials, MeshData.TriangleMaterialSlots,
		/*bCommitMeshDescription*/ true, Settings->bEnableNaniteOutput))
	{
		UE_LOG(LogCSNaniteCutHLOD, Warning, TEXT("[CSNaniteCutHLOD] %s: static mesh build failed."), *InHLODBuildContext.AssetsBaseName);
		return {};
	}
	StaticMesh->ClearFlags(RF_Public | RF_Standalone);

	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	Component->SetStaticMesh(StaticMesh);
	Component->SetWorldLocation(Pivot);
	return { Component };
}

#endif // WITH_EDITOR
