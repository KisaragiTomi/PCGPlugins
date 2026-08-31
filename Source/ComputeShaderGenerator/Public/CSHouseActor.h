#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"
#include "CSHouseDecor.h"
#include "CSHouseFrame.h"
#include "CSHouseProfile.h"
#include "CSHouseResize.h"
#include "CSHouseRoof.h"
#include "CSHouseSeam.h"
#include "CSHouseVine.h"
#include "CSTinyGlade.h"
#include "CSHouseActor.generated.h"

class ACSGroundActor;
class UCSGpuInstancedMeshComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
class UStaticMesh;
struct FCSGpuMeshCPUData;

/**
 * 房体三角汤的**全部**输入。ACSHouseActor::RebuildBodyMesh 从自己的属性组一份，
 * automation 测试从字面量组一份 —— 后者是这个结构存在的唯一理由：
 * 「墙顶到屋面底那道楔形缝里到底有没有实体」只能逐三角验，而 actor 进不了纯 CPU 用例
 * （测试不起 world、不碰 RHI，见 Tests/CSHouseLogicTests.cpp 的文件头）。
 */
struct FCSHouseBodyDesc
{
	/** 屋面描述。三处关键高度与墙顶该砌到哪都从 CSHouseRoof.h 的求值器取。 */
	FCSRoofDesc Roof;

	/** 底面尺寸 cm（局部 X/Y），与 Roof.Footprint 同值。 */
	FVector2D Footprint = FVector2D(600.0, 400.0);

	float WallThickness = 24.0f;
	/** 墙顶高，与 Roof.EaveZ 同值。 */
	float WallHeight = 300.0f;
	/** 相邻洞之间保留的墩宽 cm。 */
	float PierWidth = 40.0f;

	/** 这一轮要切出来的洞（几何上不挖，逐像素 clip）。 */
	TArray<FCSWallOpening> Openings;

	/**
	 * D7 接缝在墙上切的段：插进邻居 footprint 里的那截墙（同样几何上不挖，逐像素 clip）。
	 *
	 * **与 Openings 分开是有意的**（理由逐字见 `FCSWallCut`）：洞是设计意图，接缝裁剪是事实。
	 * 混成一张表会让谓词、墩迟回、门框砖三处一起误把它当洞。
	 */
	TArray<FCSWallCut> SeamCuts;

	/** 把局部坐标烘成世界坐标（常驻流口径）。测试传 Identity 即得局部坐标。 */
	FTransform World = FTransform::Identity;
};

/**
 * 房体三角汤的**唯一**生成点（计划 D4：屋面方程一旦散开，铺瓦 / 铺梁 / 落窗谓词就会各写一份）。
 * 从 RebuildBodyMesh 里原样抽出来，除了让测试能拿到三角汤以外没有第二个目的。Out 按**追加**写。
 */
COMPUTESHADERGENERATOR_API void CSHouse_BuildBodySoup(const FCSHouseBodyDesc& Desc, FCSGpuMeshCPUData& Out);

/**
 * 房体顶点色 R 通道的构件色号（见下面 ACSHouseActor 的通道字典）。
 * 值直接以 id/255 写进顶点色，8-bit 量化后逐位还原。
 */
UENUM()
enum class ECSHousePart : uint8
{
	Wall = 0,
	Roof = 1,
	Gable = 2,
	/** 门框砖：沿洞缘曲线铺的离散块，负责填满 clip 留下的厚度断口（不是扫掠面）。 */
	Frame = 3,
	Pillar = 4,
};

/**
 * 一扇窗的**诉求**（D8）。
 *
 * ⚠️ **这是一份显式列表，不是"按剩余墙面自动填窗"的规则。** Tiny Glade 的窗 100% 是玩家
 * 手放的（`ui_place_decorator` → 光标射线 → `DecoratorDst{Wall|Roof|…}`），全仓找不到任何
 * 读墙长去分配窗位的系统（`Docs/TinyGlade/TinyGlade_模块对照与进度.md（卷二）` 第二节）。本项目还没有放置 UI，
 * 而「门洞触发规则」是唯一没拍板的一条 —— 所以窗的来源就停在这份列表上：属性面板、
 * Blueprint、测试脚本都往这里填，将来的 `ACSWindowMarker` 也只是多一个填表的人。
 * **不要在这里发明触发规则。**
 *
 * 诉求 ≠ 结果：能不能砌出来由 `ACSHouseActor::QueryFeaturePlacement` 说了算（门拱优先），
 * 被拒的诉求留在列表里、只是这一轮不出洞（`GetWindowRejectCount` 数得出来）。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSHouseWindow
{
	GENERATED_BODY()

	/** 边缘线段索引：0 南(+X 向) 1 东(+Y 向) 2 北(-X 向) 3 西(-Y 向)，与 CSHouse_GetEdge 同号。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "0", ClampMax = "3"))
	int32 EdgeIndex = 0;

	/** 沿边弧长上的窗心（从边起点算）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "0.0"))
	float CenterS = 150.0f;

	/** 窗宽 cm。默认取 TG `window_cottage_1x1` 的实测宽（78）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "10.0"))
	float Width = 78.0f;

	/** 窗台高 = 洞底 Z。低于 `WindowMinSillZ` 会被谓词拒（窗台压在地上时几何合法但观感荒唐）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "0.0"))
	float SillZ = 90.0f;

	/** 窗高 cm；洞顶 = SillZ + Height，超过 墙高 − LintelBand 会被谓词拒。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "10.0"))
	float Height = 110.0f;

	/**
	 * 洞形。**三种原型全都不用改一行 shader** —— `FCSOpeningClipField` 本来就是二维的，
	 * `Rect`/`Circle` 上下都有界，材质里那段 HLSL 也早已覆盖三个形状 id。
	 * `Arch` 用来做尖顶窗（洞底在 SillZ，下界由洞面板的底承担）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window")
	ECSOpeningShape Shape = ECSOpeningShape::Rect;
};

/**
 * 叶子的季节（TG 的 `MI_{summer,autumn,winter}_ivy_leaf_color` 三张齐全，见对照文档 §7）。
 *
 * ⚠️ **切季节不换材质资产**：换资产会在实例路上换一次材质绑定，而季节是会被反复来回切的量；
 * 落地做法是母材质里三张贴图按一个 `Season` 标量混、actor 缓存一个 MID 只写那个标量
 * （见 `ACSHouseActor::EnsureVineComponents`）。三张贴图恒定采样的代价换掉了 shader 重绑。
 */
UENUM(BlueprintType)
enum class ECSVineSeason : uint8
{
	Summer UMETA(DisplayName = "Summer"),
	Autumn UMETA(DisplayName = "Autumn"),
	Winter UMETA(DisplayName = "Winter"),
};

/**
 * Tiny Glade 式房屋（TinyGladeHouse_Plan.md D4/D6/D9 的"房子×地面交互"纵切片）。
 *
 * 声明式重求值：任何唤醒（移动 / 改参 / 地面 OnGroundChanged 直推）都走同一条
 * ReevaluateSite() —— ① 落座：房底 Z = max(footprint 全域地面高度) + HeightOffset，
 * 绝对式、升降对称（地形隆起多高屋顶抬多高，塌陷同理回落）；② 门拱：边缘线段等分
 * 为子段，逐子段采样道路顶点色（双探测线）+ 离地连续收窄 + 滞回点亮，点亮子段生成
 * 拱洞（门是固定拱原型，直接参数化生成——布尔的"纯 mesh 操作数"入口尚不存在，见计划
 * 开放问题）；③ 承重柱：周界支撑点逐点比地面落差，悬空处生柱，独立组件不进房体。
 *
 * 两份产物各自哈希守卫（含量化后的世界变换）：房体 desc 变才重建房体网格，柱 desc
 * 变才重建柱网格 —— 画路只可能动房体，纯地形变化只动柱与落座，互不牵连。
 *
 * 与地面的接线是直推：PostRegisterAllComponents 时订阅 Ground->OnGroundChanged，
 * 收到即 ReevaluateSite（无条件唤醒，幂等哈希把无效唤醒吸收成零成本）。
 *
 * -----------------------------------------------------------------------------
 * 房体顶点色的通道字典（P2 冻结；**全项目唯一仲裁点**，别在别处先到先得地占用）
 * -----------------------------------------------------------------------------
 *   R = 构件色号 ECSHousePart / 255   墙 0 / 屋顶 1 / 山墙 2 / 门框砖 3 / 柱 4
 *   G = 洞的 Tag / 255                悬停高亮单个拱用；非洞构件恒 0
 *   B = 洞形状 id / 255               ECSOpeningShape；255 = 这块面板没有洞
 *   A = 保留                          预定：季节 t
 *
 * UV 通道（房体声明 2 组，靠 FCSMeshStreamLayout::NumTexCoordSets 的逐 mesh 变体）：
 *   UV0 = 墙面贴图坐标 (沿边弧长 S, 高度 Z) / UVScale
 *   UV1 = 解析裁剪场 q —— 材质按它逐像素 discard 切出洞（Tiny Glade 原版做法）。
 *         判据见 CSHouseProfile.h 的 CSHouse_ClipKeeps()，材质里那份必须逐字对应。
 *
 * 之所以能这么用：房体是**无共享顶点的三角汤**（AddTri 每次新建三个顶点），位域打包是免费的，
 * 也不会被邻接顶点插值污染。**地面正相反** —— 它的顶点是共享的、插值会毁掉位域，而且 R 已经
 * 被道路权重占用，所以地面绝不打包位域。
 *
 * 这三十二位是本项目能被材质读到的**唯一**自定义逐顶点语义（对照 Tiny Glade 的 41 种自定义
 * 语义属性），扩 UV1 在 proxy 绑定处有静默地雷，见计划 D14 通道二。
 */
UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADERGENERATOR_API ACSHouseActor : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	ACSHouseActor();

	// -------------------------------------------------------------------------
	// Footprint / Body
	// -------------------------------------------------------------------------

	/** 底面尺寸 cm（X=长边候选，Y=短边候选）；位置/朝向用 actor transform（支持任意 yaw）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "100.0"))
	FVector2D FootprintSize = FVector2D(600.0, 400.0);

	/** 檐口高 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "100.0"))
	float WallHeight = 300.0f;

	/** 墙厚 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "5.0"))
	float WallThickness = 24.0f;

	/** 双坡屋顶坡度（度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "5.0", ClampMax = "70.0"))
	float RoofPitch = 35.0f;

	/** 屋檐外挑 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "0.0"))
	float RoofOverhang = 25.0f;

	/** 屋顶板厚 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "2.0"))
	float RoofThickness = 12.0f;

	/**
	 * 屋脊沿哪根局部轴走。**显式状态而非从长轴隐式导出**（计划 D4）：隐式的话 D5 单边推拉
	 * 一旦让 X 穿过 Y，脊与山墙就原地 90° 跳变。每次重求值按 RidgeSwitchRatio 做滞回更新，
	 * 用户也可以在这里直接指定。
	 *
	 * NonTransactional 与 DoorSlotOpen 同理：它是滞回的记忆，被无关改参的 Ctrl+Z 回滚会让
	 * 屋顶莫名其妙翻面（凡双阈滞回的状态一律照此办理）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, NonTransactional, Category = "CS House")
	ECSRidgeAxis RidgeAxis = ECSRidgeAxis::X;

	/** 脊向换轴的滞回比：另一根轴要长出当前脊轴这么多倍才换向。1 = 无滞回。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float RidgeSwitchRatio = 1.15f;

	/**
	 * 尺寸禁带的半宽比例（2026-08-30 裁决四「房屋尺寸更换有最小距离」的落地口径）。
	 *
	 * `PushEdge` 推拉时，被推的那一维不许停在 `|X − Y| < FootprintBandFraction × 另一维` 里面，
	 * 落进去就跳到带外沿。**它与 `RidgeSwitchRatio` 是一对**：带宽的实际下界由滞回比反解
	 * （见 `CSHouseResize_EffectiveBandFraction`），保证整段滞回模糊区被吞掉 ——
	 * 尺寸因此永远停不到翻轴阈上，翻轴只可能与"跳带"那一步的尺寸跳变同步发生。
	 *
	 * 0 = 关掉禁带（退回纯滞回：单边推拉扫过阈值时屋顶仍会在某个连续步里原地翻面）。
	 * 默认 0.20 > 1.15 − 1，已经在下界之上，改小到 0.15 以下会被下界顶回去。
	 *
	 * ⚠️ 只作用在 `PushEdge` 这条**推拉**路径上，**不回写** `FootprintSize` 属性本身 ——
	 * 程序去纠正用户正在 details 面板里输入的数是"与输入源抢写"，与拖拽期回写 handle 位置
	 * 同一条教训（计划 D5「回位规则」）。脚本/蓝图直接设 `FootprintSize` 时不受约束，那是
	 * 显式赋值不是推拉。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float FootprintBandFraction = 0.20f;

	/** 单边推拉的尺寸下限 cm（`PushEdge` 的硬下界，禁带给它让路）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "100.0"))
	float MinFootprint = 200.0f;

	/** 相对地面参考高度的抬升；落座公式 = max(footprint 地面高度) + HeightOffset。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	float HeightOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Material")
	TObjectPtr<UMaterialInterface> WallMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Material")
	TObjectPtr<UMaterialInterface> RoofMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Material")
	TObjectPtr<UMaterialInterface> PillarMaterial;

	// -------------------------------------------------------------------------
	// Door（边缘线段分割制，D6）
	// -------------------------------------------------------------------------

	/** 子段目标间距 cm：N = round(可用长 / 此值)，实际段长 = 可用长 / N（等分，拱宽随之微伸缩）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "60.0"))
	float DoorPitchTarget = 150.0f;

	/**
	 * 相邻拱之间保留的墩宽 cm（拱宽 = 段长 − 墩宽，再乘离地收窄）。
	 *
	 * ⚠️ 默认值**钉在 `FrameBrickDepth` 上**，不是随手取的：墩上那一列砖骑在墩心、横向占
	 * `FrameBrickDepth` ⇒ 墩宽超过它，墩两侧就会露出 `墩宽 − FrameBrickDepth` 的可见缝
	 * （灰泥已经被 `CSHouse_PierClipField` 裁掉了，缝里直接透到背景）。
	 * 见 `bPierStyleEnabled` 的字段注释。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float PierWidth = 20.0f;

	/**
	 * 连续拱之间的窄残料按**墩**处理：起拱线以下那片灰泥被裁掉，只剩门框砖站着。
	 * 关掉即退回"整跨都是灰泥墙"的旧观感。
	 *
	 * ⚠️ **裁掉 ≠ 不生成**（2026-08-30 裁决三，全项目架构不变量）：面板照常砌成实心盒，
	 * 那片灰泥由 `CSHouse_PierClipField` 在像素阶段 discard。几何永远实心，烘成 StaticMesh
	 * 之后洞仍由材质切出（裁决六）。
	 *
	 * ⚠️ **墩上是一列砖，不是两列**（2026-08-30 随解析推导一起改）：旧路让两侧的拱各出一条
	 * 门樘砖脚、各伸进跨度一半，于是两列砖在**墩正中共面对接**，从地面一直贯着一条竖缝
	 * （出图 `pier_after_pier.png` 可见），而 TG 实拍里墩就是一列。现在墩由
	 * `CSHouseFrame::MakePierPath` 单独出一条竖直砖路，两侧的拱都不出那一侧的门樘。
	 *
	 * ⚠️ **墩宽与砖进深仍是一对必须配平的参数**：那一列砖骑在墩心、横向占 `FrameBrickDepth`
	 * ⇒ 跨度 ≤ `FrameBrickDepth` 时正好盖满整条跨度（实拍里那种"一块砖宽的墩"），再宽下去
	 * 墩两侧会露出 `跨度 − FrameBrickDepth` 的**可见缝**（渲染层的缝，不是几何洞，但一样难看）。
	 * 条件与旧路"两条砖脚各伸进一半"给出的完全相同，所以 `PierWidth` 的默认值照旧钉在
	 * `FrameBrickDepth` 上，回归里那条断言继续有效，别把两者之一单独调走。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door")
	bool bPierStyleEnabled = true;

	/** 残料跨度 ≤ 此值 ⇒ 转墩（双阈迟回的低阈）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float PierStyleMaxWidth = 60.0f;

	/**
	 * 已是墩的跨度 ≥ 此值才转回灰泥墙（双阈迟回的高阈）。
	 *
	 * 与门拱点亮同一条纪律：跨度是拉尺寸/离地收窄的连续函数，单阈会让样式在阈值附近来回切换。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float PierStyleRestoreWidth = 75.0f;

	/** 每面墙两端的护角保留 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float CornerMargin = 60.0f;

	/** 拱顶目标高 cm（受 墙高 − 过梁带 约束）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "80.0"))
	float DoorHeight = 220.0f;

	/** 拱上方保留的过梁带 cm（保证墙顶连续）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float LintelBand = 40.0f;

	/** 沿子段的道路采样步长 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "5.0"))
	float DoorSampleStep = 25.0f;

	/** 墙线内外两条探测线的偏移 cm（取两者较大道路权重——路铺到墙根即算经过）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float DoorProbeOffset = 30.0f;

	/** 单个采样点计为"有路"的顶点色 R 权重阈值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float DoorOnWeight = 0.5f;

	/** 子段点亮的覆盖率阈值（滞回高阈）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float SlotOnCoverage = 0.6f;

	/** 已点亮子段熄灭的覆盖率阈值（滞回低阈）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlotOffCoverage = 0.4f;

	/** 离地收窄：落差 ≤ 此值门全宽 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float DoorGapFull = 30.0f;

	/** 离地收窄：落差 ≥ 此值门完全消失 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float DoorGapZero = 120.0f;

	/** 拱宽下限 cm，低于即不点亮。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float DoorMinWidth = 40.0f;

	/** 拱宽进哈希前的量化步长 cm——地形微抖不触发全量重建。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.5"))
	float DoorWidthQuantum = 2.0f;

	// -------------------------------------------------------------------------
	// Opening（洞 = 原型剖面 + 摆位，D4/D8）
	// -------------------------------------------------------------------------

	/**
	 * 洞缘曲线的弦高容差 cm：分段数由它反解（N = ceil(π / 2·acos(1 − Tol/R))），
	 * 所以房子放大时折角**不会**跟着线性放大 —— 写死段数才会。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Opening", meta = (ClampMin = "0.02", ClampMax = "5.0"))
	float OpeningChordTolerance = 0.2f;

	/** 两个洞的面板格之间必须留出的净距 cm（同边一维 S 区间按它膨胀后相交即判冲突）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Opening", meta = (ClampMin = "0.0"))
	float OpeningClearance = 10.0f;

	// -------------------------------------------------------------------------
	// Window（D8：诉求走显式列表，房子照旧只认谓词）
	//
	// ⚠️ **本节不含任何"自动填窗"的规则，也不许加。** TG 的窗全是玩家手放的，本项目的放置 UI
	// 还没做、「门洞触发规则」又是唯一没拍板的一条 —— 窗的来源就停在 `Windows` 这份列表上。
	//
	// 窗与门共用**同一条**通路：同一张 openings 表、同一个 clip 场、同一条谓词、同一套门框砖。
	// 窗多出来的只有两件事：① 洞底离地（`Z0 > 0`）⇒ 房体在洞面板下面另砌一块无 clip 的实心
	// 窗台盒；② 洞的**下边界**也是一条 clip 边 ⇒ 砖路多出第四段（`CSHouseFrame::FPath::bSill`）。
	// -------------------------------------------------------------------------

	/** 关掉即整份列表不出洞（出图脚本靠它拍"同机位只切开关"的对照图）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Window")
	bool bWindowsEnabled = true;

	/** 窗户诉求列表。属性面板 / Blueprint / 测试都往这里填；被拒的条目留着，只是这一轮不出洞。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Window")
	TArray<FCSHouseWindow> Windows;

	/**
	 * 窗台下限 cm：洞底低于它的**窗**判 `SillTooLow`。
	 *
	 * 谓词原本只判 `Z0 < 0`（几何合法性），而窗台贴地的窗在几何上完全成立、观感上是个门洞。
	 * 门不受这条约束 —— 门恒 `Z0 = 0`，拿它卡门等于把所有门都拒了（判据里按 `Type` 分流）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Window", meta = (ClampMin = "0.0"))
	float WindowMinSillZ = 40.0f;

	// -------------------------------------------------------------------------
	// Frame（门框砖：洞缘那圈砌块）
	//
	// **这是 clip 路线的配套件，不是可选装饰**：discard 只丢像素、不生成表面，而判据沿墙厚
	// 方向恒定 —— 洞的范围内外脸、内脸、上下盖会被同时弃掉，中间不剩任何面，从洞口一眼看穿墙。
	// TG 用的就是这一手：拱/楣是与墙砖并列的真实实例（flags&32：按拱高压扁贴合曲线 + 免拱裁剪），
	// 用来遮住裁剪断口。
	//
	// 网格用 TG 提取出来的那块 brick —— 整个游戏只有这一块砖，是个 100³ 的**居中单位立方体**，
	// 尺寸全靠逐实例非均匀缩放（逆向报告 §1.4/§1.6）。所以这里给的是"想要多大"，不是"选哪块"。
	// -------------------------------------------------------------------------

	/** 是否砌门框。关掉会直接露出 clip 断口，只在调试判据时才该关。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	bool bFrameEnabled = true;

	/** 门框砖的字典网格（TG 的 /Game/TinyGlade/Meshes/brick）。留空则不砌。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	TObjectPtr<UStaticMesh> FrameBrickMesh;

	/** 门框砖材质。GPU 实例化只有一个材质槽，必须勾 "Used with Instanced Static Meshes"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	TObjectPtr<UMaterialInterface> FrameMaterial;

	/** 一块砖沿洞缘曲线的长度 cm（铺装缩放会在此基础上微调，让整条恰好铺满）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "4.0"))
	float FrameBrickLength = 26.0f;

	/** 砖沿面内径向的尺寸 cm —— 也就是砌块伸进墙体的那一维。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "2.0"))
	float FrameBrickDepth = 20.0f;

	/** 砖穿过墙厚的尺寸 cm。留 0 = 用墙厚（砖正好填满断口，两侧各露一个面）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "0.0"))
	float FrameBrickThickness = 0.0f;

	/**
	 * 相邻砖之间的**排布缝** cm（随铺装缩放一起缩）。默认 0：净缝整个交给 FrameBrickBloat 出。
	 *
	 * 净缝 = FrameBrickLength × (1 − FrameBrickBloat) + FrameBrickGap，**必须为负**，理由见下。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "0.0"))
	float FrameBrickGap = 0.0f;

	/**
	 * 砖沿曲线（长度轴）的**胀大系数**：渲染尺寸 = FrameBrickLength × 本值，而排布位置一步不动
	 * ⇒ 相邻两块必然互相穿插，**砖缝是负的**。小于 1 会被夹回 1 —— 正缝正是它要消掉的那个缺陷。
	 *
	 * 这是照抄 TG 的一步，也是它"逐帧重排砖却完全看不出跳变"的物理原因：
	 * `_wall_wall_brick_lod0...vs_main.glsl:160-175` 把逐实例缩放乘在单位立方体的局部坐标上，
	 * `flags & 4` 置位、`flags & 1` 未置位那一支就是**水平两轴 ×1.1、竖直 ×1.0**。
	 * 砖数一变只是穿插量微调；**正缝**会在块数跳变的那一帧沿整条拱缘露出一条缝
	 * （旧默认 FrameBrickGap = 1.5 就是正缝，`Saved/TinyGladeShots/lit_after_archframe.png`
	 * 里一格一格断开的拱缘就是它）。
	 *
	 * 轴的对位 —— 本项目没有 TG 那套 flags，简化成一条规则、只胀长度轴：
	 *   · 长度轴（+Y，沿洞缘曲线，见 CSHouseFrame.usf 的基约定）对应 TG 的"沿砌层水平轴"，
	 *     是**唯一有邻居、唯一会露缝的轴** —— 必须胀。
	 *   · 穿墙轴（+Z）钉死在墙厚上（砖正好封住 clip 断口）、径向轴（+X）是拱缘的可见带宽，
	 *     两者都不与任何邻居相接：胀它们只会漂移一个被刻意标定过的尺寸，正是 TG 让竖直轴
	 *     ×1.0 的那条理由（"砖一层层码上去，竖直也胀会让层高漂移"），所以都留 1.0。
	 * TG 那条 10% "整块不胀"的抖动**没抄**：它是给成千上万块墙砖打散规律用的，摊到一条
	 * 十几块砖的拱缘上就是随机两块露出可见缝，还会直接推翻 House.FrameBrickOverlap 那条
	 * "任何块数下都不露缝"的断言 —— 而那条断言才是本字段存在的全部意义。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "1.0", ClampMax = "1.5"))
	float FrameBrickBloat = 1.1f;

	/** 铺装随机种子；同参数同结果（洞的 Tag 与边号参与派生）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	int32 FrameSeed = 1;

	/**
	 * Reserved instance capacity for the door-frame bricks.
	 *
	 * Paid once at registration so the interactive path never reaches the growth branch:
	 * the allocation has to happen on the render thread, so growing always blocks, and which
	 * brush dab it lands on depends entirely on where the user happens to paint — a stall that
	 * is both hard to reproduce and hard to attribute. One brick is 5 float4 = 80 bytes, so 512
	 * bricks cost 40 KB. Growth still happens if a house genuinely lays more than this.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "64"))
	int32 FrameReserveCapacity = 512;

	// -------------------------------------------------------------------------
	// Seam（D7 接缝，2026-08-30 裁决二）—— 纯函数，零共享状态
	//
	// 两栋房 footprint 真重叠时**只**产生两样东西：轮廓交点上的接缝砖柱，以及把插进邻居
	// 房间里的那截墙抹掉的裁剪场。除此之外两栋房的任何内容都保持独立 —— 没有接缝 actor、
	// 没有归属、没有跨房簿记、没有撤销。算法与"为什么两栋房各画一份是有意的"见 CSHouseSeam.h。
	//
	// 接缝砖**共用门框砖那一个组件**（`FrameComponent`）：TG 全库也只有一块 `brick`，
	// 而且这样容量、交接、剔除球、`SaveToStaticMesh` 出口、以及"材质勾没勾
	// bUsedWithInstancedStaticMeshes"那条执行面判据全都白拿。砖数因此共享 `FrameReserveCapacity`。
	// -------------------------------------------------------------------------

	/**
	 * 关掉即**这栋房**不出接缝（它那一份砖与裁剪一起没）。
	 *
	 * 出图脚本靠它拍"同机位只切开关"的对照图。
	 *
	 * ⚠️ **它是"我画不画我这一份"，不是"这条缝存不存在"** —— 一栋房**不读**邻居的这个开关。
	 * 读了的话几何就取决于两栋房谁先重建（实测：分两句改开关时，先重建的那栋看到对方的旧值，
	 * A 报 0 根缝而 B 报 2 根），而顺序无关正是本模块要保证的东西。裁决二列的输入里也没有它。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Seam")
	bool bSeamEnabled = true;

	// -------------------------------------------------------------------------
	// Pillar（承重柱，D9——独立组件，不进房体网格）
	// -------------------------------------------------------------------------

	/** 底边支撑点间距 cm（四角必有）。 */
	// -------------------------------------------------------------------------
	// 藤蔓（D13）—— 墙矩形 → 点集 → 填 ISM
	//
	// ⚠️ **既有的 `VineScatter` 三个入口对房子一个都用不了**：`CollectSurfaceTriangles`
	// （`CSVineScatter.cpp:38`）只遍历 `UStaticMeshComponent`，而房体挂在
	// `UCSMeshRenderComponent` 上 ⇒ 它**静默返回空三角集、不报错**。这条是新写的通路。
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	bool bVineEnabled = true;

	/** 枝：`ivy_branch`。⚠️ 它**只有 `Vertex_Position`**，法线与 UV 由
	 *  `CSHouseVine::BuildBaseMesh` 现补（见那里的注释），换别的网格前先读那一段。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UStaticMesh> VineBranchMesh;

	/** 叶：`ivy_leaf`。自带法线与 UV，只做换轴。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UStaticMesh> VineLeafMesh;

	/** ⚠️ 必须勾了 `bUsedWithInstancedStaticMeshes` —— 没勾的话引擎**静默退回默认材质**，
	 *  画面上是一片灰而所有 readback 断言照绿（石阶那个坑的同一条）。
	 *  `MI_ivy_branch_color` 的母材质 `M_TG_Texture` 恰恰没勾，所以本项目自建了
	 *  `M_TinyGladeIvyBranch`（`Scripts/TinyGladeMakeIvyMaterials.py`）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UMaterialInterface> VineBranchMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UMaterialInterface> VineLeafMaterial;

	/** 花：`ivy_flower`。实测包围盒 (76.04, 77.99, 38.40)、底面在 Z = 0 ⇒ 长度轴已经是 **+Z**，
	 *  `BuildBaseMesh` 的换轴参数取 2（恒等）。留空即不长花。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UStaticMesh> VineFlowerMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UMaterialInterface> VineFlowerMaterial;

	/** 叶子的季节。只写母材质上的 `Season` 标量，不换材质资产（见 `ECSVineSeason` 的注释）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	ECSVineSeason VineSeason = ECSVineSeason::Summer;

	/** 沿墙每隔多少 cm 起一根藤。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "20.0"))
	float VineStrandSpacing = 90.0f;

	/** 一段的世界长度 = 一个 `ivy_branch` 实例的长度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "5.0"))
	float VineSegmentLength = 26.0f;

	/** 一根藤最多几段。与 `VineStrandSpacing` 一起决定容量上限，所以它**必须有上界**：
	 *  容量是按配置一次预留的，交互期不许扩容（零阻塞纪律）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "1", ClampMax = "128"))
	int32 VineMaxSegments = 22;

	/** 每段方向的随机扰动（弧度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	float VineWander = 0.55f;

	/** 相对"正上"的最大偏角（弧度）。撞到上界时倾角**镜像**而不是夹死 —— 夹死会让藤沿墙角
	 *  笔直爬一长条，一眼看出是程序生成的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.1", ClampMax = "1.5"))
	float VineMaxLean = 1.15f;

	/** 长度轴胀大系数。**与 `FrameBrickBloat` 同一条 TG 实证**：正缝会在藤的每个折点露出
	 *  一条亮缝，而折点恰恰最显眼；胀大之后相邻两段必然互穿，段数一变只是穿插量微调。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "1.0", ClampMax = "1.6"))
	float VineBloat = 1.15f;

	/** 枝的截面直径（世界 cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "1.0"))
	float VineThickness = 9.0f;

	/** 沿墙面法线离墙多远，避免与墙面 z-fighting。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0"))
	float VineStandOff = 3.0f;

	/** 墙洞外扩多少才算"撞上"。TG 的 `ivy_grower` 读集里有 `PrevWallHoles`，藤是避让洞的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0"))
	float VineHoleClearance = 12.0f;

	/** 每段长叶子的概率。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineLeafChance = 0.72f;

	/** 叶片的世界长度与它的对称抖动。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "2.0"))
	float VineLeafSize = 26.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float VineLeafSizeJitter = 0.35f;

	/** 段的开花概率。**只在 `VineFlowerFromFrac` 以上的段上掷** —— 贴地那一圈的花会被地形与
	 *  杂物挡住，是纯白付的实例。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineFlowerChance = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineFlowerFromFrac = 0.45f;

	/** 花簇的世界**宽度**。高度 = 宽度 × `CSHouseVine::FParams::FlowerAspect`（实测高宽比）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "2.0"))
	float VineFlowerSize = 22.0f;

	/** 走到墙角时拐上相邻那面墙的概率（TG 的 `check_for_wall_jump`）。0 = 只镜像折返。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineJumpChance = 0.5f;

	/** 让藤爬上山墙三角（墙顶随屋面斜边升高）。关掉就一律停在檐口高度（第一档行为）。
	 *  ⚠️ 只有**山墙**那两面会因此变高；檐墙的墙顶本来就是平的，它上面是屋面板不是墙。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	bool bVineClimbGable = true;

	/** 逐实例随机的用户种子。⚠️ 随机**只由 (墙号, 藤号, 段号, 种子) 决定**，不取
	 *  `InterlockedAdd` 的槽位 —— 槽位每次重扫都重掷，症状是"重建一次全场变色"，
	 *  而且不会有任何断言报红（S1 已经栽过一次）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	int32 VineSeed = 1;

	// -------------------------------------------------------------------------
	// 装饰摆件（D12 的**锚点那一半**）—— 围着已有构件长，不是在地面上找空地
	//
	// ⚠️ 计划 D12 写的是「复杂度场 `RT_DecorField` + tile-argmax」，而 TG 里**没有对位物**
	// （它是七家锚点生产者 + 候选点烘在资产里）。两套方案的取舍是**挂起的决策 C2**，
	// 这里**只做锚点这一半**、不实现场、也不替 C2 下结论。详见 `CSHouseDecor.h` 的文件头。
	//
	// 每一家有自己的一组网格（TG 的对位物是每个生产者各自的 `ClutterMeshes` 读集）：
	// 一张网格 = 一个 palette 条目 = 一个实例化组件。**留空即这一家不长任何东西**。
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	bool bDecorEnabled = true;

	/** 门/拱两侧与门前引道两侧（TG 的 `add_autoclutter_around_gates`）。桶、箱、摊子这一类。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TArray<TObjectPtr<UStaticMesh>> DecorGateMeshes;

	/** 墙脚。柴垛、篮子、农具这一类。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TArray<TObjectPtr<UStaticMesh>> DecorWallFootMeshes;

	/** 檐口与屋脊（TG 的 `add_birdnests`）。鸟窝、鸟屋这一类。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TArray<TObjectPtr<UStaticMesh>> DecorRoofMeshes;

	/**
	 * 摆件材质（三家共用一张）。
	 *
	 * ⚠️ **母材质必须勾 `bUsedWithInstancedStaticMeshes`**，否则引擎在实例路径上会
	 * **静默换成默认材质**，症状与"没绑材质"逐像素相同（一片灰），而所有 readback 断言照绿。
	 * `IsDecorDrawable()` 把这条做成了显式判据；供给侧是 `Scripts/TinyGladeMakeDecorMaterial.py`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TObjectPtr<UMaterialInterface> DecorMaterial;

	/** 墙脚每隔多少 cm 一个锚点。**摆件的多少完全由锚点个数决定** —— 这是锚点法与场法最本质的
	 *  差别（TG：有几扇窗就有几个花箱位）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "40.0"))
	float DecorWallFootSpacing = 150.0f;

	/** 檐口每隔多少 cm 一个锚点（屋脊按它的 1.13 倍，见 `CSHouseDecor::FParams`）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "40.0"))
	float DecorEaveSpacing = 220.0f;

	/** 摆件之间的最小间距 cm（球测）。计划 D12 的「同类间距球」在只有一层锚点时退化成这一条。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.0"))
	float DecorMinSpacing = 90.0f;

	/** 道路权重过阈的锚点直接丢掉（TG 的 `PathRaster` mask 订阅）。摆件因此自动分列路的两侧。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DecorRoadReject = 0.3f;

	/**
	 * 整体缩放。**0.5 是量出来的不是猜的**：`clutter/` 那 58 张网格的包围盒实测在
	 * 100–250 cm 之间（`barrel` 106×106×175、`stall_veggies` 206×122×148、`birdnest` 104×97×36），
	 * 而本工程的墙高默认 300 cm —— 原尺寸摆上去一个桶就有半堵墙高。
	 * 枚举值写在这里而不是 `CSHouseDecor::FParams`：模块侧不该知道资产多大（同藤蔓的
	 * `VineThickness`）。换别的网格前先量一遍包围盒。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.05"))
	float DecorScale = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.0", ClampMax = "0.8"))
	float DecorScaleJitter = 0.16f;

	/**
	 * 逐实例随机的用户种子。⚠️ 随机**只由 (家族, 锚点 id, 种子) 决定**，不取 `InterlockedAdd`
	 * 的槽位 —— 槽位每次重扫都重掷，症状是"重建一次全场变色"，而且不会有任何断言报红
	 * （S1 已经栽过一次）。这个身份同时是将来 `DeletedAutoClutter` 抑制集的键，见 `CSHouseDecor.h`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	int32 DecorSeed = 7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "50.0"))
	float PillarSpacing = 250.0f;

	/** 悬空判定阈值 cm：支撑点落差超过它才生柱。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "1.0"))
	float PillarMinGap = 10.0f;

	/** 方柱截面边长 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "5.0"))
	float PillarSize = 30.0f;

	/** 柱脚扎入地面的深度 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "0.0"))
	float PillarEmbed = 5.0f;

	// -------------------------------------------------------------------------
	// Ground wiring
	// -------------------------------------------------------------------------

	/** 交互的地面。为空时 PostRegisterAllComponents 自动找关卡里第一个 ACSGroundActor。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	TObjectPtr<ACSGroundActor> Ground;

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/** 清空全部哈希与滞回状态后全量重求值（调参后的手动强刷入口）。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS House")
	void RebuildHouse();

	/** 声明式重求值：落座 → 门拱 → 柱，两份 desc 各自哈希守卫。幂等，可被高频调用。 */
	virtual void ReevaluateSite() override;

#if WITH_EDITOR
	/**
	 * 把本房子**全部**实例路产物烘成 StaticMesh 资产 —— 裁决六 ① 在房子这一侧的用户入口。
	 *
	 * 一族一张：门框砖（含接缝砖，两者共用一个组件）/ 藤枝 / 藤叶 / 藤花 / 每个摆件 palette。
	 * 资产落在 `BakeFolder/SM_<actor>_<family>`；返回真的烘出来的张数（这一族没有实例就跳过，
	 * 跳过不算失败 —— 一栋不长花的房子是合法的）。
	 *
	 * ⚠️ **阻塞**（每族两次回读 + 一次 StaticMesh 构建），而这是**有意**的：它是用户主动发起的
	 * 离线操作，不在任何交互路径上。它照旧被 `UCSMesh::GetBlockingFlushCount()` 数到，
	 * 十一条 `flushes=0` 断言会在它被误接进重建链路的那一刻报红。
	 *
	 * 房体 / 柱那两条走的是网格路（`UCSMeshRenderComponent::SaveToStaticMesh`），不在这里。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	int32 SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets = false);
#endif

	/**
	 * 单边推拉（计划 D5 的**机制入口**）：把第 EdgeIndex 面墙沿它的世界外法线推 Offset cm，
	 * 对侧墙世界位置不变、中心随动，然后立刻重求值。返回**实际**生效的位移。
	 *
	 * ⚠️ 这一轮**不做**抓手 / gizmo / EdMode（与 D8 窗户同一条纪律）。这里是"尺寸连续变化时
	 * 派生物跟得住"的那套机制的唯一入口，将来的 handle actor 也只是往这里喂 Offset。
	 *
	 * 三件事都在这条路上一次做完，分开做就会各漏一样：
	 *  ① 禁带 + `MinFootprint`（`CSHouse_ApplyEdgePush`，纯函数、可单测）；
	 *  ② `UCSHouseSubsystem::MarkHouseDirty` —— 拖动期不必等 0.25 s 的兜底快扫；
	 *  ③ `ReevaluateSite()` —— 走的是与平移完全相同的那条零阻塞路径，不另开快路。
	 *
	 * `bFinished` 传 true 表示"松手"：等价于 gizmo 的 `PostEditMove(bFinished=true)`，
	 * 会置 `bForceFullRebuild` 把拖动期为了不阻塞而留下的容量/包围盒余量重新收紧。
	 *
	 * ⚠️ **历史：这里曾经挂着一条「拖动中的画面 ≠ 松手后的画面」的未结案缺陷**，
	 * 2026-08-31 已定位并修掉。当时的现象：同一份世界状态下两图差 6.3–9.0% 像素、
	 * 背光墙整片压到精确 (0,0,0)，而 CPU 侧每个量都相同、零像素位移。
	 * **真因是两条互相独立的缺陷，都不在拉尺寸这条路上**：
	 *  ① `CSMeshOps.usf` 的 `TransformMeshCS` / `NegateNormalsCS` 用 **uint 的逻辑右移**解包
	 *     snorm8 切空间，把每个分量的符号剥掉了 ⇒ 凡是被 `ApplyBodyPlacement` 增量搬过的
	 *     网格，法线全部折进 +X+Y+Z 卦限，朝 −X/−Y 的墙当场翻面。修法见
	 *     `UnpackSnorm8888`。（拖动归位、平移、快扫唤醒都走那条增量路，所以不只拉尺寸中招。）
	 *  ② `RebuildFrame` 的早退哈希漏了墙框架，拉垂直方向时门框砖整段不重排。
	 *     修法见 `CSHouse_HashElementFrames`，判据见 `GetFrameScatterCount`。
	 * 修完同机位实测：拖动态 vs 松手态 6.83% → **0.013%**，`SCS_BASE_COLOR` 与关掉 Lumen
	 * 两组都是 **0.000%**（`Scripts/TinyGladeShotResizeProbe.py`）。
	 *
	 * 所以「画面收敛以松手为准」**不再成立** —— 拖动中的画面就是对的。
	 * 但 `bFinished` 仍然该传：它是把拖动期为了不阻塞留下的容量/包围盒余量收紧的唯一入口，
	 * 只是它不再背着一条画面正确性的债。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	float PushEdge(int32 EdgeIndex, float Offset, bool bFinished = false);

	/** 当前禁带在给定边上的合法区间（出图 / 断言用；X 是下沿、Y 是上沿）。带关掉时两者相等。 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	FVector2D GetFootprintBandRange(int32 EdgeIndex) const;

	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetOpenDoorCount() const;

	/** 当前洞的总数（门 + 窗 + 注入）。 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetOpeningCount() const { return CurrentOpenings.Num(); }

	/**
	 * 稳定身份。subsystem 的注册表以它为 key，D7 的接缝 key 也用它（两房 GUID 的无序对）——
	 * 指针不行：接缝要能在 actor 重建/流送进出之后仍指向"同一栋房子"。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	FGuid GetHouseId() const { return HouseId; }

	/**
	 * 兜底快扫比对的量：量化世界变换 + footprint + 檐口高。
	 *
	 * 只包含**别人会关心的**状态（摆位与占地），不含门集合 —— 门是房子自己的派生物，
	 * 由 ReevaluateSite 内部的形状哈希守卫，混进来只会让快扫在门变化时白唤醒一次。
	 */
	uint32 GetTrackingHash() const;

	/**
	 * 这个洞放得下吗（D8 的可行性谓词，纯参数判定、零 GPU 回读）：与任一已有洞的面板格按
	 * OpeningClearance 膨胀相交即拒绝（**同边一维 S 区间**，Z 不参与 —— 用户裁决 2026-08-30，
	 * C1 选甲：永久放弃"门上开窗"），落在墙面之外或超出墙高也拒绝。
	 * 门拱优先于特征标记 —— 已点亮的子段先占位，窗再来就判不可行。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	bool QueryFeaturePlacement(const FCSWallOpening& Candidate) const;

	/**
	 * 同一条谓词，但**把拒绝原因给出来**（计划 D8 的 `FCSFeaturePlacement::Reason`）。
	 *
	 * 为什么必需而不是锦上添花：门拱优先于特征标记（D6），一面墙被道路点亮成连拱时窗
	 * **永远**放不进去 —— 那是预期行为，但用户看到的是"窗放上去就消失"。没有回执，
	 * 这两件事在画面上逐像素相同。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	ECSFeatureReject QueryFeatureReject(const FCSWallOpening& Candidate) const;

	/** 这一轮真正砌出来的窗洞数（`Windows` 里过了谓词的那些）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window")
	int32 GetWindowCount() const { return CurrentWindowCount; }

	/** 这一轮被谓词拒掉的窗诉求数。**必须与上一条一起看** —— 只看前者分不清"没填"与"被拒"。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window")
	int32 GetWindowRejectCount() const { return CurrentWindowRejectCount; }

	/**
	 * **诊断 / 验收专用**：窗这一帧到底会不会出现在画面上，不会的话原因是什么。
	 *
	 * ⚠️ 执行面照抄 `IsVineDrawable` / `IsDecorDrawable`（本项目在"readback 全绿而画面上
	 * 什么都没有"这件事上栽过两次），但窗多一环、且那一环是窗**独有**的致命项：
	 * **洞是墙材质用 OpacityMask 逐像素切出来的** —— 墙材质一旦不是 Masked，洞在画面上
	 * 根本不存在，而洞数、砖数、三角形数、零阻塞四条断言**全部照绿**。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window", meta = (DevelopmentOnly))
	bool IsWindowDrawable(FString& OutReason) const;

	/** 同上，但把原因当返回值给出来（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window", meta = (DevelopmentOnly))
	FString GetWindowUndrawableReason() const;

	/** 这一轮与几个邻居交汇（= 交点数，一个交点一根接缝砖柱）。0 = 没和谁碰上。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetSeamCornerCount() const { return CurrentSeamCornerCount; }

	/** 接缝砖数（含在 `GetFrameBrickCount()` 里 —— 两者共用一个组件与一份容量）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetSeamBrickCount() const { return CurrentSeamBrickCount; }

	/** 这一轮被接缝抹掉的墙段数（clip，不是几何洞）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetSeamCutCount() const { return CurrentSeamCuts.Num(); }

	/**
	 * 接缝画得出来吗（**执行面**判据，不是数值判据）。
	 *
	 * ⚠️ 形状照抄 `IsWindowDrawable`，理由也一样：接缝有**两半**，两半各自能静默失效 ——
	 * 洞那一半根本不是几何（墙材质不是 Masked ⇒ 一段墙都没抹掉），砖那一半走 GPU 实例路
	 * （母材质没勾 `bUsedWithInstancedStaticMeshes` ⇒ 引擎静默换默认材质）。两种情形下
	 * 交点数 / 砖数 / 裁剪段数三条数值断言**全部照绿**。本项目在这上面栽过两次。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam", meta = (DevelopmentOnly))
	bool IsSeamDrawable(FString& OutReason) const;

	/** 同上，但把原因当返回值给出来（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam", meta = (DevelopmentOnly))
	FString GetSeamUndrawableReason() const;

	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetPillarCount() const { return CurrentPillarCount; }

	/** 当前砌出的门框砖总数。CPU 侧本来就排好了记录，这个数不需要回读 GPU。 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetFrameBrickCount() const { return CurrentFrameBrickCount; }

	/**
	 * `RebuildFrame` **真的重排过几次砖**（单调递增，不序列化）。
	 *
	 * ⚠️ 这是「砖跟没跟上」的唯一无头判据，砖数**答不了**这个问题：拉尺寸时砖数逐位不变
	 * （洞集合没变），而砖该去的地方已经变了。2026-08-31 实测的缺陷正是这个形状 ——
	 * 连拉 8 帧只重排 1 次，而 `GetFrameBrickCount()` / GPU 回读 / 三角形数 / 零阻塞
	 * **四条断言全绿**，只有像素看得见（拱圈上一块砖都没有）。
	 * 拿它写断言时要判**增量下界**（"拉 N 帧至少重排 N−k 次"），不要判等号：
	 * 哈希短路吸收无效唤醒是**设计**，同一帧被唤醒两次只该重排一次。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics")
	int32 GetFrameScatterCount() const { return FrameScatterCount; }

	// -------------------------------------------------------------------------
	// GPU 侧真值（**诊断 / 验收专用，阻塞**）
	//
	// ⚠️ **绝对不许进交互路径**：回读就是阻塞，且每一次都被 `UCSMesh::GetBlockingFlushCount()`
	// 数到 —— 十一条 `flushes=0` 断言会在误用的那一刻报红，那是有意的警戒线。
	//
	// 为什么非要有这一条：上面那些 `GetXxxCount()` 全是 **CPU 侧的记录**。本项目已经出现过
	// 「CPU 说 0、GPU 说 12」—— 砖数掉到 0 时实例源被撤走、交接缓存被清空，下一轮
	// `EnsureFrameComponent` 又把同一批 buffer 交回去，**带着陈旧的计数器**，画面上 12 层砖
	// 原样立着，而砖数 / 三角形数 / 零阻塞所有无头断言全绿。两个数必须能被同一条断言比出来。
	// -------------------------------------------------------------------------

	/** 门框砖 + 接缝砖在 GPU 上的实例计数器（两者共用一个组件）。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadFrameBrickCountGpuSync() const;

	/** 藤枝在 GPU 上的实例计数器。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadVineBranchCountGpuSync() const;

	/** 藤叶在 GPU 上的实例计数器。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadVineLeafCountGpuSync() const;

	/** 同上，花那一路。⚠️ 没配 `VineFlowerMesh` 时它恒为 0 —— 那是合法状态，不是缺陷。 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Debug", meta = (DevelopmentOnly))
	int32 DebugReadVineFlowerCountGpuSync() const;

	/** 摆件在 GPU 上的实例计数器（所有 palette 组件合计）。−1 = 任一组件读不到。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadDecorInstanceCountGpuSync() const;

	/**
	 * 本房子每一条 GPU 实例路上，**GPU 真的在画的基础网格 / 材质**是不是我们以为的那两样。
	 * 空串 = 是。原因串带家族前缀（门框 / 藤枝 / 藤叶 / 摆件[i]）。
	 *
	 * 与 `IsVineDrawable` 那一族的分工：那边查"能不能画"（组件在不在、注册没注册、非空没非空），
	 * 这边查"画的是不是那个" —— 网格那一半是把上传到 GPU 的那份**回读出来**跟资产对，
	 * 不是信任 CPU 快照。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	FString DebugGetGpuAssetMismatchSync() const;

#if WITH_EDITOR
	/**
	 * **自动化测试专用**：把门框砖（含接缝砖，同一个组件）走真正的
	 * `UCSGpuInstancedMeshComponent::SaveToStaticMesh` 出口烘成资产，再从烘出来的
	 * `GetMeshDescription(0)` 里数回来 —— 裁决六 ①②的判据本身，抄的是岩壳那条
	 * `RockShell.CapSkirtSurvivesBake` 的形状（走**组件自己的出口**，不另拼等价路径）。
	 *
	 * `OutDistinctBakedRandoms` 是烘焙件顶点色 **A** 里出现过的不同取值个数，也就是
	 * `CSInstanceRandom` 这条通道（字典见 `UCSGpuInstancedMeshComponent` 类注释）。
	 * ⚠️ **只有一种取值 = 整片同色**，而那正是"烘完就没有 PerInstanceRandom 了"这条缺陷的
	 * 全部症状 —— 它不报错、不掉三角、不改包围盒，只有这个数看得见。
	 *
	 * `bOutRandomsMatchGpu` 把烘出来的那组取值与 **GPU 上 packed 行的 `Origin.w`** 逐个对，
	 * 两边各算一次哈希再比"看着都挺随机"是没有意义的断言。
	 */
	bool DebugBakeFrameBricksSync(const FString& AssetPath, int32& OutTriangles, int32& OutVertexInstances,
		int32& OutUVChannels, int32& OutDistinctBakedRandoms, int32& OutGpuInstanceCount, bool& bOutRandomsMatchGpu);

	/**
	 * **出图 / 验收专用**：把藤枝走真正的 `SaveToStaticMesh` 出口烘成资产并返回它。
	 *
	 * 为什么挑藤枝做出图对照而不是门框砖：`M_TinyGladeBrick` 是一张**纯常数**材质，
	 * 它读不到逐实例随机 ⇒ 拿它对照，"通道丢没丢"这件事在像素上根本不显影，门是假的。
	 * 藤枝材质有 0.80~1.15 的逐实例明暗，通道一断整墙藤就塌成同一个色 —— 那才是活门。
	 *
	 * 烘回**本 actor 的局部空间**，所以把资产挂在一个摆在同一变换上的 StaticMeshActor 上，
	 * 就该与实例版逐像素重合。⚠️ 阻塞 + 建资产，只给出图与测试。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	UStaticMesh* DebugBakeVineBranchesSync(const FString& AssetPath);

	/**
	 * 把藤枝的实例组件藏起来 / 放出来。出图对照必须"同机位、只换渲染路径"，
	 * 两条路一起画的话差异率永远是 0，门看着绿其实什么都没测。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	// 参数不叫 bHidden：AActor 的作用域里已经有一个同名的（UHT 不许遮蔽）。
	void DebugSetVineBranchInstancesHidden(bool bHideInstances);
#endif

	/**
	 * 这一轮判为**墩**的残料跨度个数（不含判为墙的那些）。
	 *
	 * 迟回是路径依赖的，只有把结论暴露出来才断言得了"跨度在 60/75 之间来回时样式不切换" ——
	 * 从外面只看得见三角形数，而三角形数还同时被拱宽、洞数、屋面拖着动。
	 *
	 * ⚠️ 定义就写在声明处（同 `GetPillarCount`）：UFUNCTION 会让 UHT 生成 exec 桩，
	 * 少一个函数体是**链接错误**而不是编译错误 —— 上一轮在这里栽过一次，只有全量构建照得出来。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetPierSpanCount() const { return CurrentPierSpanCount; }

	UFUNCTION(BlueprintPure, Category = "CS House|Vine")
	int32 GetVineSegmentCount() const { return CurrentVineSegmentCount; }

	UFUNCTION(BlueprintPure, Category = "CS House|Vine")
	int32 GetVineLeafCount() const { return CurrentVineLeafCount; }

	UFUNCTION(BlueprintPure, Category = "CS House|Vine")
	int32 GetVineFlowerCount() const { return CurrentVineFlowerCount; }

	/**
	 * **诊断 / 验收专用**：藤蔓这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * ⚠️ **形状照抄 `ACSGroundActor::IsRockShellDrawable`，理由也一样**：GPU 石阶的
	 * `StairMesh` / `StairMaterial` 在两张演示关卡里一直是 NULL，画面上是一撮黑块，
	 * 而所有 readback 断言全绿 —— 因为 **readback 证明的是"buffer 里有数"，对"画的是哪张网格、
	 * 有没有材质、材质勾没勾实例化"一个字都没说**。藤蔓这条路比石阶更容易中招：
	 * 现成的 `MI_ivy_*` 全都挂在 `M_TG_Texture` 下，而它**没有勾
	 * `bUsedWithInstancedStaticMeshes`** ⇒ 引擎静默退回默认材质，一片灰。
	 * 所以这里连"材质支持实例化"都做成显式判据。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Vine", meta = (DevelopmentOnly))
	bool IsVineDrawable(FString& OutReason) const;

	/**
	 * 同上，但**把原因当返回值给出来**（空串 = 画得出来）。
	 *
	 * ⚠️ **`IsVineDrawable(FString&)` 在 UE Python 侧是残废的**（实测）：Python 把
	 * "bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串、**不可画时拿到 `None`**，
	 * 原因串整个丢掉。恰好在唯一需要它的时候失效：红灯只能说"画不出来"，说不出为什么，
	 * 而这个函数存在的全部价值就在那句原因上。脚本一律调这一版；`IsVineDrawable` 保留给
	 * C++ 与蓝图，并在返回前把原因打进日志兜底。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Vine", meta = (DevelopmentOnly))
	FString GetVineUndrawableReason() const;

	/** 当前摆出来的装饰件总数（所有 palette 合计）。CPU 侧本来就排好了记录，不需要回读 GPU。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor")
	int32 GetDecorInstanceCount() const { return CurrentDecorInstanceCount; }

	/** 这一轮生产出来的锚点个数。**摆件密度就是这个数**（过完填充概率与间距球之后才是上一条）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor")
	int32 GetDecorAnchorCount() const { return CurrentDecorAnchorCount; }

	/**
	 * 其中门/拱那一家的锚点个数（TG 的 `add_autoclutter_around_gates`）。
	 *
	 * 单独暴露它是因为**总数对这一家是盲的**：画一笔路开出拱时，门那一家长出来的锚点
	 * 与被门口净空、路面排掉的墙脚锚点**数量上可以刚好抵消**（实测就碰上过：
	 * 20 → 20）。拿总数做断言会在那一刻变成空判据。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor")
	int32 GetDecorGateAnchorCount() const { return CurrentDecorGateAnchorCount; }

	/**
	 * **诊断 / 验收专用**：装饰摆件这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * ⚠️ 执行面照抄 `IsVineDrawable`（目前最完整的一版），理由也一样：本项目在"readback 全绿
	 * 而画面上什么都没有"这件事上栽过两次 —— 石阶的 `StairMesh`/`StairMaterial` 一直是 NULL、
	 * 画面是黑块；母材质没勾 `bUsedWithInstancedStaticMeshes` 会被引擎**静默换成默认材质**，
	 * 症状与"没绑材质"逐像素相同。所以"材质支持实例化"必须是显式判据，不能只查材质非空。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor", meta = (DevelopmentOnly))
	bool IsDecorDrawable(FString& OutReason) const;

	/** 同上，但把原因当返回值给出来（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor", meta = (DevelopmentOnly))
	FString GetDecorUndrawableReason() const;

	UFUNCTION(BlueprintPure, Category = "CS House")
	UCSMesh* GetPillarMesh() const { return PillarMesh; }

	/**
	 * 当前屋面描述。屋顶坡板、山墙、以及将来的瓦/梁/尖顶/雪与 D8「落屋顶 → 不生成」谓词
	 * 全部从这一份组装 —— 屋面方程只有 CSHouseRoof.h 一个真源。
	 */
	FCSRoofDesc GetRoofDesc() const;

	/** 属性 → 禁带参数。只有这一处组装，别在各调用点各写一份（带宽下界由滞回比反解）。 */
	FCSHouseResizeBand MakeResizeBand() const;

	// -------------------------------------------------------------------------
	// 判定纯函数（无 GPU、无 world 依赖 —— 直接进 CSHouseLogicTests）
	//
	// 计划纪律：门洞区间、接触段、柱布点、openings 排布这类"能不能 / 在哪 / 多大"的判定
	// 全部做成纯函数并单测，几何生成只负责照着摆。
	// -------------------------------------------------------------------------

	/**
	 * 边缘线段分割（D6）：可用长 = 线段长 − 2×CornerMargin，**等分**成
	 * N = clamp(round(可用长 / PitchTarget), 1, 32) 段。
	 *
	 * 等分而非定模数槽位：拱阵天然对称、没有余量与护角的特判，段长只在目标值附近浮动 ⇒
	 * 拱宽随之微伸缩，正是 Tiny Glade 那种"拱会呼吸"的观感。可用长装不下一个最小拱时返回 0
	 * （这条边不开门）。OutFirstS = 第一段起点沿边弧长，OutPitch = 段长。
	 */
	static int32 SplitEdgeIntoSlots(float EdgeLength, float CornerMargin, float PitchTarget, float MinWidth,
		float& OutFirstS, float& OutPitch);

	/**
	 * 离地收窄系数（D6，用户裁决"离地越高门越窄，直至消失"）：落差 ≤ GapFull 全宽，
	 * ≥ GapZero 归零，中间线性。**连续量因此不需要滞回** —— 消失点发生在拱已经很窄时，
	 * 观感上不突兀；但算出的宽度必须量化后再进形状哈希，否则地形每抖一下都是一次全量重建。
	 */
	static float ComputeDoorWidthScale(float GapMax, float GapFull, float GapZero);

	//~ AActor interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

private:
	/** 材质三槽重绑（房体墙/顶 + 柱），不碰几何 —— 计划 D14「纯外观量绝不进 desc 哈希」。 */
	void BindHouseMaterials();

	/** Ground 为空时自动解析；顺带完成 OnGroundChanged 的订阅/换订阅。 */
	void ResolveGroundAndSubscribe();
	void UnsubscribeGround();
	void HandleGroundChanged(ACSGroundActor* ChangedGround, const FBox& ChangedBounds);

	/** 落座目标：max(footprint 全域地面高度) + HeightOffset。无地面时返回当前 Z（不动）。 */
	double ComputeSeatZ() const;

	/** 边缘线段分割 + 采样点亮（滞回），产出 CurrentDoors。返回房体**形状**哈希（不含世界变换）。 */
	uint32 ComputeDoors();

	/** `Windows` 列表 → 候选洞（尚未过谓词）。身份从列表槽位派生，见实现里那段。 */
	void BuildWindowOpenings(TArray<FCSWallOpening>& OutCandidates) const;

	/** 谓词的输入打包成一份 `FCSOpeningSite`。**唯一**的一处 —— 别在调用点各填一遍。 */
	FCSOpeningSite MakeOpeningSite() const;

	/** 周界支撑点落差判定，产出柱盒列表。返回柱**形状**哈希（局部布点 + 柱长，不含世界变换）。 */
	uint32 ComputePillars(TArray<FVector>& OutPillarCenters, TArray<float>& OutPillarLengths) const;

	/**
	 * 量化世界变换的哈希，房体与柱共用（同一个 actor 变换）。
	 *
	 * 与形状哈希分开是**功能必需而非优化**：常驻流是世界空间，落座（D4 ①）本身就在改 Z，
	 * 而容差与量化都是 0.5 cm ⇒ 混在一个哈希里时「地形抬 1 cm → 落座 → 全量重建」是常态，
	 * 计划 D4 那条"仅 Z 变则走 TransformMesh"的便宜路径永远不可达。
	 */
	uint32 ComputePlacementHash() const;

	/** 烘进常驻流时用的世界变换：只取 yaw + 位置（常驻流是世界空间，不支持缩放/pitch/roll）。 */
	FTransform GetBuildTransform() const;

	void RebuildBodyMesh();
	void RebuildPillarMesh(const TArray<FVector>& Centers, const TArray<float>& Lengths);

	/** 保证门框的实例组件存在并绑好网格/材质（多退少补，同藤蔓/摆件的做法）。 */
	void EnsureFrameComponent();

	/**
	 * 组装门框的**解析砖路**（2026-08-30「裁决一」选乙，**唯一**的一条）。返回门框 desc 哈希。
	 *
	 * CPU 只算**逐路**的标量（弧长、砖数、铺装缩放、墙框架），一块砖的位置与朝向都不在 CPU 上
	 * 出现 —— 那是 `CSHouseFrame.usf` 里"一线程一砖"的事。规模因此是 `O(洞数)` 而不是
	 * `O(砖数)`，容量恒定（注册期 `ReserveCapacity` 一次付清，交互期不扩容、不阻塞）。
	 *
	 * 砖路取自 `CSHouse_ComputeClipField` 那条**解析**洞缘 —— 与材质切洞判据同源，砖因此
	 * 正好骑在切出来的那条边上。每个洞出两条：上边界（含两侧门樘）与下边界（有窗台时才有）。
	 *
	 * 曾经并存的旧路（`BuildFramePlan`：B 样条 + 逐砖记录 + 可扩容，靠 `csh.FrameLegacy` 切换）
	 * 已随裁决一第二步整条删除。两条路"把砖摆在同一个地方"这条等价性判据仍然活着，靠的是
	 * 单测 `House.FrameAnalyticMatchesLegacy` 里那份**测试内部的 CPU 镜像**（不调任何产线代码）。
	 */
	uint32 BuildFrameArches(TArray<CSHouseFrame::FElement>& OutElements, int32& OutBrickCount) const;

	/** 这栋房喂给接缝纯函数的那份输入（身份 + 摆位 + 尺寸，无任何派生表）。 */
	CSHouseSeam::FHouse MakeSeamHouse() const;

	/**
	 * 外接圆够得着的邻居，**按 GUID 升序**。
	 *
	 * 读的是邻居的权威属性（变换 / footprint / 墙高），**不是它的任何缓存或派生表** ——
	 * 裁决二那句"零共享状态"约束的是状态，不是只读的输入。粗筛用外接圆而不是"最近 N 个"：
	 * 前者是纯几何谓词（谁在谁不在只由当前摆位决定），后者要排序、会在并列时抖。
	 */
	void GatherSeamNeighbours(TArray<CSHouseSeam::FHouse>& Out) const;

	/** 接缝裁剪段（写 `CurrentSeamCuts`），返回它对房体形状哈希的贡献。 */
	uint32 ComputeSeamCuts();

	/**
	 * 接缝砖：**追加**进门框砖那份元素表（同一个组件、同一次 dispatch），返回它的哈希贡献。
	 *
	 * 非 const（与 `BuildFrameArches` 不同）：它顺手写 `CurrentSeamBrickCount`。那个数只有它
	 * 算得出来 —— 门框砖那边的总数由 `RebuildFrame` 统一落账，而接缝砖是总数里的一个子段。
	 */
	uint32 BuildSeamBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount);

	void RebuildFrame();

	/** 藤蔓：组件/容量/交接一次付清（同 EnsureFrameComponent），交互期只剩录 pass。 */
	void EnsureVineComponents();

	/** 墙矩形（世界空间）—— 与房体面板同一份 `CSHouse_GetEdge`，不另起一套口径。 */
	void BuildVineStrips(TArray<CSHouseVine::FWallStrip>& OutStrips) const;

	/** 规划 + 录一趟打包 pass；返回这次的形态哈希（喂幂等短路）。 */
	void RebuildVine();

	/** 摆件：组件/容量/交接一次付清（同 EnsureVineComponents），交互期只剩录 pass。 */
	void EnsureDecorComponents();

	/** 锚点生产者要读的世界（墙矩形 + 洞 + 屋面 + 地面采样器）。 */
	void BuildDecorSite(CSHouseDecor::FSite& OutSite) const;

	/** 参数打包：细节面板只暴露改观感的那几个，其余以 `CSHouseDecor::FParams` 的默认值为准。 */
	CSHouseDecor::FParams MakeDecorParams() const;

	/** 生产锚点 + 规划 + 录一趟打包 pass（幂等哈希短路无效唤醒）。 */
	void RebuildDecor();

	/** 拱间墩：按双阈迟回给 `CurrentOpenings` 打 `CSHouse_StylePier*` 位，并刷新迟回表。 */
	void ResolvePierSpans();

	/**
	 * 整栋房子**一次** EditMeshAsync：上传基体 → 排序分段组进同一个 EditFunc、同一张 RDG 图。
	 *
	 * 为什么不能"每个算子各发一次异步编辑"：EditMeshAsync 在途时会拒绝第二次（返回 false 且
	 * OnComplete 永不触发），两个算子各发一次必然互相拒绝。也不能"只把上传异步化" ——
	 * 那是 2 次 flush 变 1 次，不是变 0 次。
	 *
	 * 在途被拒时把目标存进 pending 槽，OnComplete 里补发最新的那一份（**最新态合并**）。
	 * 拖拽 30 Hz 下被拒是常态而非边界情形；这条链给出的速率自动等于 GPU 实际完成速率，
	 * 不需要调参，也不会像固定节流那样在最后一个 tick 落进窗口时吞掉末帧。
	 */
	void SubmitBodyMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot);
	void SubmitPillarMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot);

	/** 异步编辑的游戏线程尾巴：发布分段表 + 补发 pending。 */
	void OnBodyEditComplete(bool bSorted);
	void OnPillarEditComplete();

	/**
	 * 形状未变、只是搬了地方：一个位置+切线 pass 把已有几何搬过去，不重建、不重传。
	 *
	 * **同样走异步**：拖动房子是交互热路径，纪律是"一次设备同步都不许有"。增量始终相对
	 * BuiltAtTransform（= GPU 实际所在）算，所以在途被拒时**什么都不用记** —— 下一次重试
	 * 自然算出更大的那个增量，天然自愈、绝不会累加两次。
	 *
	 * 返回是否已把几何搬到位；false = 这一次没送出去（在途 / 被拒），调用方不许推进摆位哈希，
	 * 否则这次移动就永远丢了。补送由 OnBodyEditComplete 兜底。
	 */
	bool ApplyBodyPlacement();
	bool ApplyPillarPlacement();

	/** 承重柱网格宿主（独立于基类房体组件——纯地形变化只动它，不碰房体重建）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS House", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSMeshRenderComponent> PillarMeshComponent;

	/** 柱网格对象。Transient：派生物，加载后由 ReevaluateSite 重建。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> PillarMesh;

	/**
	 * 当前生效的洞（房体形状 desc 的一部分）。门由道路推导、窗由特征标记注册，两者同表 ——
	 * 墙板生成只认剖面 + 摆位，不关心洞是谁提的。
	 */
	UPROPERTY(Transient)
	TArray<FCSWallOpening> CurrentOpenings;

	/**
	 * 子段滞回状态：key = (Edge<<24) | (N<<16) | SlotIndex。N 进 key，墙长变化重排时不误继承。
	 *
	 * **必须序列化，且必须 NonTransactional**（计划 D6）：滞回让 ReevaluateSite() 成为路径依赖
	 * 函数，承载路径的表冷加载时为空 ⇒ 覆盖率落在 [SlotOff, SlotOn) 的已开拱重开关卡直接消失。
	 * NonTransactional 不能省 —— 普通 UPROPERTY 会被事务缓冲整份捕获，一次无关的 details 改参
	 * + Ctrl+Z 就把滞回表回滚到旧代（与"笔刷家族无 Undo"的既定裁决同向，把口头约定变成类型级
	 * 保证）。凡双阈滞回的开关表都照此办理。
	 */
	UPROPERTY(NonTransactional)
	TMap<uint32, bool> DoorSlotOpen;

	/**
	 * 拱间墩的迟回状态：key = (Edge<<24) | (该边洞数<<16) | 跨度序号。
	 *
	 * 洞数进 key 与 `DoorSlotOpen` 把 N 进 key 同一个理由：多开/少开一个拱会把整条边的跨度
	 * 重新编号，不把编号基准放进 key 就会把旧跨度的样式误继承给完全不同的一段墙。
	 * 序列化与 NonTransactional 的理由同上一条，逐字适用（迟回让重求值成为路径依赖函数）。
	 */
	UPROPERTY(NonTransactional)
	TMap<uint32, bool> PierSpanIsPier;

	// 两级哈希（形状 / 摆位）各自守卫房体与柱：形状变 → 全量重建；只有摆位变 → TransformMesh 一刀。
	uint32 BodyShapeHash = 0;
	uint32 BodyPlacementHash = 0;
	uint32 PillarShapeHash = 0;
	uint32 PillarPlacementHash = 0;

	/** 上次把几何烘进常驻流时的世界变换；增量变换相对它算（同 ACSGroundActor::MeshBuiltAtLocation）。 */
	FTransform BodyBuiltAtTransform = FTransform::Identity;
	FTransform PillarBuiltAtTransform = FTransform::Identity;

	int32 CurrentPillarCount = 0;
	int32 CurrentFrameBrickCount = 0;

	/** `RebuildFrame` 越过早退门、真的重排砖的次数（`GetFrameScatterCount` 读）。 */
	int32 FrameScatterCount = 0;

	/** 窗：过了谓词的 / 被拒的。两个数一起才说得清"没填"与"被门吃掉了"的区别。 */
	int32 CurrentWindowCount = 0;
	int32 CurrentWindowRejectCount = 0;

	/** 这一轮判为墩的跨度个数（`ResolvePierSpans` 写、`GetPierSpanCount` 读）。 */
	int32 CurrentPierSpanCount = 0;

	/** 门框砖的 GPU 实例宿主（单 palette：TG 只有一块 brick）。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> FrameComponent;

	/** 实例行与计数的 pooled buffer（渲染线程分配、渲染线程释放）。 */
	TArray<CSShaperSteps::FPaletteBuffers> FrameGpuBuffers;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	TArray<uint32> FrameHandedCapacities;
	FBox FrameHandedLocalBounds = FBox(ForceInit);

	uint32 FrameDescHash = 0;

	/** 这一轮的接缝裁剪段（D7）。**派生物，每轮从两房的摆位重算**，不序列化、不做增量。 */
	TArray<FCSWallCut> CurrentSeamCuts;
	int32 CurrentSeamCornerCount = 0;
	int32 CurrentSeamBrickCount = 0;

	/** 藤蔓的两个 GPU 实例宿主：0 = 枝、1 = 叶。分两个组件是因为它们是两张网格、两份材质。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> VineBranchComponent;

	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> VineLeafComponent;

	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> VineFlowerComponent;

	/** 三季叶用的 MID（父 = `VineLeafMaterial`）。缓存在 actor 上而不是每次重建 ——
	 *  蓝图重跑构造脚本会销毁组件，MID 活在 actor 上才不会跟着一起没。 */
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> VineLeafSeasonMID;

	/** 实例行与计数的 pooled buffer（[0] = 枝、[1] = 叶）。容量按**配置上限**一次预留，
	 *  规划结果再多也只截断不扩容 —— 交互期一次设备同步都不许有。 */
	TArray<CSShaperSteps::FPaletteBuffers> VineGpuBuffers;

	TArray<uint32> VineHandedCapacities;
	FBox VineHandedLocalBounds = FBox(ForceInit);

	uint32 VineDescHash = 0;
	int32 CurrentVineSegmentCount = 0;
	int32 CurrentVineLeafCount = 0;
	int32 CurrentVineFlowerCount = 0;
	/** 基础网格快照建成过没有。⚠️ 它是"材质有没有可用 UV/法线"那条判据的另一半：
	 *  快照没建成时组件画的是**上一次**的网格，而不是什么都不画。 */
	bool bVineBaseMeshReady = false;

	/** 快照是从哪两张网格建的。⚠️ **不能只靠 `bVineBaseMeshReady` 一个 bool**：
	 *  在细节面板里换掉 `VineBranchMesh` 时组件不一定被重建，那时候 bool 仍是 true，
	 *  画面上还是旧网格 —— 症状是"换了资产但什么都没发生"，与坑表里
	 *  "CDO 默认值不传播到已存在实例"同一族的静默失效。 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> VineBranchMeshBuiltFrom;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> VineLeafMeshBuiltFrom;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> VineFlowerMeshBuiltFrom;

	/** 摆件的 GPU 实例宿主：**一张网格一个**（一个 palette 条目 = 一个组件）。
	 *  顺序恒为「门 → 墙脚 → 屋顶」，`DecorPaletteRanges` 记的就是这三段的起止。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> DecorComponents;

	/** 实例行与计数的 pooled buffer，逐 palette。容量按**配置上限**一次预留，
	 *  规划结果再多也只截断不扩容 —— 交互期一次设备同步都不许有。 */
	TArray<CSShaperSteps::FPaletteBuffers> DecorGpuBuffers;

	/** 每一家能用 palette 的哪一段，按 `CSHouseDecor::EFamily` 下标。
	 *  窗户那一家恒 `{0, 0}`（不长），见 `CSHouseDecor.h` 的文件头。 */
	TArray<CSHouseDecor::FPaletteRange> DecorPaletteRanges;

	TArray<uint32> DecorHandedCapacities;
	FBox DecorHandedLocalBounds = FBox(ForceInit);

	uint32 DecorDescHash = 0;
	int32 CurrentDecorInstanceCount = 0;
	int32 CurrentDecorAnchorCount = 0;
	int32 CurrentDecorGateAnchorCount = 0;

	/** 基础网格快照建成过没有（同 `bVineBaseMeshReady`：没建成时组件画的是**上一次**的网格）。 */
	bool bDecorBaseMeshReady = false;

	/** 快照是从哪几张网格建的。⚠️ 与 `VineBranchMeshBuiltFrom` 同一条：在细节面板里换掉网格时
	 *  组件不一定被重建，只靠一个 bool 会画着旧网格而"什么都没发生"。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> DecorMeshesBuiltFrom;

	/** 下一次重求值强制全量重建：手动强刷，以及拖动松手时清掉增量变换攒下的浮点误差。 */
	bool bForceFullRebuild = false;

	/**
	 * 拖动期的**原始诉求**尺寸（未经禁带修正）。`PushEdge` 的累加器，**不序列化**。
	 *
	 * 为什么必须有（禁带的实现前提）：带把墙吸在外沿上时 `Applied` 恒 0，而生效尺寸又是
	 * 下一帧的起点 ⇒ 只看生效尺寸的话墙永远跨不过带。这里记的是"手往外走了多远"，
	 * 跨带因此要攒够半个带宽的拖动量 —— 裁决四那句"尺寸更换有最小距离"的字面执行面。
	 *
	 * 不进任何哈希、不进事务缓冲：它是交互中间量，被 Ctrl+Z 回滚或跟着尺寸存盘都只会
	 * 让下一次拖动第一帧自己跳（与 `DoorSlotOpen` 的 NonTransactional 同一条纪律，
	 * 只是这一条连序列化都不需要 —— 拖动一结束它就该等于 FootprintSize）。
	 */
	FVector2D RawFootprintSize = FVector2D::ZeroVector;

	/** 异步编辑在途时到达的最新目标快照（被拒即入槽，OnComplete 里补发）。 */
	TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> PendingBodySnapshot;
	TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> PendingPillarSnapshot;

	/** 稳定身份，随关卡序列化；首次注册时生成。 */
	UPROPERTY()
	FGuid HouseId;

	FDelegateHandle GroundChangedHandle;
	bool bInReevaluate = false; // SetActorZ 落座引发的重入保护
};
