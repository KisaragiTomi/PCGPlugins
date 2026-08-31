#pragma once

#include "CoreMinimal.h"
#include "CSTinyGlade.h"
#include "CSGroundShaperActor.generated.h"

class ACSGroundActor;
class UBillboardComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * 地形塑形物：放在地面上的**不可见高度影响体**（计划 D9；高度不是笔刷画的）。
 *
 * 形状口径对齐用户的 Houdini 原型 `TinyGlade.hip` `/obj/geo1/DeformSource`，但**不逐节点搬**：
 * 那条链（circle -> deformbyinfluence -> line/resample/copytopoints/ray/blast/copytopoints）
 * 在 UE 侧压成一件事 —— 一个解析高度场。
 *   · 高度场：`Radius` 盘内恒为台高（原型里 circle 是 poly 面，`xyzdist` 面内为 0），
 *     盘外按 `FalloffDistance` smoothstep 羽化到 0（原型是 bspline lerp 斜坡），再叠上
 *     裙边噪声（只减不加的侵蚀）与二次抬升。公式的**唯一权威**是
 *     `Public/CSGroundShaperField.h` <-> `Shaders/Private/CSGroundShaperField.ush` 那一对。
 *
 * **石阶不在这里**（2026-08-30 裁决一，旧路已删）：塑形物曾经自持一条
 * 「CPU 解等高线 -> B 样条 -> 沿曲线铺块」的石阶路（`RebuildSteps` / `BuildStepPlan` /
 * `CSShaperSteps::EnsureCapacity` / `CSGroundSteps.usf`）。它已经整条删掉，石阶现在**只有一条**：
 * 地面自己的 `ACSGroundActor::RebuildStairs`（marching squares 等值线 + 定容 +
 * `InterlockedAdd`，见 `CSGroundStairs.usf`）。这里因此不再有任何 palette、实例组件或
 * GPU 缓冲 —— 塑形物只负责高度场。
 *
 * **产物全在 GPU 线程上生成**：地形位移是 `UCSMeshOps::DisplaceGroundShapers` 一个 compute
 * pass（区域更新，不重传整张地面）；CPU 只算一份打包参数，权威数据仍在地面镜像。
 *
 * **归属**（用户提问的裁决）：切割生成的岩石归本 actor 持有。生命周期是纯从属关系
 * （删掉塑形物 = 地形塌回 + 产物全消失，已经是原子的），没有接缝那种对称归属问题；
 * 独立 actor 只会多出必须与地面刷新配对的销毁路径。因此基类从 `AActor` 换成 `ACSTinyGlade`：
 * 基类的 `UCSMesh` 槽位留给将来切割出来的岩石（空实例零 GPU 分配、无 scene proxy，不花钱）。
 *
 * 通知（计划 D9 裁决）：塑形物**只与地面对话**（登记/注销/变更），由地面重导出 `Mirror.Heights`
 * 后广播。⚠️ 反向订阅 `OnGroundChanged` 也随旧路一起删了 —— 它当初只为"别人画路时本座重摆石阶"
 * 而存在，而石阶早已归地面自己管（地面在同一条 `FlushPaintToGpu` 尾巴上重扫）。
 *
 * 可视化：自身在游戏模式下不可见 —— 编辑器里用圆柱 + billboard 示意，两者都是
 * `bIsEditorOnly` + `HiddenInGame`；（将来的）岩石才是真实产物。
 */
UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API ACSGroundShaperActor : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	ACSGroundShaperActor();

	// -------------------------------------------------------------------------
	// Shape（对应原型 DeformSource + deformbyinfluence 的三个参数）
	// -------------------------------------------------------------------------

	/** 台顶半径 cm。盘内高度恒为台高（不做二次衰减），对应 circle SOP 的 scale。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper", meta = (ClampMin = "1.0"))
	float Radius = 150.0f;

	/**
	 * 台高 cm（相对本 actor 所在高度），对应 deformbyinfluence 的 scale。
	 *
	 * ⚠️ **契约是「抬升幅度」，不是「台顶高」**（用户裁决 2026-08-30，照原型留 2% 溢出）：
	 * 二次抬升按原型口径照抬台顶，所以台顶实际 = `LiftHeight × (1 + SecondaryLiftScale)`
	 * （默认 300 → **306.3**）。**这不是缺陷，别再当 bug 报，也别"顺手"归一化掉。**
	 * 想让本值严格等于台顶高，把 `SecondaryLiftScale` 设 0 —— 那是配置，不是改语义。
	 *
	 * 连带纪律：**任何断言都不许拿绝对台高当期望值** —— 要么和 `SampleHeight` 比、
	 * 要么写成表达式。演示回归里那 5 条已经这么改了，那才是守得住的不变量。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper", meta = (ClampMin = "0.0"))
	float LiftHeight = 300.0f;

	/** 羽化裙边宽 cm：盘外这段距离内从台高降到 0，对应 deformbyinfluence 的 maxdist。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper", meta = (ClampMin = "1.0"))
	float FalloffDistance = 200.0f;

	// -------------------------------------------------------------------------
	// 裙边噪声（原型 noisebysourcestress）与二次抬升（原型 attribwrangle5）
	// 公式与两条必须保住的性质写在 Shaders/Private/CSGroundShaperField.ush 里，不在这里重复。
	// -------------------------------------------------------------------------

	/**
	 * 裙边侵蚀幅度，**以台高为单位**（0 = 关掉整条噪声；原型的 turbnoise 幅度就活在这个
	 * 归一化域里，所以这里不写 cm —— 大土台自动得到成比例的侵蚀）。
	 *
	 * 默认 0.5 不是随手取的：它是"两座相接土台的折痕被打碎"这条验收（计划裁决六）能量到
	 * 的最小档。半径 300 / 羽化 400 / 台高 300、中心相距 1200 的两座实测：折痕线的横向游走
	 * 从 0.00 cm（噪声关时折痕是一条**精确**的中垂线）涨到 σ≈14 cm、峰值 46 cm，横向曲率的
	 * 变异系数从 0.11 涨到 0.46。0.35 只到 σ≈10 cm、变异系数 0.34，肉眼仍读得出一条直线。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SkirtNoiseAmount = 0.5f;

	/**
	 * 噪声底层格的波长 cm。默认 300 与地面 `CellSize`（50）的比是 6 —— 再细地面网格就采不住了，
	 * 高度场上明明有的起伏在画面里会退化成锯齿（石阶读的是解析场，不受这条限制，会先于地面
	 * 显出细节，看着像"石阶和地面对不上"）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "1.0"))
	float SkirtNoiseWavelength = 300.0f;

	/** 噪声种子。噪声域是**本座局部坐标**，所以两座塑形物即使同种子也拿到不同实现；这条只是重掷用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "0"))
	int32 SkirtNoiseSeed = 0;

	/**
	 * 二次抬升系数（原型 `attribwrangle5` 的 scale = 0.021）：`+ pow(S, 1.5) · 台高 · 本值`，
	 * 偏向台顶的纯观感微调。
	 *
	 * ⚠️ 它按原型口径**照抬台顶**，所以台顶实际高度是 `LiftHeight × (1 + 本值)`，不再等于
	 * `LiftHeight`。要让 `LiftHeight` 严格等于台顶高度就把它设成 0。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float SecondaryLiftScale = 0.021f;

	// -------------------------------------------------------------------------
	// Wiring / 可视化
	// -------------------------------------------------------------------------

	/** 目标地面。留空则自动找场景里第一个 ACSGroundActor。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper")
	TObjectPtr<ACSGroundActor> Ground;

	/** 编辑器示意体（默认引擎 Cylinder），只在编辑器里画，游戏模式不可见。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Editor")
	TObjectPtr<UStaticMesh> EditorShapeMesh;

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/** 全量：让地面按塑形物重导出高度（区域更新 + 广播）。参数改动/移动都走这条。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Shaper")
	void RebuildTerrain();

	/** 声明式入口（基类语义）：等价于 RebuildTerrain，幂等。 */
	virtual void ReevaluateSite() override;

	/**
	 * 本塑形物对某点的高度贡献（世界 XY → 相对地面基面的抬升，恒 ≥ 0）。
	 * 地面重导出的纯函数输入：多塑形物重叠取 max（计划 D9）。
	 */
	float SampleShapeHeight(const FVector2D& WorldXY) const;

	/**
	 * 高度场的 GPU 参数（`CSGroundShaperField::Float4sPerShaper` 个 float4，布局见该头文件）。
	 * compute pass 与 CPU 镜像**共用这一份打包结果**：`SampleShapeHeight` 也是先打包再求值，
	 * 所以两侧连"参数是怎么算出来的"都不会分叉，只剩公式本身要人盯。
	 */
	void GetHeightFieldParams(FVector4f& OutProfile, FVector4f& OutTop, FVector4f& OutNoise) const;

	/** 影响范围的世界 XY 矩形（= 盘 + 裙边），也是区域重导出发布的脏区。 */
	FBox2D GetFootprintRect2D() const;

	//~ AActor interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	/** 解析地面指针 + 向地面登记自己（幂等）。**不订阅** `OnGroundChanged` —— 见类注释。 */
	void ResolveGroundAndRegister();

	/** 向地面注销（销毁路径）。bRefreshGround 时让地面把自己的隆起抹掉。 */
	void UnregisterFromGround(bool bRefreshGround);

	/** 按 Radius/LiftHeight 摆正编辑器示意圆柱（底面贴 z=0，直径 = 2×Radius）。 */
	void UpdateEditorShape();

	/** 台顶相对地面基面的高度（= 自身 Z 偏移 + LiftHeight，钳到 ≥ 0）。 */
	double ComputeTopHeight() const;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> EditorShapeComponent;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> SpriteComponent;
#endif

	/** 上次已生效的足迹：移动/改参时要把 union(旧, 新) 交给地面，才能把旧位置的隆起抹掉。 */
	FBox2D LastAppliedFootprint = FBox2D(ForceInit);
};
