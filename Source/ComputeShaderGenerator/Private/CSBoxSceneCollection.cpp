#include "CSBoxSceneCollection.h"

#include "CSBoxSceneCollectionImpl.h"
#include "MeshGeneratorInternal.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

using namespace CSMeshGenInternal;

// -----------------------------------------------------------------------------
// FCSBoxScenePreparedData
// -----------------------------------------------------------------------------

bool FCSBoxScenePreparedData::HasAnyTriangles() const
{
	if (!Impl) return false;
	return !Impl->ResolvedRequests.IsEmpty()
		|| GetTriangleMeshDataTriangleCount(Impl->LandscapeTriangleData) > 0
		|| !Impl->NaniteTriangles.IsEmpty();
}

int32 FCSBoxScenePreparedData::GetMaterialRegistryNum() const
{
	return Impl ? Impl->MaterialRegistry.Num() : 0;
}

UMaterialInterface* FCSBoxScenePreparedData::GetMaterialByRegistryIndex(int32 Index) const
{
	if (!Impl || !Impl->MaterialRegistry.IsValidIndex(Index)) return nullptr;
	return Impl->MaterialRegistry[Index].Get();
}

// -----------------------------------------------------------------------------
// CollectBoxSceneTriangles
// -----------------------------------------------------------------------------

FCSBoxScenePreparedData CSBoxSceneCollection::CollectBoxSceneTriangles(
	UWorld* World,
	const FCSBoxSceneCollectOptions& Options)
{
	FCSBoxScenePreparedData Result;
	if (!World || !Options.QueryBox.IsValid) return Result;

	auto ImplData = MakeShared<FCSBoxScenePreparedDataImpl, ESPMode::ThreadSafe>();

	const int32 SafeMaxTriangles = FMath::Max(1, Options.MaxTriangles);
	// 没有参照点时距离过滤没有意义：归零后下游统一按"盒内全收"处理，省掉每个消费端各自判空。
	const float SafeRefDist = Options.ReferencePoints.IsEmpty() ? 0.0f : FMath::Max(0.0f, Options.ReferenceFilterDistance);

	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequestsInternal(World, Options.QueryBox, Options.LODIndex, Requests);

	if (!Options.RequiredActorTags.IsEmpty())
	{
		const TArray<FName>& RequiredActorTags = Options.RequiredActorTags;
		Requests.RemoveAll([&RequiredActorTags](const FCSStaticMeshTriangleRequest& Req)
		{
			if (!Req.SourceActor) return false;
			for (const FName& Tag : RequiredActorTags)
			{
				if (!Tag.IsNone() && Req.SourceActor->ActorHasTag(Tag)) return false;
			}
			return true;
		});
	}

	// RequiredActorTags 有意不传给地形：标签是用来挑"要哪些道具"的，连地面一起筛掉会让几何悬空。
	if (Options.bIncludeLandscape)
	{
		BuildBoxSceneLandscapeTrianglesInternal(
			World, Options.QueryBox, Options.ReferencePoints, SafeRefDist, SafeMaxTriangles,
			ImplData->LandscapeTriangleData);
	}

	ImplData->TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests, Options.ExcludedActor, Options.ExcludedActorTags, true, ImplData->ResolvedRequests, &ImplData->MaterialRegistry,
		Options.bUseMeshDescriptionSourceTriangles ? &ImplData->NaniteTriangles : nullptr,
		Options.bPreserveSourceMaterialSlots);

	ImplData->ReferencePoints = Options.ReferencePoints;
	ImplData->ReferenceFilterDistance = SafeRefDist;
	ImplData->SafeMaxTriangles = SafeMaxTriangles;

	Result.Impl = ImplData;
	return Result;
}
