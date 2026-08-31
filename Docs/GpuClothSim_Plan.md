# GPU 布料模拟计划：XPBD 常驻算子驱动 UCSMesh 形变

承接 [`GpuMesh_DynamicMeshWrap_Plan.md`](../../../GpuMesh_DynamicMeshWrap_Plan.md)（`UCSMesh` / `FCSMeshEditContext` / `UCSMeshRenderComponent` 对象层已落地）。本文规划在 GPUMesh 管线内自写布料模拟：布料状态常驻 GPU，每帧一组 XPBD compute pass 解算，结果直接写回 `UCSMesh` 的 Position / TangentBasis 流，由现有渲染叶子绘制。所有行号为 2026-08-24 现状。

姊妹篇：[`GpuRigidSettle_Plan.md`](GpuRigidSettle_Plan.md)——共享粒子基建的批量刚体沉降 / 稳定性校验；多实例批处理与粒子 spatial hash 在彼处先落地。

## 结论

- **可行，定位是「固定拓扑的每帧形变器」，不是通用物理引擎**。运行期只改顶点位置与法线，不改三角数、不改流布局——这正好避开管线里最难的「计数只在 GPU 上」问题：布料的计数在 setup 时就是 CPU 已知的固定值。
- **求解器自写 XPBD，不是「把 Chaos 搬上 GPU」**。Chaos 布料求解器没有 GPU 路径可迁移（见下节），而自写所需的基建（常驻流、借图编辑入口、LBVH、GDF 采样、多 substep 单图先例）项目里全部现成。
- **sim 状态不进 `UCSMesh` 流集合**，由新组件 `UCSClothSimComponent` 自持 pooled buffer（`FCSSurfaceVoxelGPUBuffers` 同形态）；每帧通过 `FCSMeshRenderThreadEdit`（`CSMesh.h:313`）借图写回网格流。这是本计划最重要的一条边界，理由见 D1。
- **运行循环零回读**：setup 一次性 `ReadbackMeshSync`（`CSMesh.h:632`）取拓扑、CPU 建约束、上传；之后每帧纯 GPU。
- **渲染与落盘免费**：`UCSMeshRenderComponent` 绑同一个 `UCSMesh`，每帧只有 buffer 内容变、身份不变（AllocationGeneration 不动），proxy 零重建；`CopyToStaticMesh` 读的就是被模拟的流，冻结当前姿态成资产是零成本副产品。

## 与 Chaos 的关系

- 引擎事实（5.7.4 源码核实）：`Engine/Plugins/ChaosCloth` 下 `.usf`/`.ush` 数量为 0，求解实现是 3 个 `.ispc`（`ChaosClothingSimulationSolver.ispc` 等）——CPU + SIMD，跑在物理线程。「Chaos 布料上 GPU」没有可迁移的实现，只能自写。
- 结果互通留缝（**不在本计划范围**）：引擎 `SkeletalMeshDeformerHelpers.h:50` 的 `GetClothBuffersForReading` 暴露 Chaos 解算结果的 GPU SRV（sim 粒子位置法线 + 渲染顶点重心映射），DeformerGraph 的 `DataInterfaceCloth.ush` 里有现成插值数学。将来若要「Chaos 解算、GPUMesh 加工」，一个读 SRV 写常驻流的 pass 即可，与本计划互不阻塞。
- `GPUSkeletalTreeComponent`（`GPUSkeletalTreeComponent.h:36`）现在的 CPU 摆动是本功能的前身：布料落地后树叶/垂藤类摆动可换用真模拟。

## 与 Marvelous Designer 的对比

定位完全不同，不构成二选一；三者按场景分工。

| 维度 | Marvelous Designer / CLO | 本计划（GPUMesh XPBD） | Chaos Cloth（参照） |
| --- | --- | --- | --- |
| 定位 | 离线服装设计 DCC：2D 版片 → 缝合 → 成衣模拟 | 引擎内运行时形变器，`UCSMesh` 常驻流上解算 | 引擎内运行时角色服装（骨骼驱动） |
| 运行时机 | 制作期交互模拟；结果烘缓存（Alembic 等）导入引擎回放 | 游戏运行时每帧解算 | 游戏运行时（物理线程 CPU） |
| 求解预算 | 离线级：高密度网格、大迭代数、精确自碰 / 摩擦 / 多层衣料 | 帧预算内 substep × 迭代；自碰 P3 可选 | 帧预算内，粒子数千级 |
| rest 状态语义 | 2D 版片平面态（真衣料的自然静止态） | rest = setup 时的 3D 姿态 | ClothAsset sim mesh |
| 交互性 | 游戏内无 | 风 / 碰撞体 / 钉点运行时全可变 | 跟随动画与碰撞体 |
| 产物 | 高保真网格 / 动画缓存资产 | 每帧姿态；`CopyToStaticMesh` 随时冻结 | 蒙皮渲染结果 |

- **分工结论**：过场级高保真布料在 MD 制作、烘 Alembic 走 GeometryCache 回放；游戏内交互性布料（旗、披风、幕布、程序化网格摆动）走本计划；骨骼角色服装走 Chaos。运行时 XPBD 不追平离线质量，也不必。
- **可借鉴点——版片 rest 语义**：衣料的真实静止态是 2D 版片平面，不是 3D 摆放姿态。若网格来自 MD/CLO 导出（UV 即版片坐标），`StretchEdges` / `BendPairs` 的 RestLength 可改按 UV 空间计算（开关 `bRestLengthFromUV`），褶皱行为更接近真衣料；程序化网格保持 3D 边长。已列入 Open Questions。
- MD/CLO 自带 GPU 加速模拟选项（知识性说明，未逐版本核实），但那是 DCC 内的制作加速，与引擎运行时无关。

## 现状盘点：可复用设施

| 设施 | 位置 | 在布料里的角色 |
| --- | --- | --- |
| `FCSMeshRenderThreadEdit` | `CSMesh.h:313` | 每帧写回入口：借 caller 的图、不 flush、析构自动恢复流访问态 |
| `EditMeshSync` | `CSMesh.h:482` | setup 期一次性写入（rest 姿态上传、`SetKnownCounts`） |
| `AddVertexWeldPasses` 的 grid hash | `CSGpuTriangleUtilities.h:68` | setup 焊接语义的参照（P0 用 CPU 版）；P3 自碰撞 spatial hash 的同族前例 |
| `AddTriangleLBVHBuildPasses` / `AddFastWindingMultipolePasses` | `CSGpuTriangleUtilities.h:34` / `:49` | P2 场景三角碰撞：最近点查询 + 内外判定救回 |
| `FGDFSampleService::EnqueueGDFJob` | `GDFSampleService.h:106` | P2 全局距离场碰撞；job 在渲染器图内 base pass 后执行（GDF 参数此刻才就绪，`GDFSampleService.cpp:24`） |
| ShallowWater 求解 tick | `ComputeShaderShallowWater.h:332` | GT timer → render command → 单图多 substep + ping-pong `Swap` 的既有形态 |
| `VinePerlinNoise.ush` | `Shaders/Private/` | P1 风场噪声采样 |
| `CSGpuMemoryBudget` | `CSGpuMemoryBudget.h` | sim buffer 分配预检 |
| `CSMeshOps.usf` 的流写入约定 | `Shaders/Private/CSMeshOps.usf:27` | 写回 pass 的 RW 布局直接照抄（打包 8888 切线、BGRA 色） |

## 目标设计

### D1 数据：FCSClothState（组件自持，不进 UCSMesh）

不把 sim 状态做成 `AuxVertex` 流（`CSGpuMeshTypes.h:63`）挂进网格，理由三条：

- **粒子数 ≠ 渲染顶点数**：sim 粒子是焊接后的唯一位置，渲染顶点沿 UV 缝有重复；硬塞会污染 readback/save 语义（速度 buffer 不该被 `CopyToStaticMesh` 看见）。
- **算子契约保持干净**：`UCSMeshOps` 任意拓扑算子（布尔、焊接）跑过后布料约束即作废。分离后 `UCSMesh` 仍然「任何算子可跑」，sim 侧靠 Generation 检测失配停摆，而不是让算子迁就布料。
- 有先例：`FCSSurfaceVoxelGPUBuffers`（actor 持 retained pooled 集）就是这个形态。

```cpp
// 粒子域（N = 焊接后唯一位置数），全部 TRefCountPtr<FRDGPooledBuffer>，随组件生灭
Position          // float4 × N，当前位置（世界空间，w 备用）
PrevPosition      // float4 × N，上一 substep 位置；XPBD 速度 = (x - xPrev) / dt
InvMass           // float  × N，0 = 钉死（kinematic 锚点）
DeltaAccum        // int    × 3N，Jacobi 修正累加器（定点），每次 Apply 后清零
RestTangent       // uint   × 2N，rest 切线基（8888 打包），法线重算时的正交化参考

// 约束域（setup 时 CPU 一次性构建上传，运行期只读）
StretchEdges      // uint2  × E + float RestLength，唯一边拉伸约束
BendPairs         // uint2  × B + float RestLength，cross-edge 弯曲约束（P0 选型，见 Open Questions）
Tethers           // uint2  × T + float RestLength，钉点→粒子长程约束（抗下垂），可空

// 映射域
CornerToParticle  // uint × RenderVertexCount，渲染顶点 → 粒子，蒙皮 scatter 用

// 碰撞域（P0）
Colliders         // float4 × C，球/胶囊/半空间参数，GT 每帧上传的小数组
```

`DeltaAccum` 用定点 int + `InterlockedAdd`：float 原子加平台性不一，项目 shader 里 `InterlockedAdd` 前例遍地（`RoadBuilder.usf`、`MeshBoolean.usf` 等）。

### D2 组件与 tick：UCSClothSimComponent

- **P0 宿主形态照 ShallowWater**：GT tick 收集 dt / 风参 / 碰撞体 → 一条 `ENQUEUE_RENDER_COMMAND` → RT 上自建一张 `FRDGBuilder` 图跑完全部 substep → 图尾构造 `FCSMeshRenderThreadEdit(GraphBuilder, *Mesh->GetResidentPtr())`（`CSMesh.h:642`）注册网格流、跑蒙皮/法线 pass、析构恢复访问态 → 执行图。不用 `EditMeshSync`：它每次 flush（`CSMesh.h:479` 注释），每帧 flush 不可接受；`FCSMeshRenderThreadEdit` 正是为「caller 自己持图」的形态设计的。
- **GDF 碰撞模式（P2）改宿主**：整张 substep 图改塞进 `EnqueueGDFJob`（组件每帧 re-enqueue 一个 job），因为 GDF 参数只在渲染器图内可得。代价是解算发生在 base pass 之后——当帧绘制用的是上一帧结果，固有一帧延迟（Chaos 布料同样一帧延迟）。P0 的 analytic 碰撞不需要 view，走自建图：无延迟，且**可 headless 自动化测试**（不渲染 view 也能驱动）。
- **绑定协议**：`SetGpuMesh(Target)` + `SetupFromMeshSync()`（回读 → 焊接 → 建约束 → 上传 → 缓存 mesh 的 Generation / AllocationGeneration）。每帧解算前校验 AllocationGeneration 与缓存一致；不一致（别人 resize / 换布局了）→ 停摆 + log，等待重新 Setup。sim 的每帧写回不 bump Generation、不广播事件——BorrowedGraph 编辑本就不管这些（`CSMesh.h:308` 注释），渲染叶子每帧照常绘制同一批 buffer，正确。
- **Bounds**：`FCSMeshResident::WorldBounds` 是 GT 可读字段、渲染组件 `CalcBounds` 的来源。setup 时一次性放大为「rest bounds ∪ 钉点半径 ∪ 碰撞域 + 摆幅 padding」，运行期不逐帧更新，避免 GT/RT 同步。

### D3 求解循环：XPBD substep pass 序

整帧 pass 流与写回边界示意：[`GpuClothSim_PassFlow.svg`](GpuClothSim_PassFlow.svg)。

```text
for s in 0..Substeps:                # small-steps XPBD：substep 数替代迭代数，P0 取 4~8
  Predict          # 1 thread/粒子：v=(x-xPrev)/dtPrev; xPrev=x; x += (v*damp + (g+wind)*dt)*dt
  for i in 0..Iterations:            # P0 取 1~2
    SolveStretch   # 1 thread/边：XPBD 拉伸修正 → InterlockedAdd 进 DeltaAccum（定点）
    SolveBend      # 1 thread/弯曲对：同上
    SolveTether    # 1 thread/tether：同上（P3）
    ApplyDelta     # 1 thread/粒子：x += decode(DeltaAccum) * ω（SOR 松弛 ω≈1.5）；清零累加器
  Collide          # 1 thread/粒子：analytic 球/胶囊/半空间投影；P2 换/加 GDF 或 LBVH
SkinBack           # 1 thread/渲染顶点：RW_Positions[corner] = x[CornerToParticle[corner]]
RecomputeNormals   # 三步：清零 → 1 thread/三角 面法线 InterlockedAdd 到三粒子 →
                   #   normalize + 对 RestTangent 正交化 → pack 8888 写 RW_Tangents
```

- **Jacobi 累加而非 graph coloring**：P0 取简单正确；coloring（并行 Gauss-Seidel，收敛快、免 atomics）列 P4 优化项——拓扑固定，setup 期 CPU 上色可行。
- 所有 pass 用参数结构体声明资源走 `GraphBuilder.AddPass`，**不得**用会丢依赖边的手写 dispatch——vine SC 的既有教训（换 `FComputeShaderUtils::AddPass` 丢 RDG 依赖边直接挂 GPU）；每阶段验收都开 `r.RDG.ImmediateMode` 跑一遍。

### D4 碰撞分层

| 层 | 内容 | 阶段 |
| --- | --- | --- |
| analytic | 半空间 / 球 / 胶囊，GT 小数组每帧上传 | P0 |
| 场景三角 LBVH | setup 时对目标 box 提取场景三角 + `AddTriangleLBVHBuildPasses` 建一次并常驻化；粒子最近三角投影 + FastWinding 内外救回 | P2 候选 |
| GDF | `EnqueueGDFJob` 内采样全局距离场，距离 + 梯度推出穿透 | P2 候选 |
| 自碰撞 | 粒子 spatial hash（weld grid hash 同族），默认关 | P3 |

### D5 setup 管线（一次性，可阻塞）

1. Target 就绪（`CopyFromStaticMesh` 或任意算子链产物）→ `ReadbackMeshSync` 一次取快照。
2. CPU 焊接位置（简单 grid hash 即可）→ 唯一粒子表 + `CornerToParticle`。
3. 唯一边集 → `StretchEdges` + RestLength；共享边的对顶点对 → `BendPairs`；可选 tether。
4. `InvMass` 钉点来源：P0 = 世界空间 box 集合参数；P1 = 顶点色通道 mask（`SetVertexColors` 已有写入路径，Color 流已回读）。
5. 过 `CSGpuMemoryBudget` 预检后上传 sim buffer；`EditMeshSync` 写 rest 姿态 + `SetKnownCounts`。

## 阶段计划

| 阶段 | 内容 | 验收门 |
| --- | --- | --- |
| P0 | 组件 + `CSClothSim.usf`（Predict/Stretch/Bend/Apply/analytic Collide/SkinBack/Normals）+ box 钉点 + 自动化测试 | headless 测试（`Private/Tests/`，仿 `CSGpuMeshObjectTests` 模式）：悬挂布 N 帧后钉点不动、自由端 Z 下降、边长误差 < 5%、无 NaN；编辑器目视 30k 粒子稳定；`r.RDG.ImmediateMode` 全绿 |
| P1 | 风场（Perlin）+ 顶点色 mask 钉点 + BP 参数面板（stiffness/damping/substeps） | 目视摆旗；测试补风力方向断言 |
| P2 | 碰撞升级：LBVH 或 GDF（先行者见 Open Questions） | 布落到场景网格不穿插（目视 + 粒子-表面距离采样断言） |
| P3 | tether + 自碰撞开关 | 布叠布不互穿目视 |
| P4 | graph coloring、substep 自适应 / 距离 LOD、多实例池化 | profile 对比数据 |

每阶段独立可回退；P0 的测试同时是后续所有阶段的回归基线。

## 风险

- **sim 自有 buffer 的访问态**：`FCSMeshRenderThreadEdit` 只恢复网格流，sim 自持的 pooled buffer 每帧 `RegisterExternalBuffer` 后由组件自己负责末态（跨帧 UAV 复用为主，图尾统一处理）。漏掉的症状与 `FinalAccessForRole`（`CSGpuMeshTypes.h:246`）注释描述的一样：某帧之后悄然失效，极难回溯。
- **定点累加器溢出**：修正量 × 2^scale 超 int32 要 clamp；scale 的选取和有效范围必须写进 shader 注释。
- **爆炸 / NaN**：dt clamp + 单次修正上限 + debug 模式 NaN 检测 pass。NaN 会经 SkinBack 污染渲染流，且 `ReadbackMeshSync` 落盘也带毒——检测要在写回之前。
- **TDR**：P2 LBVH 每粒子查询在大场景要设深度上限（本机 `MeshBoolean` 有百万级三角 TDR 前科）；粒子数 × 查询代价预算化。
- **一帧延迟只在 GDF 模式**：P0 自建图当帧生效；切 GDF 宿主后延迟一帧，文档与参数面板都要明示。
- **unity/jumbo 同名符号**：新 TU 的 file-local helper 一律 `CSCloth_` 前缀（`CSMB_` 教训）。
- **headless 测试判定看日志不看退出码**（本机无头编辑器退出约 20% 概率异常的既有结论）。

## Open Questions

- P2 先 LBVH 还是先 GDF？GDF 免建免维护、但精度粗、拖一帧延迟、依赖 DF 开启；LBVH 精确自管、但要处理场景变化后的失效重建。
- 弯曲约束选型：cross-edge distance（快、P0 倾向）还是 dihedral angle（准、参数直观）？
- 多实例（N 面旗）：N 组件 N 张图，还是单图批处理（buffer 拼接 + 每实例偏移）？P4 池化时一并定；批处理形态在沉降姊妹篇先落地，可直接搬。
- UV 缝处法线：粒子域法线天然全平滑，缝两侧渲染顶点共享粒子法线；需要保硬边时的角度阈值方案延后。
- 解算结果（位置/速度）是否暴露给 Niagara 做飘落粒子 / 交互反馈？
- `bRestLengthFromUV`：来自 MD 版片语义的 rest 长度开关（见「与 Marvelous Designer 的对比」）——P1 顺路捎带还是延后？
