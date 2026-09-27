#pragma once

#include "CoreMinimal.h"
#include "CSTinyGlade.h"
#include "CSStairs.h"
#include "CSStairsConnection.h"
#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "CSStairsActor.generated.h"

class ACSGroundActor;
class ACSHouseActor;
class UCSGpuInstancedMeshComponent;
class UMaterialInterface;
class USplineComponent;
class UStaticMesh;
class ACSStairsWidthHandleActor;

struct FCSStairsTerraceLink
{
	TWeakObjectPtr<ACSHouseActor> House;
	int32 PointIndex = INDEX_NONE;
	FCSStairsTerraceConnection Connection;
};

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
 * 构造脚本、样条数据/组件变换通知、编辑器拖动和地面变化统一唤醒既有重建队列。
 * 样条拖动不依赖构造脚本重跑：通知在下一帧合并消费，松手补齐尚未消费的改动。
 * 实例是**组件局部**的，拖 actor 的途中楼梯整体跟着走，地面贴合等松手那次重建再对齐。
 *
 * ⚠️ `SetInstances` 带一次阻塞的渲染刷新，所以重建前先比砖表哈希，没变就不上传
 * —— 地面广播一次就会叫每个订阅者，大多数楼梯根本不在变化区域里。
 *
 * ## 与 TG 的取舍（附录 D §9.3）
 *
 * - **支撑**：沿样条规划拱洞，复用墙体剖面和排砖，按实际空间决定拱高；托架仍未接入。
 * - **梯子**：坡度 > 2.5 的段 TG 出木梯，本项目复用木板网格生成边梁与横档（`GetLadderRunCount`）。
 * - **还没做**：挂墙节点与穿墙开洞（§4）、分叉的图（§1.2，这里只有一条开链或闭环）。
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

	/** 开放样条两端自动对齐附近的平屋顶，并给露台围边留入口；不改原始样条。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Connection")
	bool bConnectTerraces = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Connection", meta = (ClampMin = "0.0", EditCondition = "bConnectTerraces"))
	float TerraceSnapDistance = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Connection", meta = (ClampMin = "0.0", EditCondition = "bConnectTerraces"))
	float TerraceSnapHeight = 100.0f;

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

	/** 沿楼梯路径砌到地面的支撑墙，复用房屋砖和拱洞剖面。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Support", meta = (EditCondition = "bFollowGround", DisplayName = "Grounded Supports"))
	bool bSolidToGround = true;

	/** 有足够离地空间时生成贯穿拱洞；关闭时为实心支撑墙。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Support", meta = (EditCondition = "bSolidToGround"))
	bool bArchedSupport = true;

	/** 横向切砖、进深随机数的种子。同参数同结果（拖样条时不闪）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs")
	int32 Seed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Railing")
	ECSStairsRailing Railing = ECSStairsRailing::Wooden;

	/** 木栏杆与陡段梯子共用，按资产包围盒适配尺寸。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Railing")
	TObjectPtr<UStaticMesh> WoodMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Stairs|Railing")
	TObjectPtr<UMaterialInterface> WoodMaterial;

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

	/** 与房屋同名的蓝图入口：在楼梯中段两侧生成临时宽度把手；重复调用只归位。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Stairs|Resize")
	void EnterResizeMode();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Stairs|Resize")
	void ExitResizeMode();

	UFUNCTION(BlueprintPure, Category = "CS Stairs|Resize")
	bool IsInResizeMode() const;

	UFUNCTION(BlueprintPure, Category = "CS Stairs|Resize")
	TArray<ACSStairsWidthHandleActor*> GetResizeHandles() const;

	/** 设置整体宽度（45–400 cm），立即重建踏步、支撑、栏杆及露台接口；返回生效宽度。 */
	UFUNCTION(BlueprintCallable, Category = "CS Stairs|Resize")
	float SetStairWidth(float NewWidth);

	/** 使用 BuildRuns 的已解析路径，保持接地、露台吸附和逐点缩宽的同一口径。 */
	bool GetWidthHandleFrame(FVector& Center, FVector& Right, float& WidthScale) const;
	void SnapResizeHandles();
	void NotifyResizeHandleDestroyed(ACSStairsWidthHandleActor* Handle);

	/** 只读查询，同一份输入同时供楼梯落点与房屋围边入口使用。 */
	void GetTerraceConnections(TArray<FCSStairsTerraceLink>& OutLinks) const;
	void FlushPendingReevaluate() const;

	UFUNCTION(BlueprintPure, Category = "CS Stairs|Connection")
	int32 GetTerraceConnectionCount() const;

	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	USplineComponent* GetSpline() const { return Spline; }

	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	UCSGpuInstancedMeshComponent* GetBrickComponent() const { return BrickComponent; }

	/** 上一次重建出了几级踏步。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetStepCount() const { FlushPendingReevaluate(); return CurrentStepCount; }

	/** 上一次重建出了几块砖（一级可以横向切成几块）。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetBrickCount() const { FlushPendingReevaluate(); return CurrentBricks.Num(); }

	/** 上一次重建真的上传了几次（砖表哈希短路的观测量，单测用）。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetUploadCount() const { FlushPendingReevaluate(); return UploadCount; }

	/** 上一次重建识别出的木梯分段数。 */
	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetLadderRunCount() const { FlushPendingReevaluate(); return CurrentLadderRunCount; }

	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetRailingPieceCount() const { FlushPendingReevaluate(); return CurrentRailingCount; }

	UFUNCTION(BlueprintPure, Category = "CS Stairs")
	int32 GetLadderRungCount() const { FlushPendingReevaluate(); return CurrentLadderRungCount; }

	/** 上一次重建的砖表（世界空间）。脚本与单测读它对答案，不必回读 GPU。 */
	const TArray<CSStairs::FBrick>& GetBricks() const { FlushPendingReevaluate(); return CurrentBricks; }

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
	virtual void PostRegisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

protected:
	virtual void GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const override;

private:
	/** UI 状态不存盘、不复制，也不随宽度撤销复活已删除的临时把手。 */
	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	TArray<TObjectPtr<ACSStairsWidthHandleActor>> ResizeHandles;

	/** 楼梯的路径 —— **默认子对象**，放进关卡就带着一条三点的上行样条。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Stairs", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplineComponent> Spline;

	/** 踏步砖的实例组件（CPU 数组路；楼梯是几十到几百块的量级）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Stairs", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSGpuInstancedMeshComponent> BrickComponent;

	UPROPERTY(VisibleAnywhere, Category = "CS Stairs|Railing")
	TObjectPtr<UCSGpuInstancedMeshComponent> RailingComponent;

	UPROPERTY(VisibleAnywhere, Category = "CS Stairs|Railing")
	TObjectPtr<UCSGpuInstancedMeshComponent> LadderComponent;

	uint32 RailingHash = 0;
	uint32 LadderHash = 0;
	int32 CurrentRailingCount = 0;
	int32 CurrentLadderRungCount = 0;

	/** 订阅着的地面。Transient：每次重建按需重新找。 */
	UPROPERTY(Transient)
	TObjectPtr<ACSGroundActor> Ground;

	FDelegateHandle GroundChangedHandle;

	TArray<CSStairs::FBrick> CurrentBricks;
	int32 CurrentStepCount = 0;
	int32 CurrentLadderRunCount = 0;
	uint32 BrickHash = 0;
	int32 UploadCount = 0;
	uint32 ConnectionHash = 0;
	bool bReevaluatePending = false;
	bool bInRebuild = false;
	bool bConnectionsDisabled = false;
	FDelegateHandle TerraceChangedHandle;
	FDelegateHandle RootTransformChangedHandle;
	TWeakObjectPtr<USplineComponent> ObservedSpline;
	FDelegateHandle SplineChangedHandle;
	FDelegateHandle SplineUpdatedHandle;
	FDelegateHandle SplineTransformChangedHandle;
#if WITH_EDITOR
	/** 非实时/被节流的视口不跑 actor tick，使用仅在有变化时登记的一次性编辑器刷新。 */
	FTSTicker::FDelegateHandle EditorRebuildTicker;
#endif
	void RequestReevaluate();
	void SubscribeChanges();
	void UnsubscribeChanges();
	void UnsubscribeSplineChanges();
	void NotifyTerraces() const;
	void HandleTerraceChanged(ACSHouseActor* House);
	void HandleComponentTransformChanged(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);

	void ResolveGroundAndSubscribe();
	void UnsubscribeGround();
	void HandleGroundChanged(ACSGroundActor* ChangedGround, const FBox& ChangedBounds);
	CSStairs::FGroundSampler MakeGroundSampler() const;
};
