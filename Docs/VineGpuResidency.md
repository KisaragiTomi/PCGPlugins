# 藤蔓 GPU 常驻化：现状与剩余计划

记录 `AVineContainer` 生成链路（表面体素 → 空间竞争 → VisVine 网格）从"CPU 回读 + `UDynamicMesh`"收敛到 GPU 常驻的当前状态，以及还没做的部分。

上游的 GPU 三角形统一化背景见 [`GpuTriangleUnified_Plan.md`](GpuTriangleUnified_Plan.md)。

## 现状

整条链路是**一张 `FRDGBuilder` 图**：空间竞争、前缀和、concat、建网格全部记录在 `FVineMeshSceneProxy::BuildGeometry` 里，中间产物是图生命期的瞬态 buffer。

| 阶段 | 实现 | 数据去向 |
| --- | --- | --- |
| 表面体素（位置/法线/目标点场） | `AComputeShaderMeshGenerator::PrepareBoxSceneSurfaceVoxelsGPU` | pooled buffer 常驻，零回读；有效数留在 `Counter[0]` |
| 三角形缓存 | `EnsureTriangleCacheByBox` | 全程 RenderTarget，零回读 |
| 空间竞争生长 | `AddVineSCPasses`（每源一次） | 瞬态，紧凑计数只写进 GPU buffer |
| 逐源计数前缀和 + concat | `AddVineFusedSCConcatPasses` | 瞬态，合并后的三条 + 总计数 |
| 藤蔓网格 | `AddVineMeshPasses` | 直接写 `UCSGpuMeshComponent` 的常驻 stream，每帧绘制 |

游戏线程只做 CPU 侧准备（`AVineContainer::PrepareVineFusedSCInputs`），把结果塞进 `FVineBuildInput::FusedSC` 就返回；解算本身在渲染线程建图时才发生。

`UDynamicMesh` 已从藤蔓表面路径移除；`Save Mesh` 走 `UCSGpuMeshComponent::SaveRenderedMeshToStaticMesh`。

## 仅剩的 CPU 交互

只剩 `Save Mesh` 的一次性回读：固有且按需，StaticMesh 资产必须在 CPU 上构建，不属于卡点。

合图删掉的东西：

- 三处 `FlushRenderingCommands`（SC 两处、concat 一处）；
- SC / concat 的 `ConvertToExternalBuffer`；
- SC 之后的四-uint `LineCounts` 回读；
- `FVineSCGPUBuffers` 结构体和 `AVineContainer::TubeLineGPUBuffers` 字段；
- N 条 render command（N = 源数量）合成 0 条。

## 计数只在 GPU 上意味着什么

`CreateBuffer` 收的是 CPU 元素数，所以**分配尺寸仍由 CPU 决定**：每源按 `min(TargetCount × (SC_MAX_BACKTRACK + 1), VV.MaxVinePointCount / 源数量)` 分配，concat 目标和 `FVineMeshSceneProxy::RegisterStreams()` 的 stream 容量是这些的和。真实点数只有 GPU 知道，因此下游一律"按容量分配、按 GPU 计数派发和绘制"。

支撑这条规则的三处机制：

- **`VineDispatchArgsCS`**（`VVVoxel.usf`）：单线程直接派发，把 `[lineCount, pointCount, segmentCount]` 换算成四组 `DispatchIndirect` 参数，外加 `DrawIndexedIndirect` 参数和 CPU 回读用的 mesh counters。它总会执行，所以零长度藤蔓也会写出一个规规矩矩的零索引 draw，而不是留下上一个 owner 在 pooled buffer 里的残值。

  | 槽位 | 线程数 | 消费者 |
  | --- | --- | --- |
  | 0 | 路径点数 | `ApplyNoise` / `BuildAxes` / `PerlinNoise` / `FinalProject` / `ResampleSurface` / `SmoothPath`×N / `BuildParallelTransportFrame` / `VineUV.Centers` / `VineUV.ScanBlock` / `VineUV.ScanApply` |
  | 1 | 段数 | `VineUV.SegLen` |
  | 2 | `max(顶点数, 段数)` | `BuildVVVoxelCS`（两半用不同索引） |
  | 3 | 顶点数 | `VineUV.Write` |

- **`ConcatPrefixSumCS`**（`SpaceColonizationQueue.usf`）：单线程扫描逐源计数，产出每源 `[pointBase, segBase, clampedPoints, clampedSegs]` 和批次总计数。`FConcatOffsetInt4CS` / `FConcatCopyFloat4CS` 从这个 buffer 读 base 和 count，位置拷贝因此从 `AddCopyBufferPass`（CPU 字节偏移）变成 compute 拷贝。

- **容量只作范围守卫**：`PathPointCount` / `SegmentCount` / `OutputVertexCount` / `OutputIndexCount` 这几个 uniform 现在是分配容量，shader 里只用来夹住索引；真实计数从 `VineMeshCounts` / `VineUV_Counts` 读。轴向 V 的扫描（下一节）三个 kernel 一律按 `VineUV_Counts[1]` 定范围——按容量走会让扫描跑满 26 万槽位。

concat 目标 buffer 在写入前整体清零：group size 向上取整会让最多 63 个线程越过真实计数，`PathPointMeta` 的 `Count` 读到 0 才不会让下游 kernel 拿着任意索引乱跳。

**不要改成"每源固定容量切片"**：那样各源的 segment 在全局不连续，`RWIndirectArgs[0]` 无法用"总段数 × ProfileCount × 6"计算；绕开就得给无效槽位写退化三角形，等于每帧常驻绘制约 150 万个退化三角形（按 `MaxVinePointCount = 262144`、`ProfileCount = 3` 估算）。前缀和是必需项。

## 轴向 V：分段前缀和

基础流路径上的 mesh `UV.y` 由 `VVVoxel.usf` 的 `VineUV*` 一组 kernel 在 GPU 上算出，`BuildVVVoxelCS` 先写的 `0` 被它们覆盖。它算的**不是**点到终点的距离，而是沿线累积的"归一化弧长"，并在每条线的起点清零：

```text
Inc[P]  = (SegLen[P-1] / max(avg(RingCirc[P-1], RingCirc[P]), 1e-4)) * LengthScale
Head[P] = (P == 0) || (SegLen[P-1] < 0)   // SegLen 清成 -1，-1 表示没有段进入 P
CurveV  = Inc 的分段包含式前缀和，在每个 Head 处重启
```

除以平均环周长是关键：V 的单位是"周长"而不是世界单位，贴图沿藤蔓的纵横比才不随粗细变化。`Head` 处 `Inc` 定义为 0，所以段首的 `CurveV` 正好是 0。

因为它是标准的 segmented scan，用的就是两级并行方案，取代了原来 `[numthreads(1,1,1)]` 的串行循环：

| kernel | 派发 | 职责 |
| --- | --- | --- |
| `VineUVScanBlockCS` | indirect，槽位 0 | 块内 LDS 分段 Hillis-Steele，另写出块聚合 `(尾段和, 块内是否含 Head)` |
| `VineUVScanBlockOffsetsCS` | 直接 `1×1×1` | 单块按 tile 循环扫块聚合，产出每块的 exclusive carry-in |
| `VineUVScanApplyCS` | indirect，槽位 0 | 把 carry-in 加回去，跳过块内前缀已经撞上 Head 的点 |

分段合并算子是 `(v,f) × (v',f') -> (f' ? v' : v+v', f|f')`：某个 lane 的窗口一旦包含 Head，它的值就已经定稿，后续步长不能再把更早的 lane 折进来。`VineUVScanBlockCS` 写出的 `VineUV_ScanFlags[P]` 就是"块内 `[0,P]` 是否出现过 Head"，`VineUVScanApplyCS` 直接拿它当"不必加 carry"的判据。

几个容易踩的点：

- **块大小必须等于 `VineMeshGroupSize`**（C++ 通过 `VINE_UV_SCAN_BLOCK` 定义下发）。相等，`VineUVScanBlockCS` 的分块才和槽位 0 的 `DispatchIndirect` 参数一致，不用再开一个 arg 槽位。
- **末块的尾 lane 补 `(0, 0)`**（合并算子的单位元），于是 lane `BLOCK-1` 自然携带最后一个有效 lane 的聚合值；聚合写入必须放在 `P < N` 守卫之外，否则末块根本不写聚合。
- **`VineUVScanBlockOffsetsCS` 单块够用**：它按 tile 循环，一次派发覆盖任意块数（容量 262144 点 = 4096 块 = 64 个 tile）。tile 可能不满，跨 tile 的 carry 要取"最后一个**有效** lane"而不是 lane `BLOCK-1`。
- LDS 用量 `64×4 + 64×4 + 4 = 516` 字节，远低于 Vulkan 保底的 16 KB。

中间 buffer（`VineUV.ScanFlags` / `ScanBlockSum` / `ScanBlockFlag` / `ScanBlockCarry`）由 CPU 按容量派生尺寸（RDG 建图期必须给元素数），但填它们的三个 pass 一律由 GPU 计数定范围，没有任何计数回到 CPU。

## 容量上限

`FVisVineGPUParameter::MaxVinePointCount`（默认 `262144`）是整批藤蔓共用的路径点上限，同时是 GPU buffer 的分配容量。

每源容量取 `min(TargetCount × (SC_MAX_BACKTRACK + 1), MaxVinePointCount / 源数量)`，即理论界与批上限份额中更紧的那个 —— 目标数少时仍按理论界分配，不会因为引入上限反而多占显存。被上限压到时打 Verbose 日志。

代价：`RegisterStreams()` 现在按容量开常驻 stream。极端情况（容量吃满 `262144`、`ProfileCount = 3`）约 44 MB 常驻 + 约 30 MB 图内瞬态。实测场景（36 个 target、单源）容量只有 `3636`，不是问题；真实规模下 `MaxVinePointCount` 需要按场景校准。

## 管径（锥度）：实测链路与"藤蔓变细"的真实来源

管径只由一条公式决定，`BuildVVVoxelCS` 里没有第二个入口：

```text
ring radius = 10 · VV.CircleScale · PathPoints[P].w
PathPoints[P].w = CurveLUT(l / (N-1)) · (TargetScale · StartSourceScale · 分叉锥度)
```

`l` 是点在**本线内**的序号、`N` 是**重采样后**的本线点数（`CurveSpaceColonizationCS` 用 `NewLineOffset` / `NewLineLength`）。锥度在 concat **之前**、每源各自的紧凑布局上求值，所以 concat 的前缀和与 base 语义不参与锥度。

在 `L_TestWorld`（2 源 / 980 target）上做过一次端到端回读核对（临时代码，已删）：

| 量 | 实测 |
| --- | --- |
| GPU 计数 | lines=303 points=12054 segments=11751（容量 197960） |
| `.w` | min 0 / max 1.933 / mean 0.466，其中 303 个 0 恰好等于线数（每条线的 tip） |
| 实测环半径 | min 0 / max 3.866 / mean 0.932 cm |
| 实测半径 ÷ `10·CircleScale·.w` | **1.00000**（11751 个点全部） |

结论：**从 `.w` 到顶点的几何段完全忠实**，`.w` 也没有在 `ConcatCopyFloat4CS` 里被截断或改写。逐线抽样同样对得上，例如 base=15 count=22 的线：`w[mid]` 在 `l=11` 处等于 `CurveLUT(11/21)=0.5238 × 1.749`，`w[last]` 在 `l=21` 处等于 `1.0 × 0.176`（0.176 = 1.749 × `ForkTaperEndScale` 0.1）。**锥度参数 `t` 没有错位。**

真正决定"粗细"的是**曲线本身**：`VV.CurveControl` 在 `L_TestWorld` 的实例上是 `None`，`PrepareVineFusedSCInputs` 于是 `NewObject<UCurveLinearColor>` 兜底。`UCurveLinearColor` 的构造函数（`WITH_EDITOR`）会塞进 black(0)→white(1) 两个 key，因此 `.G` 是一条 **0→1 线性斜坡**：LUT `[0]=0`、`[128]=0.502`、`[255]=1`、mean `0.5`。也就是说，未指定曲线时锥度是"tip 半径 0、根部满值"，**平均管径只有平坦曲线（`.G ≡ 1`）的一半**，且每条线的第一个环是退化的零半径环。这不是 bug，是"没配曲线"的默认外观；想要更粗就给 `VV.CurveControl` 指一条 `.G` 抬高的曲线，或调 `VV.CircleScale`（默认 `0.2`）。

## 风险与未验证项

- **整体外观一致性未确认。** 已实测的是"跑得起来"：在 `Lvl_TopDown` 上用合成的 1 源 / N×N target 网格跑通 `GenerateVines`，整张 `VineMesh.Build` 图在 GPU 上执行完毕，无 TDR、无崩溃、无 shader 参数校验失败。除轴向 V 与上一节的管径外，**没有**与旧路径做过逐顶点或截图比对。
- **SC 输出与合图前的版本逐数对齐**：`L_TestWorld` 上合图后回读到 points=12054 / segments=11751，和合图前那次日志里的 `Vertices=36162 Indices=211518`（= 12054×3 / 11751×3×6）完全一致。这一批改动没有动几何。
- **每源容量被批上限压到时会静默截断。** `PerSourceCapacity = min(TargetCount × 101, MaxVinePointCount / 源数量)` 直接当 `EmitSpaceColonizationLinesCS` 的 `PathPointCapacity`，越界点只是 `break`；而 `LineLength` / `LineOffset` 仍是未截断的值，`CountResample` / `EmitResample` 会读到越界（返回 0）的点，弧长和锥度随之失真。计数只在 GPU 上，所以**没有任何警告**（`MaxVinePointCount` 的属性注释说"会截断并打警告"，实际没有）。实测场景容量 197960 vs 真实 12054，远未触发。
- **轴向 V 已做过几何自洽核对**（不是逐顶点 A/B）：`Save Mesh` 烘出 StaticMesh 后，从索引缓冲重建环拓扑（管状网格每个三角形恰有两点同环、一点在邻环），逐对相邻环核对 `V[b] == V[a] + dist(Ca,Cb)/avg(周长)*LengthScale`，段首允许 `V == 0`。14003 环 / 26105 对相邻环全部通过，最差相对偏差约 `1e-3`，且不随点数增长——这个量级是 StaticMesh 把 UV 存成 half 造成的量化底噪，不是扫描误差。跨块 carry 若丢失会造成整块量级的偏差（相对误差百分之几），核对里没有出现。
- 上述核对的**取样噪声底是 half 精度 UV**，只能验到约三位有效数字。要更严格就得在 GPU 上直接回读 `VineUV.CurveV`（当前没有这条通路）。
- `Save Mesh` 的 local-space 烘焙改用 actor 变换（leaf 被钉在 identity 世界变换上，用其自身组件变换会导致双重偏移），生成的 `AStaticMeshActor` 同样摆在 actor 变换上。该路径在上面的 V 核对里被实际走通了一次（能烘出 StaticMesh 并读回顶点），但摆放位置的正确性仍未核对。
- `FCSSurfaceVoxelGPUBuffers` 的 `VoxelCount`（真实数）与 `VoxelCapacity`（容量）在 `VineLeaf_BuildVineBuildInput` 的两条分支里取值不一致：GPU-voxel 分支把 `VoxelCount` 也设成容量。`BuildVineVoxelHashCS` 已改为从 `Counter[0]` 读真实数并早退（容量 slack 全是同一个零 cell，插进哈希表会让线性探测退化成 O(slack²) 原子操作，实测直接把 GPU 挂死），但其余 kernel 的 `VoxelCount` 仍是容量，只影响回退路径的松紧，未收敛。
- `Docs/VisVineGPU_AlgorithmFlow.svg` 与 `Docs/GpuTriangleUnified_Architecture.svg` 仍描述已删除的 `DispatchVVGPU_Voxel` / `BuildDynamicMeshFromGPUVineOutput` 流程，需要重绘。

## Open Questions

- SC 是 `SC.Iteration` 轮 × 每轮若干 pass，且每源一遍。合图后 N 个源全在一张图里，pass 数是 N × (3 + 5 × Iteration + 10)，建图开销和 RDG Insights 可读性会随源数量变差。源很多时值得把 SC 改成单次 dispatch 内按 source index 分流。
- `MaxVinePointCount` 的默认值 `262144` 未经真实场景校准。它现在直接决定常驻显存，需按实际藤蔓规模调整。
- `AVineContainer::DrawDebugCachedVineSCStagePoints` 已退化成只打一条警告的存根（CPU 侧 SC 阶段点缓存随解算上 GPU 一并移除），只为不破坏已有蓝图绑定而保留。确认无蓝图依赖后可以删除。（同类的 `SpaceColonizationWithScales` 存根已删除。）
- ~~**`BP_VineSource` 编译不过**~~（2026-08-13 已修）：删 `UDynamicMeshComponent` 那一批之后，`GenerateVines` 的返回值从 `UDynamicMesh*` 变成 `bool`，蓝图里 `GenerateVines → ReturnValue` 还接着 `IsValid` 的 `InputObject`，另有 6 处 `Get DynamicMeshComponent` 变量取值失效。已在蓝图里删掉这条早就无产出的支线（旧链路只是把返回的**空** `UDynamicMesh` 塞回 `SetDynamicMesh`，所以断掉本就不影响藤蔓外观），生成结果已人工确认正确。
