#pragma once

#include "CoreMinimal.h"
#include "CSBrushEdModeBase.h"

class ACSHouseActor;
class UWorld;

/**
 * 点一下在墙上加一扇窗（计划 D8，2026-09-06 用户裁决改成笔刷模式）。
 *
 * stroke 生命周期、笔刷球、Esc 取消全归 `FCSBrushEdModeBase`；本叶子只换三件事：
 *
 *  - **光标求交走解析式**：房子的 gpumesh 全线 `NoCollision`，引擎 trace 一栋都打不到，所以
 *    `TraceCandidatePoint` 覆盖成 `UCSHouseLibrary::PickHouse`（与地面笔刷同型）。
 *  - **不做圆盘散布**：`SamplePendingPoints` 空实现（基类明写支持这种叶子）。一次点击 = 一扇窗，
 *    落在**光标命中点**上；基类默认那套散布是给"刷一片"的笔刷用的。
 *  - **落笔即退出**：`bExitAfterCommit = true`，对上用户要的"点击后创建、随即退出笔刷模式"。
 *
 * ⚠️ **为什么创建要走笔刷，而不是把窗蓝图拖进视口。** 拖放那条路要靠 actor 自己的 forward 去
 * 解析宿主，而**朝向什么时候被应用在两条 spawn 路径上不一样**：`UEditorEngine::AddActor` 把
 * Rotation 一起传给 `SpawnActor`（回调时朝向已对），而 `EditorActorSubsystem` 那条是**先放置、
 * 回调之后**才 `SetActorLocationAndRotation`（回调那一刻 forward 还是默认 +X —— 实测咬上 11 m
 * 外的另一栋房并判 `SillTooLow`）。视口点击给的是**相机射线 + 精确命中点**，不依赖 actor 自身
 * 朝向，从根上没有这个问题。
 *
 * 落地全在 `UCSHouseLibrary::PlaceMarkerAlongRay`（**唯一执行面**，静态 `BlueprintCallable`）——
 * 本模式只是它的触发器，与抓手族纪律 ⑤ 同型：无头测试直接调那个函数，不需要视口、不需要 Slate。
 */
class FCSWindowBrushEdMode : public FCSBrushEdModeBase
{
public:
	static const FEditorModeID EM_CSWindowBrush;

	void SetTargetActor(ACSHouseActor* InTargetActor);

protected:
	virtual AActor* GetBrushTargetActor() const override;
	virtual FCSBrushSettings GetBrushSettings() const override;
	virtual void SamplePendingPoints() override;
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) override;
	virtual void ClearBrushTarget() override;
	virtual bool TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const override;

private:
	UWorld* GetTargetWorld() const;

	TWeakObjectPtr<ACSHouseActor> TargetActor;

	/**
	 * 最后一次光标命中的世界点与**外**法线。
	 *
	 * ⚠️ 基类只暴露 `GetBrushLocation()`，不给法线，而 `CommitSamples` 需要一条**指向墙**的射线
	 * 才能喂给 `PlaceMarkerAlongRay`。在这里缓存一份是最省事的做法 —— `mutable` 是因为
	 * `TraceCandidatePoint` 是 const（它在基类里被当成纯查询用）。
	 *
	 * ⚠️ 用命中点反推射线、而不是拿相机射线：这样放置由**命中**决定，掠射角点击也落在同一处。
	 */
	mutable FVector LastHitLocation = FVector::ZeroVector;
	mutable FVector LastHitNormal = FVector::UpVector;
	mutable bool bLastHitValid = false;
};
