# GPU 批量刚体沉降计划：粒子簇形状匹配求稳定终态

[`GpuClothSim_Plan.md`](GpuClothSim_Plan.md) 的姊妹篇，共享同一套粒子域 GPU 基建。解决的问题：**一堆物体（撒布的岩石、瓦砾、堆叠结构）的稳定终态**——生成期把它们沉降到物理上站得住的姿态，并给出「本来就稳 / 沉降后可用 / 不可救」的裁决。这是 tool-time / 生成期批处理，不是运行时物理引擎，结果不进 gameplay。所有行号为 2026-08-25 现状。

## 结论

- **可行，且正好落在 GPU 刚体成立的生态位**：批量（PCG 撒布动辄数千实例）、无每步 CPU 反馈（跑完一次回读）、精度要求宽松（要的是「合理」不是「精确」）、无帧预算（tool-time 可以把 substep 堆到收敛）。运行时 GPU 刚体的全部经典反对——双向耦合、确定性、帧内回读——在这里都不成立。
- **求解路线选粒子簇形状匹配（FleX 一路），不做经典凸体接触求解**。每个实例 = 一簇粒子，碰撞全部退化为粒子级（负载均匀、无窄相 divergence、天然支持凹形），刚性由每实例一个 ShapeMatch pass 恢复（协方差 + 极分解）。代价是接触精度 ≈ 粒子半径、摩擦近似——对「稳定性裁决」够用，对「精确认证」不够，阈值必须保守。
- **与布料计划共享基建，互为反哺**：粒子域 buffer 形态、substep 宿主形态、世界碰撞层（LBVH / GDF / analytic）、spatial hash 全部共用；布料 Open Questions 里的「多实例批处理」在本计划先落地，粒子 spatial hash（布料 P3 自碰撞）在本计划先建成。
- **本项目特有的正向理由**：要碰撞的世界（三角缓存、LBVH、表面体素、GDF）本来就常驻 GPU，有些路径计数只在 GPU 上；走 CPU 物理（Chaos / 编辑器 simulate）先要付一笔「回读场景建 CPU 碰撞体」的税，GPU 侧校验是顺流。
- **产出是三分裁决 + 可选烘焙**：Stable（原摆放保留）/ Settled（初始不合理，但烘沉降终态即可用）/ Unstable（永不入睡或滚出域，重摆）。工具既判也修。
- **四项拍板（2026-08-25）**：入口 = `ACSRigidSettle` 上的 BlueprintCallable `StartSettle(模拟次数)`，由用户蓝图触发，宿主整体照 `ACSShallowWaterCapture`；场景三角只在启动时采集一次，之后的场景变化一律不管；恢复系数（弹跳）是需求，不做全非弹性简化；一切包围盒取现成 bound，不自算（模板尺度/质量 = mesh bounds，沉降域 = 沉降 actor 自身 bound，阈值归一 = 实例自身 bound 尺度）。
- **主约束是 TDR**：沉降要几百上千 substep，必须分片提交（每张图跑一段），本机 `MeshBoolean` 有百万级作业 TDR 前科。

## 现状盘点：可复用设施

| 设施 | 位置 | 在沉降里的角色 |
| --- | --- | --- |
| `FStaticMeshRenderDataPointSampler::SamplePointsSync` | `StaticMeshRenderDataPointSampler.h:20` | setup 期按唯一网格采粒子模板（表面点） |
| `FCSSurfaceVoxelGPUBuffers` / 表面体素管线 | `ComputeShaderMeshGenerator.h:215` | 凹形/体内粒子采样的备选来源（延后） |
| `AddTriangleLBVHBuildPasses` / `AddFastWindingMultipolePasses` | `CSGpuTriangleUtilities.h:34` / `:49` | 世界碰撞主力：最近三角推出 + 内外救回；快筛层的穿插/悬空查询 |
| `FGDFSampleService::EnqueueGDFJob` | `GDFSampleService.h:106` | 世界碰撞备选（需 view，headless 不可用） |
| `AddVertexWeldPasses` 的 grid hash | `CSGpuTriangleUtilities.h:68` | 实例间粒子碰撞的 spatial hash 同族前例 |
| `SparseTileDispatchHelper` compact + indirect | `SparseTileDispatchHelper.h:30` | 睡眠实例跳过：活跃粒子压实 + indirect dispatch |
| ShallowWater 求解 tick + `StartSolver` 入口 | `ComputeShaderShallowWater.h:332` / `:261` | GT timer → render command → 分片 substep 的宿主形态；BlueprintCallable+CallInEditor 入口同形照搬 |
| `UCSGpuInstancedMeshComponent::SetInstances` / `UpdateInstanceTransform` | `CSGpuInstancedMeshComponent.h:239` / `:226` | P0 烘焙出口：沉降终态写回 CPU 实例表 |
| `SetInstanceSourceGPU`（`FCSGpuInstanceSourceGPU`） | `CSGpuInstancedMeshComponent.h:248` / `:72` | P3 就地沉降：GPU 实例 buffer 原位读写，零回读 |
| `CSGpuMemoryBudget` | `CSGpuMemoryBudget.h` | 粒子/模板 buffer 分配预检 |
| 布料计划 D1/D3 | `GpuClothSim_Plan.md` | 粒子域 buffer 纪律、substep 图形态、定点累加经验直接搬 |

## 目标设计

### D1 数据：FCSSettleState

组件/actor 自持 pooled buffer（同布料 `FCSClothState` 的边界裁决：不进任何渲染对象的流集合）。

```cpp
// 模板域（每唯一网格一份，实例共享；setup 期 CPU 构建上传）
TemplateRestPos   // float4 × ΣK，相对质心的 rest 粒子位置，w = 粒子半径
TemplateRange     // uint2  × T，模板在 TemplateRestPos 里的 offset/count（K 每形状 8~64）
TemplateMass      // float2 × T，总质量 + 1/K 均分的粒子 InvMass

// 粒子域（P = Σ 实例粒子数）
Position          // float4 × P，当前位置（世界空间）
PrevPosition      // float4 × P，上一 substep 位置（XPBD 速度来源）

// 实例域（I = 实例数）
InstanceState     // float4×3 × I，COM + 旋转四元数 + 初始 transform 引用索引
InstanceMetrics   // float4 × I，累计位移 / 累计转角 / 动能滑动均值 / 入睡 substep 序号
InstanceFlags     // uint   × I，bit：Asleep / Unstable / OutOfDomain / NaN
InstanceTemplate  // uint   × I，模板索引

// 碰撞域
HashGrid          // 计数排序 cell start/end，粒子广相（weld grid hash 同族）
Colliders         // float4 × C，analytic 半空间/盒（P0 地面）
WorldLBVH         // setup 建一次并常驻化（ConvertToExternalBuffer），场景三角
```

### D2 代理生成（setup，一次性可阻塞）

1. 收集唯一网格集合 → `SamplePointsSync` 表面采样 K 点（K 按包围盒尺度取 8~64；包围盒一律取现成 bound，模板/质量用 `UStaticMesh` bounds，不自算），减质心、附半径（相邻点距的一半），成模板。
2. 质量按包围盒体积近似；粒子 InvMass = K/m。凹形（拱、槽）表面粒子天然贴形，比单凸包代理准——这是选粒子簇而不是凸体的直接理由之一。
3. 实例展开：每实例记模板索引 + 初始 transform；粒子域按模板 rest 位置乘初始 transform 铺开。
4. 世界碰撞按沉降 actor 自身 bound 提取场景三角 → LBVH 建一次常驻（沉降对象自身不进世界 LBVH，避免自碰自）。采集只发生在 `StartSettle` 启动时这一次（已拍板）：之后的场景变化一律不管，不做脏标记/重建策略；要新场景就重新触发。
5. 全部分配过 `CSGpuMemoryBudget` 预检。10k 实例 × 32 粒子 = 32 万粒子，粒子域 ~10MB 量级，不构成压力。

体内粒子（P4，表面体素路）的定位——为什么镂空/细杆网格补体内粒子，而不是给实例上三角碰撞：实例侧的碰撞物质就是粒子本身，实例间没有对世界那样的 FastWinding 内外救回，从表面粒子缝隙钻进去的粒子面对的是真空，没有任何响应推它出来；镂空网格在 K≤64 的表面采样下要么漏杆（有材质处无碰撞，直接穿杆），要么因采样间距大把粒子半径撑粗（把洞糊死，该穿过的穿不过）。体素路沿材质芯布点、半径≈杆件半厚，杆是实心杆、洞还是洞。给动着的实例挂真三角 = 每 substep 逐实例 refit BVH + 粒子×三角双向窄相，把「经典网格接触求解」搬回来，正是本路线特意扔掉的——三角只属于建一次不动的静态世界。

### D3 求解循环：substep pass 序与收敛

```text
每次提交（一张 FRDGBuilder 图）跑 S 个 substep（S 按 TDR 预算取，如 32）：
for s in 0..S:
  Predict          # 1 thread/粒子：重力积分 + 阻尼；Asleep 实例经 compact+indirect 跳过
  BuildHash        # 计数排序重建粒子 hash（每 substep 或每 4 substep 一次）
  CollideWorld     # 1 thread/粒子：LBVH 最近三角推出 + FastWinding 内外救回；analytic 地面；
                   #   接触法向入射速度按 -e 反射（改写 PrevPosition 编码出射速度，低速截断防抖）
  CollideParticles # 1 thread/粒子：hash 邻域，异实例粒子对 push-out + 切向阻尼（摩擦近似）；
                   #   恢复系数处理同 CollideWorld
  ShapeMatch       # 1 thread/实例：协方差 Σ p·qᵀ → 极分解取 R（3×3 迭代数次）→
                   #   粒子吸回 R·rest + COM；刚性在此恢复，无需任何铰接/接触求解器
  SleepCheck       # 1 thread/实例：动能滑动均值 < ε 连续 M substep → Asleep；
                   #   受邻域冲量超阈值则唤醒；全批 Asleep 计数写 counter
提交间 GT 读 counter：全睡 或 substep 总数到顶（上限 = StartSettle 入参「模拟次数」）→ 收敛，进入裁决回读
```

- **底层冻结级联是堆的收敛关键**：入睡实例即成 kinematic 支撑（InvMass 视 0），堆从底往上逐层入睡——这等效 shock propagation，是 Jacobi 类并行解算在堆叠场景收敛差的主要解药。唤醒规则必须在（防止半空拱桥被过早锁死）。
- **恢复系数（弹跳）是需求（已拍板）**：e 挂 actor UPROPERTY（默认偏低）。位置式求解里弹跳走速度层：碰撞 pass 推出的同时按 -e·v_n 改写 PrevPosition；法向速度低于截断阈值按全非弹性，防无限微弹跳不入睡。
- **宿主与入口整体照 CSSW（已拍板）**：`ACSRigidSettle` 照 `ACSShallowWaterCapture` 形态——入口 = BlueprintCallable+CallInEditor 的 `StartSettle(模拟次数)` / `StopSettle()`（`StartSolver` 同位），由用户蓝图触发；GT timer 驱动分片提交，编辑器保持响应；参数挂 actor UPROPERTY，`Clean` / 世代护栏（`SolverGeneration` 同款）照搬。analytic + LBVH 碰撞不需要 view，**全程可 headless 自动化测试**。
- 全部 pass 走参数结构体声明依赖（vine SC 丢依赖边教训）；每阶段 `r.RDG.ImmediateMode` 验收。
- 不承诺 run-to-run 确定性（原子序、并行归约序）；语义是「烘一次即事实」。

### D4 快筛层（不需要动力学的三档）

沉降前先跑一遍，便宜且能直接出裁决、并从动力学批次里剔除已合格实例（堆叠场景剔除率有限，诚实预期）：

| 查询 | 实现 | 判定 |
| --- | --- | --- |
| 穿插 | 粒子对世界 LBVH 最近距离 + FastWinding 内外 | 嵌入深度 > 阈值 → 直接 Unstable 或送沉降修复 |
| 悬空 | 底部粒子向下 LBVH 射线 / GDF 采样 | 支撑距离 > 阈值 → 送沉降 |
| 静稳定近似 | 质心投影 vs 支撑接触点凸包（每实例小 kernel） | 出界 → 送沉降 |

### D5 裁决与烘焙

| 裁决 | 条件 | 动作 |
| --- | --- | --- |
| Stable | 快入睡且位移/转角 < 阈值 | 保留原摆放 |
| Settled | 入睡但位移/转角超阈值 | 烘沉降终态：`SetInstances` / `UpdateInstanceTransform` 写回（P3 起 GPU 就地） |
| Unstable | 到顶不睡 / 滚出域 / NaN | 标记重摆；报告里带 metrics 供排查 |

回读一次 `InstanceState` + `InstanceMetrics` + `InstanceFlags`（几十 KB 量级）；阈值全部集中在一个 options struct，位移/转角按实例自身 bound 尺度比例归一（单位化已拍板），默认值标定列 Open Questions。

## 阶段计划

| 阶段 | 内容 | 验收门 |
| --- | --- | --- |
| P0 | `ACSRigidSettle`（照 CSSW 形态，BlueprintCallable `StartSettle(模拟次数)` 入口）+ `CSRigidSettle.usf`（Predict/CollideWorld/ShapeMatch/SleepCheck）+ 恢复系数 e + analytic 地面 + LBVH 世界（启动时采集一次）+ 分片提交 + 一次回读；无实例间碰撞 | headless 测试（`Private/Tests/`）：斜面上单箱滑落入睡、平面上单箱原地 Stable、悬空箱 Settled 且终态贴地（嵌入 < 粒子半径）、e>0 落箱弹跳后仍入睡、无 NaN；`r.RDG.ImmediateMode` 全绿 |
| P1 | 实例间 spatial hash 碰撞 + 底层冻结级联 + 唤醒规则——**用户主用例（堆）在此达成** | 100~1000 实例落箱成堆：全批入睡、终态互穿 < 粒子半径、堆不爆不飞；分片提交下编辑器不卡死 |
| P2 | 快筛层三查询 + 三分裁决报告 + `SetInstances` 烘焙出口（蓝图入口已在 P0） | 构造混合场景：悬空/穿插/合格实例各若干，裁决分类全对；烘焙后重跑快筛全绿 |
| P3 | `SetInstanceSourceGPU` 就地沉降（零回读直到存盘）+ Asleep 实例 sparse 跳过（compact + indirect） | GPU 源实例沉降结果与 P2 CPU 路 parity；大批量下活跃粒子数随入睡递减（profile 佐证） |
| P4 | 摩擦/恢复系数分档（基础 e 已在 P0）、阈值标定工具、体内粒子（表面体素路，镂空/细杆网格，理由见 D2）、规模 profile | 1 万实例级 profile 数据；标定文档 |

每阶段独立可回退；P0 的测试是全线回归基线。

## 风险

- **TDR 分片纪律**：单次提交的 substep 数 × 粒子数 × LBVH 查询深度要预算化；宁可多分片。本机前科见 `MeshBoolean` 百万级 TDR。
- **过早入睡锁死伪稳态**：半空拱、互相卡住的斜靠会以非物理姿态入睡。唤醒阈值 + 「入睡前 N substep 位移趋势」双重判据；裁决报告里带 time-to-sleep 供人工抽查。
- **摩擦/恢复失真**：切向阻尼不是摩擦锥，恢复系数是速度层反射近似，粒子半径决定接触精度。裁决阈值保守；本工具是「明显不合理过滤器」，不是「精确认证器」——这句话写进类注释。
- **弹跳拉长收敛**：e>0 把 time-to-sleep 拉长，「模拟次数」给小了会把仍在弹的实例误判 Unstable；低速截断阈值要随 e 一起标定，报告里的 time-to-sleep / 末速用于区分「还在弹」和「真不稳」。
- **NaN 扩散**：ShapeMatch 的极分解在退化协方差（共线粒子簇）上会出 NaN；模板生成时检测共线并加扰动，运行时 NaN 置 Unstable 并冻结该实例，不许污染 hash 邻域。
- **常驻 buffer 访问态**：LBVH 常驻集与粒子域跨提交复用，图尾统一恢复末态（`FinalAccessForRole` 同款纪律，症状是某次提交后悄然失效）。
- **unity/jumbo 同名符号**：file-local helper 一律 `CSRS_` 前缀。
- **headless 判定看日志不看退出码**（既有结论）；GDF 碰撞层需 view，headless 测试只用 analytic + LBVH。

## Open Questions

- 裁决阈值默认值：位移/转角/入睡时限的具体默认值（单位化已拍板：自身 bound 尺度比例）；是否出一个标定小工具。
- 与布料共批：同一张图里布料 + 沉降共享 hash 与碰撞层是否值得（P4 后再议）。
