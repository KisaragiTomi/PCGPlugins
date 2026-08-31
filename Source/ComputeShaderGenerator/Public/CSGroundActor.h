#pragma once

#include "CoreMinimal.h"
#include "CSGroundDecor.h"        // 裙边摆件（D12 第五家）：FSite / FShaperRing，规划段共用 CSHouseDecor
#include "CSGroundRockShell.h"
#include "CSGroundStairs.h"
#include "CSMeshOps.h"
#include "CSTinyGlade.h"
#include "CSGroundActor.generated.h"

class ACSGroundActor;
class ACSGroundShaperActor;
class UCSGpuInstancedMeshComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
class UStaticMesh;

DECLARE_MULTICAST_DELEGATE_OneParam(FCSGroundPaintEditorRequest, ACSGroundActor*);
DECLARE_MULTICAST_DELEGATE_TwoParams(FCSGroundChanged, ACSGroundActor*, const FBox& /*ChangedWorldBounds*/);

/**
 * 地面的 CPU 权威镜像。高度 + 顶点色随关卡序列化；GPU 网格（UCSMesh）只是它的投影，
 * 加载/改参后由 RebuildGroundMesh() 全量重建，交互期间由笔刷做"镜像 + GPU"双写。
 *
 * 所有 gameplay 查询（道路权重、地面高度、拾取）只打镜像，永不回读 GPU —— 这是本 actor
 * 的第一纪律：任何新的写入路径必须双写，否则查询与画面分叉。
 *
 * 顶点布局约定：Id = y * NumVertsX + x（行主序），笔刷 pass 与镜像换算共同依赖。
 */
USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSGroundMirror
{
	GENERATED_BODY()

	UPROPERTY() int32 NumVertsX = 0;      // 顶点数 = 格数 + 1
	UPROPERTY() int32 NumVertsY = 0;
	UPROPERTY() float CellSize = 50.0f;   // cm/格，镜像自带一份防止 actor 配置改了老数据错配

	UPROPERTY() TArray<float> Heights;    // 相对 actor 的局部高度，初始全 0 = 平地
	UPROPERTY() TArray<FColor> Colors;    // 权威顶点色；R 通道 = 道路权重（见计划文档 D6）

	bool IsInitialized() const { return NumVertsX > 1 && NumVertsY > 1 && Heights.Num() == NumVertsX * NumVertsY && Colors.Num() == Heights.Num(); }
	int32 VertexIndex(int32 X, int32 Y) const { return Y * NumVertsX + X; }
};

/**
 * gpumesh 地面：一块规则网格 UCSMesh + CPU 权威镜像（FCSGroundMirror），不用 Landscape。
 * TinyGladeHouse_Plan.md 的 D1/D2。网格底座（渲染组件 / UCSMesh / 快照上传）在基类
 * ACSTinyGlade；地面是权威数据源，不消费世界变化，因此不 override ReevaluateSite()。
 *
 * 交互入口沿用点笔刷的启动模式：StartVertexColorPaint()（CallInEditor 按钮）广播静态
 * 委托，PCGEditorProcess 应答并激活 FCSGroundPaintEdMode；EdMode 的光标 trace 走
 * RaycastGround —— GPU 网格全线 NoCollision，引擎 line trace 打不到它。
 *
 * 变换约定：仅支持平移。常驻流是世界空间，旋转/缩放会让镜像查询（世界 XY → 格点）失效，
 * 编辑器拖动期间用 TranslateMesh 增量平移，松手后全量重建对齐。
 */
UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API ACSGroundActor : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	/** Raised by StartVertexColorPaint(); the editor module answers it by activating the paint EdMode. */
	static FCSGroundPaintEditorRequest OnGroundPaintEditorRequest;

	/**
	 * 地面变动的直推通知（v1 架构裁决：不用 CSSceneDirty3D，任何变动必然广播，消费者
	 * 无条件重求值、靠幂等哈希兜底）。笔刷每次落笔、全量重建、编辑器拖动平移都会触发。
	 *
	 * 参数带本次变化的世界盒；v1 消费者可以无视它（全员重算就是正确语义），它是将来
	 * 切回 dirty 系统时的过滤接缝。注意：广播只覆盖"之后的变化"——消费者注册/加载时
	 * 必须自己先主动重求值一次，别指望赶上地面加载重建的那一次广播（注册顺序无保证）。
	 */
	FCSGroundChanged OnGroundChanged;

	// -------------------------------------------------------------------------
	// Ground Settings
	// -------------------------------------------------------------------------

	/** X 方向格数。顶点数是格数 + 1；改动会重置镜像（高度/顶点色清空重来）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Ground", meta = (ClampMin = "1", ClampMax = "1024"))
	int32 NumCellsX = 64;

	/** Y 方向格数。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Ground", meta = (ClampMin = "1", ClampMax = "1024"))
	int32 NumCellsY = 64;

	/** 格尺寸 cm。改动会重置镜像。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Ground", meta = (ClampMin = "1.0"))
	float CellSize = 50.0f;

	/** UV0 的世界平铺周期 cm：UV = 局部坐标 / 周期，材质密度不随地面尺寸变。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground", meta = (ClampMin = "1.0"))
	float UVWorldPeriod = 500.0f;

	/** 整片地面的材质（顶点色混合：基底与道路层按 R 通道混）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground")
	TObjectPtr<UMaterialInterface> GroundMaterial;

	/**
	 * 镜像重置时铺的底色。R 分量同时是"无道路"的语义零点。
	 *
	 * **改它不会自动重刷已有顶点色** —— 权威是 Mirror.Colors，快照只搬镜像、不读本字段，
	 * 所以早先"改底色就全量重建"是一次纯空转（13 万三角重传 + 全场唤醒，画面零变化）。
	 * 要把新底色铺下去请点 ResetPaint()。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground")
	FLinearColor BaseColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);

	// -------------------------------------------------------------------------
	// Brush Settings（EdMode 每次落笔读这里，和点笔刷一样归目标 actor 所有）
	// -------------------------------------------------------------------------

	/** 笔刷球半径 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush", meta = (ClampMin = "1.0"))
	float BrushRadius = 300.0f;

	/** 衰减占半径的比例：0 硬边到底，1 从球心开始衰减。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BrushFalloff = 0.5f;

	/** 整笔强度缩放。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BrushStrength = 1.0f;

	/** 笔刷颜色。默认画 R = 道路权重。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	FLinearColor PaintColor = FLinearColor(1.0f, 0.0f, 0.0f, 1.0f);

	/** 通道门：> 0 的通道才被笔刷影响。默认只动 R。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	FLinearColor PaintChannelMask = FLinearColor(1.0f, 0.0f, 0.0f, 0.0f);

	/** 混合公式见 ECSMeshPaintBlendOp 注释；擦除道路把它切到 Erase。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	ECSMeshPaintBlendOp PaintBlendOp = ECSMeshPaintBlendOp::Replace;

	/** Leave the brush mode after each committed stroke. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	bool bExitAfterCommit = false;

	// -------------------------------------------------------------------------
	// Stairs（计划「石阶改造：100% GPU 决策 + 零回读」的 S1/S2/S3）
	//
	// **为什么石阶归地面而不归塑形物**：扫描域是全局的 —— marching squares 扫的是全部塑形物
	// 合成之后的高度场，格不与任何一座对齐，跨在两座接合处的那一格照常出等值线。归任一座
	// 都不对，也正是这一点让"两座相接土台的接合处石阶断掉"那条缺陷从根上消失。
	//
	// 曾经并行的旧路（塑形物自持的 `RebuildSteps` + `CSShaperSteps`）已随 2026-08-30
	// 「裁决一」第二步整条删除（S3）⇒ **这里是全项目唯一的一条石阶路**。
	// -------------------------------------------------------------------------

	/** 石阶基础网格。**留空 = 整条石阶路径关闭**（不建组件、不分配显存、不发 dispatch）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	TObjectPtr<UStaticMesh> StairMesh;

	/** 石阶材质。GPU 实例化只有一个材质槽；必须勾 "Used with Instanced Static Meshes"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	TObjectPtr<UMaterialInterface> StairMaterial;

	/**
	 * 等值线扫描格的边长 cm。
	 *
	 * ⚠️ **它直接决定石阶间距**：marching squares 每格每层出一级，间距完全由格密度决定 ——
	 * 太密石阶互相穿模，太疏断断续续。所以它必须 ≈ `StairBlockSize.Y`（石阶长度），
	 * 而**不能**复用地面的 `CellSize`（50 cm，会密一倍）。这也是 Tiny Glade 把 contouring
	 * 格单开一个 `contouring_grid_dims`、而不是复用 heightmap 分辨率的原因。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "10.0"))
	float StairCellSize = 100.0f;

	/** 每级台阶的升高 cm。等距分层（不学 TG 的非等距 mix(0.8, 10, i/16)）：石阶语义要求等高。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "1.0"))
	float StairStepHeight = 30.0f;

	/** 道路权重阈值：格心的 R 通道过阈才算"路经过这里"。与岩壳的隐藏阈值共用同一个数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StairRoadThreshold = 0.35f;

	/**
	 * 沿坡**向上**推进 cm，让踏面扎进坡里（原型 attribwrangle7 的 Noffset）。负值则向外挑出。
	 *
	 * ⚠️ **默认值不是调出来的，是闭式解：`StairEmbed = StairBlockSize.X / 2`。**
	 *
	 * 摆放规则是"块**心**落在等值点、块底钉在该层高度上"，于是块的**下坡底棱**悬在空中，
	 * 悬空量 `f = (X/2 − e) · 坡度`。把 e 取成 X/2 时下坡底棱正好落在自己那条等值线上 ——
	 * **`f ≡ 0` 与坡度无关**，等值线因此变成踏面的前缘（踏步鼻），这正是"台阶"该读出来的样子。
	 *
	 * 两次踩过的坑都是**拿一个与坡度相关的数当默认值**：
	 *   · 继承来的 25：默认剖面（台高 300 / 羽化 400，坡度 1.125）下把 30 cm 踏步整个埋掉，
	 *     裙边中段只露 3 cm。
	 *   · S1 那轮改的 10：踏面是露出来了，但 `f = +26.8 cm`（岩壳把台高抬到 700 之后，
	 *     坡度 1.342）—— 每块石阶的前缘都吊在半空，整条读成一道**支棱着的石墙**而不是台阶。
	 * 两个数都会在剖面一改就重新出错，X/2 不会。
	 *
	 * 与之配套、同样与坡度无关的两条不变量（改 e 时必须一起复核）：
	 *   · 可见踏面 `T = min(StepHeight, StairBlockSize.Z) / 坡度` —— 取 X/2 时上界恰好由
	 *     `Z ≥ StepHeight` 保证（本文件下面那条"Z 略大于 StairStepHeight"就是它）。
	 *   · 可见踏步高恒 = `StairStepHeight`（相邻两块的顶面差），**与 e、与坡度都无关**。
	 *
	 * ⚠️ 剩下的一条**没法用 e 修**：踏步高 / 踏面 = 坡度本身（22.4 cm 踏面配 30 cm 踏步，
	 * 坡度 1.342）。石阶贴着地形走，它的坡度就是地形坡度，`StepHeight` 只能同时缩放两者。
	 * 想要"能走"的比例只能改地形剖面或让路径斜切上坡 —— 那是塑形物/路径的事，不是本参数的。
	 *
	 * 逐实例进深抖动（`StairSizeJitter`，±12%）会让 `f` 在 `±0.12 · X/2 · 坡度`（约 ±4.8 cm）
	 * 上下浮动 —— 量级已经落到裙边噪声之下，读起来就是石头本该有的参差，不是悬空。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	float StairEmbed = 30.0f;

	/** 相对该层高度的竖直微调 cm（块底默认贴在等值线上，见 FScanParams::Rise）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	float StairZOffset = 0.0f;

	/**
	 * 想要的石阶三轴尺寸 cm：X = 踏面进深、Y = 沿等值线的长度、Z = 高。
	 * 内部除以基础网格自身的包围盒尺寸得到缩放 —— 喂居中单位立方体（`stairs_step` 是 100³
	 * 的居中盒）时这里填的就是最终尺寸；某一轴填 0 表示"这一轴保持网格自带的尺寸"。
	 *
	 * 两条尺寸关系（不满足时画面会露馅，但一个断言都不会红）：
	 *   · Y ≈ `StairCellSize`。⚠️ **S2 起 Y 只是下限**：渲染长度跟着本格的等值线**弦长**走
	 *     （`max(弦长, 本值) × StairLengthBloat × (1 + 单侧拖动)`）—— 弦长在 [格距, √2×格距]
	 *     上变，定长块在斜走的等值线上会露正缝。详见 `StairLengthBloat` 的注释。
	 *   · Z **略大于** `StairStepHeight` —— 地面是光滑斜坡不是真台阶，块底钉在等值线上时，
	 *     踏面前缘一定悬空（悬空量 = 半个进深 × 坡度）。Z 比层高大一截，相邻两级在竖直方向
	 *     重叠，下一级正好挡住上一级的悬空前缘，整条读起来才是连续的石阶而不是一排浮块。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	FVector StairBlockSize = FVector(60.0, 100.0, 45.0);

	/** 单格最多跨几层。`LiftHeight` 越大层数越线性膨胀，没有这个钳位一格就能吃光固定容量。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "1", ClampMax = "256"))
	int32 StairMaxLayersPerCell = 32;

	/**
	 * 石阶实例的**固定**容量。零回读的代价与保证：CPU 不知道实际有几级，所以容量一次性定死、
	 * 永不重算，越界静默丢弃。改它会重新分配（一次阻塞），所以它是配置项、不是运行期的量。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "64", ClampMax = "1048576"))
	int32 MaxStairInstances = 4096;

	// -------------------------------------------------------------------------
	// 小石子（TG `_rocky_terrain_stairs_stairs.cs:511-547`）
	//
	// TG 的石阶 CS 每摆一级台阶，都有 15% 的概率在同一段等值线上再撒一颗小石子。它是"石阶不像
	// 一排整齐积木"的主要来源之一，成本只有一次哈希 —— 但它必须与 S2 的抖动**共用格身份哈希**，
	// 不能用 InterlockedAdd 的槽位（理由见 `StairJitterSeed` 那一节，S1 已经栽过一次）。
	//
	// 石子与石阶是两张基础网格，而一个实例组件只绑一张 ⇒ 走**自己的**组件与实例源。
	// -------------------------------------------------------------------------

	/**
	 * 小石子的基础网格（提取资产里现成的 `stairs_pebble`）。
	 * **留空 = 只关掉石子这一支**，石阶照常（与 `StairMesh` 留空关掉整条路径不是一回事）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble")
	TObjectPtr<UStaticMesh> StairPebbleMesh;

	/** 石子材质。同石阶：GPU 实例化只有一个材质槽，必须勾 "Used with Instanced Static Meshes"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble")
	TObjectPtr<UMaterialInterface> StairPebbleMaterial;

	/** 每段等值线额外出一颗石子的概率。0.15 是 TG 反编译实测（`:522` 的 `> 0.85`）。0 = 关掉。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StairPebbleChance = 0.15f;

	/**
	 * 石子最长轴的目标尺寸 cm，`X = 下限 / Y = 上限`（均匀缩放，在这个区间里随机取）。
	 *
	 * 默认值是 TG 那条 `mix(0.2, 0.4)` 的 cm 换算，不是调出来的：TG 的均匀缩放乘在
	 * `stairs_pebble` 原件上，而原件实测最长轴 1.352 m ⇒ **27 – 54 cm**。
	 * 这里写 cm 而不是写 0.2/0.4，是为了与 `StairBlockSize` 同一口径 ——
	 * 缩放系数在内部由本值除以基础网格自身的包围盒算出来，将来谁把资产按别的比例重导，
	 * 石子的**世界尺寸不会跟着悄悄变**（写系数就会）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble")
	FVector2D StairPebbleSize = FVector2D(27.0, 54.0);

	// -------------------------------------------------------------------------
	// S2：逐实例抖动
	//
	// **随机源是格身份（格坐标 + 层号 + 段号 + 种子），不是 InterlockedAdd 拿到的槽位。**
	// 槽位由线程组完成顺序决定，同一份世界状态两次 dispatch 可以给同一块石阶不同的槽；
	// 而这条路上重扫是**每一 dab 一次**的，用槽位当种子的症状就是"画一笔路，整片石阶乱跳"。
	// （S1 写 packed 行 .w 的 PerInstanceRandom 用的正是槽位，本轮一并订正。）
	//
	// TG 的对位物：`_rocky_terrain_stairs_stairs.cs:504` 的种子是 `uint((Tx+Ty+Tz)*100)`
	// —— 实例平移量之和量化到 cm，同样是位置派生、与槽位无关。改用格身份是因为拖塑形物时
	// 高度场在动、等值线跟着滑，位置派生的种子会在整个拖动过程里不停重掷。
	// -------------------------------------------------------------------------

	/**
	 * 长度轴（沿等值线）的**胀大系数**：渲染长度 = 弦长 × 本值，摆位一步不动 ⇒ 相邻两块互相
	 * 穿插，**砖缝是负的**。小于 1 会被夹回 1 —— 正缝正是它要消掉的缺陷。
	 *
	 * ⚠️ **它和 `ACSHouseActor::FrameBrickBloat` 不是同一件事，别照抄那边的做法**：
	 * 门框砖的槽距由 `SolveBlockLayout` 定成近恒定，所以一个常数系数就能保证负缝；石阶的槽距
	 * 是**几何决定**的 —— 每格每层出一级，等值线与格成 45° 时弦长 = √2 × 格距，比轴向大 41%。
	 * 定长块（S1 的做法）在斜走的等值线上会露出 0.41 × 格距 的**正缝**，任何常数系数都盖不住
	 * 一个随走向变化 √2 倍的量。所以长度轴必须**跟着弦长走**（`StairBlockSize.Y` 退化成下限），
	 * 本系数只负责把"恰好首尾相接"推成"确定互相穿插"。
	 *
	 * TG 的实证是 k ≡ 1（`_rocky_terrain_stairs_stairs.cs:509` 把 X 轴缩放直接写成
	 * `distance(A, B)`）—— 它靠石头自身的不规则轮廓 + VS 的 bevel 噪声藏接缝；我们用的是
	 * 光板方盒，零缝会读成一条折痕，所以叠一个常数胀大。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "1.0", ClampMax = "1.5"))
	float StairLengthBloat = 1.06f;

	/**
	 * 长度轴的抖动幅度（比例），**单侧：只增不减**。
	 *
	 * 单侧是硬要求不是口味：负缝的保证是"每块的长度 ≥ 自己那条弦"，对称抖动会让两块同时缩、
	 * 把接缝重新拉开 —— 与门框砖"只胀有邻居的那一轴、且只增不减"是同一条纪律。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StairLengthJitter = 0.10f;

	/**
	 * 进深（+X）与高（+Z）的**对称**抖动幅度（比例）。这两轴沿等值线方向都没有邻居，所以敢双向抖。
	 *
	 * ⚠️ 与 `StairStepHeight` 有一条不成文的关系：`StairBlockSize.Z × (1 − 本值)` 必须仍然
	 * **大于** `StairStepHeight`，否则抖矮的那些块不再与下一级竖直重叠，踏面前缘的悬空就露出来
	 * （断言：GroundStairs.StepOverlap）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StairSizeJitter = 0.12f;

	/**
	 * 绕世界 +Z 的偏航抖动**度数**，±。
	 *
	 * 只抖 yaw：踏面必须是水平的（贴着坡面倾斜的是岩壳不是台阶，见 kernel 文件头），
	 * pitch/roll 一抖这条就没了。
	 *
	 * ⚠️ **它会吃掉负缝**：块转过 δ 之后沿等值线方向的投影长度只剩 cos δ 倍，所以负缝的
	 * 充要条件是 `StairLengthBloat × cos(本值) > 1`。默认 1.06 × cos6° = 1.054 ⇒ 每个接头
	 * 仍然互相穿插约 5.4% 弦长。调大本值或调小胀大系数会直接把这条断言打红。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float StairYawJitter = 6.0f;

	/** 抖动种子。同种子同格身份 ⇒ 逐位相同的结果（`RebuildStairs` 幂等的一部分）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter")
	int32 StairJitterSeed = 1;

	// -------------------------------------------------------------------------
	// Rock Shell（计划 D9「侧面碎石：Tiny Glade 式披挂岩壳」的链 B）
	//
	// **裁决二：碎石归地面，不归塑形物**（这推翻了计划 D9 :504 岩石那一半；石阶不变，
	// 仍归塑形物）。碎裂图案覆盖整张地面、与任何单座塑形物无关，mask 由**全部塑形物合成后**
	// 的坡度决定，归任一座都不对。删掉塑形物 → 高度场塌回 → 坡度降到阈下 → 那批胞腔自己
	// 写 NaN，**归属簿记整个消失**，不是变简单。
	//
	// 与石阶严格互补：石阶要路、碎石要没路。但两者实现方式不同（裁决五）——
	// 石阶用 `StairRoadThreshold` 阈值门控，壳**没有任何 road 显隐判据**，只有连续下沉。
	// -------------------------------------------------------------------------

	/**
	 * 整条岩壳路径的总开关。
	 *
	 * ⚠️ **默认开，而且刻意不拿"材质为空"当开关**（`StairMesh` 那条就是这么写的，而两张演示
	 * 关卡里它一直是 NULL，石阶因此在画面里是一撮黑块，单测与回归却全绿）。壳的几何是生成的，
	 * 材质为空只会退回引擎默认表面材质 —— 那仍然是"画出来了"，所以材质不能兼任开关。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	bool bRockShell = true;

	/** 岩壳材质。为空退回引擎默认表面材质（壳照样画，只是灰的）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	TObjectPtr<UMaterialInterface> RockShellMaterial;

	/**
	 * 碎裂图案的来源网格：Tiny Glade 原件 `rocky_terrain_shell.glb` 导进来的那张 StaticMesh
	 * （`Scripts/TinyGladeImportRockShell.py`）。它只当**数据**读（逐顶点胞腔属性在 UV1/UV2），
	 * 从不作为网格被渲染。
	 *
	 * ⚠️ **默认值是硬编码的资产路径而不是空** —— 同 `bRockShell` 那条注释里的教训：留空
	 * 让别人去填，就会像 `StairMesh` 那样在两张演示关卡里一直是空的，而所有断言照绿。
	 * 抽不出数据时 `IsRockShellDrawable` 会给出具体原因（缺资产 / UV 通道不够 / CPU 访问没开）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	TSoftObjectPtr<UStaticMesh> RockShellPatternMesh =
		TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(CSRockShell::DefaultPatternAssetPath));

	/**
	 * 碎裂图案 → 世界的各向同性缩放。**1.0 = 原件的原生口径**：tile 136.5 m、609 个胞腔、
	 * 间距 5.53 m、盖三角等边等效边长 1.25 m —— 与 Tiny Glade 逐字相同的绝对密度。
	 *
	 * 本项目地面 128 m（`NumCells 256 × CellSize 50`）小于 136.5 m，所以 1.0 就是
	 * **原生尺寸铺一张、中心对齐、每边富余 4.25 m**，既不平铺也不缩放：
	 *   · 平铺**不成立** —— 实测 tile 的两侧边界点不一致，不是周期的，接缝会露；
	 *   · 缩放会同时改变胞腔尺寸与三角边长，直接丢掉「与 TG 同绝对密度」这个唯一的观感锚点。
	 * 富余的那圈落在地面外，由 kernel 的域判据关掉（见 `FDisplaceParams::DomainMin/Max`）。
	 *
	 * ⚠️ 5.53 m 的胞腔让计划 D9 的塑形物尺度警告从建议变成**硬要求**：默认
	 * `Radius=150` / `FalloffDistance=200` 的裙边只有 2 m 宽，一块碎石都长不出来。
	 * 演示关卡因此改成 `Radius=600` / `Falloff=800`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.05", ClampMax = "4.0"))
	float RockShellPatternScale = 1.0f;

	/** 坡度软阈的下端：|∇h| 低于它完全没有壳。与 TG 的 `smoothstep(0.75, 1.25, ·)` 同口径。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellSlopeLo = 0.75f;

	/** 坡度软阈的上端。必须 > `RockShellSlopeLo`，否则 smoothstep 退化成 0/0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellSlopeHi = 1.25f;

	/**
	 * 道路权重的沉降增益（TG 实测 10×）。壳在 `road ≥ 1/本值` 处就已经沉到底。
	 *
	 * ⚠️ **这是壳与石阶"严格互补"的实现方式**，而互补不是靠阈值判断做到的（裁决五禁止
	 * 在显隐判据里出现 road）：`1/RoadFade` 必须**小于** `StairRoadThreshold` —— 默认
	 * 1/10 = 0.1 < 0.35，即路刚画到石阶还没长出来时，壳就已经完全埋进土里了。
	 * 单测 `RockShell.Contract` 守着这条不等式（`CSGroundRockShellTests.cpp`）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "1.0"))
	float RockShellRoadFade = 10.0f;

	/**
	 * road 满值时沿地形法线的下沉量 cm（TG 三项合计约 1.6 m）。
	 *
	 * ⚠️ **必须比壳自身的起伏厚**，否则路上还会露出石头尖。它也是"沉下去不是隐藏"这条裁决
	 * 的唯一执行面：用 NaN 关掉的话，画路时三角会一个一个啪地消失（popping）；连续沉降的
	 * 观感才是"石头慢慢埋进土里"。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellRoadSink = 160.0f;

	/**
	 * 逐胞腔沿径向的随机胀缩幅度 cm（**图案空间**；TG 实测 −52..+19.5 cm）。
	 *
	 * ⚠️ 契约的 `DirToCentroid` 实测**指向质心**（与计划注释的符号相反），所以正的位移是
	 * **收缩**。kernel 用对称随机，本值只是幅度、与符号无关。
	 *
	 * 默认 0：它是**定长**径向位移，顶盖内部点离质心只有一两米，几十厘米就足以把点推过质心、
	 * 翻转周围三角（`Docs/TinyGlade/CSRockShellPattern.md` 的坑 1）。kernel 已经乘了一个到质心的半径
	 * 淡入兜底，但这条属于 P2 的观感项，默认仍然不开。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellCellJitter = 0.0f;

	/**
	 * 壳沿地形法线的**厚度** cm：底圈沉本值，顶圈浮 0..本值（逐胞腔随机）。**块感的唯一来源。**
	 *
	 * TG `displace:577` 的 `mix(-0.3, 0.1*mix(0,3,rand(cell)), cell_bby)`，而 `cell_bby`
	 * **实测就是 `bIsTopRim`**（顶圈恒 1 / 底圈恒 0，零例外）—— `CSGroundShaper.md` 读作
	 * 「本胞腔沿法线凹还是凸」已被推翻。所以它不是"有的胞腔沉、有的凸"，而是给整张壳一个
	 * 沿法线的厚度，再由逐胞腔的随机浮高让相邻的盖互相错开。
	 *
	 * ⚠️ **不是可选项**：调到 0，披挂出来的就只是一张贴着地形的毯子，读不成一块块石头
	 * （计划 P3 把这一层列为"让壳读成石头"的那一层，实测重读之后结论不变）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellCellRelief = 30.0f;

	/** 表面 FBM 沿法线的幅度 cm。角点不加噪声（TG `displace:579`），否则共享角会裂开缝。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellNoiseAmount = 6.0f;

	/** 表面 FBM 的波长 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "1.0"))
	float RockShellNoiseWavelength = 150.0f;

	/** 逐胞腔随机与表面噪声的种子。同种子同胞腔 ⇒ 逐位相同的结果（`RebuildRockShell` 幂等的一部分）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	int32 RockShellSeed = 1;

	// -------------------------------------------------------------------------
	// 塑形物裙边摆件（D12 锚点层的第五家）
	//
	// **归地面，不归塑形物**：完整依据（三条，都不是"哪边写起来方便"）写在
	// `CSGroundDecor.h` 的文件头，一句话是「塑形物只提供高度场，地面负责派生几何」——
	// 与石阶、披挂岩壳同一条归属理由，塑形物那一侧因此一行代码都不用加。
	//
	// 锚点取法也在那个头文件里：**解析剖面的等值带上按弧长布点**，
	// 锚点数 = 环周长 / `SkirtDecorSpacing` ⇒ 密度完全由锚点个数决定（D12 的口径），
	// 没有场、没有阈值、没有随机撒点。
	// -------------------------------------------------------------------------

	/** 关掉整条裙边摆件路径（不建组件、不分配显存、不发 dispatch）。留空网格表等价。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	bool bSkirtDecorEnabled = true;

	/** 摆件网格表（一张网格 = 一个 palette = 一个实例化组件）。全空 = 这一家关掉。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	TArray<TObjectPtr<UStaticMesh>> SkirtDecorMeshes;

	/**
	 * 摆件材质。
	 *
	 * ⚠️ **必须勾 `bUsedWithInstancedStaticMeshes`**：没勾的材质在实例路径上会被引擎
	 * **静默换成默认材质**，画面一片灰而所有 readback 断言照绿。`GetSkirtDecorUndrawableReason()`
	 * 把这条做成了显式判据；供给侧是 `Scripts/TinyGladeMakeDecorMaterial.py` 那张
	 * `M_TinyGladeDecor`（房子那四家用的也是它 —— clutter 的颜色全烘在顶点流里，
	 * 一张母材质就够，两张只会在下一次调色时分叉）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	TObjectPtr<UMaterialInterface> SkirtDecorMaterial;

	/** 裙边环上每隔多少 cm 一个锚点（**弧长**，不是角度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "40.0"))
	float SkirtDecorSpacing = 260.0f;

	/** 锚点落在裙边的哪一档：0 = 台顶边缘，1 = 裙边外沿。依据见 `CSHouseDecor::FParams::SkirtBandT`。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SkirtDecorBandT = 0.62f;

	/** 摆件之间的最小间距（球测）。两座塑形物挨得近时靠它挡掉挤在一起的那几件。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0"))
	float SkirtDecorMinSpacing = 140.0f;

	/** 道路权重过阈就不摆 —— 与石阶严格互补（路穿裙边的那一段长的是台阶，摆件让开）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SkirtDecorRoadReject = 0.3f;

	/** 摆件缩放。clutter 网格实测 100–250 cm，原尺寸摆在裙边上比土台还高（同房子那边的 0.5 档）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.05"))
	float SkirtDecorScale = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0", ClampMax = "0.8"))
	float SkirtDecorScaleJitter = 0.18f;

	/**
	 * 逐实例随机的种子。⚠️ 随机源是 **(本座 actor 名的 CRC, 环上第几号)** 的身份哈希，
	 * **不是** `InterlockedAdd` 的槽位、不是塑形物在 `Shapers` 数组里的下标、也不是它的世界坐标
	 * —— 三条为什么都不行，逐条写在 `CSGroundDecor.h` 的文件头（S1 已经在槽位上栽过一次，
	 * 症状是"画一笔路全场变色"且没有任何断言报红）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	int32 SkirtDecorSeed = 11;

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/** Opens the editor-side vertex-colour paint tool for this actor. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground", meta = (DevelopmentOnly))
	void StartVertexColorPaint();

	/** 从镜像全量重建 GPU 网格（加载、改参、需要对齐时）。镜像尺寸与配置不符时先重置镜像。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground")
	void RebuildGroundMesh();

	/**
	 * 塑形物登记（计划 D9）：塑形物在 PostRegisterAllComponents 登记、销毁时注销，两处都会
	 * 触发一次高度重导出。登记幂等 —— 重复调用只是重算一遍，结果相同。
	 */
	void RegisterShaper(ACSGroundShaperActor* Shaper);
	void UnregisterShaper(ACSGroundShaperActor* Shaper);

	/**
	 * 声明式重导出：Mirror.Heights = 基底 0 与全部登记塑形物贡献的 max（计划 D9 "重叠取 max"），
	 * 高度真的变了才重建网格 + 标脏 + 直推广播 —— 幂等，加载期/无效唤醒自然短路。
	 *
	 * v1 是全域重导出（256² 顶点 × 塑形物数，纯 CPU 距离场，微秒级）；将来切区域更新时
	 * 只需把遍历范围换成 union(旧足迹, 新足迹)，公式一行不动。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground")
	void RebuildHeightsFromShapers();

	/**
	 * 区域版重导出：只重算 WorldRectXY 覆盖的格点，GPU 侧走一个 compute pass 原地改常驻流的
	 * Z 与法线（`UCSMeshOps::DisplaceGroundShapers`），**不再重建整张快照、不再整网格重传**。
	 *
	 * 调用方负责给出 union(旧足迹, 新足迹) —— 撤掉旧位置的隆起与压出新位置的隆起是同一趟。
	 * 公式是"基底 0 与全部塑形物取 max"的绝对式，所以区域内重算与全量重算结果逐位相同。
	 */
	void RefreshHeightsInRegion(const FBox2D& WorldRectXY);

	/** 塑形物高度场的 GPU 参数（每座 2 个 float4），CPU 镜像与 compute pass 共用同一份构造。 */
	void BuildShaperGpuParams(TArray<FVector4f>& OutParams) const;

	/**
	 * 重扫一遍石阶（计划 S1）：一个 dispatch，CPU 侧只递交"扫哪块矩形、格多大、阈值多少"。
	 *
	 * 幂等且便宜到不值得做哈希短路 —— 整条链就是一次 dispatch，没有"短路点落在昂贵计算之后"
	 * 那种问题（已删的塑形物石阶旧路记在案的缺陷 M4）。`StairMesh` 为空时零成本返回。
	 *
	 * 调用时机的硬约束：它读的是**地面网格 GPU 色流**里的道路权重，而落笔只写镜像 + 排队，
	 * 所以必须排在 `FlushPaintToGpu` 之后。渲染命令 FIFO 保证了顺序，但漏调的症状是
	 * "石阶比路慢一笔"。本类内部已经在每条会改变高度或道路的路径末尾调过它。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Stairs")
	void RebuildStairs();

	/**
	 * **诊断 / 自动化测试专用**：阻塞回读石阶实例（数量 + 世界空间原点）。
	 *
	 * 运行路径一个字节都不回读；这个函数存在的唯一理由是让"接合处不断裂"这条 S1 核心验收项
	 * 可断言 —— 摆位判定全在 GPU 上，除了把结果读回来没有别的办法证明它。别在每帧路径上调。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Stairs", meta = (DevelopmentOnly))
	int32 DebugReadStairsSync(TArray<FVector>& OutWorldOrigins);

	/**
	 * **自动化测试专用**：连 packed 行一起回读（5 行 / 实例，**组件空间**）。
	 * S2 的抖动只体现在被缩放过的基上，不看基就断言不了"同一格同一层恒等"。
	 * 不是 UFUNCTION —— 回读是阻塞的，不该出现在任何脚本/蓝图路径上。
	 */
	int32 DebugReadStairRowsSync(TArray<FVector4f>& OutRows);

	/**
	 * **诊断 / 自动化测试专用**：阻塞回读小石子（数量 + 世界空间原点）。
	 *
	 * 石子和石阶一样，摆位判定全在 GPU 上 —— "15% 这一支真的在出东西、而且是按格身份出的"
	 * 除了回读没有别的证明办法。别在每帧路径上调。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Stairs", meta = (DevelopmentOnly))
	int32 DebugReadStairPebblesSync(TArray<FVector>& OutWorldOrigins);

	/**
	 * **自动化测试专用**：石子的 packed 行（5 行 / 实例，**组件空间**）。
	 * "石子的随机源是格身份不是槽位"这条纪律只能靠**复算哈希**来证明，而哈希落在被缩放过的基
	 * 与第 4 行的 `.w` 上 —— 只看数量/位置的断言在槽位恰好稳定时会静默通过。
	 */
	int32 DebugReadStairPebbleRowsSync(TArray<FVector4f>& OutRows);

	// -------------------------------------------------------------------------
	// GPU 侧真值（**诊断 / 验收专用，阻塞**）
	//
	// ⚠️ **绝对不许进交互路径**：回读就是阻塞，且每一次都被 `UCSMesh::GetBlockingFlushCount()`
	// 数到 —— 十一条 `flushes=0` 断言会在误用的那一刻报红，那是有意的警戒线。
	//
	// 与上面那几条 `DebugRead*Sync` 的分工（**不是重复**）：那几条读的是**生产者自持**的
	// 那批 buffer，证明"散布 pass 写出了什么"；这几条读的是**组件手上那一份**，也就是剔除
	// pass 与 indirect draw 真正消费的那个计数器。门框砖那个既有 bug 的现场恰恰是两者不一致：
	// 生产者以为清干净了，组件却被重新交接回同一批带着陈旧计数器的 buffer。
	// -------------------------------------------------------------------------

	/** 石阶在 GPU 上的实例计数器（组件手上那一份）。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadStairCountGpuSync() const;

	/** 小石子在 GPU 上的实例计数器（组件手上那一份）。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadStairPebbleCountGpuSync() const;

	/**
	 * 岩壳这一帧的 `DrawIndexedIndirect` 会消费多少个索引 —— 即 GPU 上那份间接绘制参数的
	 * `IndexCountPerInstance`，不是 CPU 侧的三角数。
	 *
	 * 岩壳走的是网格路不是实例路（一张 `UCSMesh`，一次间接绘制），所以它的"GPU 侧真值"
	 * 就是这个数。⚠️ 注意它**不会跟着壳的死活变**：壳是靠往顶点写 NaN 让三角自己塌掉的
	 * （裁决二那条"没有一行注销代码"），索引数恒等于图案的三角数 ×3。它答的是
	 * "这次绘制到底会不会读到东西"，答不了"还剩几个三角活着" —— 后者请用
	 * `DebugReadRockShellSync` 逐顶点判 NaN。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadRockShellDrawIndexCountGpuSync() const;

	/**
	 * 石阶 / 小石子这两条 GPU 实例路上，**GPU 真的在画的基础网格 / 材质**是不是我们以为的
	 * 那两样。空串 = 是。原因串带家族前缀（石阶 / 石子）。
	 *
	 * ⚠️ 存在的理由就是坑表里那两条：`StairMesh` / `StairMaterial` 一直是 NULL 时画面上是
	 * 一撮黑块而 readback 全绿；母材质没勾 `bUsedWithInstancedStaticMeshes` 时引擎**静默换成
	 * 默认材质**，症状与"没绑材质"逐像素相同。`IsRockShellDrawable` 那一族只查"非空"。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	FString DebugGetGpuAssetMismatchSync() const;

	// -------------------------------------------------------------------------
	// Rock Shell（链 B）
	// -------------------------------------------------------------------------

	/**
	 * 重跑一趟披挂岩壳（计划 D9 链 B）。
	 *
	 * ⚠️ **第一句就是哈希比较** —— 短路必须发生在昂贵计算**之前**（已删的塑形物石阶旧路
	 * 恰好把短路点放在 181 行的 `BuildStepPlan` 之后，那是它记在案的缺陷）。
	 * 哈希覆盖"会改变壳形状的一切"：
	 * 塑形物集合与它们的高度场参数、地面几何配置、壳自己的参数、以及**落笔计数**
	 * （道路权重变了必须重披挂 —— 那正是"沉下去不是隐藏"的执行面）。
	 *
	 * 调用时机的硬约束与石阶相同：它读的是**地面网格 GPU 色流**里的道路权重，而落笔只写镜像 +
	 * 排队，所以必须排在 `FlushPaintToGpu` 之后。本类内部已经在每条会改变高度或道路的路径
	 * 末尾调过它。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Rock Shell")
	void RebuildRockShell();

#if WITH_EDITOR
	/**
	 * 把地面这一侧**全部**实例路产物（石阶 + 15% 小石子）烘成 StaticMesh 资产 ——
	 * 裁决六 ① 在地面这一侧的用户入口，与 `ACSHouseActor::SaveInstancedToStaticMeshes` 同形。
	 *
	 * 资产落在 `BakeFolder/SM_<actor>_<family>`；返回真的烘出来的张数。
	 * ⚠️ **阻塞**（每族两次回读 + 一次 StaticMesh 构建），是用户主动发起的离线操作，
	 * 不在任何交互路径上；它照旧被 `UCSMesh::GetBlockingFlushCount()` 数到。
	 *
	 * 地面本体与岩壳走的是网格路（`UCSMeshRenderComponent::SaveToStaticMesh`），不在这里。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	int32 SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets = false);
#endif

	/**
	 * **诊断 / 验收专用**：岩壳这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * ⚠️ **这个函数存在的唯一理由是今天刚踩过的那个坑**：GPU 石阶的 `StairMesh` /
	 * `StairMaterial` 在两张演示关卡里一直是 NULL，石阶在画面里是一撮黑块，而单测 53/53、
	 * 回归 55 条全绿 —— 因为验收全部走 readback 断言，而 **readback 证明的是"buffer 里有数"，
	 * 对"画的是哪张网格、有没有材质、组件注册没注册"一个字都没说**。
	 *
	 * 所以它检查的是渲染那一侧的每一环：组件存在且已注册且可见、网格已绑定、常驻流已分配、
	 * 三角容量非零、且**解析得到一个非空材质**。任何一环断掉都会写进 `OutReason`。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	bool IsRockShellDrawable(FString& OutReason) const;

	/**
	 * 同上，但**把原因当返回值给出来**（空串 = 画得出来）。
	 *
	 * ⚠️ **`IsRockShellDrawable(FString&)` 在 UE Python 侧是残废的**（实测）：Python 把
	 * "bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串、**不可画时拿到 `None`**，
	 * 原因串整个丢掉，恰好在唯一需要它的时候失效。出图/回归脚本一律调这一版。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	FString GetRockShellUndrawableReason() const;

	/**
	 * **自动化测试专用**：披挂 pass 跑过几次。
	 *
	 * 与 `GetGpuDisplaceCount` 同一个用途：让"壳真的被重算了"这件事可断言，
	 * 而不是断言一个从来没跑过的 pass 留下的初始值。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	int32 GetRockShellDisplaceCount() const { return RockShellDisplaceCount; }

	/**
	 * **自动化测试专用**：阻塞回读岩壳的逐顶点世界位置。
	 *
	 * 运行路径一个字节都不回读；这个函数存在的唯一理由是让链 B 的三条核心验收项可断言
	 * （陡坡上出现 / 平地上不出现 / 画路之后**连续下沉**）。别在任何每帧路径上调它。
	 *
	 * 被 NaN 关掉的三角在返回值里就是 NaN，调用方自己判 `ContainsNaN()`。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	int32 DebugReadRockShellSync(TArray<FVector>& OutWorldPositions);

	/**
	 * **自动化测试 / 诊断专用**：岩壳顶点色 R 通道的盖 / 裙分离统计（回读 GPU 常驻流）。
	 *
	 * 存在的理由与 `IsRockShellDrawable` 同一条：`bIsCapTri` 在 aux 槽 33 里躺着**没人读**，
	 * 而 aux 流既进不了顶点工厂也进不了回读集 —— 材质拿不到它，`SaveToStaticMesh` 也带不走它，
	 * 而这两件事**都不会报错**。盖/裙因此被写进顶点色 R（通道字典见 `CSGroundRockShell.h`），
	 * 这个函数是那条通道的执行面：数出来的两个数必须与图案里的盖/裙三角数逐个对上。
	 *
	 * 返回读到的顶点总数（0 = 回读失败）。⚠️ 阻塞回读，**只给测试与诊断**，不许进交互路径。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	int32 DebugReadRockShellCapSplitSync(int32& OutCapVerts, int32& OutSkirtVerts);

#if WITH_EDITOR
	/**
	 * **自动化测试专用**：把岩壳走真正的 `SaveToStaticMesh` 出口烘成资产，再从烘出来的
	 * `FMeshDescription` 里数盖 / 裙角点 —— 裁决六 ②「顶点色通道字典必须随网格保住」的判据。
	 *
	 * 为什么必须真烘一遍而不是只看 GPU 侧那份：中间隔着 `ReadbackResidentSync` →
	 * `BuildGpuMeshDescription` → StaticMesh 构建三道，任何一道把顶点色丢掉都**不报错**，
	 * 症状只是"烘出来的资产在编辑器里是一整片同色"，而那时人已经离开这条链很久了。
	 *
	 * `AssetPath` 传测试自己的临时路径；调用方负责删。返回是否烘成功。
	 * ⚠️ 阻塞 + 建资产，**只给测试**。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	bool DebugBakeRockShellCapSplitSync(const FString& AssetPath, int32& OutCapCorners,
		int32& OutSkirtCorners, int32& OutTriangles);
#endif

	/**
	 * **自动化测试专用**：图案抽取的实测结果（`Docs/TinyGlade/CSRockShellPattern.md`「首次导入后必须
	 * 核对的四项」的机读版）。三角数 / UV 通道数 / CellId 上界 / 绕序 / dir 一致度。
	 *
	 * 期望值（原件实测）：49,598 三角、≥3 条 UV、CellId 上界 608（**不是 0..1** —— 有的导入
	 * 路径会把 UV 归一化，一旦归一化 cell_id 就废了，而且是静默的）。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	bool GetRockShellPatternStats(int32& OutTriangles, int32& OutUVChannels, float& OutMaxCellId,
		bool& bOutFlipWinding, float& OutDirAgreement) const;

	/**
	 * **自动化测试专用**：区域位移 pass（`UCSMeshOps::DisplaceGroundShapers`）跑过几次。
	 *
	 * 存在的唯一理由是让 CPU/GPU 高度场一致性那条断言**不会静默通过**：
	 * `RefreshHeightsInRegion` 在"网格还没建 / actor 被拖过"时会退回 `RebuildGroundMesh()`，
	 * 那条路是拿 CPU 镜像直接上传的 —— 回读到的顶点 Z 与镜像当然相等，断言绿着却什么都没测。
	 * 测试必须先看这个计数涨了，才知道自己比的是 GPU 那一份。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 GetGpuDisplaceCount() const { return GpuDisplaceCount; }

	// -------------------------------------------------------------------------
	// 塑形物裙边摆件（D12 第五家）
	// -------------------------------------------------------------------------

	/**
	 * 重摆一遍裙边摆件。**幂等**，且第一句就是哈希短路（短路点在任何昂贵计算之前，
	 * 同 `RebuildRockShell` —— 已删的塑形物石阶旧路把短路放在 181 行规划之后，那是它记在案的缺陷）。
	 *
	 * 调用时机的硬约束与石阶 / 岩壳相同：它读的是**镜像**里的高度与道路权重，而落笔只写镜像 +
	 * 排队，所以排在同一条尾巴上就够了。本类内部已经在每条会改变高度或道路的路径末尾调过它。
	 *
	 * 交互期零阻塞：容量在这里按**配置上限**（`CSGroundDecor::MaxRecordsBound` 再过一遍
	 * `CSShaperSteps::ReserveCount`）一次付清，包围盒量化只涨不缩，之后画笔刷 / 拖塑形物
	 * 走的永远是"哈希没变，直接返回"这条零成本分支。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Skirt Decor")
	void RebuildSkirtDecor();

	/** 这一轮生产出来的裙边锚点数。**摆件数 ≤ 锚点数**是 D12「密度由锚点个数决定」的形式化。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor")
	int32 GetSkirtDecorAnchorCount() const { return CurrentSkirtDecorAnchorCount; }

	/** 这一轮真的摆出去的件数（锚点过了填充概率与最小间距球之后剩下的）。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor")
	int32 GetSkirtDecorInstanceCount() const { return CurrentSkirtDecorInstanceCount; }

	/**
	 * **验收专用**：裙边摆件这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * 执行面照抄 `ACSHouseActor::GetDecorUndrawableReason` —— 逐环检查渲染那一侧
	 * （组件存在/注册/可见、实例源已交接、基础网格快照非空、GPU 网格已分配、材质非空、
	 * **且母材质勾了 `bUsedWithInstancedStaticMeshes`**）。最后那一条是本项目栽过的三条
	 * "静默换默认材质"里的一条，readback 断言对它一个字都说不了。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	bool IsSkirtDecorDrawable(FString& OutReason) const;

	/**
	 * 同上，但**把原因当返回值给出来**（空串 = 画得出来）。
	 *
	 * ⚠️ 脚本一律调这一版：UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 ——
	 * 可画时拿到空串、**不可画时拿到 `None`**，原因串整个丢掉，恰好在唯一需要它的时候失效。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	FString GetSkirtDecorUndrawableReason() const;

	/**
	 * **诊断 / 验收专用，阻塞**：裙边摆件在 GPU 上的实例计数器（**组件手上那一份**，
	 * 也就是剔除 pass 与 indirect draw 真正消费的那个）之和。
	 *
	 * 与 `GetSkirtDecorInstanceCount()` 的分工同门框砖那条：CPU 的账与 GPU 的账不一致
	 * 正是"生产者以为清干净了、组件却握着陈旧计数器"那类 bug 的现场。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	int32 DebugReadSkirtDecorInstanceCountGpuSync() const;

#if WITH_EDITOR
	/**
	 * **自动化测试专用**：把裙边摆件走真正的 `UCSGpuInstancedMeshComponent::SaveToStaticMesh`
	 * 出口烘成资产，再从烘出来的 `FMeshDescription` 里读回判据 —— 裁决六 ①②③ 的执行面。
	 *
	 * 判据与 `ACSHouseActor::DebugBakeFrameBricksSync` 逐条同形（那条守着门框砖）：
	 * ① 出口走不走得通；② 顶点色通道与 UV 有没有活下来；
	 * ③ **逐实例随机有没有活下来** —— 烘完就没有实例了，`PerInstanceRandom` 恒 0，
	 *    材质那条 `lerp(0.78, 1.22, rnd)` 会把整片摆件烘成同一个色，而三角数 / 包围盒 /
	 *    实例数**全都看不见它**。随机数烘在**顶点色 A**（材质两条路统一读
	 *    `PerInstanceRandom + VertexColor.A`），所以判据就是"A 通道还剩几种取值"。
	 *
	 * ⚠️ **UV 那一半与门框砖不同，且是有意的**：摆件的基础网格走 `CSHouseVine::BuildBaseMesh`
	 * （clutter 与 `ivy_branch` 一样可能**根本没有法线与 UV**，那个读取器负责现补），
	 * 它把 `NumTexCoordChannels` 钉成 1 ⇒ 这条路上**只有一组 UV**，多组 UV 那一半由门框砖
	 * 那条（走 `SetBaseMesh`，原样保留资产的全部通道）守着。所以这里的判据是
	 * "那**一组** UV 活下来了且不是退化的"，`OutDistinctUVs` 就是后半句 ——
	 * 全是 (0,0) 的 UV 不报错，症状只是烘焙件贴图变成一整片同一个像素。
	 *
	 * 只烘第 0 个 palette（多 palette 时其余同构，多烘几张只是多花时间）。⚠️ 阻塞 + 建资产。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	bool DebugBakeSkirtDecorSync(const FString& AssetPath, int32& OutTriangles, int32& OutVertexInstances,
		int32& OutUVChannels, int32& OutDistinctUVs, int32& OutDistinctBakedRandoms, int32& OutGpuInstanceCount,
		bool& bOutRandomsMatchGpu);
#endif

	/** 顶点色全部铺回 BaseColor（高度不动），并重建。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground")
	void ResetPaint();

	/**
	 * 一次落笔：GPU 球刷 pass + 镜像 CPU 孪生双写，公式严格同 ECSMeshPaintBlendOp 契约。
	 * EdMode 在 stroke 期间每次鼠标移动调一次；Python/测试也可直接调。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void ApplyPaintStroke(FVector WorldCenter);

	/** Stroke 括号：Begin 清累计脏盒，End 标脏包。变更通知不在这里——每次落笔已直推 OnGroundChanged。 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void BeginPaintStroke();

	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void EndPaintStroke();

	/**
	 * 把本帧攒下的笔刷落笔一次性推给 GPU（**一次异步编辑，零 flush**）。EdMode 每帧调一次。
	 *
	 * 落笔本身只写镜像（本就是权威）并把这一笔排进队列 —— 交互热路径上一次设备同步都没有。
	 * 副作用只有"GPU 顶点色比镜像滞后 ≤1 帧"，纯视觉：所有 gameplay 查询都走镜像。
	 *
	 * bBlockIfNeeded 只给收笔用：异步在途时退回一次同步提交，保证抬笔后 GPU 与镜像一致
	 * （否则 EdMode 退出后队列没人再推，画面会一直停在上一帧）。一次 stroke 至多一次。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void FlushPaintToGpu(bool bBlockIfNeeded = false);

	/** 镜像双线性采样：世界 XY → 世界 Z。超出范围返回 actor Z。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground")
	float SampleHeight(FVector2D WorldXY) const;

	/** 镜像双线性采样：世界 XY → 道路权重（R 通道，0..1）。超出范围返回 0。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground")
	float SampleRoadWeight(FVector2D WorldXY) const;

	/** 镜像双线性采样：完整顶点色。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground")
	FLinearColor SampleColor(FVector2D WorldXY) const;

	/** 射线 ∩ 地面（解析，不走碰撞）。平地走平面求交，有起伏时定步进 march。 */
	bool RaycastGround(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutHit) const;

	/** 地面的世界 XY 矩形（笔刷范围提示用）。 */
	FBox2D GetWorldRect2D() const;

	//~ AActor interface
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

private:
	/** 扫场景收集全部塑形物（加载顺序无保证，塑形物自己也会登记，两条路都幂等）。 */
	void ResolveShapers();

	/** 镜像尺寸与配置不符时重置为平地 + BaseColor。返回是否发生了重置。 */
	bool EnsureMirrorInitialized();

	/** 镜像 → 世界空间快照（位置/法线/UV/颜色/索引），交给 CopyFromMeshSnapshot 上传。 */
	void BuildSnapshotFromMirror(struct FCSGpuMeshCPUData& OutSnapshot) const;

	/** GPU 球刷的 CPU 孪生：对镜像里笔刷矩形内的顶点跑同一套权重/混合公式。 */
	void ApplyPaintToMirror(const FVector& WorldCenter);

	/**
	 * 笔刷球在镜像格点上的闭区间矩形。CPU 孪生的遍历范围与 GPU 区域派发的范围**共用这一个**
	 * 口径——两边各算一份就会在边界上分叉（镜像有色而画面没有，或反过来）。
	 * 返回 false 表示笔刷完全落在镜像外，这一笔什么都不用做。
	 */
	bool ComputeBrushGridRect(const FVector& WorldCenter, FIntPoint& OutMin, FIntPoint& OutMax) const;

	/** 镜像格点的世界坐标（含高度）。 */
	FVector VertexWorldPosition(int32 X, int32 Y) const;

	/** 世界 XY → 连续格坐标；返回是否落在镜像矩形内（出界时也写出 clamp 后的值）。 */
	bool WorldToGrid(const FVector2D& WorldXY, FVector2D& OutGrid) const;

	/** 双线性采样的公共实现。 */
	bool SampleBilinear(const FVector2D& WorldXY, float& OutHeight, FLinearColor& OutColor) const;

	/** 整块地面的世界 AABB（XY 矩形 × 高度范围），全量类广播用。 */
	FBox ComputeGroundWorldBox() const;

	/**
	 * 保证石阶实例组件存在并绑好基础网格/材质，同时刷新从网格推导出来的三个量
	 * （包围球、块缩放、抬升）。`StairMesh` 为空时销毁组件并释放缓冲区。返回是否可以扫。
	 */
	bool EnsureStairComponent();

	/**
	 * 保证岩壳的网格与渲染组件存在、图案已上传、包围盒已按地面矩形写死。返回是否可以披挂。
	 *
	 * ⚠️ **这条路是阻塞的**（声明流集 / 分配 / 上传各 flush 一次），所以它自己也带一层短路：
	 * 已经按当前地面矩形与图案建好就直接返回 true，一次 enqueue 都不发。交互期（画笔刷、
	 * 拖塑形物）走的永远是这条零成本分支 —— 与 `CSGroundStairs::EnsureBuffers` 同一条纪律。
	 */
	bool EnsureRockShellMesh();

	/**
	 * 岩壳的输入哈希：塑形物集合与它们的高度场参数、地面几何配置、壳自己的参数、落笔计数。
	 * `RebuildRockShell()` 的第一句就用它短路，短路点在任何昂贵计算之前。
	 */
	uint32 RockShellInputHash() const;

	/**
	 * 保证裙边摆件的实例组件、基础网格快照、GPU 缓冲与容量都就位。返回是否可以摆。
	 *
	 * 稳态下**零阻塞**：容量按配置上限一次付清（只涨不缩），包围盒量化只涨不缩，
	 * 两者都没变时直接返回，一次 enqueue 都不发 —— 同 `EnsureRockShellMesh` / `EnsureStairComponent`。
	 */
	bool EnsureSkirtDecorComponents();

	/** 把登记在案的塑形物读成裙边生产者要的环表（**高度场参数与 GPU 位移 pass 同一份来源**）。 */
	void BuildSkirtDecorSite(CSGroundDecor::FSite& OutSite) const;

	/** 参数打包：细节面板只暴露改观感的那几个，其余以 `CSHouseDecor::FParams` 的默认值为准。 */
	CSHouseDecor::FParams MakeSkirtDecorParams() const;

	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> StairComponent;

	/** 小石子的实例组件。石子与石阶是两张基础网格，一个组件只绑一张 ⇒ 必须各自一个。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> StairPebbleComponent;

	/** 岩壳的 GPU 投影与它的渲染组件（都是 transient：常驻数据不随关卡存盘）。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> RockShellMesh;

	UPROPERTY(Transient)
	TObjectPtr<UCSMeshRenderComponent> RockShellComponent;

	/** 上次建壳时的图案资产与世界矩形：只有它们真变了才需要再走一次阻塞的建壳路径。 */
	TWeakObjectPtr<UStaticMesh> RockShellBuiltPattern;
	FBox2D RockShellBuiltRect = FBox2D(ForceInit);
	float RockShellBuiltScale = 0.0f;

	/** 上次披挂时的输入哈希（0 = 还没披挂过）。 */
	uint32 RockShellBuiltHash = 0;

	/** 披挂 pass 跑过的次数（诊断用，见 GetRockShellDisplaceCount）。 */
	int32 RockShellDisplaceCount = 0;

	/**
	 * 落笔计数：每次改动镜像顶点色就 +1。
	 *
	 * **为什么岩壳的哈希必须带上它**：壳在路上是"连续下沉"而不是隐藏（裁决五），下沉量由
	 * 道路权重驱动 ⇒ 画一笔路就必须重披挂一次。而顶点色是 257² 个字节，逐笔哈希整张表太贵；
	 * 一个单调计数器给出同样的"变了没有"判定，代价是常数。
	 */
	uint32 PaintRevision = 0;

	/** 固定容量的实例行与计数（渲染线程分配、渲染线程释放）。 */
	CSGroundStairs::FStairBuffers StairBuffers;

	/**
	 * 从 `StairMesh` + `StairBlockSize` 推导出来的量，`EnsureStairComponent` 刷新。
	 * `StairRise` = −局部包围盒 Min.Z × Z 缩放：把块抬起半个身位，否则盒心落在等值线上、
	 * 石块一半埋在地里（旧路 `PaletteRise` 记下来的实测修正，这里逐字沿用）。
	 */
	FVector3f StairBaseSphereCentre = FVector3f::ZeroVector;
	float StairBaseSphereRadius = 0.0f;
	FVector3f StairBlockScale = FVector3f(1.0f, 1.0f, 1.0f);
	float StairRise = 0.0f;
	/** 基础网格的局部 Y 尺寸：kernel 要拿它把"想要的世界长度"（跟着弦长走）换算回缩放。 */
	float StairBaseSizeY = 1.0f;

	/** 从 `StairPebbleMesh` + `StairPebbleSize` 推导：包围球（剔除用）与均匀缩放的上下限。
	 *  `EnsureStairComponent` 与石阶那三个量一起刷新。网格为空时缩放为 0（= 这一支关掉）。 */
	FVector3f StairPebbleSphereCentre = FVector3f::ZeroVector;
	float StairPebbleSphereRadius = 0.0f;
	float StairPebbleScaleMin = 0.0f;
	float StairPebbleScaleMax = 0.0f;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。
	 *  容量是固定的、包围盒按地面矩形 × MaxAbsHeight 写死，所以稳态下这里永远不触发。 */
	uint32 HandedStairCapacity = 0;
	FBox HandedStairBounds = FBox(ForceInit);
	uint32 HandedPebbleCapacity = 0;
	FBox HandedPebbleBounds = FBox(ForceInit);

	/**
	 * 权威数据，随关卡序列化。别在 details 里展开它 —— 就是两条百万级数组。
	 *
	 * **NonTransactional 是正确性必需，不是优化**：镜像若进事务缓冲，"改个属性 → 画路 →
	 * Ctrl+Z 撤那次属性改动"会把镜像整份回滚成"无路"，而屏幕上路还在（GPU 顶点色不在事务里）
	 * ⇒ SampleRoadWeight 说没路、房子下次重求值时拱全关，即"镜像/GPU 双写漂移"的事务成因。
	 * 与"笔刷家族无 Undo"的既定裁决同向；附带省下 257² 每次事务捕获的约 528 KB。
	 * 撤销后的对齐由 PostEditUndo() 负责。
	 */
	UPROPERTY(NonTransactional)
	FCSGroundMirror Mirror;

	/** 上次把镜像烘进常驻流时的 actor 位置；PostEditMove 的增量平移相对它算。 */
	FVector MeshBuiltAtLocation = FVector::ZeroVector;

	/** 高度绝对值上界，决定 RaycastGround 走平面还是 march。由 EnsureMirrorInitialized 从镜像刷新。 */
	float MaxAbsHeight = 0.0f;

	/** 区域位移 pass 跑过的次数（诊断用，见 GetGpuDisplaceCount）。 */
	int32 GpuDisplaceCount = 0;

	/** 登记在本地面上的塑形物（transient：加载后由各自 PostRegisterAllComponents 重新登记）。 */
	TArray<TWeakObjectPtr<ACSGroundShaperActor>> Shapers;

	// --- 裙边摆件（D12 第五家）：与房子那四家同构，一个 palette = 一张网格 = 一个组件 ---

	UPROPERTY(Transient)
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> SkirtDecorComponents;

	TArray<CSShaperSteps::FPaletteBuffers> SkirtDecorGpuBuffers;

	/** 按 `CSHouseDecor::EFamily` 下标。**只有 `Skirt` 那一格非空** —— 其余四家的载体是房子，
	 *  地面对它们一无所知，留 `{0, 0}` 就是"这一家在这里一件都不长"。 */
	TArray<CSHouseDecor::FPaletteRange> SkirtDecorPaletteRanges;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	TArray<uint32> SkirtDecorHandedCapacities;
	FBox SkirtDecorHandedLocalBounds = FBox(ForceInit);

	/** 上次那一轮的输入哈希（0 = 还没摆过）。`RebuildSkirtDecor()` 的第一句就用它短路。 */
	uint32 SkirtDecorHash = 0;
	int32 CurrentSkirtDecorAnchorCount = 0;
	int32 CurrentSkirtDecorInstanceCount = 0;

	/** 基础网格快照建好了没有；组件数或网格资产一变就必须重建（palette 与组件**按下标**对齐）。 */
	bool bSkirtDecorBaseMeshReady = false;

	/** 快照是从哪几张网格建的。⚠️ **不能只靠上面那个 bool**：在细节面板里换掉网格时组件不一定
	 *  被重建，那时 bool 仍是 true，画面上还是旧网格 —— 症状是"换了资产但什么都没发生"。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> SkirtDecorMeshesBuiltFrom;

	/** 本次 stroke 的累计世界脏盒：EndPaintStroke 判断要不要标脏包；也是将来切 dirty 系统时的区域发布素材。 */
	FBox StrokeDirtyBounds = FBox(ForceInit);
	bool bPaintStrokeOpen = false;

	/**
	 * 还没推给 GPU 的落笔队列（按落笔顺序）。混合公式对同一顶点是可结合地按序作用的，
	 * 所以"一帧内攒 N 笔、一张图里按序录 N 个 pass"与"逐笔各录一次"逐位等价。
	 * 异步在途被拒时队列原样留到下一帧，不丢也不乱序。
	 */
	TArray<UCSMeshOps::FCSMeshPaintSphereDab> PendingPaintDabs;
};
