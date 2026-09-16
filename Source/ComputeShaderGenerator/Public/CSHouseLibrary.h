#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Templates/SubclassOf.h"
#include "CSHouseProfile.h"   // FCSWallHit —— PickHouse 的出参
#include "CSHouseLibrary.generated.h"

class ACSHouseActor;
class ACSHouseFeatureMarker;
class UWorld;

/**
 * 房屋的世界级查询（2026-09-16 取代 `UCSHouseSubsystem`）。
 *
 * **没有登记表。** 房子只在"我动了、松了手"那一刻需要知道世界里还有哪些房子（D7 接缝：去发现
 * 从没碰过我的陌生房子），而这份名单引擎本来就替我们维护着 —— `TActorRange<ACSHouseActor>`
 * 就是它。自己再登记一张表，接缝路上换不来任何东西：房子自身的更新由根组件 `TransformUpdated`
 * 与各机制入口（`PushEdge` 等）直接标脏，邻居的变化由对端 `ReceiveContactFrom` 送到，
 * 原来那条 0.25 s 的兜底快扫已无事可做。
 *
 * 这里的三个查询都是无状态的：遍历 world、按 GUID 升序取最近。
 */
UCLASS()
class COMPUTESHADERGENERATOR_API UCSHouseLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 只读枚举 world 里全部房子（跳过模板与 pending kill），**按 GUID 升序**。
	 *
	 * ⚠️ **排序不是洁癖**：关卡 actor 数组的次序取决于加载与生成历史，同一个关卡换个加载顺序就能
	 * 换一个次序 —— 接缝砖的序号跟着它走，于是"同一份世界状态两次给出不同砖列"，而且不会有任何
	 * 断言报红。GUID 升序与摆位、加载顺序、以及谁先生成全都无关。没有 GUID 的房子（刚 spawn、
	 * 尚未走 `PostRegisterAllComponents`）排在最前，调用方按需跳过。
	 */
	static void GetHouses(const UWorld* World, TArray<ACSHouseActor*>& Out);

	/**
	 * 射线找宿主（D8 特征标记）。返回最近命中的房子，`OutHit` 是**该房局部空间**的墙面命中。
	 *
	 * ⚠️ **解析求交，不走引擎 trace** —— 房子的 gpumesh 全线 `NoCollision`，`LineTraceSingle`
	 * 一栋都打不到（计划 D8 明写）。判据全在 `CSHouse_RayHitWall` 这个纯函数里，这里只负责
	 * "遍历全部房子 + 解世界变换 + 取最近"。平局（两栋房同距）按 GUID 升序取先者。
	 */
	static ACSHouseActor* PickHouse(const UWorld* World, const FVector& WorldOrigin, const FVector& WorldDir,
		float MaxDistance, FCSWallHit& OutHit);

	/**
	 * 就近找宿主（射线落空时的退路，计划 D8 的 `csh.WindowSnapDist`）。
	 * 判据是"点到外墙面的垂距"，同样取最近、平局按 GUID 升序。
	 */
	static ACSHouseActor* PickHouseNear(const UWorld* World, const FVector& WorldPoint, float MaxDistance,
		FCSWallHit& OutHit);

	/**
	 * **点击创建窗户的唯一执行面**（2026-09-06 用户裁决：改用笔刷模式，点一下就建、随即退出）。
	 *
	 * 拿一条世界射线（视口点击那条），命中墙就在那儿生成一个标记，并**直接把宿主与锚点交给它** ——
	 * 标记出生就是"几何体记录坐标"的形态，spawn 期一条射线都不用打。
	 *
	 * ⚠️ 这条替掉的是"把窗蓝图拖进视口、让 actor 自己拿 forward 去解析宿主"那条路。那条路脆在
	 * **朝向什么时候被应用**：`UEditorEngine::AddActor` 把 Rotation 一起传给 `SpawnActor`，而
	 * `EditorActorSubsystem` 那条是先放置、回调之后才 `SetActorLocationAndRotation` —— 两条 spawn
	 * 路径行为不同，回调里的 forward 未必可信（实测咬上 11 m 外的另一栋房）。视口点击给的是
	 * **相机射线 + 精确命中点**，不依赖 actor 自身朝向，所以这条从根上没有那个问题。
	 *
	 * EdMode（`FCSWindowBrushEdMode`）只是它的触发器 —— 与抓手族纪律 ⑤ 同型：无头测试直接调它，
	 * 不需要视口、不需要 Slate。脚本侧：`unreal.CSHouseLibrary.place_marker_along_ray(house, ...)`
	 * —— 第一个参数是任意能解出 world 的对象（房子本身最顺手）。
	 *
	 * @return 生成出来的标记；射线没打到任何房子时返回 nullptr（此时**什么都不生成**）。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Window", meta = (WorldContext = "WorldContextObject"))
	static ACSHouseFeatureMarker* PlaceMarkerAlongRay(const UObject* WorldContextObject,
		TSubclassOf<ACSHouseFeatureMarker> MarkerClass, const FVector& RayOrigin, const FVector& RayDir,
		float MaxDistance);

	/** world 里当前的房子数（调试统计 / 无头断言）。 */
	UFUNCTION(BlueprintPure, Category = "CS House", meta = (WorldContext = "WorldContextObject"))
	static int32 GetHouseCount(const UObject* WorldContextObject);
};
