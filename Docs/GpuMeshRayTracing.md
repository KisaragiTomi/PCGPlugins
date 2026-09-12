# GPU mesh 的光追 BLAS 与 Lumen surface cache 接入

`FCSGpuMeshSceneProxy` 为 GPU 常驻网格建底层加速结构（BLAS），让硬件 Lumen、光追阴影与光追反射能看见它们（2026-09-09）；再给它们一份 Lumen surface cache，让光线命中点有颜色、能把光反弹出去（2026-09-11）。行号为各自落地当日的现状。

## 结论

- **本工程的 Lumen 走硬件光追，给 GPU mesh 写距离场对它没用**。`Config/DefaultEngine.ini` 开着 `r.RayTracing=True`，5.7.4 里 `r.Lumen.HardwareRayTracing` 默认 1，运行时实测三项全为 1。Lumen 追的是 TLAS，不读 mesh SDF，也不读 Global Distance Field。距离场只对距离场阴影与材质里的距离场节点有用。
- 接入前 GPU mesh 对 Lumen **完全不存在**：代理既没有光追表示，也显式关掉了距离场支持。表现是屏幕外与被遮挡部分既不遮挡也不反弹，典型症状是房体内部漏天光。
- BLAS 的三角数**只能来自 CPU**：RHI 没有 indirect BLAS build，`bSupportsRayTracingIndirectInstanceData` 只管 TLAS 的实例变换。所以 BLAS 跟着已有的 draw-args 回读镜像走，不新增任何回读。
- **只有 BLAS 时只遮挡、不反弹**（2026-09-11 实测）。工程默认的 surface cache 光照模式下，命中没有 mesh card 的图元得到 `bValid=false`、radiance 0，GI 直接把 0 写出去，没有重追（只有反射有 `r.Lumen.Reflections.HardwareRayTracing.HitLighting` 这条补救）。
- **surface cache 现在由代理提供**：包围盒六面 card，加上只给 card 采集用的静态 batch，计数来自同一份回读镜像。凹进去的面（窗洞内侧、架空房底）六个方向都看不到，仍然发黑。

## 设计

### 三角数从哪来

BLAS 复用 `FCSMeshResident` 已有的 DrawIndexedIndirect 回读镜像（那份镜像本来是给虚拟阴影图用的，见 `CSMesh.h:124` 起的长注释）。新增 `GetDrawArgsPublishSerial()`，每次回读落地加一；代理在帧末泵里比对 serial 决定要不要重建。

| 状态 | BLAS 行为 |
| --- | --- |
| 计数未知（回读从未落地） | 不建，`GetDynamicRayTracingInstances` 直接返回 |
| serial 未变 | 保留现有 BLAS，零成本 |
| serial 前进 | 全量重建 |
| 回读在途（编辑刚发生） | **保留旧 BLAS**，它是烘好的结构，不再读 buffer |
| 三角数为 0 | 释放 BLAS，并记下 serial 以免每帧重试 |

镜像的"在途即覆盖"策略让逐帧连续编辑时 publish 被饿死，于是**天然限流**：编辑停下来才重建。这也是为什么这条路不需要分帧调度。

### 重建方式

`ReleaseResource` + `SetInitializer` + `InitResource`，`bFastBuild=true`，全量。与引擎的 ProceduralMeshComponent 同款。

**不能走 refit**：5.7.4 的 `FRayTracingGeometryManager::RequestBuildAccelerationStructure` 把 `InBuildMode` 硬写成 `Build`（`RayTracingGeometryManager.cpp:216`），要 refit 得自己管 scratch 并直接调 `BuildAccelerationStructures`。

### 线程与生命周期

建 / 释 BLAS **不在** `GetDynamicRayTracingInstances` 里，也不在 `CreateRenderThreadResources` 里——两者都可能跑在 ParallelFor 任务线程（`RayTracing.cpp:1185`）。改用带锁的代理注册表加一个 `OnEndFrameRT` 泵：

```cpp
// ComputeShaderGenerator.cpp:25 / :34 —— 模块 Startup/Shutdown 经 ENQUEUE_RENDER_COMMAND 注册与注销。
FCSGpuMeshSceneProxy::RegisterRayTracingPump();
FCSGpuMeshSceneProxy::UnregisterRayTracingPump();   // 注销后 FlushRenderingCommands
```

注销后必须 `FlushRenderingCommands`：热重载前要把指向本 DLL 的委托摘干净。

### 每个流的最终 access

**`Index` 流的最终 access 必须含 `SRVMask`**。BLAS 构建按 SRV 读输入，RHI 不替你转换（引擎自己在 `RayTracingDynamicGeometryUpdateManager.cpp:938` 做 UAV→SRVMask）。`FinalAccessForRole(Index)` 已从 `VertexOrIndexBuffer` 改成 `VertexOrIndexBuffer | SRVMask`，`StreamContract` 测试同步改。

### 哪些叶子参与

`WantsRayTracingGeometry()` 默认 true。`FCSGpuInstancedMeshSceneProxy` 返回 false：它的流只放一份源网格，实例变换在 GPU buffer 里，建单实例 BLAS 会在原点冒出一个幻影。

**`UpdateVisibleInLumenScene()` 要在最派生的构造里再调一次**——基类构造里看不到子类的 override。

## Surface cache

硬件 Lumen 在命中点查命中图元的 surface cache 取颜色。代理为此提供两样东西，缺一样都还是黑的。

### Mesh cards

- `GetMeshCardRepresentation()` 返回按本地包围盒生成的六面 card（`MeshCardRepresentation::SetCardsFromBounds`，引擎给骨骼网格体用的同一套，`SkeletalMeshSceneProxy.cpp:670`）。`OnTransformChanged` 里随包围盒重建。
- 引擎的 card 生成是编辑器里对静态网格三角做的离线构建，GPU 上的三角用不上；包围盒 card 的代价是覆盖率，见结论最后一条。
- **Lumen 给一个图元只建一次 card 集**，之后的 transform 更新只搬动它（`FLumenSceneData::UpdateMeshCards`）。几何长出 card 包围盒时只能换新代理。

### Card 采集 batch

- card 采集**没有动态路径**，只遍历图元缓存的静态 mesh draw command（`LumenSceneCardCapture.cpp:856`），所以 `DrawStaticElements` 为每个 draw batch 注册一个静态 batch。
- `GetViewRelevance` 始终不报 static relevance。其它 pass 都经这个标志才访问静态网格（`SceneVisibility.cpp:1512`），所以这些 batch 只有 card 采集会画。batch 上 `bUseForMaterial=true`（采集只收这类，`LumenSceneCardCapture.cpp:861`），深度、遮挡、阴影全关。
- 直接绘制，不走 indirect，理由与 VSM 阴影 batch 相同：缓存命令走 GPU-Scene 实例剔除，args 按 CPU 侧 `NumPrimitives` 重造。
- **计数未知时一个都不注册**。空采集下次刷新就补上；旧计数配新索引会把乱三角烘进 card。

### 回读落地后的刷新

引擎只在图元加入场景和 transform 更新时收集静态 batch（`RendererScene.cpp:5956-5960`），回读落地又没有游戏线程事件，所以由 `UCSMeshRenderComponent` 轮询 publish serial：

| 事件 | 组件动作 | 结果 |
| --- | --- | --- |
| 代理创建 | 记下 card 包围盒与 serial，挂轮询 | 计数已知则当场收集 batch |
| 内容编辑 | transform 更新，挂轮询 | 计数已退役，batch 撤掉；已采集的 card 保留旧内容 |
| 回读落地，几何仍在 card 包围盒内 | transform 更新 + `InvalidateLumenSurfaceCache_GameThread` | batch 按新计数重收，card 全部重采 |
| 回读落地，几何超出 card 包围盒 | `RecreateRenderState_Concurrent` | 新代理、新 card 集 |
| 回读 10 秒不落地 | 停止轮询 | 下次编辑重新挂上 |

**代理必须关掉冗余 transform 跳过**（`SetCanSkipRedundantTransformUpdates(false)`）。内容编辑和刷新都不改变换，包围盒通常也不变，正是引擎默认丢弃的那种更新（`r.SkipRedundantTransformUpdate`，`RendererScene.cpp:1600`）。实例化叶子不注册 batch，构造里把它还原成允许跳过。

## 控制

| 名字 | 默认 | 作用 |
| --- | --- | --- |
| `r.CSGpuMesh.RayTracing` | 1 | 0 = 下一帧末丢弃全部 BLAS 且不再新建 |
| `r.CSGpuMesh.SurfaceCache` | 1 | 0 = 不给 card 也不注册采集 batch（只遮挡）；改动会重建全部渲染状态 |
| `CSGpuMesh.DumpRayTracing` | — | 从渲染线程打印每个受跟踪代理的 BLAS 状态 |

## 验证

### 自动化测试

`PCGPlugins.ComputeShaderGenerator.GpuMeshObject.RayTracingGeometry`：回读落地前无 BLAS，落地后段数与三角数精确，编辑在途时保留，再落地后重建且 serial 加一。整组 19 条全过。

`PCGPlugins.ComputeShaderGenerator.GpuMeshObject.SurfaceCache`（待跑）：回读落地前有六面 card、无采集 batch；落地后同一个代理注册一个 batch，三角数精确、只用于材质；编辑在途时 batch 撤掉、card 跟随新包围盒；再落地且几何超出旧 card 时换新代理并重新注册。

```bash
UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests PCGPlugins.ComputeShaderGenerator.GpuMeshObject" -TestExit="Automation Test Queue Empty" -unattended -nopause -nosplash -stdout -AbsLog=<log>
```

**不能加 `-nullrhi`**：光追测试要真 RHI。

### TLAS 归属（决定性）

关卡 `/PCGPlugins/HouseTest/L_HouseGroundDemo`，`ShowFlag RayTracingDebug` + `r.RayTracing.Visualize Barycentrics`，切 `r.CSGpuMesh.RayTracing`：

| 状态 | 画面覆盖率 |
| --- | --- |
| 开 | 27.1% |
| 关 | 0.5% |
| 再开 | 27.1% |

关掉后地面、道路、墙体、柱子**整片从 TLAS 消失**，只剩两个普通静态网格（它们有自己的 BLAS）。

### Lumen 光照

同关卡，Lumen 最终色，`on / on2 / off / on3` 四拍，每拍 90 帧预热。

| 对比 | 全表面样本均值 |
| --- | --- |
| 噪声 on-vs-on2 | 1.14 |
| 效果 on-vs-off | 2.23 |
| 还原 on-vs-on3 | 1.30 |

**均值是错的统计量**：效果是稀疏局部的，被 97% 不受影响的区域稀释。按分布看才对——**2.6% 的表面像素超过噪声 p99（=8），这些像素上的平均差是 22.4**。噪声差异图只有细边缘走样，效果差异图是成片亮区，位置全在窗洞凹陷、门洞、屋顶面与岩壳轮廓，开阔地面完全不变。这正是"只遮挡、不反弹"（无 surface cache）应有的签名。

### 反弹基线（surface cache 之前）

2026-09-11，同关卡，关掉屏幕追踪只看世界空间路径，曝光锁在 `TG_PostProcess` 的 -2 EV100。草有风动，逐像素比较会被闪烁淹没，所以只统计重复帧稳定的像素，并用差中差扣掉两种模式共有的遮挡：`(HitLighting 开−关) − (SurfaceCache 开−关)`。

| 近景指标 | 结果 |
| --- | --- |
| Surface cache 模式下 BLAS 开−关 | 43.6% 像素变暗，0.55% 变亮（只遮挡） |
| 差中差（GPU mesh 命中点送回的光） | 32.3% 像素变亮 |
| 右房正面墙亮度（0–255） | surface cache 64，Hit Lighting 112 |
| 门洞亮度 | surface cache 4，Hit Lighting 53 |

surface cache 落地后的同口径复测待跑。

### 复跑 A/B 的两个坑

- **调试视图是全局 CVar `r.RayTracing.Visualize` armed 的，不是逐 SceneCapture 的 show flag。** 给两个捕获组件分别设 `show_flag_settings` **隔离不了**，第二组仍会抓回重心坐标图。debug 相与 lumen 相必须**分成两次独立运行**，lumen 那次开头显式 `r.RayTracing.Visualize Off` 且全程不设 `RayTracingDebug`。
- `CSMeshRenderComponent` 的 `bounds` 属性与 `get_local_bounds()` **都没暴露给 Python**，取包围盒只能用 `actor.get_actor_bounds(False)`。

## Open Questions

- BLAS 构建目前不计入任何帧预算。接入分帧调度见 [`FrameQuotaScheduler_Plan.md`](FrameQuotaScheduler_Plan.md)，refit 也留在那一轮。
- 每次回读落地都会让整份 card 重采，而采集按 card page 各画一遍整个网格。百万三角的地面单次重采的开销未测。
- 几何超出 card 包围盒时换新代理，新代理的 BLAS 要到那一帧帧末才建，所以这一帧 TLAS 里没有它。要消掉这一帧，得让 BLAS 挂在常驻集上跨代理交接。
- 包围盒 card 覆盖不到的凹面仍然发黑。要补得换真正的 card 生成（按面片聚类），或者只对这些命中开 Hit Lighting。
- 实现 surface cache 之前，Surface Cache 可视化（`r.Lumen.Visualize 5`）里近景部分 GPU mesh 显示品红（有 card 索引但无覆盖），而不是无 card 的黄色。已排除 RayTracingGroupId 合并，原因未查明；有了真 card 之后这个现象是否还在，待复测时一并看。
