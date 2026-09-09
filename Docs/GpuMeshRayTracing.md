# GPU mesh 的光追 BLAS 接入

`FCSGpuMeshSceneProxy` 为 GPU 常驻网格建底层加速结构（BLAS），让硬件 Lumen、光追阴影与光追反射能看见它们。2026-09-09 落地。所有行号为当日现状。

## 结论

- **本工程的 Lumen 走硬件光追，给 GPU mesh 写距离场对它没用**。`Config/DefaultEngine.ini` 开着 `r.RayTracing=True`，5.7.4 里 `r.Lumen.HardwareRayTracing` 默认 1，运行时实测三项全为 1。Lumen 追的是 TLAS，不读 mesh SDF，也不读 Global Distance Field。距离场只对距离场阴影与材质里的距离场节点有用。
- 接入前 GPU mesh 对 Lumen **完全不存在**：代理既没有光追表示，也在 `CSGpuMeshSceneProxy.cpp:28` 显式关掉了距离场支持。表现是屏幕外与被遮挡部分既不遮挡也不反弹，典型症状是房体内部漏天光。
- BLAS 的三角数**只能来自 CPU**：RHI 没有 indirect BLAS build，`bSupportsRayTracingIndirectInstanceData` 只管 TLAS 的实例变换。所以 BLAS 跟着已有的 draw-args 回读镜像走，不新增任何回读。
- **没做 surface cache**。GPU mesh 在 Lumen 里是"遮挡但命中点发黑"，反射侧可开 `r.Lumen.Reflections.HardwareRayTracing.HitLighting=1` 补色。

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

## 控制

| 名字 | 默认 | 作用 |
| --- | --- | --- |
| `r.CSGpuMesh.RayTracing` | 1 | 0 = 下一帧末丢弃全部 BLAS 且不再新建 |
| `CSGpuMesh.DumpRayTracing` | — | 从渲染线程打印每个受跟踪代理的 BLAS 状态 |

## 验证

### 自动化测试

`PCGPlugins.ComputeShaderGenerator.GpuMeshObject.RayTracingGeometry`：回读落地前无 BLAS，落地后段数与三角数精确，编辑在途时保留，再落地后重建且 serial 加一。整组 19 条全过。

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

### 复跑 A/B 的两个坑

- **调试视图是全局 CVar `r.RayTracing.Visualize` armed 的，不是逐 SceneCapture 的 show flag。** 给两个捕获组件分别设 `show_flag_settings` **隔离不了**，第二组仍会抓回重心坐标图。debug 相与 lumen 相必须**分成两次独立运行**，lumen 那次开头显式 `r.RayTracing.Visualize Off` 且全程不设 `RayTracingDebug`。
- `CSMeshRenderComponent` 的 `bounds` 属性与 `get_local_bounds()` **都没暴露给 Python**，取包围盒只能用 `actor.get_actor_bounds(False)`。

## Open Questions

- Surface cache 未做。要让命中点有颜色需要三件事：`GetMeshCardRepresentation()` 返回 cards（`MeshCardRepresentation::SetCardsFromBounds` 可从包围盒直接生成六面）、实现 `DrawStaticElements` 并打开 static relevance（`LumenCardCapture` 只消费缓存的静态 draw command，`LumenSceneCardCapture.cpp:629`）、几何变化后调 `InvalidateLumenSurfaceCache_GameThread`。
- BLAS 构建目前不计入任何帧预算。接入分帧调度见 [`FrameQuotaScheduler_Plan.md`](FrameQuotaScheduler_Plan.md)，refit 也留在那一轮。
