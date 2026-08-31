#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"   // FPaletteBuffers / ReserveCapacity / ReleaseOnRenderThread

class UStaticMesh;
struct FCSGpuMeshCPUData;
struct FCSWallOpening;

/**
 * 墙面藤蔓（TinyGladeHouse D13）。
 *
 * 分工与门框砖同型：**CPU 只解"藤爬到哪里"**，实例行的组装在渲染线程的一张 RDG 图里跑完，
 * CPU 不算一个变换、不回读一个字节。区别只有一条 —— 门框走曲线求值，藤蔓的段是**直线段**，
 * 所以不需要样条，一趟 dispatch 打完。
 *
 * ⚠️ **不要拿 `VineScatter::Scatter*` 那三个入口来做这件事**：`CollectSurfaceTriangles`
 * （`CSVineScatter.cpp:38`）只遍历 `UStaticMeshComponent`，而房子挂在 `UCSMeshRenderComponent`
 * 上 ⇒ 它对房子**静默返回空三角集、不报错**。这条路是新写的「墙矩形 → 点集 → 填 ISM」。
 *
 * ⚠️ **随机数一律由记录自带**，绝不取 `InterlockedAdd` 的槽位：槽位由线程组完成顺序决定，
 * 同一份世界状态两次 dispatch 可以把同一段藤放进不同槽 ⇒ 材质拿它做颜色变化时"重建一次
 * 全场变色"，而且**不会有任何断言报红**（S1 已经栽过一次）。这里的身份是
 * (墙号, 藤号, 段号, 用户种子)，在 CPU 侧可精确复算，所以"随机只由身份决定"是可断言的。
 */
namespace CSHouseVine
{
/** 一面墙的外皮矩形。原点在墙脚外棱，U 沿墙、Up 向上、N 朝屋外，三者右手正交。 */
struct FWallStrip
{
	FVector Origin = FVector::ZeroVector;   // 世界，墙脚外棱的起点
	FVector U = FVector::ForwardVector;     // 世界单位，沿墙
	FVector Up = FVector::UpVector;         // 世界单位，向上
	FVector N = FVector::RightVector;       // 世界单位，墙面外法线
	float Length = 0.0f;
	float Height = 0.0f;                    // 檐口高（= 墙高）。山墙的三角部分见下面三个字段
	int32 EdgeIndex = 0;                    // 与 FCSWallOpening::EdgeIndex 同一套编号

	/**
	 * 山墙三角：墙顶在 S 处比 `Height` 高出多少（檐墙三项全 0 ⇒ `TopAt` 恒等于 `Height`）。
	 *
	 * ⚠️ **刻意存成三个标量而不是让规划器去问 `FCSRoofDesc`**：`BuildPlan` 是纯函数、单测里
	 * 没有 actor 也没有屋顶描述；把"墙顶在哪"降成墙自己的一维剖面，屋面方程仍然只有
	 * `CSHouseRoof.h` 那一份真源（`ACSHouseActor::BuildVineStrips` 负责把它折算成这三个数）。
	 * 折算成立的前提是**跨度坐标沿 S 线性**——矩形 footprint 上成立；将来 footprint 变成
	 * 多边形时这三个数要跟着重新推导，别以为它是普适的。
	 */
	float GableTan = 0.0f;                  // 屋面坡度的 tan：离山尖每远 1 cm，墙顶降低多少
	float GablePeakS = 0.0f;                // 山尖（脊线）落在这面墙的哪个 S 上
	float GableHalfSpan = 0.0f;             // 山墙半跨（S 单位）。超出这个范围墙顶就回到 Height

	/** 墙顶在 S 处的高度。藤爬到这条线就停 —— 它上面是屋面板，不是墙。 */
	float TopAt(float S) const
	{
		return Height + GableTan * FMath::Max(0.0f, GableHalfSpan - FMath::Abs(S - GablePeakS));
	}
};

/** 藤蔓的形态参数。全部由 `ACSHouseActor` 的属性喂进来，这里不留默认策略。 */
struct FParams
{
	float StrandSpacing = 90.0f;      // 沿墙每隔多少 cm 起一根藤
	float SegmentLength = 26.0f;      // 一段的世界长度（= 一个 ivy_branch 实例）
	int32 MaxSegments = 22;           // 一根藤最多几段（也是高度上限的另一半保险）
	float Wander = 0.55f;             // 每段方向的随机扰动（弧度）
	float MaxLean = 1.15f;            // 相对"正上"的最大偏角（弧度），超过就掰回来
	float Bloat = 1.15f;              // 长度轴胀大系数 —— 与门框砖同一条理由：正缝会露出断口
	float Thickness = 9.0f;           // 截面直径（世界 cm）
	float StandOff = 3.0f;            // 沿墙面法线离墙多远，避免与墙面 z-fighting
	float HoleClearance = 12.0f;      // 墙洞外扩多少才算"撞上"
	float LeafChance = 0.72f;         // 每段长叶子的概率
	float LeafSize = 26.0f;           // 叶片的长度（世界 cm）
	float LeafSizeJitter = 0.35f;     // 叶片尺寸的对称抖动
	float FlowerChance = 0.10f;       // 段的开花概率（只在 FlowerFromFrac 以上的段上掷）
	float FlowerFromFrac = 0.45f;     // 从一根藤的第几成开始才可能开花
	float FlowerSize = 22.0f;         // 花簇的**宽度**（世界 cm）
	/**
	 * 花簇高/宽比。`ivy_flower` 实测包围盒 (76.04, 77.99, 38.40) ⇒ 38.40 / 77.0 ≈ 0.50。
	 * 这个数在这里而不是硬编在 kernel 里，是因为 `FPaletteBuffers::BlockSize` 把 xy 钉成
	 * 边长 = `FlowerSize` 的正方形截面，z 只留 1/网格长度 —— 记录必须自己把"想要多高"说出来，
	 * 否则花会被拉成 `FlowerSize` 高的柱子（枝是管、拉长正确；花是簇、拉长就穿帮）。
	 */
	float FlowerAspect = 0.50f;
	/**
	 * 转角跨墙的概率（TG 的 `check_for_wall_jump`）。藤走到墙角时：
	 * 掷中 ⇒ 拐上相邻那面墙继续长；没掷中 ⇒ 照旧把倾角镜像回来。
	 * 0 = 完全关掉（回到第一档行为）。
	 */
	float JumpChance = 0.5f;
	int32 Seed = 1;
};

/**
 * 一条实例记录 = 3 个 float4，与 `CSHouseVine.usf` 文件头的布局逐字对应。
 * 分开成结构体只是为了让 CPU 侧读得懂，上传时按 `FVector4f` 平铺。
 */
struct FRecord
{
	FVector3f WorldPos = FVector3f::ZeroVector;
	float LengthScale = 1.0f;
	FVector3f Dir = FVector3f(0.0f, 0.0f, 1.0f);
	float Random01 = 0.0f;
	FVector3f Normal = FVector3f(0.0f, 1.0f, 0.0f);
	float SizeScale = 1.0f;
};

/** 一次规划的产物：三个调色板（0 = 枝、1 = 叶、2 = 花）各自的记录。 */
struct FPlan
{
	TArray<FRecord> Branch;
	TArray<FRecord> Leaf;
	TArray<FRecord> Flower;

	bool IsEmpty() const { return Branch.IsEmpty() && Leaf.IsEmpty() && Flower.IsEmpty(); }
	void Reset() { Branch.Reset(); Leaf.Reset(); Flower.Reset(); }
};

/** 调色板序号。**与 `ACSHouseActor::VineGpuBuffers` / `Pack` 的下标是同一套**，别各写各的。 */
enum EPalette : int32 { Palette_Branch = 0, Palette_Leaf = 1, Palette_Flower = 2, Palette_Num = 3 };

/**
 * 身份哈希：(墙号, 藤号, 段号, 佐料, 用户种子) → uint。
 *
 * ⚠️ **这就是"不许用槽位"那条纪律的执行面**。身份里刻意**不含位置** —— 拖房子时墙面在动，
 * 位置派生的种子会让整片藤在拖动过程里不停重掷；身份只在"这面墙上第几根藤的第几段"
 * 真的变化时才变。TG 的对位物是 `_rocky_terrain_stairs_stairs.cs:504` 的
 * `uint((Tx+Ty+Tz)*100)`（位置派生），这里比它更稳。
 */
COMPUTESHADERGENERATOR_API uint32 IdentityHash(int32 EdgeIndex, int32 Strand, int32 Segment, uint32 Salt, int32 Seed);

/** uint → [0,1)，与 `CSGpuInstancedMesh.usf:120-126` 的收尾段同一份数学。 */
COMPUTESHADERGENERATOR_API float Hash01(uint32 H);

/**
 * 纯函数：墙矩形 + 洞表 + 参数 → 记录。**不碰任何 GPU 资源，可以在纯 CPU 单测里跑。**
 *
 * 这条"能在没有 world 的用例里跑"的性质不是附赠品：藤蔓最容易错的两件事
 * （避不避墙洞、身份稳不稳）都只能在这一层断言，等到 GPU 那一侧就只剩一个实例计数了。
 */
COMPUTESHADERGENERATOR_API void BuildPlan(const TArray<FWallStrip>& Strips, const TArray<FCSWallOpening>& Openings,
	const FParams& Params, FPlan& OutPlan);

/**
 * 墙面点 (S, Z) 落在这面墙的某个洞里（洞形外扩 `Clearance`）？
 *
 * ⚠️ **判据必须与材质那份 clip 场同源，不能拿洞的外接矩形凑合**（第一档就是矩形）：
 * 拱的**肩角**（拱脚线以上、半圆之外的那两块）在几何上是**实心墙**，材质也确实把它画出来。
 * 这里走 `CSHouse_ComputeClipField` + `CSHouse_ClipKeeps` —— 与逐像素 discard **同一条曲线**，
 * 所以"藤让开的地方"与"墙真的被切掉的地方"逐点一致（裁决三的一条可判定形式）。
 *
 * ⚠️ **别指望它能治"拱附近变稀"**（实测，2026-08-31）：换成解析场之后拱肩上也只多出 0~2 段，
 * 因为 `HoleClearance`（12 cm，下界由门框砖的 `FrameBrickDepth / 2` 钉死）几乎把肩上
 * "藤够得着"的那条自由带吃光了。变稀的主因是**落地拱把整条墙脚吃掉**、第一档"藤脚在洞里
 * 就整根不长"，解药是 `CSHouseVine_EscapeRoot` 的侧移。这条注释是给下一个想"再放宽一点"
 * 的人看的：放宽外扩不行（会穿进门框砖），放宽形状已经做完了，剩下的空间在**起点**上。
 *
 * 外扩的做法是把**洞本身**胀大 `Clearance` 再算场（不是把场的结果外扩）：拱胀大以后拱脚线
 * `Z1 − HalfWidth` 逐位不变、半圆半径 +Clearance，正好是那条曲线的等距外偏移。
 *
 * ⚠️ 两条**有意**与材质保持一致的偏差，别当成漏洞去"修"：
 *   · `Skew`（楼梯斜洞顶）不参与 —— `FCSOpeningClipField` 里根本没有这个量，材质也不读；
 *   · Arch 的场在拱脚线以下**无下界**，所以这里另外补一条 `Z ≥ Z0 − Clearance`
 *     （窗台以下那一截是实心盒，藤该长在上面）。
 */
COMPUTESHADERGENERATOR_API bool IsInsideOpening(const TArray<FCSWallOpening>& Openings, int32 EdgeIndex,
	float S, float Z, float Clearance);

/**
 * 把基础网格读成一份自带**法线与 UV** 的快照，并把它的长度轴换到 +Z。
 *
 * ⚠️ **这个函数存在的唯一理由是 `ivy_branch` 的顶点流只有 `Vertex_Position`** ——
 * 没有法线也没有 UV（对照文档 §7.4 实测）。直接丢给任何常规材质都是一块死黑，
 * 而 `UCSGpuInstancedMeshComponent::SetBaseMesh` 会原样把那份空的切线基搬进快照。
 * TG 自己的做法是在 VS 里现搭截面基；本项目不写自定义顶点工厂，所以改成**导入期补齐**：
 *   · 法线 = 相邻面法线累加（`ivy_branch` 是 12 顶点 / 6 三角的开口三棱管，顶点不共享 ⇒
 *     累加出来正好是逐面平法线，这对棱柱是正确答案，不是近似）；
 *   · UV = 绕长度轴的柱面展开（U = 极角，V = 沿长度归一），贴图因此不会被非均匀缩放拉成条。
 * 源网格自带非退化的法线/UV 时（`ivy_leaf` / `ivy_flower` 就是）原样沿用，只做换轴。
 *
 * `LengthAxis` 是**源网格**的长度轴（0=X 1=Y 2=Z），换轴后一律变成 +Z ——
 * `CSHouseVine.usf` 的基约定只认 +Z，两种网格共用一支 kernel。
 *
 * 返回是否读出了可用的几何（读不出时 Out 被清空，调用方应当据此报错而不是画一块灰）。
 */
COMPUTESHADERGENERATOR_API bool BuildBaseMesh(UStaticMesh* Source, int32 LengthAxis, FCSGpuMeshCPUData& Out);

/**
 * 录一趟打包 pass：三个调色板各上传一次记录 → 各一个 dispatch 写满 packed 行 + counter。
 *
 * **录完直接返回，不阻塞**（Palettes 按值捕获，`TRefCountPtr` 拷贝即加引用）。前置条件是
 * `CSShaperSteps::ReserveCapacity` 已经把容量备好 —— 这里只用现有容量，一个字节都不分配，
 * 所以交互期（拖房子、画路）走不到任何阻塞路径。容量不够时**截断**而不是扩容：
 * 少画几段藤远好过在拖动的某一帧里付一次设备同步。
 *
 * 返回是否真的录了 pass。
 */
COMPUTESHADERGENERATOR_API bool Pack(const FPlan& Plan, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes,
	const FMatrix44f& WorldToComponent);
}
