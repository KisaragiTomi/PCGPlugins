#pragma once

#include "CoreMinimal.h"
#include "CSMesh.h"

class UStaticMesh;

/**
 * 披挂岩壳（计划 D9「侧面碎石：Tiny Glade 式披挂岩壳」的链 B）。
 *
 * **裁决一：碎裂图案是 2D 静态资产，三维形态 100% 运行时披挂。** 图案就是 Tiny Glade 自己
 * 那一张（`rocky_terrain_shell.glb` → `/Game/TinyGlade/Meshes/rocky_terrain_shell/...`）：
 * 顶圈 cap + 横向朝外错开 19.5 cm 的底圈 skirt，非索引三角汤，逐顶点胞腔数据打包在多 UV
 * 通道里。运行时一趟 compute 逐三角：坡度 mask 决定显隐 → 每个顶点用**自己的 XY** 取地面
 * 高度 → 重算面法线。不跑运行时 Voronoi、不挤出、不减面。烘焙口径与实测见
 * `Docs/TinyGlade/CSRockShellPattern.md`。
 *
 * **裁决二：碎石归地面 actor，不归塑形物。** 图案覆盖整张地面、与任何单座塑形物无关；
 * mask 由**全部塑形物合成后**的坡度决定，归任一座都不对。删掉塑形物 → 高度场塌回 →
 * 坡度降到阈下 → 那批胞腔自己写 NaN，归属簿记整个消失（不是变简单）。
 *
 * **裁决三：解析高度场直接喂。** kernel 直接调 `CSGroundShaperField.ush` 的
 * `GroundShaperHeightAtXY`，与地面位移 pass 共用同一份 `GroundShaperParams` ⇒
 * 壳与地面的一致性是**构造保证**的，不是靠两条路算出同一个数。
 *
 * 与 `CSGroundStairs` 的关系：两者读同一份道路权重，**严格互补** —— 石阶要路、碎石要没路。
 * 但实现方式不同（裁决五）：石阶用阈值门控，壳**没有**任何 road 显隐判据，只有连续下沉。
 */
namespace CSRockShell
{
/** 图案 StaticMesh 的默认落点（`Scripts/TinyGladeImportRockShell.py` 导到这里）。 */
COMPUTESHADERGENERATOR_API extern const TCHAR* const DefaultPatternAssetPath;

/**
 * `FPattern::CellFlags` 的位布局。**全项目唯一仲裁点** —— kernel 侧那份写在
 * `CSGroundRockShell.usf` 的 `RockShellCellFlags` 声明注释里，两边必须逐位相同。
 */
enum class ECellFlag : uint32
{
	TopRim = 1u << 24,   ///< 顶圈（cap 环）/ 底圈（skirt 环）。实测 TG 的 `cell_bby` 就是这一位
	Corner = 1u << 25,   ///< Voronoi 角点，= `@intersectionpt`。角点不加 FBM（钉死缝宽）
	CapTri = 1u << 26,   ///< 逐三角的盖 / 裙标记，= TG 的 `is_top`
};

/**
 * 岩壳网格的**顶点色通道字典**（裁决六 ② 的执行面：这份语义必须随网格烘进 StaticMesh）。
 *
 * | 通道 | 含义 |
 * | --- | --- |
 * | R | `bIsCapTri`：**1 = 盖三角（顶盖）/ 0 = 裙三角（侧壁）** |
 * | G, B, A | 留白，恒 1 |
 *
 * **为什么是顶点色而不是 aux 流**：`bIsCapTri` 原本只在 aux 槽 33 里，而那条流是
 * `VfType = VET_None`（"aux 不进顶点工厂"）且不在回读集里 ⇒ 材质读不到它，
 * `SaveToStaticMesh` 也带不走它。顶点色是标准流：`ReadbackResidentSync` 会读它、
 * `BuildGpuMeshDescription` 会把它写成 `VertexInstanceColors` ⇒ 烘完仍在资产里，
 * 材质只用一个 `VertexColor` 节点就能读，**不依赖任何只有 gpumesh 代理才提供的逐图元数据**。
 *
 * **为什么 1 = 盖而不是 1 = 裙**：消费侧是 `M_TG_Texture`，它下面挂着 459 个 MI。
 * 没有色流的网格在材质里读出来是**白色**，所以中性值必须是 1 —— 把"盖"（= 不改动的那一半）
 * 定成 1，材质里那一项在白色输入下就恒等于"什么都没做"，别的 MI 逐像素不变。
 * 倒过来定的话，全库 458 个 MI 会一起被当成裙。
 *
 * ⚠️ **kernel 一个字节都不写顶点色** —— 盖/裙是烘死的图案属性，每趟披挂重写它纯属浪费；
 * 而且 `Displace` 只声明了 Positions/Tangents 两个 UAV，多写一条流就要动那份声明。
 */
namespace VertexColor
{
constexpr int32 CapTriChannel = 0;      ///< R
constexpr float CapValue = 1.0f;        ///< 盖三角（也是"没有色流"时材质读到的中性值）
constexpr float SkirtValue = 0.0f;      ///< 裙三角
}

/**
 * 岩壳 `UCSMesh` 的额外 aux 流槽位。
 *
 * 槽位从 32 起：`AuxVertex` 槽 0 归标准集的逐三角材质 id（`FCSMeshStreamLayout` 强制开，
 * 关不掉），`CSGpuInstancedMeshSceneProxy` 那个 leaf 已经占了 16..22。
 * ⚠️ **槽位冲撞不会报错**：`FCSMeshResident::AddStream` 直接返回 false，之后绑上一个空
 * buffer，日志里什么都没有 —— 表现是"壳一个三角都不出现"，而没有任何东西指向槽位。
 */
enum class EAuxSlot : uint8
{
	RestDir   = 32,   // Buffer<float4>，1/顶点：RestXY.xy + DirToCentroid.xy
	CellFlags = 33,   // Buffer<uint>，  1/顶点：CellId | bIsTopRim<<24 | bIsCorner<<25 | bIsCapTri<<26
	/**
	 * Buffer<float2>，1/**胞腔**（Fixed）：质心 XY。
	 *
	 * ⚠️ **契约里没有这一条，是本实现从几何算出来的派生量**（`Docs/TinyGlade/CSRockShellPattern.md`
	 * 的 aux 表只列 32/33）。它只服务 `CellJitter` 的半径淡入 —— 契约的 `DirToCentroid` 是
	 * **单位**向量，定长径向位移会把顶盖靠近质心的内部点推过质心、翻转周围三角（该文的坑 1）。
	 * 文档建议的另一条出路是把半径比例烘进打包字的 bit 27..31，那是**改契约**，没有擅自做。
	 * 609 个 float2 = 4.9 KB，代价可以忽略。
	 */
	Centroids = 34,
};

/**
 * 从图案 StaticMesh 抽出来的一份逐三角展开数据。**只有 XY 有意义** —— 静止姿态的厚度轴
 * （UE 的 Z，约 312 cm）只用来区分顶圈/底圈，运行时被整个丢弃替换。
 *
 * ⚠️ **必须按索引缓冲展回 `Tri*3 + k`**：原件 148,794 个顶点槽里有 65,702 个（44.2%）逐字节
 * 相同，UE 的静态网格构建会把它们焊掉。焊接本身不丢语义，但它毁掉了逐三角展开 —— 而
 * 「写一个 NaN 到第 0 个顶点就让整个三角出局」这条裁决正是靠这个布局。
 */
struct FPattern
{
	uint32 TriangleCount = 0;
	uint32 VertexCount = 0;      // == TriangleCount * 3（展开后，顶点不共享）
	uint32 CellCount = 0;        // = CellId 的最大值 + 1（原件 609）

	FVector2f BoundsMin = FVector2f::ZeroVector;   // 静止姿态的平面包围盒 cm
	FVector2f BoundsMax = FVector2f::ZeroVector;
	float ThicknessCm = 0.0f;    // 静止姿态的厚度轴跨度（原件 312），只作核对

	/**
	 * 实测出来的绕序：true = 盖三角的 `cross(P2−P0, P1−P0)` 指向 **−Z**，kernel 要取负。
	 *
	 * **为什么是测出来的而不是写死**：glTF 是右手 Y-up、UE 是左手 Z-up，导入器换轴时**可能
	 * 翻绕序**，而 `Docs/TinyGlade/CSRockShellPattern.md`「首次导入后必须核对的四项」第 4 条明说这一项
	 * 本轮无法实测。盖三角在静止姿态是水平的，叉积必然是 ±Z，所以这个测量是平凡可靠的。
	 * 测一次胜过写死一个猜测 —— 猜错的症状是整张壳翻在里面（从上方完全看不见），
	 * 而没有任何断言会红。
	 */
	bool bFlipWinding = false;

	TArray<FVector4f> RestDir;   // VertexCount 个：RestXY.xy + DirToCentroid.xy
	TArray<uint32>    CellFlags; // VertexCount 个
	TArray<FVector2f> Centroids; // CellCount 个

	// --- 「首次导入后必须核对的四项」的机读版，供日志与单测（见 Docs 同名小节）---
	int32 NumUVChannels = 0;     // 期望 ≥ 3
	float MaxCellId = 0.0f;      // 期望 608（UV1.x 未被归一化到 0..1）
	/**
	 * 重算出来的 `DirToCentroid` 与烘焙件 `TEXCOORD_0` 的一致度（点积中位数，期望 ≈ +1）。
	 *
	 * **为什么 kernel 用重算的那份而不是烘焙的那份**：UV 通道不参与轴变换，而 POSITION 会 ——
	 * 导入器一旦翻掉一个平面轴，烘焙的 `dir` 就与实际坐标不再对应，而且**没有任何报错**。
	 * 从坐标现算免疫这一整类故障；把烘焙件留作核对项，正好把"轴被翻了"这件事变成可观测的。
	 */
	float DirAgreement = 0.0f;

	bool IsValid() const
	{
		return TriangleCount > 0 && VertexCount == TriangleCount * 3
			&& RestDir.Num() == int32(VertexCount) && CellFlags.Num() == int32(VertexCount)
			&& CellCount > 0 && Centroids.Num() == int32(CellCount);
	}

	FVector2f Centre() const { return (BoundsMin + BoundsMax) * 0.5f; }
	FVector2f Extent() const { return (BoundsMax - BoundsMin) * 0.5f; }
};

/**
 * 进程内共享的那一份图案（按资产缓存，同一张网格只抽一次）。
 *
 * **共享而不是每个地面各抽一份**：抽一次要遍历 148,794 个顶点槽 + 逐胞腔求质心，而重建路径
 * （关卡加载、改格数、松手后的全量对齐）每次都会走到 —— 每次都重抽是纯浪费。
 * 抽不出来时返回一个 `IsValid() == false` 的空件，调用方据此把整条路关掉（而不是画出垃圾）。
 */
COMPUTESHADERGENERATOR_API const FPattern& GetSharedPattern(UStaticMesh* PatternMesh);

/** 一趟披挂的全部标量参数。命名与 `CSGroundRockShell.usf` 的 uniform 一一对应。 */
struct FDisplaceParams
{
	/** 图案 → 世界的映射：世界 XY = WorldCentre + (RestXY − PatternCentre) × Scale。 */
	FVector2f PatternCentre = FVector2f::ZeroVector;
	FVector2f WorldCentre = FVector2f::ZeroVector;
	float Scale = 1.0f;
	/** 标称胞腔半径 cm（图案空间），`CellJitter` 的半径淡入用。原件 5.53 m 间距 ⇒ 约 277。 */
	float CellRadiusCm = 277.0f;

	/**
	 * 允许出现壳的世界 XY 矩形（通常 = 地面矩形）。矩形外的三角直接写 NaN。
	 *
	 * ⚠️ **这一条是承重的**：原件 tile 是 136.5 m 而地面 128 m，边上那圈三角本来就落在地面外
	 * （`Docs/TinyGlade/CSRockShellPattern.md` 的坑 7）。塑形物摆在地面边缘时它的裙边会伸出地面矩形，
	 * 那圈三角于是**真的**过坡度阈、真的被披挂出来 —— 而 `WorldBounds` 是按地面矩形写死的，
	 * 结果就是超出包围盒的几何被剔除逻辑随机砍掉（症状：转动视角时地面边缘的石头一闪一闪）。
	 */
	FVector2f DomainMin = FVector2f::ZeroVector;
	FVector2f DomainMax = FVector2f::ZeroVector;

	/** 盖三角的绕序需要取负（由 `FPattern::bFlipWinding` 实测得出）。 */
	bool bFlipWinding = false;

	/** 地面镜像格（只用来取道路权重：色流的 R 通道，V = Y * VertsX + X）。 */
	FVector2f GroundOriginXY = FVector2f::ZeroVector;
	float GroundCellSize = 50.0f;
	FIntPoint GroundVerts = FIntPoint(0, 0);
	float GroundBaseZ = 0.0f;

	/** mask = smoothstep(SlopeLo, SlopeHi, |∇h|)，与 TG 的 rocky_terrain.x 同口径。 */
	float SlopeLo = 0.75f;
	float SlopeHi = 1.25f;
	/** road 的沉降权重（TG 用 10×：很小的路权重就足以把壳压下去）。 */
	float RoadFade = 10.0f;
	/** road 满值时沿地形法线的下沉量 cm（TG 合计约 1.6 m）。 */
	float RoadSink = 160.0f;
	/**
	 * 逐胞腔沿 `DirToCentroid` 的随机胀缩幅度 cm，**图案空间**。
	 * ⚠️ `DirToCentroid` 实测**指向质心**，所以正的位移是**收缩**（TG ④ 因此是「最多缩 0.52 m、
	 * 最多胀 0.195 m」，`CSGroundShaper.md` 写的符号是反的）。kernel 里用的是对称随机，
	 * 所以本值只是幅度、与符号无关。
	 */
	float CellJitter = 0.0f;
	/**
	 * 壳沿地形法线的**厚度** cm：顶圈浮 0..本值（逐胞腔随机），底圈沉 本值。
	 *
	 * TG `displace:577` 的 `mix(-0.3, 0.1*mix(0,3,rand(cell)), cell_bby)`，而 `cell_bby`
	 * **实测就是 `bIsTopRim`**（不是 `CSGroundShaper.md` 读的「本胞腔凹还是凸」）。
	 * 这一层才让壳读成一块块石头而不是一张贴着地形的毯子。
	 */
	float CellRelief = 30.0f;
	/** 表面 FBM 沿法线的幅度 cm；角点不加（见 kernel 注释）。 */
	float NoiseAmp = 0.0f;
	float NoiseFrequency = 1.0f / 150.0f;
	uint32 Seed = 1;
};

/**
 * 建壳网格：声明流集（标准集 + 三条 aux）→ 上传图案 → 把 `WorldBounds` 按地面矩形**写死**。
 *
 * **这是一次性的、阻塞的路径**（`SetStreamLayoutSync` / `EnsureCapacitySync` / `EditMeshSync`
 * 各自都会 flush 一次），只应该在注册 / 加载 / 改配置时被调到。交互期（画笔刷、拖塑形物）
 * 必须只走 `Displace`，一次 enqueue 都不发 —— 与 `CSGroundStairs::EnsureBuffers` 同一条纪律。
 *
 * `WorldBounds` 写死是必需而非优化：kernel 用 NaN 关掉看不见的三角，而 NaN 会污染任何
 * "从顶点算出来"的包围盒（计划已定这是对的做法）。
 *
 * 返回是否建成。图案无效 / 显存预检拒绝时返回 false，调用方应当把整条路关掉。
 */
COMPUTESHADERGENERATOR_API bool BuildMesh(
	UCSMesh* ShellMesh, const FPattern& Pattern, const FBox& HardWorldBounds,
	const FVector2f& WorldCentre, float Scale, float UVWorldPeriod);

/**
 * 跑一趟披挂：一个 dispatch 原地重写壳的位置与切线。**录完 pass 直接返回，不阻塞。**
 *
 * `GroundResident` 是地面网格的常驻流集合，只读它的色流取道路权重 —— 读的正是笔刷双写出来的
 * 那一份权威投影，所以"画面上看到的路"与"壳沉下去的判据"是构造上同源的。
 *
 * 两份常驻流各开一个 `FCSMeshRenderThreadEdit`（壳写、地面读），访问状态由它们各自恢复。
 * 直接写流再手工恢复是同一条规则的第二份拷贝，而漂掉的那份不会报错，只是安静地停止工作。
 *
 * `ShaperParams` 是 `ACSGroundActor::BuildShaperGpuParams` 打的那一份（每座
 * `CSGroundShaperField::Float4sPerShaper` 个 float4），按值搬进渲染命令。返回是否真的录了 pass。
 */
COMPUTESHADERGENERATOR_API bool Displace(
	UCSMesh* ShellMesh,
	const FCSMeshResidentRef& GroundResident,
	const FDisplaceParams& Params,
	const TArray<FVector4f>& ShaperParams);
}
