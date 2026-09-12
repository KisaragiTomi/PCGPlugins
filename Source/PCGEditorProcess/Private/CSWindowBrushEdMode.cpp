#include "CSWindowBrushEdMode.h"

#include "CSHouseActor.h"
#include "CSHouseFeatureMarker.h"
#include "CSHouseProfile.h"
#include "CSHouseSubsystem.h"
#include "Editor.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

const FEditorModeID FCSWindowBrushEdMode::EM_CSWindowBrush = TEXT("CSWindowBrushEdMode");

void FCSWindowBrushEdMode::SetTargetActor(ACSHouseActor* InTargetActor)
{
	TargetActor = InTargetActor;
	bLastHitValid = false;
	ResetBrushState();
}

AActor* FCSWindowBrushEdMode::GetBrushTargetActor() const
{
	return TargetActor.Get();
}

UCSHouseSubsystem* FCSWindowBrushEdMode::GetHouseSubsystem() const
{
	const ACSHouseActor* Target = TargetActor.Get();
	const UWorld* World = Target ? Target->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UCSHouseSubsystem>() : nullptr;
}

FCSBrushSettings FCSWindowBrushEdMode::GetBrushSettings() const
{
	FCSBrushSettings Settings;
	// 笔刷球在这里纯粹是个光标指示物 —— 它不参与散布（`SamplePendingPoints` 是空的），
	// 半径只影响你看到多大一个球。给一个"约等于一扇窗"的尺度，方便瞄。
	Settings.Radius = 60.0f;
	Settings.TraceRadius = 0.0f;
	Settings.MinSpacing = 0.0f;
	Settings.SamplesPerMouseMove = 1;
	// **点击后创建、随即退出**（2026-09-06 用户裁决）。这个字段基类本来就有。
	Settings.bExitAfterCommit = true;
	// 背面剔除交给 `CSHouse_RayHitWall` 自己判（它只认外表面），这里不需要再来一道。
	Settings.bRejectBackFaces = false;
	return Settings;
}

bool FCSWindowBrushEdMode::TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	bLastHitValid = false;

	UCSHouseSubsystem* Subsystem = GetHouseSubsystem();
	if (!Subsystem) return false;

	const FVector Delta = End - Start;
	const double Len = Delta.Size();
	if (Len <= UE_KINDA_SMALL_NUMBER) return false;
	const FVector Dir = Delta / Len;

	// **解析求交，不是引擎 trace**：房子的 gpumesh 全线 `NoCollision`，`LineTraceSingle` 一栋都
	// 打不到（计划 D8 明写）。这里扫的是**花名册上所有房子**，不只是按钮所属的那一栋 ——
	// 笔刷开着的时候点哪栋就该往哪栋上放。
	FCSWallHit Hit;
	ACSHouseActor* House = Subsystem->PickHouse(Start, Dir, float(Len), Hit);
	if (!House || !Hit.bHit) return false;

	// `FCSWallHit` 是**房子局部空间**的 (边号, 弧长, 高度)，要还原成世界点与外法线。
	// 走公开的 `AnchorToWorld`：它返回的变换 **X 轴就是墙的内法线**，位置就是外皮上那一点
	// （`Standoff = 0`、`HalfHeight = 0` ⇒ 恰好落在命中高度上）。
	// ⚠️ 不直接用 `GetBuildTransform()` 自己拼 —— 它是 private，而且"构建空间只取 yaw"这条
	// 口径必须只有一处实现（三处写岔的症状是房子一转窗就贴到旁边去）。
	const FCSWallAnchor Probe = CSHouse_MakeWallAnchor(Hit, House->GetFootprint(), House->WallThickness, Hit.Z);
	if (!Probe.IsValidAnchor()) return false;

	const FTransform WallFrame = House->AnchorToWorld(Probe, 0.0f, 0.0f);
	LastHitLocation = WallFrame.GetLocation();
	LastHitNormal = -WallFrame.GetUnitAxis(EAxis::X);   // X = 内法线 ⇒ 取反得外法线
	bLastHitValid = true;

	OutHit = FHitResult();
	OutHit.bBlockingHit = true;
	OutHit.ImpactPoint = LastHitLocation;
	OutHit.Location = LastHitLocation;
	OutHit.ImpactNormal = LastHitNormal;
	OutHit.Normal = LastHitNormal;
	return true;
}

void FCSWindowBrushEdMode::SamplePendingPoints()
{
	// **故意留空**（基类头文件明写支持这种叶子：pending 集为空，stroke 生命周期与笔刷球照常）。
	// 一次点击 = 一扇窗，落在光标命中点上；基类默认那套圆盘散布是给"刷一片"的笔刷用的，
	// 用在这里会在一次点击里撒出好几扇窗、而且都不在你瞄的地方。
}

void FCSWindowBrushEdMode::CommitSamples(const TArray<FCSBrushSample>& /*Samples*/)
{
	ACSHouseActor* Target = TargetActor.Get();
	UCSHouseSubsystem* Subsystem = GetHouseSubsystem();
	if (!Target || !Subsystem || !bLastHitValid) return;

	// 从命中点往墙里反推一条短射线。⚠️ 不复用相机射线：那样掠射角点击会落在别处，而这条
	// 恒垂直于墙、必然重新命中同一面（`CSHouse_RayHitWall` 只认外表面，`dot(Dir, 外法线) < 0`
	// 在这里恒成立）。
	static constexpr float Standoff = 50.0f;
	static constexpr float Reach = 200.0f;
	const FVector Origin = LastHitLocation + LastHitNormal * Standoff;

	// 放哪一档窗由房子上的 `WindowBrushClass` 决定；留空就退回 C++ 默认那块 cottage 框板。
	TSubclassOf<ACSHouseFeatureMarker> Class = Target->WindowBrushClass;
	if (!Class) Class = ACSWindowMarker::StaticClass();

	FScopedTransaction Transaction(NSLOCTEXT("CSWindowBrush", "PlaceWindow", "Place Window"));
	ACSHouseFeatureMarker* Marker = Subsystem->PlaceMarkerAlongRay(Class, Origin, -LastHitNormal, Reach);

	// 打空不算错：`PlaceMarkerAlongRay` 在没命中时**什么都不生成**（没有"游离标记"这种状态），
	// 所以这里也没有需要回滚的东西。
	if (!Marker)
	{
		Transaction.Cancel();
		return;
	}
	GEditor->SelectNone(false, true);
	GEditor->SelectActor(Marker, true, true);
}

void FCSWindowBrushEdMode::ClearBrushTarget()
{
	TargetActor.Reset();
	bLastHitValid = false;
}
