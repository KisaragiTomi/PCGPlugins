#include "CSHouseSubsystem.h"

#include "CSHouseActor.h"
#include "CSHouseSeam.h"   // IdLess —— 花名册的确定次序（unity 构建下别指望上一行替你带进来）
#include "Engine/World.h"

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
