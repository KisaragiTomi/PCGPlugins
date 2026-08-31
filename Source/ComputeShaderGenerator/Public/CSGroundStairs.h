#pragma once

#include "CoreMinimal.h"
#include "CSMesh.h"
#include "RenderGraphResources.h"

/**
 * 石阶：100% GPU 决策 + 零回读（计划「石阶改造：marching squares」的 S1/S2/S3）。
 *
 * **全项目唯一的一条石阶路**（S3 已完成，2026-08-30「裁决一」）。曾经并存的旧路
 * （`ACSGroundShaperActor::RebuildSteps` + `CSShaperSteps` + `CSGroundSteps.usf`：
 * CPU 解层 / 弧段 / 铺装、GPU 只组装变换）已整条删除。删它的直接理由是那条路的等高线半径
 * 用「关于某座塑形物中心的星形」闭式解，**两座相接土台的接合处直接断掉**；本路的
 * marching squares 在**合成后**的高度场上逐格出等值线段，接合处只是普通的一格，
 * 而且 CPU 不知道有几级台阶。
 *
 * **零回读怎么保住**：CPU 既然不知道实例数，容量就必须一次性定死、永不重算 ——
 * `EnsureBuffers` 只在容量真的变了（= 配置改了）时分配一次，交互期（画笔刷、拖塑形物）
 * 走的永远是"容量够用"的分支，一次 enqueue 都不发。越界静默丢弃，包围盒按地面矩形写死。
 * 这比旧路严格：那条路每次重排记录都要重算需要多少格，于是逐笔扩容、逐笔阻塞。
 */
namespace CSGroundStairs
{
/** 固定容量的实例源，直接喂 `UCSGpuInstancedMeshComponent::SetInstanceSourceGPU`。 */
struct FStairBuffers
{
	TRefCountPtr<FRDGPooledBuffer> PackedInstances;   // Buffer<float4>，5 个 / 实例
	TRefCountPtr<FRDGPooledBuffer> Counter;           // Buffer<uint>，[0] = 活跃数（只有 GPU 知道）
	uint32 Capacity = 0;

	/**
	 * 小石子（TG 的 15% 支线）走**自己的**一对 buffer。
	 *
	 * 不是"顺手多分一块"：一个 `UCSGpuInstancedMeshComponent` 只绑一张基础网格，而石子与石阶
	 * 是两张网格 ⇒ 两个组件 ⇒ 两套实例源。`FCSGpuInstanceSourceGPU` 里也没有偏移量字段，
	 * 塞进同一块 buffer 的后半段是画不出来的。TG 同样另开一条 indirect draw。
	 */
	TRefCountPtr<FRDGPooledBuffer> PebbleInstances;
	TRefCountPtr<FRDGPooledBuffer> PebbleCounter;
	uint32 PebbleCapacity = 0;

	bool IsValid() const { return PackedInstances.IsValid() && Counter.IsValid() && Capacity > 0; }
	/** 石子的 UAV 是**无条件绑定**的（RDG 不接受空参数），所以它必须始终有效，与出不出石子无关。 */
	bool HasPebbles() const { return PebbleInstances.IsValid() && PebbleCounter.IsValid() && PebbleCapacity > 0; }
	void Reset() { *this = FStairBuffers(); }
};

/** 一趟扫描的全部标量参数。命名与 `CSGroundStairs.usf` 的 uniform 一一对应。 */
struct FScanParams
{
	FMatrix44f WorldToComponent = FMatrix44f::Identity;

	/** 扫描格：原点是 (0,0) 格的角，`CellSize` **必须 ≈ 石阶长度**（每条穿越边出一级，
	 *  间距完全由格密度决定：太密穿模、太疏断续）。这是 TG 把 contouring 格单开一个维度的原因。 */
	FVector2f GridOriginXY = FVector2f::ZeroVector;
	float CellSize = 100.0f;
	FIntPoint GridDims = FIntPoint(0, 0);

	/** 地面镜像格（只用来取道路权重：色流的 R 通道，V = Y * VertsX + X）。 */
	FVector2f GroundOriginXY = FVector2f::ZeroVector;
	float GroundCellSize = 50.0f;
	FIntPoint GroundVerts = FIntPoint(0, 0);
	float GroundBaseZ = 0.0f;

	float StepHeight = 30.0f;
	float RoadThreshold = 0.35f;
	float Embed = 25.0f;
	/** 把块抬起半个身位（= −基础网格局部包围盒 Min.Z × Z 缩放），否则盒心落在等值线上、
	 *  石块一半埋在地里 —— 旧路记下来的实测修正，这里逐字沿用。 */
	float Rise = 0.0f;
	float ZOffset = 0.0f;
	uint32 MaxLayersPerCell = 32;

	FVector3f BaseSphereCentre = FVector3f::ZeroVector;
	float BaseSphereRadius = 0.0f;
	/** 三轴块尺寸。基础网格自带真实尺寸时留 (1,1,1)；喂居中单位立方体（`stairs_step` 是
	 *  100³ 的居中盒）时，非均匀缩放本身就是石阶的尺寸。轴向同 kernel：X 进深、Y 长度、Z 高。 */
	FVector3f BlockSize = FVector3f(1.0f, 1.0f, 1.0f);

	// --- S2：逐实例抖动（随机源是**格身份**，不是 InterlockedAdd 的槽位，理由见 kernel）---

	/** 基础网格的局部 Y 尺寸。kernel 要把"想要的世界长度"换算回缩放，没有它就换算不了。 */
	float BaseSizeY = 1.0f;
	/** 长度轴胀大系数（≥ 1）。负缝的来源 —— 与 `ACSGroundActor::StairLengthBloat` 同义。 */
	float LengthBloat = 1.0f;
	/** 长度轴抖动幅度，**单侧**（只增不减；对称抖会把负缝抵消掉）。 */
	float LengthJitter = 0.0f;
	/** 进深 / 高度的对称抖动幅度（这两轴沿等值线没有邻居，所以敢双向抖）。 */
	float SizeJitter = 0.0f;
	/** 绕世界 +Z 的偏航抖动，**弧度**，±。不抖 pitch/roll —— 踏面必须水平。 */
	float YawJitterRad = 0.0f;
	uint32 JitterSeed = 0;

	// --- 小石子（TG `_rocky_terrain_stairs_stairs.cs:511-547`）------------------

	/** 每段等值线额外出一颗石子的概率（TG 实测 0.15）。**0 = 整条支线一个字节都不写**。 */
	float PebbleChance = 0.0f;
	/** 均匀缩放的下/上限，**已经是"乘在基础网格上的系数"**（cm 口径的换算在 actor 侧做完）。 */
	float PebbleScaleMin = 0.0f;
	float PebbleScaleMax = 0.0f;
	FVector3f PebbleSphereCentre = FVector3f::ZeroVector;
	float PebbleSphereRadius = 0.0f;
};

/**
 * 把容量补到 `Capacity`（只涨不缩）。**分配必须在渲染线程，所以真扩容那一趟是阻塞的** ——
 * 因此它只应该在注册 / 加载 / 改配置时被调到，交互期永远走"已经够大"的零成本分支。
 *
 * 返回缓冲区是否可用。
 */
COMPUTESHADERGENERATOR_API bool EnsureBuffers(FStairBuffers& Buffers, uint32 Capacity, uint32 PebbleCapacity);

/**
 * 跑一趟扫描：清零 counter → 一个 dispatch 覆盖整张扫描格。**录完 pass 直接返回，不阻塞**。
 *
 * `GroundResident` 是地面网格的常驻流集合，只读它的色流取道路权重 —— 读的正是笔刷双写出来的
 * 那一份权威投影，所以"画面上看到的路"与"长石阶的判据"是构造上同源的，不存在第三条路。
 * 用 `FCSMeshRenderThreadEdit` 进出，访问状态由它恢复（直接写流再手工恢复是同一条规则的第二份
 * 拷贝，而漂掉的那份不会报错，只是安静地停止工作）。
 *
 * `ShaperParams` 是 `ACSGroundActor::BuildShaperGpuParams` 打的那一份（每座
 * `CSGroundShaperField::Float4sPerShaper` 个 float4，布局见该头文件），
 * 按值搬进渲染命令。返回是否真的录了 pass。
 */
COMPUTESHADERGENERATOR_API bool Scan(
	const FCSMeshResidentRef& GroundResident,
	const FStairBuffers& Buffers,
	const FScanParams& Params,
	const TArray<FVector4f>& ShaperParams);

/** 把最后一份引用交回渲染线程释放，避免在游戏线程上把在途帧正在读的显存抽走。 */
COMPUTESHADERGENERATOR_API void ReleaseOnRenderThread(FStairBuffers& Buffers);

/**
 * **诊断 / 自动化测试专用**：阻塞回读 counter（可选连实例原点一起）。
 *
 * 运行路径一个字节都不回读 —— 这个函数存在的唯一理由是让"接合处不断裂"这条验收项可断言：
 * 摆位判定全在 GPU 上，除了把结果读回来，没有别的办法证明它。别在任何每帧路径上调它。
 *
 * `OutOrigins` 非空时填**组件空间**的实例原点（packed 行的第 4 行 xyz）。
 * `OutRows` 非空时把整份 packed 行原样带出（**5 行 / 实例**，含被缩放过的基与剔除球）——
 * S2 的抖动只体现在基上，不看基就断言不了"同一格同一层恒等"。返回实例数
 * （已按容量钳过：GPU 侧的 counter 会数到越界丢弃的那些）。
 *
 * `bPebbles` 读的是石子那一对 buffer，其余口径完全相同。
 */
COMPUTESHADERGENERATOR_API int32 DebugReadInstancesSync(
	const FStairBuffers& Buffers, TArray<FVector>* OutOrigins = nullptr, TArray<FVector4f>* OutRows = nullptr,
	bool bPebbles = false);
}
