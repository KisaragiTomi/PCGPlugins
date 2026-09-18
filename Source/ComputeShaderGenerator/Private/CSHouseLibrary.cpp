#include "CSHouseLibrary.h"

#include "CSHouseActor.h"
#include "CSHouseFeatureMarker.h"
#include "CSHouseSeam.h"   // IdLess —— 枚举的确定次序（unity 构建下别指望上一行替你带进来）
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"   // TActorRange
#include "UObject/Class.h"   // UClass::HasAnyClassFlags / CLASS_Abstract（同上：unity 构建会替你藏起来）

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeHouseLibrary, Log, All);

void UCSHouseLibrary::GetHouses(const UWorld* World, TArray<ACSHouseActor*>& Out)
{
	Out.Reset();
	if (!World) return;
	// `TActorRange` 默认跳过 pending kill；模板（CDO / 蓝图预览）不在关卡里，这里再挡一道只是保险。
	for (ACSHouseActor* House : TActorRange<ACSHouseActor>(World))
	{
		if (IsValid(House) && !House->IsTemplate()) Out.Add(House);
	}
	// 关卡 actor 数组的次序由生成历史决定（理由见头文件），排完才是可复现的名单。
	Out.Sort([](const ACSHouseActor& A, const ACSHouseActor& B) { return CSHouseSeam::IdLess(A.GetHouseId(), B.GetHouseId()); });
}

ACSHouseActor* UCSHouseLibrary::PickHouse(const UWorld* World, const FVector& WorldOrigin, const FVector& WorldDir,
	float MaxDistance, FCSWallHit& OutHit)
{
	OutHit = FCSWallHit();
	if (!World || MaxDistance <= 0.0f) return nullptr;
	const FVector Dir = WorldDir.GetSafeNormal();
	if (Dir.IsNearlyZero()) return nullptr;

	// 复用 GetHouses 的 GUID 升序，别在这里另写一遍遍历 —— 两处各排一次迟早分叉。
	TArray<ACSHouseActor*> Houses;
	GetHouses(World, Houses);

	ACSHouseActor* Best = nullptr;
	float BestDistance = MaxDistance;
	for (ACSHouseActor* House : Houses)
	{
		// 世界 → 局部那一步归房子自己（`RayHitWall` 里用的是 `GetBuildTransform`，即烘常驻流
		// 的那个只取 yaw 的变换）。在这里自己解一遍就是第二份口径，迟早与常驻流错开。
		const FCSWallHit Hit = House->RayHitWall(WorldOrigin, Dir, BestDistance);
		if (!Hit.bHit || Hit.Distance >= BestDistance) continue;

		Best = House;
		BestDistance = Hit.Distance;
		OutHit = Hit;
	}
	if (!Best) OutHit = FCSWallHit();
	return Best;
}

ACSHouseActor* UCSHouseLibrary::PickHouseNear(const UWorld* World, const FVector& WorldPoint, float MaxDistance,
	FCSWallHit& OutHit)
{
	OutHit = FCSWallHit();
	if (!World || MaxDistance <= 0.0f) return nullptr;

	TArray<ACSHouseActor*> Houses;
	GetHouses(World, Houses);

	ACSHouseActor* Best = nullptr;
	float BestDistance = MaxDistance;
	for (ACSHouseActor* House : Houses)
	{
		const FCSWallHit Hit = House->NearestWall(WorldPoint, BestDistance);
		if (!Hit.bHit || Hit.Distance >= BestDistance) continue;

		Best = House;
		BestDistance = Hit.Distance;
		OutHit = Hit;
	}
	if (!Best) OutHit = FCSWallHit();
	return Best;
}

ACSHouseFeatureMarker* UCSHouseLibrary::PlaceMarkerAlongRay(const UObject* WorldContextObject,
	TSubclassOf<ACSHouseFeatureMarker> MarkerClass, const FVector& RayOrigin, const FVector& RayDir,
	float MaxDistance)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World || !MarkerClass) return nullptr;

	// ⚠️ **抽象类挡在最前面，而且必须出声。** `SpawnActor` 对 `CLASS_Abstract` 恒返回 nullptr，
	// 于是这条路上唯一的症状是"点了一下，什么都没发生" —— 而 `WindowBrushClass` 是个
	// `TSubclassOf`，用户或脚本完全可以把它指到一个抽象蓝图上。挡在这里而不是让 `SpawnActor`
	// 去挡，图的是**日志说得出是哪个字段填错了**：`LogSpawn` 那句通用警告与"窗笔刷"毫无字面
	// 关联，在一屏编辑器日志里等于没有。
	//
	// ⚠️ **`ACSWindowMarker` 本身不是抽象类**，空 `WindowBrushClass` 的退路是好的 ——
	// `CLASS_Abstract` **不在 `CLASS_Inherit` 里**（`ObjectMacros.h`），所以父类
	// `ACSHouseFeatureMarker` 的 `Abstract` 不会传染给它（会传染的是同处一行的 `NotPlaceable`，
	// 那条**正是**想要的）。别看见父类是 `Abstract` 就去把退路换成别的类：
	// 单测 `House.WindowBrushPlacement` 逐条钉住"父抽象 / 子不抽象 / 子不可放置"这三件事。
	if (MarkerClass->HasAnyClassFlags(CLASS_Abstract))
	{
		UE_LOG(LogTinyGladeHouseLibrary, Warning,
			TEXT("[TinyGladeHouse] window brush: marker class '%s' is abstract and cannot be spawned; "
				 "nothing was placed. Point the house's WindowBrushClass at a concrete window "
				 "blueprint (e.g. BP_Window_Cottage_1x1)."),
			*MarkerClass->GetPathName());
		return nullptr;
	}

	FCSWallHit Hit;
	ACSHouseActor* House = PickHouse(World, RayOrigin, RayDir.GetSafeNormal(), MaxDistance, Hit);
	// **打不到墙就什么都不生成。** 与拖放那条路的关键差别：那边先落一个 actor 再问"我贴在谁身上"，
	// 于是有了"找不到宿主要不要自毁"这一整套；这边根本不生成，没有游离标记这种状态。
	if (!House || !Hit.bHit) return nullptr;

	// ⚠️ **先生成、再量尺寸**，不走 CDO：`GetDemandSize` 读的是 `OpeningMesh` 上的网格，而子蓝图
	// 只改组件模板 —— 从 CDO 上量到的未必是这一档窗的真实尺寸。生成时那个位姿是临时的，
	// 下面 `AdoptAnchor` 会按锚点重摆。
	FActorSpawnParameters SpawnInfo;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACSHouseFeatureMarker* Marker = World->SpawnActor<ACSHouseFeatureMarker>(
		MarkerClass.Get(), FTransform(RayOrigin), SpawnInfo);
	if (!Marker) return nullptr;

	// 命中 → 锚点与拖 gizmo 走**同一个**函数（洞底 = 命中 Z − 半高；窗点在墙脚附近直接落成门）。
	// 早先这里自己抄了一遍"减半个窗高"，与 `OnHandleDrag` 靠注释保持同源。
	Marker->AdoptAnchor(House, Marker->MakeAnchorFromHit(Hit, *House));
	return Marker;
}

int32 UCSHouseLibrary::GetHouseCount(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	TArray<ACSHouseActor*> Houses;
	GetHouses(World, Houses);
	return Houses.Num();
}
