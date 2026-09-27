#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CSStairsWidthHandleActor.generated.h"

class ACSStairsActor;
class UStaticMeshComponent;

/**
 * 与房屋一致的临时 actor + 编辑器 transform gizmo；仅控制楼梯整体宽度。
 *
 * 示意网格是 TG 的 `flat_arrow` + 描边 `flat_arrow_outline`（TG 楼梯拉宽 `ui_focus_stairs` 与房子拉尺寸共用这一对），
 * 与房子墙面的推墙箭头同一套摆法：躺平、头朝外、gizmo 在箭头正中。缺资产时退回引擎锥子。
 * ⚠️ TG 自己的楼梯宽度箭头是竖起来对着相机的；这里有意跟房子墙面箭头保持一致（平放），见
 * `Docs/TinyGlade/evidence/edit-signifier-arrows-20260922.txt`。
 */
UCLASS(BlueprintType, NotBlueprintable, NotPlaceable)
class COMPUTESHADERGENERATOR_API ACSStairsWidthHandleActor : public AActor
{
	GENERATED_BODY()

public:
	ACSStairsWidthHandleActor();
	void InitializeHandle(ACSStairsActor* InHost, int32 InSide);

	UFUNCTION(BlueprintPure, Category = "CS Stairs|Resize")
	ACSStairsActor* GetHost() const { return Host.Get(); }

	/** 沿路径方向看：-1 左侧，+1 右侧。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs|Resize")
	int32 GetSide() const { return Side; }

	UFUNCTION(BlueprintPure, Category = "CS Stairs|Resize")
	FVector GetOuterNormalWorld() const;

	UFUNCTION(BlueprintPure, Category = "CS Stairs|Resize")
	FVector ComputeCanonicalWorldLocation() const;

	/** 蓝图自定义拖拽：先移动此 actor，再调用本函数；编辑器 gizmo 自动调用。 */
	UFUNCTION(BlueprintCallable, Category = "CS Stairs|Resize")
	bool HandleDrag(bool bFinished);

	/** 返回该侧实际移动的 cm；对称变化使总宽增量等于两倍侧边移动量。 */
	UFUNCTION(BlueprintCallable, Category = "CS Stairs|Resize")
	float ConsumeDragToHost(bool bFinished);

	void SnapToCanonical();
	virtual void Destroyed() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
#if WITH_EDITOR
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<ACSStairsActor> Host;

	UPROPERTY(VisibleAnywhere, Category = "CS Stairs|Resize")
	TObjectPtr<UStaticMeshComponent> ArrowComponent;

	/** TG 箭头的描边，挂在本体下（同一套网格局部坐标，零相对变换）。退回锥子时没有网格。 */
	UPROPERTY(VisibleAnywhere, Category = "CS Stairs|Resize")
	TObjectPtr<UStaticMeshComponent> ArrowOutlineComponent;

	/** 本体装上的是 TG 箭头（头指网格局部 +Y）而不是兜底锥子（尖指 +Z）。 */
	bool bTGArrow = false;

	int32 Side = 1;
	// 记账点保存在宿主局部空间，整体搬动/旋转宿主不会被当成拖宽度。
	FVector LastConsumedHostLocal = FVector::ZeroVector;
	float LastAppliedOffset = 0.0f;
	static constexpr float HandleOffset = 45.0f;
	static constexpr float HandleLift = 45.0f;
	void DetachFromHost();
};
