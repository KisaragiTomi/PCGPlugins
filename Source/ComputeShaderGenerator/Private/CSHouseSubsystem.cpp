#include "CSHouseSubsystem.h"

#include "CSHouseActor.h"
#include "CSHouseFeatureMarker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#include "CSHouseActor.h"
#include "CSHouseSeam.h"   // IdLess —— 花名册的确定次序（unity 构建下别指望上一行替你带进来）
#include "Engine/World.h"
#include "UObject/Class.h"   // UClass::HasAnyClassFlags / CLASS_Abstract（同上：unity 构建会替你藏起来）

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeHouseSubsystem, Log, All);

bool UCSHouseSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// 默认只放行 Game / PIE。本项目的持久创作全在编辑器 world 里进行（PIE 里画的东西随
	// PIE world 丢弃属预期），所以 Editor 必须在列 —— 否则这个 subsystem 在实际用它的
	// 场景里根本不存在。
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::Editor;
}

void UCSHouseSubsystem::Deinitialize()
{
	Tracked.Reset();
	DirtyHouses.Reset();
	Super::Deinitialize();
}

TStatId UCSHouseSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCSHouseSubsystem, STATGROUP_Tickables);
}

void UCSHouseSubsystem::RegisterHouse(ACSHouseActor* House)
{
	if (!IsValid(House) || House->IsTemplate()) return;

	const FGuid Id = House->GetHouseId();
	if (!Id.IsValid()) return;

	FCSHouseTracked& Entry = Tracked.FindOrAdd(Id);
	Entry.House = House;
	// 基线取**当前**变换：登记时房子已经自己重求值过一次（PostRegisterAllComponents），
	// 拿旧基线只会在下一帧凭空唤醒一次。
	Entry.TrackingHash = House->GetTrackingHash();
}

void UCSHouseSubsystem::UnregisterHouse(ACSHouseActor* House)
{
	if (!House) return;
	const FGuid Id = House->GetHouseId();
	Tracked.Remove(Id);
	DirtyHouses.Remove(Id);
}

void UCSHouseSubsystem::GetTrackedHouses(TArray<ACSHouseActor*>& Out) const
{
	Out.Reset(Tracked.Num());
	// 先收键再排序，而不是直接迭代 Tracked：迭代序由插入历史与桶分布决定（理由见头文件）。
	TArray<FGuid> Keys;
	Tracked.GetKeys(Keys);
	Keys.Sort([](const FGuid& A, const FGuid& B) { return CSHouseSeam::IdLess(A, B); });
	for (const FGuid& Key : Keys)
	{
		if (const FCSHouseTracked* Entry = Tracked.Find(Key))
		{
			if (ACSHouseActor* House = Entry->House.Get()) Out.Add(House);
		}
	}
}

ACSHouseActor* UCSHouseSubsystem::PickHouse(
	const FVector& WorldOrigin, const FVector& WorldDir, float MaxDistance, FCSWallHit& OutHit) const
{
	OutHit = FCSWallHit();
	if (MaxDistance <= 0.0f) return nullptr;
	const FVector Dir = WorldDir.GetSafeNormal();
	if (Dir.IsNearlyZero()) return nullptr;

	// 复用 GetTrackedHouses 的 GUID 升序，别在这里另写一遍遍历 —— 两处各排一次迟早分叉。
	TArray<ACSHouseActor*> Houses;
	GetTrackedHouses(Houses);

	ACSHouseActor* Best = nullptr;
	float BestDistance = MaxDistance;
	for (ACSHouseActor* House : Houses)
	{
		if (!IsValid(House)) continue;
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

ACSHouseActor* UCSHouseSubsystem::PickHouseNear(
	const FVector& WorldPoint, float MaxDistance, FCSWallHit& OutHit) const
{
	OutHit = FCSWallHit();
	if (MaxDistance <= 0.0f) return nullptr;

	TArray<ACSHouseActor*> Houses;
	GetTrackedHouses(Houses);

	ACSHouseActor* Best = nullptr;
	float BestDistance = MaxDistance;
	for (ACSHouseActor* House : Houses)
	{
		if (!IsValid(House)) continue;
		const FCSWallHit Hit = House->NearestWall(WorldPoint, BestDistance);
		if (!Hit.bHit || Hit.Distance >= BestDistance) continue;

		Best = House;
		BestDistance = Hit.Distance;
		OutHit = Hit;
	}
	if (!Best) OutHit = FCSWallHit();
	return Best;
}

void UCSHouseSubsystem::MarkHouseDirty(ACSHouseActor* House)
{
	if (!IsValid(House)) return;
	const FGuid Id = House->GetHouseId();
	if (Tracked.Contains(Id)) DirtyHouses.Add(Id);
}

void UCSHouseSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Tracked.IsEmpty() && DirtyHouses.IsEmpty()) return;

	// ① 清掉已经消失的登记项（关卡卸载 / actor 被删而没走 EndPlay 的情形）。
	for (auto It = Tracked.CreateIterator(); It; ++It)
	{
		if (!It.Value().House.IsValid()) { DirtyHouses.Remove(It.Key()); It.RemoveCurrent(); }
	}

	// ② 低频兜底快扫：主通道（地面直推 / PostEditMove）覆盖不到的改动在这里被抓到 ——
	//    蓝图直设 transform、物理、Python 改属性，这三条一律不发任何委托。
	ScanAccumulator += DeltaTime;
	const bool bScanThisTick = ScanAccumulator >= ScanInterval;
	if (bScanThisTick)
	{
		ScanAccumulator = 0.0f;
		for (const TPair<FGuid, FCSHouseTracked>& Pair : Tracked)
		{
			ACSHouseActor* House = Pair.Value.House.Get();
			if (!House) continue;
			if (House->GetTrackingHash() != Pair.Value.TrackingHash) DirtyHouses.Add(Pair.Key);
		}
	}

	if (DirtyHouses.IsEmpty()) return;

	// ③ 逐个重求值。**这一步会改 Z（落座）**，所以基线必须在它之后回写。
	TArray<FGuid> Batch = DirtyHouses.Array();
	DirtyHouses.Reset();
	for (const FGuid& Id : Batch)
	{
		FCSHouseTracked* Entry = Tracked.Find(Id);
		ACSHouseActor* House = Entry ? Entry->House.Get() : nullptr;
		if (!House) continue;

		House->ReevaluateSite();
		++ScanWakeCount;

		// ④ 回写基线：用**落座之后**的变换。漏了这步，落座改的那点 Z 会在下一帧被当成
		//    "外部移动"再次唤醒 —— 幂等所以不出错，但会永远每帧空转一次并掩盖真实抖动。
		Entry->TrackingHash = House->GetTrackingHash();
	}
}

ACSHouseFeatureMarker* UCSHouseSubsystem::PlaceMarkerAlongRay(
	TSubclassOf<ACSHouseFeatureMarker> MarkerClass,
	const FVector& RayOrigin, const FVector& RayDir, float MaxDistance)
{
	UWorld* World = GetWorld();
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
		UE_LOG(LogTinyGladeHouseSubsystem, Warning,
			TEXT("[TinyGladeHouse] window brush: marker class '%s' is abstract and cannot be spawned; "
				 "nothing was placed. Point the house's WindowBrushClass at a concrete window "
				 "blueprint (e.g. BP_Window_Cottage_1x1)."),
			*MarkerClass->GetPathName());
		return nullptr;
	}

	FCSWallHit Hit;
	ACSHouseActor* House = PickHouse(RayOrigin, RayDir.GetSafeNormal(), MaxDistance, Hit);
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

	// 命中点是窗**心**的高度，锚点存的是**洞底** ⇒ 减半个窗高（口径与 `OnHandleDrag` 同源；
	// 两处写岔的症状是"窗整体偏高半扇"，而且贴檐口时会莫名判 `AboveEave`）。
	const float SillZ = FMath::Max(0.0f, Hit.Z - Marker->GetDemandHalfHeight());
	Marker->AdoptAnchor(House, CSHouse_MakeWallAnchor(Hit, House->GetFootprint(), House->WallThickness, SillZ));
	return Marker;
}

UCSHouseSubsystem* UCSHouseSubsystem::GetHouseSubsystem(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	return World ? World->GetSubsystem<UCSHouseSubsystem>() : nullptr;
}
