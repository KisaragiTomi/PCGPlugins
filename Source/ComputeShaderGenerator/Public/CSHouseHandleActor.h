#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CSHouseHandleActor.generated.h"

class ACSHouseActor;
class UMaterialInterface;
class UPrimitiveComponent;
class UStaticMesh;

/**
 * **所有房屋控制抓手的公共基类**（计划 D5 / D8 的交互层）。
 *
 * "抓手"在本项目里的定义很窄：一个挂在（或吸附到）房子上的**辅助 actor**，用户用编辑器
 * 原生 transform gizmo 拖它，拖动本身就是对宿主房的一次通知。它自己**不产任何游戏内几何**
 * —— 可见的东西要么归宿主房产出（窗洞、窗台），要么只是编辑器示意道具（TG 箭头、高度框）。
 * 现有两族：`ACSHouseResizeHandleActor`（推拉墙）与 `ACSHouseFeatureMarker`（窗等特征）。
 *
 * ⚠️ **`ACSGroundShaperActor` 不在这一族里**，虽然它也是"拖着改地形"的道具：它是
 * `ACSTinyGlade`（要产石阶网格、要上传快照），继承不到这里来。它与本类唯一的共同点是
 * 编辑器示意组件的配法，那一份由下面的 `MakeEditorGizmoProp` 共享。
 *
 * ## 基类收敛了什么（每一条都是两族各写一遍、且写错过或差点写错的）
 *
 * ① **宿主弱引用 + 换宿主纪律**。换宿主必须"先向旧的注销、再挂新的"（`SetHost`）——
 *    顺序反了会在两房相邻时留下一份永远没人来收的重复登记。
 *
 * ② **`Destroyed()` 与 `EndPlay()` 两处都要解绑，缺一不可**。编辑器 world 没有 begun play，
 *    `World->DestroyActor()` 只发 `Destroyed()`，`EndPlay` 一次都不来；而 PIE 结束与关卡卸载
 *    又只发 `EndPlay`。只写一处的症状是"在编辑器里删掉窗标记，墙上的洞还在"，且 PIE 里一切
 *    正常 —— 单测 `House.WindowMarker` 就是这么抓到的。
 *
 * ③ **spawn 之后紧跟的那一次 `PostEditMove(bFinished=true)` 不是松手**。`GEditor->AddActor`
 *    （拖入视口、`EditorActorSubsystem::spawn_actor_from_class`、Python spawn 全走它）在 spawn
 *    之后立刻补一次。把它当松手的话，抓手会在自己还在原点、周围一栋房都没有的那一瞬间判无
 *    宿主并 `Destroy()` —— 症状极难认：actor 引用还在（Python 拿得到），但组件已经是 garbage，
 *    `root_component` 是 None、`get_actor_location()` 恒为零，日志里只有一行
 *    "RegisterComponentWithWorld: Trying to register component with IsValid() == false"。
 *    基类因此把第一次 `bFinished=true` **降级成非最终裁决**（`HandleDrag(false)`）：解析、attach、
 *    登记照做，只是不自毁、不回写变换。⚠️ 2026-09-06 一度改成"整个跳过"，**那是个 bug**：
 *    `AddActor` 之后不再来任何事件（拖放的落点就是终点），跳掉 = 标记永远不解析，症状是
 *    "把窗拖到房子上一扇窗都不出"。当时给的理由（"先落位后应用旋转"）也是错的 —— `AddActor`
 *    把 Rotation 一起传给了 `SpawnActor`；会拿默认 +X 去解析的是 `PostRegisterAllComponents`，
 *    那条由 `RF_WasLoaded` 闸门挡着。降级之所以够用：没人回写变换 ⇒ 误判不会自我固化。
 *    ⚠️ 只有能被 `AddActor` 生成的子类会走到这条；`ACSHouseResizeHandleActor` 是
 *    `NotPlaceable` 且只由房子 `World->SpawnActor` 生成，从来收不到合成事件。
 *
 * ④ **自毁只在最终裁决时发生**。拖拽途中不判 —— 否则窗户从房 A 拖向房 B 的途中会半路消失。
 *
 * ⑤ **交互的唯一执行面是 `HandleDrag`**，编辑器的 `PostEditMove` 只是它的触发器。
 *    无头测试摆完 `SetActorLocation` 直接调它，不需要 gizmo、不需要 Slate。
 *
 * 子类只要回答一个问题：**这一次拖动意味着什么**（`OnHandleDrag`）。
 */
UCLASS(Abstract, NotBlueprintable)
class COMPUTESHADERGENERATOR_API ACSHouseHandleActor : public AActor
{
	GENERATED_BODY()

public:
	ACSHouseHandleActor();

	/** 当前宿主房。弱引用失效 = 房子没了。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Handle")
	ACSHouseActor* GetHost() const { return Host.Get(); }

	UFUNCTION(BlueprintPure, Category = "CS House|Handle")
	bool HasHost() const { return Host.IsValid(); }

	/**
	 * **交互的唯一执行面**（纪律 ⑤）：把"我现在在这儿"交给子类翻成对宿主的一次改动。
	 * 返回是否仍有有效宿主。
	 *
	 * `bFinished = true` 表示"松手"：只有这一刻才允许无宿主自毁（纪律 ④）。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Handle")
	bool HandleDrag(bool bFinished);

	/**
	 * 无宿主时自毁（计划 D8 裁决）。
	 *
	 * ⚠️ **无头测试与脚本务必关掉它**：脚本先 spawn 再摆位是常态，而 spawn 的那一瞬间抓手
	 * 在原点、找不到房子 —— 开着它就会当场自删，测试拿到一个 `IsValid == false` 的指针，
	 * 而"抓手没了"看起来和"它判自己放不下"一模一样。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Handle")
	bool bDestroyWhenHostless = true;

	//~ AActor
	/** 纪律 ②：编辑器 world 里只有这一条会来。 */
	virtual void Destroyed() override;
	/** 纪律 ②：这一条覆盖 PIE 结束与关卡卸载，`Destroyed` 收不到它们。 */
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
#if WITH_EDITOR
	/** gizmo 拖动的落点。纪律 ③ 的跳过在这里做，逻辑全在 `HandleDrag`。 */
	virtual void PostEditMove(bool bFinished) override;
#endif

	/**
	 * 把自己摆回"该在的地方"，并按宿主当前尺寸重算示意几何。
	 *
	 * 有规范位置的抓手（拉尺寸的四条边、调高度的那个框）覆写它；**特征标记不覆写** ——
	 * 标记的位置是用户拖出来的诉求本身，没有"该在哪"这回事（它反过来由房子的锚点吸附驱动）。
	 * 基类空实现，所以房子可以对一整排抓手无差别地调它。
	 */
	virtual void SnapToCanonical() {}

	/**
	 * 把一个组件配成"编辑器示意道具"：不挡射线、游戏里不存在、不投影、可移动。
	 *
	 * 静态是为了让 `ACSGroundShaperActor`（不同基类，见类头注释）也能用同一份配法 ——
	 * 这六行各写一遍的症状是"某个道具在游戏里露了出来"或"它把射线挡住了"。
	 */
	static void MakeEditorGizmoProp(UPrimitiveComponent* Component);

	/**
	 * 给示意组件挂上高亮自发光材质（`Scripts/TinyGladeMakeHandleMaterial.py` 建的
	 * `M_CSHandleHighlight`）。缺资产时什么都不做 —— 退成默认材质，交互不受影响。
	 *
	 * **惰性加载而不是 `ConstructorHelpers`**：后者只在 CDO 构造时跑一次，资产还没建出来的
	 * 那次编辑器启动会把"找不到"永久缓存下来，之后建好也得重启才生效。惰性加载在第一次生成
	 * 抓手时才解析，脚本跑完下一次进模式就有了。
	 *
	 * ⚠️ 放在基类而不是各子类的匿名命名空间里：unity 构建会把多个 .cpp 合进同一个翻译单元，
	 * 两个同名的匿名命名空间常量当场 C2374 重定义（2026-09-06 踩过）。
	 */
	static void ApplyHighlightMaterial(UPrimitiveComponent* Component);

	/**
	 * 描边用的**压暗**版：同一张 `M_CSHandleHighlight` 的动态实例，`Intensity` 归零，只剩它近黑的底色。
	 * 给 TG 原版箭头的 `flat_arrow_outline` 用 —— TG 就是"亮的本体 + 一圈暗边"，暗边让箭头在亮地面上也读得出来。
	 * 缺资产时同样什么都不做。不另建材质资产：参数本来就暴露着，多一张就多一处要跟着改。
	 */
	static void ApplyOutlineMaterial(UPrimitiveComponent* Component);

	/**
	 * TG 原版编辑箭头网格：`Scripts/TinyGladeMakeHandleArrows.py` 从 TG 源 json 烘到
	 * `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/<Name>`（与其余 TG 网格同目录、同名）。
	 * 惰性加载，理由同 `ApplyHighlightMaterial`；缺资产返回 nullptr，调用方留自己的兜底网格。
	 */
	static UStaticMesh* LoadTGArrowMesh(const TCHAR* Name);

protected:
	/**
	 * 子类实现：这一次拖动意味着什么。返回**是否仍有有效宿主** —— 返回 false 且 `bFinished`
	 * 时基类会按 `bDestroyWhenHostless` 自毁。
	 */
	virtual bool OnHandleDrag(bool bFinished) PURE_VIRTUAL(ACSHouseHandleActor::OnHandleDrag, return false;);


	/**
	 * 子类可覆写：与宿主解绑时要额外做什么（向宿主注销登记等）。
	 * 基类在换宿主与销毁时调，**必须幂等**（两条销毁路径都会走到）。
	 */
	virtual void OnDetachFromHost() {}

	/** 换宿主：先向旧的解绑再挂新的（纪律 ①）。传 nullptr 等同于 `DetachFromHost()`。 */
	void SetHost(ACSHouseActor* NewHost);

	/** 解绑并清空宿主。幂等。 */
	void DetachFromHost();

	/** 宿主弱引用。子类只读；要改走 `SetHost`，否则绕过换宿主纪律。 */
	UPROPERTY(Transient)
	TWeakObjectPtr<ACSHouseActor> Host;

private:
	/** 惰性加载 `M_CSHandleHighlight`（理由见 `ApplyHighlightMaterial`）。缺资产返回 nullptr。 */
	static UMaterialInterface* LoadHighlightMaterial();

	/**
	 * 已经过了"落地"这一关吗 —— **这一位是防自杀的**（纪律 ③，2026-08-31 被演示回归抓住）。
	 * spawn 之后的第一次 `bFinished=true` 一律**降级成非最终裁决**，真正的松手从第二次起才算。
	 * ⚠️ 2026-09-06 曾一度改成"整个跳过"，那会让视口拖放永远不解析（拖放之后不再来事件），
	 * 已改回降级；逐条见纪律 ③ 与 `IsPlacementResolve()`。
	 */
	bool bHasBeenPlaced = false;

};
