#pragma once

#include "CoreMinimal.h"
#include "CSHouseHandleActor.h"
#include "CSHouseHeightHandleActor.generated.h"

class ACSHouseActor;
class UStaticMeshComponent;

/** 高度框挂在房子的哪一头。决定规范高度与拖动喂给房子的哪个入口，其余（外观、记账、宿主纪律）全族一份。 */
UENUM(BlueprintType)
enum class ECSHouseHeightHandleSide : uint8
{
	/** 檐口那个框：上下拖改 `WallHeight`（`ACSHouseActor::PushHeight`）。框停在檐口高度。 */
	Eave,
	/** 房底那个框：上下拖改房底（`ACSHouseActor::PushBase`，底动顶不动）。框停在房底（actor 局部 Z = 0）。 */
	Base,
};

/**
 * 调高度的抓手（计划 D5 的竖直自由度）：**一个横放的"窗框"**，沿上下拖它就改房子的一个高度量。
 * 一共两个（`ECSHouseHeightHandleSide`）：
 *  - **檐口框**（用户裁决 2026-09-06）：停在檐口高度，拖它改 `WallHeight` —— 往上拖房子长高、往下变矮，
 *    框始终贴着墙顶，所以它同时是"墙有多高"的读数。
 *  - **房底框**（用户裁决 2026-09-14「底动顶不动」）：停在房底，拖它改 `HeightOffset` 并反向改
 *    `WallHeight` —— 往上拖房子离地变高、墙变矮，檐口与屋顶的世界高度不动；往下反过来。
 *
 * 观感由用户点名：**四根细长长方体首尾相接围成一个矩形框**，长宽 = footprint 包围盒 × `FrameScale`
 * （默认 1.2 倍）。两个框观感相同，只是高度不同。
 * ⚠️ 房底框贴着地面，可能被 40–55 cm 的草挡住；先照逻辑位置放，要不要抬高显示待编辑器里看效果再定。
 *
 * ## 与拉尺寸抓手（`ACSHouseResizeHandleActor`）的分工
 *
 * 那些锥子（每条边一个）管**水平**（各推一面墙，改 footprint）；这两个框管**竖直**。
 * 同属 `ACSHouseHandleActor` 一族，生命周期、宿主纪律、记账量法完全一致，
 * 只是投影轴不同：锥子投到自己那面墙的外法线上，框投到**世界 Z** 上。
 *
 * ## 为什么一个框是一个 actor 带四根条子，而不是四个 actor
 *
 * 一个框只有**一个自由度**。拆成四个 actor 的话，用户抓哪根都在改同一个量，四个 gizmo 互相打架；
 * 而且框的四条边必须永远闭合，分给四个 actor 就得再写一套同步。拉尺寸那边正相反 ——
 * 每条边是一个独立自由度，所以那边才一面墙一个 actor。
 *
 * ## 父子回路
 *
 * - 檐口框：`PushHeight` 只改 `WallHeight`、**不动 actor 变换**，没有回路。
 * - 房底框：`PushBase` 改了 `HeightOffset`，重求值落座会 `SetActorLocation` 把房子整体抬 / 降 ——
 *   而框 attach 在房子下，父级移动会把它一起带走，与拉尺寸锥子那条"父级移动把抓手带走"的 2× 回路同型。
 *   处理办法也照抄锥子：偏移量取"当前位置 − 上次消费过的位置"（`LastConsumedWorld`），推完**全部抓手
 *   统一重摆**（含自己）并在重摆里重置记账量 —— 父级带走的那一截被重摆直接覆盖掉，不会被下一次事件
 *   当成用户拖的。`House.BaseHandle` 逐步钉"拖 δ 房底恰好走 δ"。
 * 记账量法两种框都留着：它同时还管着"顶在下限上时不许攒残差"这一条（与 `CSHouseResize.h` 的
 * 返回值契约同一个理由）。
 */
UCLASS(NotBlueprintable, NotPlaceable)
class COMPUTESHADERGENERATOR_API ACSHouseHeightHandleActor : public ACSHouseHandleActor
{
	GENERATED_BODY()

public:
	ACSHouseHeightHandleActor();

	/**
	 * 认宿主 + 认哪一头 + 摆到规范位置。`EnterResizeMode` 生成之后立刻调，只调一次。
	 *
	 * ⚠️ attach 必须在这之前完成（`SnapToCanonical` 写的是世界位置）。
	 */
	void InitializeHandle(ACSHouseActor* InHost, ECSHouseHeightHandleSide InSide);

	/** 这个框挂在哪一头。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	ECSHouseHeightHandleSide GetSide() const { return Side; }

	/**
	 * 规范位置（世界）：房心正上方，檐口框在檐口高度（局部 Z = `WallHeight`）、房底框在房底（局部 Z = 0）。
	 * 宿主失效时返回当前位置（原地不动比跳到原点安全）。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	FVector ComputeCanonicalWorldLocation() const;

	/** 摆回规范位置、按当前 footprint 重算四条边，并重置记账量。三者必须一起做。 */
	virtual void SnapToCanonical() override;

	/**
	 * **交互的唯一执行面**：把竖直位移翻成一次改高度（檐口框 → `PushHeight`，房底框 → `PushBase`）。
	 * 返回实际生效的变化 cm。
	 *
	 * 只取**世界 Z** 分量，水平分量忽略（用户把框拖歪不该改高度；歪掉的部分在回位时清掉）。
	 * `bFinished` 直通房子那边的入口。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Resize")
	float ConsumeDragToHost(bool bFinished);

	/**
	 * 框的大小 = footprint × 这个系数（用户裁决：1.2 倍）。纯观感量，不进任何判定。
	 * 1.0 会让框正好贴在墙皮上、抓不住；小于 1 会把框埋进房子里。
	 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "1.01"))
	float FrameScale = 1.2f;

	/** 条子的粗细 cm（截面是正方形）。 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "1.0"))
	float FrameThickness = 14.0f;

protected:
	//~ ACSHouseHandleActor
	virtual bool OnHandleDrag(bool bFinished) override;
	virtual void OnDetachFromHost() override;

private:
	/** 按当前 footprint 重算四条边的长度、粗细与摆位。`SnapToCanonical` 里调。 */
	void UpdateFrameGeometry();

	/** 挂在哪一头。生成时钉死，transient。 */
	UPROPERTY(Transient)
	ECSHouseHeightHandleSide Side = ECSHouseHeightHandleSide::Eave;

	/**
	 * 记账量：上一次已经被房子消费掉的世界位置。
	 * 与拉尺寸抓手同一条纪律 —— 记请求值而不是生效值，顶在下限上时残差会一路累积。
	 */
	UPROPERTY(Transient)
	FVector LastConsumedWorld = FVector::ZeroVector;

	/** 本次 `ConsumeDragToHost` 实际生效的高度变化，供公开 API 返回。 */
	float LastAppliedOffset = 0.0f;

	/**
	 * 框的四条边 = footprint **包围盒**的四条边，下标与 `CSHouseResize_EdgeOuterLocal` 同号（0 南 1 东 2 北 3 西）。
	 * 矩形房子上它们恰好也是 `CSHouse_GetEdge` 的边号；异形房子的边数与此无关（框只画包围盒）。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BarComponents[4];
};
