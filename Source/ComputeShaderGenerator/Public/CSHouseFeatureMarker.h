#pragma once

#include "CoreMinimal.h"
#include "CSHouseHandleActor.h"
#include "CSHouseActor.h"       // FCSHouseWindow —— 标记的诉求就是它
#include "CSHouseProfile.h"     // FCSWallHit / ECSFeatureReject
// ⚠️ **完整类型，不能只前置声明**：三个网格组件都是 `UPROPERTY`，反射生成的代码要拿
// `UStaticMeshComponent::StaticClass()`。前置声明在 unity 构建下能过（邻居的 TU 把 include
// 带进来了），单独编译本头的 TU 当场炸 —— 正是"unity 藏起缺失 include"那一类，
// 2026-09-06 被 `-SingleFile` 抓住。
#include "Components/StaticMeshComponent.h"
#include "CSHouseFeatureMarker.generated.h"

class UBillboardComponent;

/**
 * 附属物（计划 D8）：**自己持有预制网格**，同时告诉房子"这里有一扇窗，请挖个洞"。
 *
 * ⚠️ **2026-09-06 用户裁决改了分工**（原口径「零可视几何、可见几何全归宿主房」在计划 D8 里
 * 作废留档）：那是布尔时代的产物 —— cutter 必须在房子的网格操作里，窗框才被迫并进房体。
 * 布尔早已退场，理由消失。现在是：
 *
 * - **附属物出「物」**：`OpeningMesh`（本体）+ `LintelMesh` / `SillMesh` / `GlassMesh`（盖顶件）+ 预制资产。
 *   它因此在视口里**点得到、选得中、能用 gizmo 拖**。
 *   ⚠️ **四件里只有 `OpeningMesh` 决定洞**，理由写在它的声明处 —— 搞错了不会报错。
 *   **组件恒在、网格可空**：空槽就是「这一件不存在」，不另设开关（判据只留一个真源）。它不是编辑器道具，**别拿 `MakeEditorGizmoProp` 配它**（那会 `bIsEditorOnly`
 *   + `HiddenInGame`，游戏里窗框就没了）。
 * - **房子只出「洞」**：按诉求在墙上挖，**不再**为窗砌窗框砖或窗台盒。洞缘的观感由预制框的
 *   翻边盖住 —— 与 TG 同构（TG 的窗 = CPU 裁砖出粗洞 + 一块预制框盖住洞缘）。
 *
 * 它仍不是 `ACSTinyGlade`：网格是**资产**不是生成物，没有"CPU 权威数据 → 快照"那条链。
 *
 * ## 锚点是权威，变换是派生量（2026-09-06）
 *
 * 序列化的是 `Anchor`（边号 + 离哪个角多远 + 窗台高），**不是**世界变换；变换每次由
 * `SnapToAnchor()` 从锚点算出来。对位 TG 的 `CachedDecoratorTransforms`（每帧从 `PublicWalls`
 * 重算的双缓冲派生量）+ `move_decorators_following_anchors` `[PDB]`。
 *
 * 两个方向别写反，写反了画面上就是"窗自己乱跑"：
 * - **用户拖 gizmo** ⇒ 世界位置 → 解析射线 → **新锚点** → 再派生回变换（这一步就是"吸附"）。
 * - **房子变了**（拉尺寸 / 改墙高）⇒ 锚点不动，由房子调 `SnapToAnchor()` 把标记搬过去。
 *   ⚠️ **这条路上绝不能重新射线**：那是拿标记的陈旧世界位置去覆盖正确的锚点 —— 等于让洞去
 *   追标记。2026-09-05 实测过它的症状：拉完尺寸窗错位，重开关卡窗**又**挪一次，落到第三个位置。
 *
 * ## 本类自己的两条纪律（生命周期那几条在基类）
 *
 * ① **宿主解析走解析求交，不走引擎 trace。** 房子的 gpumesh 全线 `NoCollision`，
 *    `LineTraceSingle` 一栋都打不到 —— 照 trace 写的症状是"窗户一放就没"（无宿主 ⇒ 自毁），
 *    而自毁是合法行为，不会有任何断言报红。判据在 `ACSHouseActor::RayHitWall`。
 *
 * ② **编辑器 tick 要显式打开。** actor 默认在编辑器 world 不 tick，必须
 *    `bStartWithTickEnabled` 配合 override `ShouldTickIfViewportsOnly() → true`，
 *    否则整条"洞跟手"在编辑器里静默失效（与接缝 actor 同一个坑）。
 *    ⚠️ 基类默认 `bCanEverTick = false`，本类在构造里自己打开。
 *
 * 「自毁只在松手时判、拖拽期间只通知不判」那一条已上移到基类（`HandleDrag`）——
 * 两族抓手同一个坑：窗户从房 A 拖向房 B 的途中会半路消失。
 *
 * ## 诉求 ≠ 结果
 *
 * 登记进去的是诉求，能不能砌出来由 `ACSHouseActor::QueryFeatureReject` 说了算（门拱优先）。
 * 裁决结果回写到 `bCausesCut` / `LastReject` —— **没有回执，"被门拱挤掉"和"根本没登记上"
 * 在画面上逐像素相同**。
 */
// ⚠️ **`NotPlaceable`：不许再拖进视口**（2026-09-06 用户裁决 —— 笔刷是创建窗户的唯一入口）。
// 这个说明符**会被继承**，所以 `ACSWindowMarker` 与它的子蓝图一并不可放置 —— 那正是想要的；
// 而 `ACSWindowMarker` 上的 `UCLASS(Blueprintable)` 必须保留（`NotBlueprintable` 同样会继承，
// 不显式打开就建不出蓝图，而且不报错，只是右键菜单里没有）。
//
// 退掉拖放这条路的理由是它**从原理上就不稳**：actor 得拿自己的 forward 去解析宿主，而朝向
// 什么时候被应用在两条 spawn 路径上不一样 —— `UEditorEngine::AddActor` 把 Rotation 一起传给
// `SpawnActor`（回调时已对），`EditorActorSubsystem` 那条却是先放置、回调之后才设朝向（回调
// 那一刻 forward 还是默认 +X，实测咬上 11 m 外的另一栋房并判 `SillTooLow`）。
// 生成的正路现在是 `UCSHouseLibrary::PlaceMarkerAlongRay` —— 它吃的是**视口点击那条射线**，
// 与 actor 自身朝向无关。
UCLASS(Abstract, NotBlueprintable, NotPlaceable)
class COMPUTESHADERGENERATOR_API ACSHouseFeatureMarker : public ACSHouseHandleActor
{
	GENERATED_BODY()

public:
	ACSHouseFeatureMarker();

	/**
	 * 宿主探针的长度 cm：沿自身 +X（面向墙的方向）打这么远找房子。
	 * 默认 600 —— 比一栋房的进深还长，拖到房子附近就能咬上。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Feature Marker", meta = (ClampMin = "1.0"))
	float HostProbeDistance = 600.0f;

	/**
	 * 射线落空时的就近吸附半径 cm（计划 D8 的 `csh.WindowSnapDist`）。
	 * 兜的是"贴着墙但朝向没摆正" —— 两条都空才判无宿主。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Feature Marker", meta = (ClampMin = "0.0"))
	float SnapDistance = 200.0f;

	/**
	 * 拖拽兜底：tick 开着但连续这么多秒既没位移也没收到结束事件 ⇒ 自动关 tick 并做最终裁决。
	 * 防的是"漏掉 `bFinished` 导致永久 tick"（计划 D8 明写这条兜底）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Feature Marker", meta = (ClampMin = "0.1"))
	float DragIdleSeconds = 2.0f;

	/**
	 * 派生变换时，原点站在墙外表面**外侧**多远 cm。
	 *
	 * ⚠️ **不能是负数（不能推进墙里）**：派生出来的位置必须是宿主解析的**不动点**，而从外皮内侧
	 * 出发时两条解析通路会一起失效（射线版得到负距离被 `Dist <= 0` 挡掉、就近版判成背面直接
	 * `continue`）—— 症状是"窗吸附一次之后再也解析不到宿主"，而无宿主会自毁，全程不报红。
	 * 逐行理由写在 `CSHouse_AnchorToLocal` 上，单测 `House.WallAnchor` 钉这条不动点。
	 * 窗框要嵌进墙里的话，调 `OpeningMesh` 自己的相对位置，别调这个。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Feature Marker", meta = (ClampMin = "0.0"))
	float WallStandoff = 5.0f;

	/** 房子确实为它切了洞吗（用户点名要的那个 bool）。 */
	UFUNCTION(BlueprintPure, Category = "CS Feature Marker")
	bool CausesCut() const { return bCausesCut; }

	/** 上一次裁决的拒绝原因。`None` + `CausesCut() == false` 表示"压根没找到宿主"。 */
	UFUNCTION(BlueprintPure, Category = "CS Feature Marker")
	ECSFeatureReject GetLastReject() const { return LastReject; }

	/** 标记的稳定身份 —— 它同时是登记进宿主的那个 `SourceId`（计划 D8 明写）。 */
	UFUNCTION(BlueprintPure, Category = "CS Feature Marker")
	FGuid GetMarkerId() const { return MarkerId; }

	/** 当前锚点 —— 序列化的那份权威。 */
	UFUNCTION(BlueprintPure, Category = "CS Feature Marker")
	FCSWallAnchor GetAnchor() const { return Anchor; }

	/**
	 * 房子拉尺寸时把锚点**重新表达**到新墙上（`ACSHouseActor::ReanchorMarkersToPreserveWorld`）。
	 * 只写锚点：不解析、不登记、不动变换 —— 吸附由同一轮末尾的 `SnapToAnchor()` 统一做。
	 *
	 * ⚠️ 这不是"洞去追标记"那条禁令的例外：输入是**旧墙上的锚点**，不是标记的世界变换。
	 * 重新表达前后指的是同一个物理点，只是换了个角来度量它。
	 */
	void Reanchor(const FCSWallAnchor& InAnchor)
	{
		Modify();
		Anchor = InAnchor;
	}

	/**
	 * 按锚点把自己搬到宿主墙上（**派生变换**，TG `CachedDecoratorTransforms` 的对位物）。
	 * 宿主重建后由 `ACSHouseActor::NotifyMarkersRebuilt` 逐个调，松手时自己调一次。
	 * 无宿主或锚点无效时什么都不做。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Feature Marker")
	void SnapToAnchor();

	/**
	 * **笔刷落笔的落地入口：直接认下宿主与锚点，一条射线都不打。**
	 *
	 * 与 `ResolveHostAndRegister` 的分工是输入不同，不是时机不同：
	 *   - `ResolveHostAndRegister`：输入是**本 actor 的变换**（"我在哪、朝哪，谁是我的墙"）——
	 *     拖 gizmo 调位置走这条；
	 *   - `AdoptAnchor`：输入是**已经解出来的命中**（"墙和位置都定了，你收下"）—— 点击创建走这条。
	 *
	 * 点击那一刻拿到的是视口的相机射线与精确命中点，比让 actor 用自己的 forward 去猜可靠得多。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Feature Marker")
	void AdoptAnchor(ACSHouseActor* NewHost, const FCSWallAnchor& InAnchor);

	/**
	 * 宿主把这一轮的裁决回写进来（房子重建后逐个推）。
	 *
	 * **必须有这条**：`bCausesCut` / `LastReject` 原来只在 `OnHandleDrag` 里写，而拉尺寸 /
	 * 改墙高**不走**那条路 —— 症状是房子已经把窗挤掉了、标记还在说"我切出洞了"
	 * （2026-09-05 核出的过期回执）。顺带按裁决显隐网格：被拒时藏起来，对位 TG 的
	 * `validate_blueprints` 不实例化被拒蓝图 `[PDB]`；不藏的话画面上是"一块窗框贴在实墙上"。
	 */
	void ApplyHostVerdict(ECSFeatureReject Reason);

	/**
	 * 正在被 gizmo 拖吗。tick 只在拖拽期开着，所以它就是判据。
	 * 房子据此跳过这一个不写变换 —— 标记**不 attach 在房子下**，没有拉尺寸抓手那种父级带动的反馈，
	 * 所以这里写它纯粹是和 gizmo 抢方向盘，跳过是对的。（拉尺寸抓手 2026-09-06 反过来改成
	 * 了"每次都重摆"，因为那边不重摆条子会跑到光标前面 —— 两边结论不同，成因也不同。）
	 */
	bool IsBeingDragged() const { return IsActorTickEnabled(); }

	/**
	 * 解析宿主 + 登记诉求，一步做完。**拖拽期每 tick 调它，松手时也调它。**
	 *
	 * `bFinal = true` 时才允许自毁（基类纪律 ④）。返回是否解析到了宿主。
	 * 无头测试直接调这一条 —— 它只是基类 `HandleDrag` 的本族别名，两者完全等价。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Feature Marker")
	bool ResolveHostAndRegister(bool bFinal) { return HandleDrag(bFinal); }

	//~ AActor
	virtual void PostRegisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	/** 纪律 ②：不 override 它，编辑器 world 里一次 tick 都不会发生。 */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	/**
	 * 细节面板改任何一项都要重排 + **重新登记诉求**。
	 *
	 * 少了重新登记，改 `PieceScale` 或换 `OpeningMesh` 之后画面上是"窗件变了、墙上的洞没变" ——
	 * 洞的权威在房子的登记表里，标记不吭声它就一直用旧尺寸。
	 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
#if WITH_EDITOR
	/** 只管拖拽期的 tick 开关；裁决本身走基类（降级 + `HandleDrag` + 自毁时机）。 */
	virtual void PostEditMove(bool bFinished) override;
	/**
	 * 撤销 / 重做走这条，**不是** `PostEditMove` —— 少了它，撤销一次移动之后 actor 回到旧位置、
	 * 宿主那份登记却还停在新位置（登记表是 transient 的派生物，事务系统不会替它回滚）。
	 */
	virtual void PostEditUndo() override;
#endif

	/** 半个洞高 —— `AnchorToWorld` 的入参，省得调用方自己拆一遍 `GetDemandSize`。 */
	UFUNCTION(BlueprintPure, Category = "CS Feature Marker")
	float GetDemandHalfHeight() const
	{
		float W = 0.0f, Hh = 0.0f;
		GetDemandSize(W, Hh);
		return Hh * 0.5f;
	}

	/**
	 * **决定洞有多大的那一件** —— 附属物的本体（窗 = 框体）。
	 *
	 * ⚠️ **只有它进 `GetDemandSize`，一个字都不多读。** 这是本类最容易搞错、而且**搞错了不报错**
	 * 的一条：把过梁并进来的话，包围盒会从 78×160 涨到 125×182，墙上真的开出一个比窗大一圈的洞，
	 * 而过梁正好把它盖住 —— 谓词、砖数、三角数全绿，只有从侧面或洞的内壁才看得见。
	 * TG 用 `setdressing_` 前缀把两类角色分开，本类用**三个具名组件**做同一件事。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Feature Marker")
	TObjectPtr<UStaticMeshComponent> OpeningMesh;

	/**
	 * 盖顶件（过梁）。对位 TG 的 `setdressing_window_lintel` / `window_gothic_*_hat`。
	 * **房子对它一无所知** —— 洞是矩形，尖拱 / 挑檐这类形状全靠它盖出来（卷二 §1.2c 实测：
	 * gothic 的帽子比框体每侧宽 12.4 cm、顶部高出 18 cm，正好盖住矩形洞的上边缘与两个上角）。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Feature Marker")
	TObjectPtr<UStaticMeshComponent> LintelMesh;

	/** 窗台。对位 TG 的 `setdressing_window_sill`。同样不参与定洞。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Feature Marker")
	TObjectPtr<UStaticMeshComponent> SillMesh;

	/**
	 * 玻璃。对位 TG 的 `window_cottage_*_glass` —— **TG 里它是独立的一件**，走单独的 nani
	 * subset（`SolidVertexColorWindowGlass`，见对照文档 §1.4 的 20 个 InstanceData 类型）。
	 *
	 * ⚠️ **少了它，洞就是**真**透的**：框体（`decorators_window_cottage_1x1`）只有 17 cm 厚、
	 * 中间是空的，玻璃不补上就能直接看进屋里 —— 这正是 2026-09-06「窗子的表现不对」的成因。
	 * 不参与定洞（同过梁 / 窗台）：它与框体等宽等高，算进去也不改洞，但把纪律写死更省事。
	 *
	 * 不需要透明材质：TG 的窗玻璃实测就是 `MSM_DefaultLit` + `BLEND_Opaque`（速查表第 6 条）。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Feature Marker")
	TObjectPtr<UStaticMeshComponent> GlassMesh;

	/**
	 * 整扇窗的尺寸倍率（X = 宽、Y = 高），四件网格一起缩。**这是"调窗户大小"的那个参数。**
	 *
	 * 洞会自动跟着变：`GetDemandSize` 量的是 `OpeningMesh` 的包围盒**乘上它自己的相对变换**，
	 * 而倍率就写在那个变换里 —— 所以这里改完，墙上的洞、谓词、砖的裁剪一起跟上，C++ 无需再动。
	 *
	 * ⚠️ **TG 自己没有这个参数**：那边靠换预制件（`window_cottage_1x1/2x1/3x1`，宽 78/143.5/213）
	 * 换档，是离散的。倍率是本项目补的连续旋钮，两者可以叠加 —— 先在子蓝图里选档，再用它微调。
	 * 想完全照 TG 就别动它（保持 1,1）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Feature Marker",
		meta = (ClampMin = "0.1", ClampMax = "10.0"))
	FVector2D PieceScale = FVector2D(1.0, 1.0);

	/**
	 * 按 `PieceScale` 重新缩放四件网格，并把过梁抬到框体顶上。
	 *
	 * ⚠️ **过梁的抬升量是现算的，不是常数**：早先写死 87.5（= 半个 160 高的框 + 半个 15 高的
	 * 过梁），换成 2x1 / 3x1 / gothic 或者一动倍率就摆错位 —— 而"过梁陷进框里"或"浮在半空"
	 * 都不会有任何断言报红。现在从两件各自的包围盒算，换什么件都对。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Feature Marker")
	void RefreshPieceLayout();

protected:
	/**
	 * 把**锚点**翻成本类型的诉求。子类实现（窗 = `ACSWindowMarker`）。
	 *
	 * ⚠️ 2026-09-06 起入参是锚点而不是命中：诉求必须从锚点派生，"房子变了 → 锚点不动 →
	 * 诉求跟着新墙重算"才成立。拿命中去算的话，房子一改尺寸诉求就停在陈旧的弧长上。
	 * `CenterS` 用 `CSHouse_AnchorS(InAnchor, Host.GetFootprint(), Host.WallThickness)` 取。
	 */
	virtual FCSHouseWindow MakeDemand(const FCSWallAnchor& InAnchor, const ACSHouseActor& InHost) const
		PURE_VIRTUAL(ACSHouseFeatureMarker::MakeDemand, return FCSHouseWindow(););

	/**
	 * 本类型的洞尺寸 cm。**锚点只存位置不存尺寸** —— 尺寸是"这个附属物多大"的属性，
	 * 归子类（窗默认从预制网格的包围盒取，见 `ACSWindowMarker::GetDemandSize`）。
	 * 命中点 → 窗台高那一步要用它（命中的是窗心，洞底还要减半个高）。
	 *
	 * **公开**：`AnchorToWorld` 要半个高才能算出标记该在的位置，无头测试与脚本得能自己算一遍
	 * 对答案；藏起来的话它们只能把 `Height` 硬编一遍，而自动取尺寸一开就对不上了。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Feature Marker")
	virtual void GetDemandSize(float& OutWidth, float& OutHeight) const
		PURE_VIRTUAL(ACSHouseFeatureMarker::GetDemandSize, );

	//~ ACSHouseHandleActor
	virtual bool OnHandleDrag(bool bFinished) override;
	virtual void OnDetachFromHost() override;

	/**
	 * 稳定身份。**必须序列化** —— 它是宿主那张登记表的 key，重开关卡后必须还是同一个，
	 * 否则同一扇窗会在谓词的"自己不与自己冲突"里变成两扇互相打架的窗。
	 */
	UPROPERTY(VisibleAnywhere, Category = "CS Feature Marker")
	FGuid MarkerId;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS Feature Marker")
	bool bCausesCut = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS Feature Marker")
	ECSFeatureReject LastReject = ECSFeatureReject::None;

#if WITH_EDITORONLY_DATA
	/**
	 * 编辑器拾取件（图标）。**它存在的唯一理由是：被谓词拒时三件网格全藏了，视口里就点不中了**
	 * —— 而那恰恰是最想抓住它、把它拖到能放的地方去的时刻。
	 *
	 * ⚠️ 因此 `ApplyHostVerdict` **只藏 `UStaticMeshComponent`**，不走根组件的传播 ——
	 * 传播会把这个图标一起藏掉，等于白加。按类型藏还有个好处：子蓝图里新加的网格件自动跟着藏，
	 * 不必回来改这段（"加了第四件忘了藏"是个不会报错的坑）。
	 */
	UPROPERTY()
	TObjectPtr<UBillboardComponent> PickSprite;
#endif

	/**
	 * 墙上的锚点 —— **唯一权威，必须序列化**。世界变换是它的派生量，别反过来存。
	 * 加载时按它登记 + 派生变换，**不重新射线**（重新射线 = "重开关卡窗又挪一次位"）。
	 */
	UPROPERTY(VisibleAnywhere, Category = "CS Feature Marker")
	FCSWallAnchor Anchor;

	/**
	 * 最后一次被**接受**的锚点。松手时若当前位置被拒就回退到它并吸附回去 ——
	 * 对位 TG 的 `backup::DecoratorBackup` / `create_or_restore_decorator_backup` `[PDB]`。
	 * 从未被接受过（无效）时不回退：标记保持游离、网格藏起来。
	 */
	UPROPERTY(VisibleAnywhere, Category = "CS Feature Marker")
	FCSWallAnchor LastAcceptedAnchor;

	/** 按当前 `Anchor` 算诉求 → 过谓词 → 登记 → 回写裁决。解析与加载两条路共用这一段。 */
	void RegisterAnchor(ACSHouseActor& InHost);

	/** 显隐**全部网格件**（按类型找，编辑器拾取件不受影响）。理由见 `PickSprite`。 */
	void SetMeshPiecesVisible(bool bVisible);

private:
	/** 拖拽态：tick 开着的累计静止时长，超过 `DragIdleSeconds` 就自己收尾。 */
	float IdleSeconds = 0.0f;
	FVector LastTickLocation = FVector::ZeroVector;

	// 「spawn 之后那一次假松手要降级」的 `bPlaced` 已上移到基类
	// （`ACSHouseHandleActor::bHasBeenPlaced`）—— 它是编辑器的通病，不是窗标记独有的。
};

/**
 * 窗（D8 的首个附属物子类）。
 *
 * 它**只多四个字段**（宽 / 高 / 形状 / 自动取尺寸）—— 其余全部继承。这正是计划要的扩展形态：
 * 将来加"烟囱 / 雨棚 / 花箱"也只是再写一个 `MakeDemand` + 挂一个预制网格，不动通知与生命周期。
 *
 * ## 蓝图分层：总蓝图调参，子蓝图换网格
 *
 * ⚠️ **`Blueprintable` 不是可有可无的**：两个基类都是 `NotBlueprintable`，而这个说明符**会被继承**
 * —— 不在这里显式打开，编辑器里根本创建不出以它为父类的蓝图，而且不报错，只是右键菜单里没有。
 *
 * 预期用法（`Content/HouseTest/`）：
 * - **总蓝图**（`BP_TinyGladeWindow`）：调参数 —— 探针长度 / 吸附半径 / `WallStandoff` /
 *   `bAutoSizeFromMesh` / 手填的宽高与洞形。子蓝图全部继承这一份。
 * - **子蓝图**（`BP_Window_Cottage_1x1` / `BP_Window_Gothic_1x1` …）：**只换三个网格槽**
 *   与它们各自的相对变换。因为 `bAutoSizeFromMesh` 默认开着，洞会自动跟着那一档的
 *   `OpeningMesh` 走，**C++ 一行不用改**。
 *
 * ⚠️ 盖顶件的相对位置**逐资产不同**，别指望一个默认值通吃：`setdressing_window_lintel` 的 origin
 * 居中 ⇒ 要抬到 Z +87.5；而 `window_gothic_*_hat` 的 origin 是 `(0, 0, +79.8)`、**已经在窗的局部
 * 空间里预摆好**（和 `setdressing_window_sill` 一样）⇒ 子蓝图里应当把它调回 **0**。
 */
UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API ACSWindowMarker : public ACSHouseFeatureMarker
{
	GENERATED_BODY()

public:
	ACSWindowMarker();

	/** 窗宽 cm。**仅在关掉 `bAutoSizeFromMesh` 或没挂网格时生效**（默认从网格包围盒取）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "10.0"))
	float Width = 78.0f;

	/** 窗高 cm。同上，默认被网格包围盒覆盖（TG 那块框板是 160 高）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "10.0"))
	float Height = 110.0f;

	/**
	 * 洞形。三种原型**一行 shader 都不用改** —— `FCSOpeningClipField` 本来就是二维的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window")
	ECSOpeningShape Shape = ECSOpeningShape::Rect;

	/**
	 * 洞的宽高从 **`OpeningMesh`** 的包围盒取（**不算过梁与窗台**）（"房子适配 mesh"的字面含义，2026-09-06 裁决）。
	 * 关掉则用上面手填的 `Width` / `Height`。**没挂网格时自动退回手填值** —— 否则换一个没设
	 * 网格的标记会得到一个零尺寸的洞，而谓词只会淡淡地说一句 `Degenerate`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window")
	bool bAutoSizeFromMesh = true;

	/**
	 * 默认窗框资产（TG `decorators_window_cottage_1x1`，实测 78 × 17 × 160 cm、32 三角）。
	 *
	 * 挑它不挑 `window_cottage_1x1`：后者是**整套窗**、进深 85 cm，塞不进 24 cm 的墙；
	 * 这一块才是文档里那块"覆在裁出来的洞口上"的薄框板。材质随资产走
	 * （`MI_window_colors_layer00`，`M_TG_Texture` 的实例、`DefaultLit` + `Opaque`）——
	 * **窗不需要玻璃材质，TG 里它也不是透明的**（用户裁决 2026-09-06）。
	 *
	 * ⚠️ **资产的 +Y 朝外、+X 是宽**，而本类约定 **+X 是墙的内法线** ⇒ 三个网格组件都带
	 * **+90° 相对 yaw**（构造里设）：资产 +X → actor ∓Y（宽度沿墙），资产 +Y → actor −X（朝外）。
	 *
	 * 「+Y 朝外」是量出来的：`setdressing_window_sill` 的 origin 是 `(0, +28, −76)`、Y 向跨
	 * `[+9, +47]`，整块压在 +Y 一侧 —— 窗台是往外挑的（挑出去才排水，花箱也挂在外面）。
	 * ⚠️ 一度写成 −90°（2026-09-06 早些时候），把资产的正面转进了墙里。**单独一块对称的
	 * 17 cm 板看不出来**，所以没暴露；加上窗台 / 过梁立刻就错，而且框体的正面法线一直是反的。
	 * 改资产时要一并确认这条，否则洞的宽高会取到进深上去。
	 */
	static const TCHAR* DefaultFrameMeshPath;

	/** 默认过梁（`setdressing_window_lintel`，125 × 65 × 15，origin 居中 ⇒ 构造里抬到 Z +87.5）。 */
	static const TCHAR* DefaultLintelMeshPath;

	/** 默认窗台（`setdressing_window_sill`，78.1 × 38 × 12，origin (0, +28, −76) 已预摆 ⇒ 零偏移）。 */
	static const TCHAR* DefaultSillMeshPath;
	/**
	 * ⚠️ 名字里**没有** `decorators_` 前缀，这不是笔误：`window_cottage_1x1` 在 TG 源里
	 * 顶层与 `decorators/` 各有一份**不同的网格**，导入时靠前缀消歧；而 `_glass` 只有
	 * `decorators/` 一份，没冲突 ⇒ 拍平后就是裸名。选错了会拿到 568 三角的整窗装配件。
	 */
	static const TCHAR* DefaultGlassMeshPath;

	virtual void GetDemandSize(float& OutWidth, float& OutHeight) const override;

protected:
	virtual FCSHouseWindow MakeDemand(const FCSWallAnchor& InAnchor, const ACSHouseActor& InHost) const override;
};
