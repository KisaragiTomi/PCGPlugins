#pragma once

#include "CoreMinimal.h"
#include "CSTinyGlade.h"
#include "CSStairs.h"
#include "CSStairsActor.generated.h"

class ACSGroundActor;
class UCSGpuInstancedMeshComponent;
class UMaterialInterface;
class USplineComponent;
class UStaticMesh;

/**
 * 玩家绘制楼梯（TG §4.1 `playermade`，2026-09-16 用户："默认 actor 中添加 spline component"）。
 *
 * **楼梯点 = 样条控制点**。TG 那边楼梯点是装饰物、边是图的无向边、每条边一段自然三次样条（附录 D §1）；
 * 本 actor 取它最常见的形态 —— 一条开链 —— 直接用引擎的 `USplineComponent` 承载：视口里拖点、
 * 加点、删点、调点的缩放全是现成的编辑器交互，一行交互代码都不用写。
 *
 * - 节点高度 = 样条点的**世界 Z**；节点宽度 = `Width × 该点缩放的 Y`（TG 的 `StairDecoratorInfo.width`，沿边线性插值）。
 * - 判据与出砖全在纯函数层 `CSStairs`（`CSStairs.h`），本类只做：样条 + 地面 → 输入，砖 → 实例组件。
 * - 砖用 TG 的 `brick` 单位盒（**不是** `stair_step`，附录 D §2.2），与房子的门框砖是同一张网格、同一个材质。
 *
 * ## 什么时候重建
 *
 * 基类 `OnConstruction → ReevaluateSite` 是唯一入口：改属性、拖样条点（组件改动会重跑构造脚本）、
 * 挪完 actor 松手、撤销都会来；地面变了（塑形物、笔刷）由订阅的 `OnGroundChanged` 补一次。
 * 实例是**组件局部**的，拖 actor 的途中楼梯整体跟着走，地面贴合等松手那次重建再对齐。
 *
 * ⚠️ `SetInstances` 带一次阻塞的渲染刷新，所以重建前先比砖表哈希，没变就不上传
 * —— 地面广播一次就会叫每个订阅者，大多数楼梯根本不在变化区域里。
 *
 * ## 与 TG 的取舍（附录 D §9.3）
 *
 * - **支撑**：TG 楼梯底下由墙构造器砌拱墙 / 柱 / 托架（§3），离地触发阈值没查清 ⇒ MVP 在踏步块底下按层砌砖到地面
 *   （`bSolidToGround`），高处读作一段实心的砖砌台基。
 * - **梯子**：坡度 > 2.5 的段 TG 出木梯，本项目没有对位资产 ⇒ 留空并告警（`GetLadderRunCount`）。
 * - **还没做**：栏杆（§5）、挂墙节点与穿墙开洞（§4）、分叉的图（§1.2，这里只有一条开链或闭环）。
 */
UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADERGENERATOR_API ACSStairsActor : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	ACSStairsActor();

	// -------------------------------------------------------------------------
	// 形状
	// -------------------------------------------------------------------------

	/**
	 * 楼梯宽 cm。每个样条点再乘上自己缩放的 Y（视口里缩放样条点 = 调那一端的宽度，沿边线性过渡）。
	 * 范围照 TG 宽度 gizmo 的夹子 45–400（`ui_focus_stairs`）；TG 的默认宽没查到，取 150。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs", meta = (ClampMin = "45.0", ClampMax = "400.0"))
	float Width = 150.0f;

	/**
	 * 一级踏步的斜边长 cm（TG `Curve::try_resample(0.5)`）。**TG 没有踏高 / 踏深参数**：
	 * 踏高 = 它 × sinθ、踏深 = 它 × cosθ，陡了踏步自然变高变浅。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs", meta = (ClampMin = "10.0", ClampMax = "200.0"))
	float StepLength = 50.0f;

	/** 一块踏步砖的最小竖向尺寸 cm（从踏面往下长；相邻级互相叠压，底面看不到）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs", meta = (ClampMin = "5.0"))
	float MinBlockHeight = 60.0f;

	/** 前后级的进深搭接倍率（TG 1.13）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs", meta = (ClampMin = "1.0", ClampMax = "2.0"))
	float DepthOverlap = 1.13f;

	/**
	 * 贴地：高度不许比地面低 `BuryTolerance`、踏面至少高出地面 `TreadClearance`、平走道整体抬到地面之上。
	 * 关掉 = 完全按样条点的高度出（场景里没有 `ACSGroundActor` 时等价于关掉）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs")
	bool bFollowGround = true;

	/** 高度最多比地面低多少 cm（TG 10）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs", meta = (ClampMin = "0.0", EditCondition = "bFollowGround"))
	float BuryTolerance = 10.0f;

	/** 踏面至少高出地面多少 cm（TG 15）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs", meta = (ClampMin = "0.0", EditCondition = "bFollowGround"))
	float TreadClearance = 15.0f;

	/**
	 * 踏步块底下按层砌砖到地面（**MVP 支撑**）：离地高的楼梯读作一段实心的砖砌台基，而不是悬在半空的一串砖。
	 * TG 的做法是楼梯底下由墙构造器砌拱墙 / 柱（附录 D §3），触发高度没查清，先用这个替。
	 * 层高 `CSStairs::FParams::SupportCourseHeight`（40 cm），层缝按世界 Z 对齐。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs", meta = (EditCondition = "bFollowGround"))
	bool bSolidToGround = true;

	/** 横向切砖、进深随机数的种子。同参数同结果（拖样条时不闪）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs")
	int32 Seed = 0;

	// -------------------------------------------------------------------------
	// 砖
	// -------------------------------------------------------------------------

	/** 踏步砖的网格：TG 的 `brick`（100 cm 居中单位立方体，靠逐实例非均匀缩放成任意尺寸）。留空 = 不出砖。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Brick")
	TObjectPtr<UStaticMesh> BrickMesh;

	/** 整体覆盖材质。**留空 = 画 `BrickMesh` 资产自带的材质**（与房子的 `FrameMaterial` 同一口径）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Brick")
	TObjectPtr<UMaterialInterface> BrickMaterial;

	/** 默认砖网格路径（TG `brick`）。 */
	static const TCHAR* DefaultBrickMeshPath;

	// -------------------------------------------------------------------------
	// 入口与观测量
	// -------------------------------------------------------------------------

	/** 从样条 + 地面全量重算砖表；砖表没变就不上传。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Stairs")
	void RebuildStairs();

	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	USplineComponent* GetSpline() const { return Spline; }

	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	UCSGpuInstancedMeshComponent* GetBrickComponent() const { return BrickComponent; }

	/** 上一次重建出了几级踏步。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetStepCount() const { return CurrentStepCount; }

	/** 上一次重建出了几块砖（一级可以横向切成几块）。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetBrickCount() const { return CurrentBricks.Num(); }

	/** 上一次重建真的上传了几次（砖表哈希短路的观测量，单测用）。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetUploadCount() const { return UploadCount; }

	/** 上一次重建有几段因为太陡（TG 出梯子）被留空。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetLadderRunCount() const { return CurrentLadderRunCount; }

	/** 上一次重建的砖表（世界空间）。脚本与单测读它对答案，不必回读 GPU。 */
	const TArray<CSStairs::FBrick>& GetBricks() const { return CurrentBricks; }

	/**
	 * 把样条 + 地面翻成纯函数层的输入：逐段密采样、夹地、按坡度分类、同类合并成 run。
	 * **公开**是为了单测拿同一份输入去核对纯函数，不必重抄一遍采样口径。
	 */
	void BuildRuns(TArray<CSStairs::FRun>& OutRuns) const;

	/** 纯函数层的参数表（本 actor 的属性 → `CSStairs::FParams`）。 */
	CSStairs::FParams MakeParams() const;

	//~ ACSTinyGlade
	virtual void ReevaluateSite() override;

	//~ AActor
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

protected:
	virtual void GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const override;

private:
	/** 楼梯的路径 —— **默认子对象**，放进关卡就带着一条三点的上行样条。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Stairs", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplineComponent> Spline;

	/** 踏步砖的实例组件（CPU 数组路；楼梯是几十到几百块的量级）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Stairs", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSGpuInstancedMeshComponent> BrickComponent;

	/** 订阅着的地面。Transient：每次重建按需重新找。 */
	UPROPERTY(Transient)
	TObjectPtr<ACSGroundActor> Ground;

	FDelegateHandle GroundChangedHandle;

	TArray<CSStairs::FBrick> CurrentBricks;
	int32 CurrentStepCount = 0;
	int32 CurrentLadderRunCount = 0;
	uint32 BrickHash = 0;
	int32 UploadCount = 0;

	void ResolveGroundAndSubscribe();
	void UnsubscribeGround();
	void HandleGroundChanged(ACSGroundActor* ChangedGround, const FBox& ChangedBounds);
	CSStairs::FGroundSampler MakeGroundSampler() const;
};
