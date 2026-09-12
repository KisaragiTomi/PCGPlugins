#pragma once

#include "CoreMinimal.h"
#include "CSMesh.h"

class UStaticMesh;

/**
 * 披挂岩壳（计划 D9「侧面碎石：Tiny Glade 式披挂岩壳」的链 B）。
 *
 * **裁决一：碎裂图案是 2D 静态资产，三维形态 100% 运行时披挂。** 图案就是 Tiny Glade 自己
 * 那一张（`rocky_terrain_shell.glb` → `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/rocky_terrain_shell/...`）：
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
 * 岩壳网格的**顶点色通道字典 v2**（裁决六 ② 的执行面：这份语义必须随网格烘进 StaticMesh；
 * 假倒角三通道的机制与消费端见 `Docs/TinyGlade/CSRockShellEdgeBevel.md`）。
 *
 * | 通道 | 含义 |
 * | --- | --- |
 * | R | `bIsCapTri`：**1 = 盖三角（顶盖）/ 0 = 裙三角（侧壁）** |
 * | G | 到本胞腔盖轮廓（上沿折痕）的平面距离，`saturate(世界 cm / RimDistMaxCm)`；**1 = 远** |
 * | B | 逐石头相位 `fract(CellId × 0.618)`，假倒角噪声的域偏移 |
 * | A | 外向方向角 `atan2(−DirToCentroid) / 2π`，盖侧法线弯向的目标 |
 *
 * **为什么是顶点色而不是 aux 流**：`bIsCapTri` 原本只在 aux 槽 33 里，而那条流是
 * `VfType = VET_None`（"aux 不进顶点工厂"）且不在回读集里 ⇒ 材质读不到它，
 * `SaveToStaticMesh` 也带不走它。顶点色是标准流：`ReadbackResidentSync` 会读它、
 * `BuildGpuMeshDescription` 会把它写成 `VertexInstanceColors` ⇒ 烘完仍在资产里，
 * 材质只用一个 `VertexColor` 节点就能读，**不依赖任何只有 gpumesh 代理才提供的逐图元数据**。
 *
 * **为什么中性值必须是 1**：消费侧母材质下挂着大量 MI，没有色流的网格在材质里读出来是
 * **白色**。R 把"盖"（= 不改动的那一半）定成 1；G 把 1 定成"离折痕远" ⇒ 白色输入下
 * 倒角强度恒为 0，别的 MI 逐像素不变（B/A 在 G=1 时不参与运算，取值无所谓）。
 *
 * ⚠️ **kernel 一个字节都不写顶点色** —— 这四个量全是烘死的图案属性（G 只在 `BuildMesh`
 * 上传时按 `Scale` 换算一次），每趟披挂重写纯属浪费；而且 `Displace` 只声明了
 * Positions/Tangents 两个 UAV，多写一条流就要动那份声明。
 *
 * ⚠️ **v3（2026-09-04 落地）改了这一条的适用范围**：真邻接载荷走 UV1..UV5（`namespace TexCoord`），
 * 由第三趟 `RockShellBevelPayloadCS` **逐趟披挂重写** —— 披挂会改盖裙夹角，烘死的邻面法线与
 * 垂距到运行时都是错的。所以上面那句「kernel 一个字节都不写」现在**只对顶点色成立**，UV 是
 * 每趟都重写的。
 *
 * 顶点色这四个通道并没有作废：R（盖/裙）与 B（逐石头相位）v3 仍在读 —— 它们是烘死的图案
 * 属性，8 bit 装一个布尔和一个相位绰绰有余。真正被 v3 取代的是 G（折痕距离）与 A（外向角），
 * 那两个正是 8 bit 量化与 atan2 回绕两类跳变的来源。**但两者都还留着**：直摆的图案 StaticMesh
 * 没有 v3 那几条 UV，`M_TinyGladeRockShell` 继续走 v2 的顶点色载荷。
 * 逐条见 `Docs/TinyGlade/CSRockShellEdgeBevel.md` 的「v3」。
 */
namespace VertexColor
{
constexpr int32 CapTriChannel = 0;      ///< R
constexpr float CapValue = 1.0f;        ///< 盖三角（也是"没有色流"时材质读到的中性值）
constexpr float SkirtValue = 0.0f;      ///< 裙三角
/** G 通道的归一上限（世界 cm）。材质端 `DistMax` 参数与离线烘焙脚本的 `DIST_MAX_M` 必须同值。 */
constexpr float RimDistMaxCm = 50.0f;
/**
 * 消费材质的标量参数名。`ACSGroundActor::EnsureRockShellMesh` 给 `RockShellMaterial` 造一个动态
 * 子实例并把 `RockShellPatternScale` 写进去：假倒角的带宽 / 咬深 / 噪声尺度全是 TG 原生的
 * **图案空间**口径，材质用它把 G 通道的世界距离与世界坐标换算回图案空间 ⇒ 胞腔缩到 1.9 m 时
 * 缺口跟着缩，而不是 5.53 m 胞腔的绝对尺寸。直摆的图案资产（Scale = 1）吃材质默认值 1。
 */
inline const TCHAR* PatternScaleParameterName = TEXT("RockShellPatternScale");
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
	/**
	 * Buffer<uint>，**2/顶点**：本角所属**重合组**在 `IncidentTris` 里的 (偏移, 数量)。
	 *
	 * 重合组 = 静止姿态下位置重合的那一批角（0.1 cm 栅格上量化）。壳是逐三角展开的三角汤，
	 * 缓冲里**没有任何顶点共享** ⇒ 拓扑必须另外烘一份，否则法线只能取面法线（那正是
	 * 2026-09-03 之前的观感，用户判为"非常干扰"）。
	 */
	IncidentRange = 35,
	/** Buffer<uint>（Fixed）：展平的入射三角序号，按重合组连续存放。 */
	IncidentTris = 36,
	/**
	 * Buffer<int>，**3/三角**：逐边邻接表，假倒角 v3 的唯一烘焙件。
	 *
	 * `Neighbours[T * 3 + k]` = 越过**对着角 k 的那条边**（= 边 `(v[(k+1)%3], v[(k+2)%3])`）
	 * 的那个三角形序号，`-1` = 该边在石头外轮廓上、没有邻居。这个下标约定与
	 * `CSGroundRockShell.usf` 里的垂距 `d[k]` 严格对齐：`d[k]` 量的就是到「对着角 k 的边」
	 * 的距离，所以 `argmin d` 直接就是 `Neighbours` 的下标，中间不需要任何重排。
	 * ⚠️ TG 自己的 `triangles[].neighbours` 用的是另一套下标（边 (v0,v1) 排在 0），
	 * 对照原版 shader 时别混。
	 *
	 * 边由两端**重合组** id 的有序对定义（与 `IncidentRange` 同一份量化键）——
	 * 壳是三角汤，按顶点序号找邻居永远找不到。
	 */
	Neighbours = 37,
};

/**
 * 岩壳 UV 通道字典（假倒角 v3）。**通道 1..5 由 kernel 每趟披挂重写**，CPU 只写通道 0。
 *
 * 为什么不是顶点色：v2 把折痕距离与外向角打进 `PF_R8G8B8A8`，8 bit 让法线方向按 1.4° 一档
 * 跳，而外向角还要经 `atan2` 存成标量、在 ±π 处回绕。UV 是 float32，两类跳变一起消掉。
 * 为什么必须重写而不是烘死：壳每趟按地形重新披挂，盖裙夹角随坡度在 37°~51° 之间变，
 * 烘死的邻面法线与垂距到运行时都是错的（v1 的 15°~25° 系统性误差正是栽在这）。
 * 与 TG 同构 —— 原版 `Triangle.normal` 由 `displace_rocky_terrain.cs` 每帧重写，
 * 只有 `neighbours` 是烘死的。
 */
namespace TexCoord
{
constexpr int32 World      = 0;   ///< 世界 XY / UVWorldPeriod，三平面贴图用（CPU 在 BuildMesh 写）
constexpr int32 EdgeDist01 = 1;   ///< (d0, d1) 世界 cm：到「对着角 0 / 角 1 的边」的垂距
constexpr int32 EdgeDist2  = 2;   ///< (d2, 0)  世界 cm；y 留白
constexpr int32 NbrNormal0 = 3;   ///< 八面体编码的邻面法线，对应边 0（-1 时写自身法线 ⇒ 混合退化成不变）
constexpr int32 NbrNormal1 = 4;   ///< 同上，边 1
constexpr int32 NbrNormal2 = 5;   ///< 同上，边 2
/**
 * 八面体编码的**自身**面法线（逐三角常量）。
 *
 * 为什么材质要它：基础法线走的是全平均（`bRockShellSmoothNormals`，2026-09-03 裁决 ——
 * 几何这一层不再表达硬边），于是材质拿到的 `VertexNormalWS` 是**平滑**的，而
 * `lerp(平滑法线, 邻面法线, t)` 在折痕两侧收敛到同一个值 ⇒ **数学上永远画不出硬边**。
 * 有了自身面法线才能在 band 内换成 chamfer 小面的常数法线（= 自身与邻面的平分向量），
 * 让 band 的两条边界各成一道真硬边 —— 用户 2026-09-04 的要求「硬边由材质表现产生」。
 */
constexpr int32 FaceNormal = 6;
constexpr int32 NumSets    = 7;   ///< 壳声明的 UV 组数；上限见 FCSGpuMeshCPUData::MaxTexCoordChannels
}

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
	/**
	 * VertexCount 个，假倒角的材质载荷（`Docs/TinyGlade/CSRockShellEdgeBevel.md`）：
	 * X = bIsCapTri、Y = 到本胞腔盖轮廓的平面距离 **cm（图案空间，未乘 Scale）**、
	 * Z = 逐石头相位、W = 外向方向角 /2π。`BuildMesh` 按 `VertexColor` 字典 v2 打包进顶点色，
	 * 其中 Y 先乘 Scale 再按 `RimDistMaxCm` 归一 —— 距离必须以世界口径进材质。
	 */
	TArray<FVector4f> BevelData;

	/**
	 * 法线**全平均**的拓扑（用户裁决 2026-09-03：不设夹角阈值，硬边整个交给材质去画）。
	 *
	 * `IncidentRange` 是 `VertexCount * 2` 个 uint，逐角一对 (偏移, 数量)，指进 `IncidentTris`；
	 * 后者按重合组连续存放该组全部入射三角的序号。GPU 第二趟逐角把这些三角的**面积加权**
	 * 面法线加起来再归一 —— 叉积不归一化，长度本身就是两倍面积。
	 *
	 * ⚠️ **拓扑是图案的常量**：图案永远不变，变的只是每趟披挂后的位置。所以它在这里烘一次，
	 * 夹角/法线留给 kernel 每趟现算。原件约 148,794 个角，两条流合计约 3.6 MB。
	 *
	 * ⚠️ **原记「组不会跨胞腔、石头之间照旧是硬边」，2026-09-04 实测推翻**：逐边邻接建完后统计，
	 * 74,180 条内部边里 **8,743 条跨胞腔**（11.8%）—— 相邻石头之间确实共享顶点。
	 * 后果有两条，都还没有裁决：① 本趟全平均**已经**在跨石头做平滑（既有行为，非本轮引入）；
	 * ② 假倒角 v3 会在这 8,743 条边上把法线混向邻面 ⇒ 石头与石头之间不再是硬边。
	 * 计数每次抽取都打进日志（`逐边邻接：… 跨胞腔 N 条`），保持可证伪。
	 */
	TArray<uint32> IncidentRange;
	TArray<uint32> IncidentTris;

	/**
	 * `TriangleCount * 3` 个：逐边邻接表，下标与语义见 `EAuxSlot::Neighbours`。
	 * 与重合组表同一个生命周期（都是图案常量，抽取期烘一次）；原件约 149 k 个 int32 ≈ 0.6 MB。
	 */
	TArray<int32> Neighbours;

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
			&& BevelData.Num() == int32(VertexCount)
			&& IncidentRange.Num() == int32(VertexCount) * 2 && !IncidentTris.IsEmpty()
			&& Neighbours.Num() == int32(TriangleCount) * 3
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

	/**
	 * 法线是否走**全平均**（第二趟 dispatch）。false = 只留第一趟的面法线，即 2026-09-03 之前
	 * 的硬边观感。见 `ACSGroundActor::bRockShellSmoothNormals`。
	 */
	bool bSmoothNormals = true;

	/**
	 * 平滑的**夹角阈值**（度）：入射三角的面法线与本三角面法线夹角超过它就不参与平均，
	 * 于是那条边在几何这一层重新成为硬边。0 ⇒ 只认自己（= 面法线）；180 ⇒ 全平均。
	 *
	 * 2026-09-04 用户裁决加回来的，推翻 09-03「不设阈值、全平均」那一条 —— 全平滑之后
	 * 盖裙折痕整个圆掉，材质再怎么画也补不回一道真硬边。当初否掉阈值的理由是「有阈值就得
	 * 烘逐边邻接」，**那条不成立**：按面法线夹角判只要逐角的重合组表，而第二趟本来就在读
	 * 每个入射三角的位置，一行点积、零额外烘焙。
	 */
	float SmoothAngleDeg = 30.0f;

	/** 地面镜像格（只用来取道路权重：色流的 R 通道，V = Y * VertsX + X）。 */
	FVector2f GroundOriginXY = FVector2f::ZeroVector;
	float GroundCellSize = 50.0f;
	FIntPoint GroundVerts = FIntPoint(0, 0);
	float GroundBaseZ = 0.0f;

	/** mask = smoothstep(SlopeLo, SlopeHi, |∇h|)，与 TG 的 rocky_terrain.x 同口径。 */
	float SlopeLo = 0.75f;
	float SlopeHi = 1.25f;
	/**
	 * 路足迹的增益（TG 用 10×）：`saturate(R × 本值)` 就是壳眼里的"这里有路"。
	 * ⚠️ 乘在**模糊之前**（第零趟 X），披挂那边不再乘 —— 顺序为什么承重见 usf 的「第零趟」。
	 */
	float RoadFade = 10.0f;
	/**
	 * 路足迹进披挂之前的高斯模糊半径 cm（= 核的单侧伸展，σ = 本值 / 3）。0 = 不模糊。
	 * 与 `ACSGroundActor::RockShellRoadBlurRadius` 同默认值，唯一调用方每次都会覆写。
	 */
	float RoadBlurRadius = 300.0f;
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
	/**
	 * 厚度所乘 mask 的下限，见 `ACSGroundActor::RockShellReliefFloor`。
	 * 默认 0 = TG 口径（那边 2026-08-31 已从 0.5 改回 0，这里跟着对齐 —— 唯一调用方
	 * `RebuildRockShell` 每次都会覆写本值，改默认只是消掉两处口径不一的隐患）。
	 */
	float ReliefFloor = 0.0f;

	// —— 「石头隆起」那一组（用户规格 2026-08-31）。逐条含义见 ACSGroundActor 的同名属性。——
	float CellExpand = 0.0f;        // ② ⑤ 每片朝外扩张 cm
	/** 裙圈倾斜：**只有底圈**再朝外推的距离（图案空间 cm，世界 = ×Scale）。斜壁交叉封缝。 */
	float SkirtTilt = 0.0f;
	float RiseMultiplier = 1.0f;    // ③ 隆起倍数
	float RiseNoiseAmp = 0.0f;      // ④ 隆起噪波幅度 cm
	float RiseNoiseFrequency = 0.0f;// ④ 1 / 波长
	float RiseExtend = 0.0f;        // ⑥ 台顶外扩 cm
	float EdgeCeiling = 0.9f;       // ⑧ 末端边缘的隆起 / 地面高度 上限（必须 < 1）
	float PeakHeight = 1.0f;        // ⑧ 的参考：全部塑形物的台顶峰值 cm（= ACSGroundActor::MaxAbsHeight）

	/**
	 * TG `displace_rocky_terrain.cs:563` 的整体基准偏移（沿世界 Z）：
	 * `lerp(−BaseSink, +BaseLift, saturate(Rock − Road))`。mask 满时壳整体浮起、
	 * mask 空或路上时沉下去。TG 的 `mix(-0.4, 0.2, ·)` ⇒ 40 / 20 cm。
	 */
	float BaseLift = 0.0f;
	float BaseSink = 0.0f;

	/**
	 * 表面起伏在**坡度 = 1** 时的满幅 cm（TG `displace:721` 的 0.3 m）。**不是常幅** ——
	 * 实际幅度 = 本值 × 坡度 × `mix(-0.2, 1−smoothstep(4,7,坡度), n01)`，偏正。角点不加。
	 */
	float NoiseAmp = 0.0f;
	float NoiseFrequency = 1.0f / 150.0f;
	/** 边缘磕碰：第二条高频带，恒幅 cm（不随坡度）。角点照旧钉死。 */
	float ChipAmount = 0.0f;
	float ChipFrequency = 1.0f / 40.0f;
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
 * 色流先在同一张图里过两趟可分离高斯（`RoadFade` 截出的足迹，按 `RoadBlurRadius` 糊开），
 * 披挂采的是糊出来的那张临时场 —— 路缘因此是一段缓坡，不是一道折痕（用户裁决 2026-09-11）。
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
