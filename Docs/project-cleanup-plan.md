# UETest574_2 项目清理计划

[返回文档索引](index.md)

本计划基于对整个工程（`Source/` + 5 个插件 `Plugins/*`，约 6.9 万行 C++ 与约 9.5 千行 HLSL）的多代理审计结果，每条发现均经过独立的反驳式验证（全仓 grep 调用点 + 二进制 Content 引用扫描 + 直接读实现）。目标是清理**冗余内容**、**多余的兼容性内容**，并将**子类公用内容上提到父类**。

- 审计范围：`Source/UETest574_2`、`Plugins/{AITool, AssetHandlerPlugin, MCPUnreal, PCGPlugins}`、`Plugins/PCGPlugins/Shaders`。
- 引擎源码 `D:/UnrealEngine-5.7.4-release` 不在清理范围。
- 全部发现：约 205 条已验证；行号为审计时快照，`ComputeShaderMeshGenerator.cpp` 正被另一会话活跃编辑，落地前需按符号名重新定位。

## 摘要

| 维度 | 可清理量（去重后估算） | 说明 |
| --- | --- | --- |
| 冗余死代码（safe delete） | ~6,000–7,500 行 C++ | 无任何调用点/CVar/UI/内容引用 |
| 多余兼容性内容（需决策） | ~1,600 行 C++ | GPU 已默认，CPU 路径仍可经 CVar/UPROPERTY 选中 |
| Epic 模板变体（需决策） | ~3,500 行 C++ + 对应 Content | `Variant_Strategy` / `Variant_TwinStick` demo 包袱 |
| Shader 死 kernel/permutation | ~1,000 行 HLSL + 大量编译排列 | 删除后显著减少 shader 编译量 |
| 重复代码 → 提取共享（净减） | ~1,000–1,300 行 | 跨模块 + 模块内复制粘贴 |
| 上提到父类（lift-to-parent） | ~350–450 行 | 5 组兄弟类共用成员 |
| 仓库卫生（非代码） | ~4.3GB + 26MB + 9MB + 杂项 | 构建产物、漂移克隆、重复资产、配置 |

> 各审计维度之间存在大量重叠（同一处死代码常被 4 个维度分别命中），上表已按去重后的**唯一可删除量**估算，而非简单累加。

## 执行前置约束（P0 · 必读）

清理开始前，以下四点必须先处理或明确，否则会造成数据丢失或误删。

### 约束一：先提交/暂存未跟踪的 WIP 源码（数据丢失暴露）

以下今天修改的**活跃开发源码在父仓库和内层仓库中均未提交**（`git status` 显示为 `??`），一旦执行 `git clean -fdx`（清理构建产物的常用命令）即被删除：

```text
Plugins/PCGPlugins/Source/ComputeShaderGenerator/Private/CSGpuMeshComponent.cpp
Plugins/PCGPlugins/Source/ComputeShaderGenerator/Private/CSGpuMeshSceneProxy.cpp
Plugins/PCGPlugins/Source/ComputeShaderGenerator/Private/CSDirectTriangleMeshComponent.cpp
Plugins/PCGPlugins/Source/ComputeShaderGenerator/Private/CSDirectTriangleMeshSceneProxy.cpp/.h
Plugins/PCGPlugins/Source/ComputeShaderGenerator/Public/CSGpuMeshComponent.h / CSGpuMeshSceneProxy.h / CSDirectTriangleMeshComponent.h
Plugins/PCGPlugins/Source/ComputeShaderGenerator/Private/Tests/
Plugins/PCGPlugins/Source/PCGEditorProcess/Private/ComputeShaderLandscapeRoad.cpp
```

**动作**：先 `git add` + commit（或 stash）这些文件，再开始任何清理。

### 约束二：WIP 代码排除在清理范围外

`CSGpuMesh*` / `CSDirectTriangleMesh*` 组件与场景代理、`SaveDirectGPUMeshToStaticMesh`（`ComputeShaderMeshGenerator.cpp`，今日修改）是**进行中的新功能**，不是死代码。本计划所有"删除"动作均**不含**这些文件；`csg-19` 的 `LastSurfaceVoxelGPUBuffers` GPU 镜像目前"产出未消费"是因为其消费者正在开发中，属正常 WIP，不清理。

### 约束三：GPU 已默认，但 CPU 兼容路径仍是活跃 fallback

审计确认三条 CPU 路径**并非死代码**，仍可被 CVar / 序列化 UPROPERTY 选中（见"多余兼容性内容"章节）。这些正是用户所指的"多余的兼容性内容"，但删除等于**移除 A/B 对照能力并放弃 CPU 回退**——需产品决策，不能盲删。默认配置下它们不执行，但代码可达。

### 约束四：保留被 Content 引用的反射符号

以下 `UFUNCTION(BlueprintCallable)` 虽无 C++ 调用点，但被编辑器工具蓝图引用，**不可删除**：

```text
UGeometryGeneral::WeldDynamicMesh      # EUW_TreeWind.uasset
UGeometryGeneral::ColorAttribTransf    # EUW_AttribTransf.uasset
UGeometryGeneral::UVAttribTransf       # EUW_AttribTransf.uasset
UGeometryGeneral::WindDataForTree      # EUW_TreeWind.uasset（仅建议未来重构时保留入口）
AVineContainer::GenerateVines          # BP_VineSource / EUW_FoliageConverterCore（改签名需刷新 BP 节点）
```

删除任何 `UPROPERTY` / `UENUM(BlueprintType)` / `UFUNCTION(BlueprintCallable)` 前，先对 `Plugins/PCGPlugins/Content` 与项目 `Content` 做 `grep -l -a "符号名"` 二进制扫描确认零引用。

## 项目概况

| 模块 | 规模 | 健康度 | 主要清理面 |
| --- | --- | --- | --- |
| `ComputeShaderGenerator` | ~18k 行 | 核心健康，迁移沉积重 | 死 readback/batch 助手、两个"产出无消费"缓存子系统、死功能文件 |
| `GeometryScriptExtraEditor` | ~14.8k 行 | 活跃管线健康，CPU 迁移沉积重 | 死 CPU 线预处理、CPU tail A/B、编译期禁用的 step-log、死 BP API |
| `PCGEditorProcess` | ~7.5k 行 | 新路径干净，旧 BP 库沉积 | 整类 `ACSLandscapeLayer`、多个死 BP 库、死调试助手 |
| `MCPUnreal` | ~9.4k 行 | 功能健康 | 死网络追踪子系统、fake-success 桩、大量小助手复制粘贴、测试模块漂移 |
| `AITool` | ~7.1k 行 | 健康（近期编写） | NKSR 参考实现中未接入的 API、三入口 glue 三重复制 |
| `Source/UETest574_2` | ~3.9k 行 | 100% Epic 模板 | 死 TopDown 基类三件套、两个模板变体 demo |
| `Shaders` | ~9.5k 行 | 新 vine/SC 路径健康，旧派发器沉积 | 死 kernel、死 permutation、绕过 `PCGMathCommon.ush` 的复制助手 |

## 一、冗余内容清理（redundant）

### 1.1 CPU→GPU 迁移遗留的死代码（旗舰项，最大收益）

GPU 空间殖民与 fused GPU vine 成为默认后，以下 CPU 侧代码**无任何调用点、CVar、UI 或内容引用**，可直接删除。

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `gse-1` | `GeometryEditorActor.cpp:1478-1820` | `PrepareVVLinesProjected` + 其专属助手簇（7 个 static 函数） | ~470 | low |
| `gse-3` | `GeometryEditorActor` 头/实现 | 死的 per-point axis 管线（`PointAxes`/`TubeLinePointAxes`，端到端恒为空） | ~30 | medium |
| `gse-7` | `GeometryEditorActor.cpp:5760+` | `constexpr false` 的 SC step-log 基础设施（if-constexpr 岛屿散布在活函数中，需外科式移除） | ~450 | low |
| `gse-8` | `GeometryEditorActor.cpp:1039,5072` | `LogVineSCStageTargetTransformMatch` 无条件热路径调试扫描 O(points×targets) | ~30 | low |
| `gse-25` | `GeometryEditorActor.cpp:7987-8033` | SC prep parity 校验块（默认配置下不可达） | ~40 | low |
| `csg-1` | `ComputeShaderMeshGenerator.cpp` 匿名命名空间 | 死 readback/batch 助手（`AppendTriangleMeshData` 等，surface-voxel 内联后失效） | ~645 | low |
| `hierarchy-21` | `GeometryEditorActor.cpp` | SC step-log / parity-debug 助手套件（与 `gse-7` 重叠，合并处理） | (含于上) | low |

**"产出但无消费"的缓存子系统**（每次调用仍在跑，纯浪费）：

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `csg-3` | `ComputeShaderMeshGenerator` | 脏体素三角缓存子系统：`GenerateVines` 刷新它，vine 路径又显式丢弃其 handle | ~900 | medium |
| `csg-2` | `ComputeShaderMeshGenerator` | generated-data 纹理缓存：每次 readback 产出，无任何消费者 | ~400 | medium |
| `hierarchy-13` | `ACSGenerateCaptureScene` 两兄弟 `Generate()` | 计算后被丢弃的 GPU readback | ~20 | low |

### 1.2 整体死亡的功能 / 类

> ✅ 本节 6 项（`csg-8`/`csg-9`/`pcgep-1`/`pcgep-2`/`pcgep-3`/`csg-4`）已执行完毕：删除 12 个整文件（含 shader `DrawHLODTexture.usf`、`LandscapeLayer.usf`）+ 局部删除 `UComputeShaderMeshFillFunctions` 类，清理 `PCGEditorProcess.Build.cs` 闲置的 `DataLayerEditor` 依赖。`UETest574_2Editor` 增量编译通过（`Result: Succeeded`）。`csg-7`（ShallowWaterVoxel）应用户要求已回退保留。

| ID | 位置 | 内容 | 行数 | 风险 | 状态 |
| --- | --- | --- | --- | --- | --- |
| `csg-8` `hierarchy-10` `xmodule-7` | `DrawHOLDTexture.h/.cpp` + shader | 教程式 compute 派发功能，全仓零引用（含死 async BP 节点） | ~240 | low | ✅ 已删除 |
| `csg-9` | `ACSRuntimeLandscapeLayer` / `...Manager` | 运行时 landscape-layer 子系统，无用户 | ~773 | medium | ✅ 已删除 |
| `pcgep-1` | `ACSLandscapeLayer`（editor） | 整类无引用，`RenderLayer()` 是 `return false` 桩，与运行时孪生重复 | ~572 | medium | ✅ 已删除（连带删孤立的 `LandscapeLayer.usf`） |
| `csg-7` `xmodule-9` | `ACSShallowWaterVoxelCapture` | "Not yet implemented" 桩类，6 个序列化成员无引用 | ~99 | low | ↩️ 已回退保留（用户要求） |
| `pcgep-2` `xmodule-8` | `ComputeShaderBasicFunctionEditor.h/.cpp` | 整文件对，单函数零引用 | ~127 | low | ✅ 已删除 |
| `pcgep-3` | `General.h/.cpp`（`UGeneralBPLibrary`） | 整个 BP 库无引用 | ~107 | low | ✅ 已删除（含 Build.cs `DataLayerEditor`） |
| `csg-4` `hierarchy-9` | `UComputeShaderMeshFillFunctions` | 两个 UFUNCTION 函数体全被注释（no-op 库） | ~245 | medium | ✅ 已删除（局部，保留在用 MeshFill） |
| `mcp-1` | `NetworkDebugRoutes.cpp` | 网络调试追踪器：`RecordRequest`/`CompleteRequest` 零调用，端点恒返回空，`OnTick` 是 no-op | ~120 | low | 待处理 |
| `game-1` | `UETest574_2Character/GameMode/PlayerController` | TopDown 基类三件套，Content 与代码零引用（BP 直接挂引擎类） | ~366 | low |
| `aitool-1..6` | `NKSRGrid.h/.cpp`, `NKSRSvh` | NKSR 参考实现中从未接入推理路径的 API（`SampleBezier`/`SplatTrilinear`/`Coarsened`/`Dual`/`EvaluateVoxelStatus`/`FNKSRLayerField`） | ~230 | low |

### 1.3 死 BlueprintCallable API 与未用 UPROPERTY

> 删除前逐一做 Content 二进制引用扫描（见约束四）。已在 `gse-26` 中排除 3 个被引用的函数。

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `csg-11` | `AComputeShaderMeshGenerator` | 无引用的 landscape-triangle BP 包装 API | ~430 | medium |
| `csg-10` | `UComputeShaderBasicFunction` | 死成员与其孤立 shader 类 | ~480 | medium |
| `pcgep-4..9` | `CSAssetProcess` / `CSShallowWaterProcess` | 死 BP 包装、死 `MaterialTest`、死调试导出/dump | ~360 | low |
| `pcgep-10..12` | `ACSLandscapeRiver` / `ACSLandscape` | 中性化的 `SimRiver` no-op、只写成员、死 Debug UPROPERTY | ~50 | mixed（含 high） |
| `mcp-2,3,19` | `AnimBP` / `RealtimeMesh` / `Fab` 路由 | fake-success 桩（返回 success 却什么都不做）、`WITH_REALTIMEMESH` 从未定义、死 store | ~70 | mixed |
| `gse-10..20,26,27` | `GeometryEditorActor` / `GeometryMathUtils` / `VDBExtra` / `LandscapeExtra` | 未用 SC 属性字段、死 3-arg 重载、`UGeneralMath` 空桩库、死 VDB `VDBMeshFromActors` 家族、`CreateLandscapeMeshTextureData` 恒返回空、死 legacy 重载、6 个内容无引用的 BP 函数 | ~800 | mixed |
| `gse-9` | `FVV` 5 个未用序列化 UPROPERTY（`bUseGPUMode` 等） | 需先在 BP 编辑器确认无 Make/Break FVV 引脚并重存 `BP_VineSource` | ~10 | **high** |
| `aitool-9,10,11` | `NKSRKernelEvaluation` / `UNKSRReconstructLibrary` | 无消费者的 gradient 管线、无调用的 `GetAIToolPluginDir`、只写成员 | ~60 | mixed |
| `game-4..12` | `Source/UETest574_2` 各处 | 死配置节、未用 UPROPERTY、只写成员、不存在类的前向声明、自我覆盖的重复调用、7 个类无 Tick 却开着 Tick | ~50 | mixed |
| `hierarchy-11,12,15,19` | 跨 vine/geometry | `EnqueueGDFJob` 零生产者、`FVV::bUseGPUMode` 未读、死/桩调试 UFUNCTION、死顶点缓冲声明 | ~240 | mixed |

### 1.4 死 Shader kernel 与 permutation（减少编译量）

所有 16 个 `.usf` 均被 `ComputeShaderGenerator.cpp:19` 的 `AddShaderSourceDirectoryMapping` 引用，无整文件孤立；死代码集中在旧"派发器 + permutation 枚举" shader。

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `shaders-3` | `BasicFunction.usf` | `CalculateGradient` 入口编译但从不派发（拖带 `ComputeGradientT` 等） | ~100 | low |
| `shaders-7` | `General.usf` | 死助手簇（`FillSharedGroup`/`Random2D/3D/4D`/`SampleBilinear` 等） | ~190 | low |
| `shaders-15` | `MeshFill.usf` | 15 个 permutation 中 4 个死（3 个空 `#elif` 体） | ~110 | medium |
| `shaders-16` | `MeshFill.usf` | `FMESHFILL_TEST` 调试 kernel + BP `CurvaturePostTest` 调试残留 | ~85 | **high** |
| `shaders-13` | `ShallowWater.usf` | `SW_SmoothHeight` 全程派发却是空 kernel（"reserved" no-op，每帧水面 bake 都在跑） | ~80 | medium |
| `shaders-11` | `LandscapeLayer.usf` | `RL_GENERATENOISEHEIGHT` / `RL_THERMALEROSION` 分支从不启用 | ~50 | medium |
| `shaders-1,2,4,5,6,10,14,17,18,19,22` | 多个 `.usf` | 死 kernel（`CS_Promote`/`BlurPixelOrig`）、被 Fast 版取代的旧版、死 permutation、死打包助手、死邻域采样等 | ~350 | low |
| `csg-5,6` | `FMeshFillMult` / `FShallowWaterSim` | 死 permutation 模式（`FMeshFillMult` 16 中 5 个从不派发，但 128 个排列全在编译）、未接线的 shader 参数 | ~140 | medium |
| `shaders-12` | `LandscapeLayer.usf` | `USE_DEBUG_VIEW` 默认为 ON 而 C++ 从不设置 → 每次都写死调试 UAV | ~6 | medium |

### 1.5 注释掉的大段代码

| ID | 位置 | 行数 | 风险 |
| --- | --- | --- | --- |
| `gse-16` | `GeometryGeneral.cpp` 等（含 `return;` 后 141 行死块引用已不存在的 `SelectionMechanic`） | ~500 | low |
| `hierarchy-14` | `GeometryGeneral.cpp` 大段注释块（与 `gse-16` 部分重叠） | ~358 | low |
| `pcgep-13` `csg-18` `hierarchy-17` | `ComputeShaderLandscape.cpp` / 零字节 README / 恒假死分支 | ~90 | low |

## 二、多余的兼容性内容清理（compat）

### 2.1 需产品决策：GPU 已默认的 CPU 兼容 A/B 路径

以下三条是**仍可达的兼容 fallback**（用户所指"多余的兼容性内容"的核心）。默认配置走 GPU，但代码可经 CVar/UPROPERTY 选中。**这不是死代码**——删除即放弃 CPU 回退与 A/B 对照能力。

| ID | 位置 | 选择开关 | 行数 | 决策 |
| --- | --- | --- | --- | --- |
| `gse-4` `hierarchy-2` | `VineCPUTail::RunPostPasses` + `DispatchVVGPU_VoxelToFP` | `bVisVineGPUUseCPUForPostPasses`（UPROPERTY，editor 可选，默认 false） | ~840–900 | GPU 后处理 parity 签署后删除；删除是序列化布局变更 |
| `gse-5` `hierarchy-4` | 非 fused 的 CPU-upload vine 路径 + 常开 Stage B3 CPU 追踪 | `r.Vine.SC.FusedPath=0` 或空 `TubeLineGPUBuffers` | ~280 | 短期保留做 A/B，稳定后删除并移除 CVar |
| `gse-6` `hierarchy-3` | CPU 空间殖民参考解算器 | `SC.bUseComputeShader=false` / `r.Vine.SC.ForceMode` | ~460（其中 `bMultThread=false` 单线程 else 分支 ~75 已**可直接删**，恒传 `true`） | 求解器整体保留为回退；仅删死单线程分支 |

> 交叉验证 `xmodule-16`（负向发现）明确：上述 CPU 路径是活跃 CVar fallback，**禁止盲删**。建议做法：确认 GPU 路径在目标平台已稳定 → 一次性移除 CVar + 分支 + 相关 Transient actor 状态，作为独立提交。

### 2.2 可直接删除的过时兼容开关 / 分支

| ID | 位置 | 内容 | 风险 |
| --- | --- | --- | --- |
| `xmodule-10` `hierarchy-12` | `FVV::bUseGPUMode` | 死序列化开关，GPU/CPU 选择已迁到 CVar | high（序列化） |
| `mcp-16,17` | `AnimBP` 路由 | legacy 别名 op + UE 5.7 改名 workaround shim；硬编码 `WITH_PCG/WITH_GAMEPLAY_ABILITIES/WITH_NIAGARA=1` 使 501 fallback 分支不可达 | low |
| `mcp-3` | `RealtimeMesh` 端点 | `WITH_REALTIMEMESH` 从未定义 | low |
| `game-4,5` | `DefaultGame.ini` / `Build.cs` | 指向不存在属性的死配置节、仅为模板代码存在的依赖与 include 路径 | low |
| `pcgep-15` | `PCGPLUGINS_DEBUG` Build.cs 块 | 在 `PCGEditorProcess` 内已死（且跨模块复制粘贴） | low |
| `shaders-2` | `Connectivity.usf` | `CPF_Findislands` 被 `CPF_FindislandsFast` 取代 + 44 行注释实验 | low |
| `shaders-17` | `SampleSpline.usf` | `SS_RASTERIZESPLINE` permutation 从不派发且函数体 100% 注释 | low |

## 三、抽取子类公用内容到父类（lift-to-parent）

| ID | 兄弟类 | 共用内容 → 目标父类 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `csg-14` `hierarchy-5` | `FCSDirectTriangleMeshSceneProxy` / `FRoadMeshSceneProxy` | 完整的 15 成员 GPU-stream 标准场景代理样板 → 已有的 `FCSGpuMeshSceneProxy` 基类 | ~110 | medium |
| `hierarchy-6` | `ACSCliffGenerateCapture` / `ACSFillTarget` | 重复成员、`IsParameterValidMult` 与捕获逻辑 → 共同基类 | ~120 | medium |
| `pcgep-19` `hierarchy-7` | `ACSLandscape` / `ACSLandscapeLayer` | 组件别名、box 设置、landscape-region 数据簇与捕获参数块 → 共同基类 | ~70 | high（若 `pcgep-1` 直接删 `ACSLandscapeLayer` 则本项自然消解） |
| `game-13` | 4 个 `ACharacter` 变体子类 | 近乎相同的 `CharacterMovement` 平面约束设置 → 变体基类 | ~20 | low |
| `gse-28` | `FWindTreeReduceData` / `FWindTreeCombineLeafData` | 验证后实为**死成员清理**而非上提：删基类 `ClassNum` + `FDynamicMeshComponentData::IsValid` + 注释的 `FComponentMergeOptions`，保留 `ParentClass` | ~20 | low |

> `hierarchy-20` 提示 `ACSLandscape`/`ACSLandscapeLayer` 的捕获参数块（`CaptureSize/MaxHeight/Scale3DZ/TextureSize`）复制粘贴，属 `pcgep-19` 的证据之一。

## 四、跨模块 / 模块内重复 → 提取共享（duplicate）

| ID | 重复内容 | 副本位置 | 处理 | 净减行 |
| --- | --- | --- | --- | --- |
| `xmodule-1` `csg-12` `hierarchy-16` | `GeometryAsync.h` 逐字节重复 | `ComputeShaderGenerator`（死副本）+ `GeometryScriptExtraEditor` | 删死副本，单一来源 | ~71 |
| `xmodule-2` | `PCGPluginDebug.h` 逐字节重复 | 同上 | 删死副本 | ~46 |
| `xmodule-3` `gse-21` `gse-27` | OpenVDB 粒子→level-set→mesh 管线 | `ComputeShaderGenerator` ⟷ `GeometryScriptExtraEditor`（`VDBParticleList` vs `FCSGeneratorVDBParticleList`） | 上提到 `ComputeShaderGenerator`（已是 Public 依赖） | ~150–200 |
| `xmodule-4` | `DynamicMesh→UStaticMesh` 保存管线 | 三个 PCGPlugins 模块各一份 | 提取到共享助手 | ~110 |
| `xmodule-5` | 同步 GPU buffer-readback 样板 | `ComputeShaderGenerator` 内复制 ~9 次 | 提取 `CSHepler` 风格模板 | ~250 |
| `xmodule-6` `hierarchy-8` `pcgep-17` | landscape-layer generate/blend 管线 editor/runtime 孪生 | `FCSLandscapeLayer` vs `FCSRuntimeLandscapeLayer` | 若删 `ACSLandscapeLayer` 后合并共享 shader 逻辑 | ~150–200 |
| `mcp-10,11,12,13,18,20` | graph-by-name 查找 ×6、transform-JSON ×3、actor 查找 ×6、vector→JSON ×10+、capabilities 手抄注册表、resolve-actor+component 样板 | `MCPUnreal` 多文件 | 全部收拢进 `MCPUnrealUtils.h` | ~400 |
| `mcp-4,5,6` | 测试模块把派发表复制为平行副本（已漂移，接受不存在的 op、漏掉真实 op） | `MCPUnrealTests` | 让测试引用生产派发表，删 `ParseJsonBody_BROKEN` | ~300 |
| `gse-22,23,29` | foliage 实例变换助手、GPU 体素上传块 ×3、foliage↔transform 薄包装 | `GeometryEditorActor.cpp` static ⟷ `UFoliageConverter` | 合并为带 `UWorld*` 的重载 | ~40 |
| `shaders-8,9,18,20,21` | bilinear 采样器、2D value-noise、`ClosestPointOnTriangle`、体素空间哈希、`HashUint` | 多个 `.usf` 各自实现，绕过 `PCGMathCommon.ush` | 收拢进 `PCGMathCommon.ush` | ~100 |
| `hierarchy-18` `pcgep-18` | `IsFiniteVector` ×5、"派生关卡文件夹路径"片段 ×7 | 跨文件 | 提取谓词/助手 | ~50 |
| `aitool-7,8` | 三入口 glue（sync BP 库 / async BP 节点 / commandlet），含仅为绕 unity-build ODR 而改名的同一函数对；二次 Bezier 基实现两遍 | `AITool` | 提取共享 glue | ~80 |
| `csg-13,15` | `AGPUSkeletalTree` 死骨架副本、static-mesh render-data resolve + index-SRV 创建 | `ComputeShaderGenerator` | 合并/删死副本 | ~175 |

## 五、仓库级卫生（hygiene · 非代码）

### 5.1 大体积构建产物 / 克隆（立即回收空间）

| ID | 目标 | 体积 | 动作 |
| --- | --- | --- | --- |
| `hygiene-1` | `Plugins/Temp/`（仅 `Binaries` + `Intermediate`，无 `.uplugin`/`Source`，git 零跟踪） | 4.3GB | 直接删除目录 |
| `critic-1` | `mcp-unreal/`（gitignore 的工作树克隆，含第二份 MCPUnreal 插件副本、22MB `.exe`、内嵌 UE 测试工程；`EditorRoutes.cpp`/`NetworkDebugRoutes.cpp` 已与在用插件漂移） | 26MB | 确定 `Plugins/MCPUnreal` 为唯一来源后删克隆，或至少删可重建的 `.exe`，并调和 2 个漂移文件 |
| `hygiene-5` | `Plugins/AssetHandlerPlugin.7z`（LFS 提交的插件快照，紧挨在用插件） | 9.2MB | 删除归档 |
| `hygiene-2` | `Plugins/PCGPlugins/Binaries_deadjunction`（断裂 junction，指向 `/d/MyWork/...` 旧仓库路径） | — | 删除断裂链接 |

### 5.2 嵌套仓库 / 跟踪问题

| ID | 问题 | 动作 |
| --- | --- | --- |
| `hygiene-3` | `Plugins/PCGPlugins` 是内层 git 仓库，同时被父仓库当普通目录跟踪（双重跟踪，两者已不同步） | 决定单一权威：转为 submodule，或删内层 `.git` 由父仓库统一跟踪 |
| `hygiene-4` | 见 P0 约束一：`CSGpuMesh*`/`CSDirectTriangleMesh*`/`Tests/` 两仓库均未提交（数据丢失暴露） | 先提交/暂存 |
| `hygiene-9` | `.gitignore` 缺口：根目录 `.claude/`、`.cursor/` 未忽略 | 补 `.gitignore`（标准 UE 目录已验证正确忽略） |
| `hygiene-10` | 生成的 591KB `UETest574_2.sln` 被跟踪 | 评估是否加入忽略（`.sln.DotSettings.user` 已正确未跟踪） |

### 5.3 配置与资产

| ID | 问题 | 动作 |
| --- | --- | --- |
| `hygiene-14` | `T_Smoke005.uasset` 同名两副本（内容不同）：`Content/Effects/Fx/0_Resources/Tex/Smoke/` 与 `Content/MaterialLibrary/Texture/CommonNoise/` | 编辑器内确认引用后合并，走重定向器（uasset 移动需 redirect 处理） |
| `hygiene-11` | `Config/DefaultEditor.ini` `SimpleMap` 指向已删关卡 `/Game/TopDown/Maps/TopDownExampleMap` | 更新为现有关卡 |
| `hygiene-12` | `Config/DefaultEngine.ini` 重复/矛盾条目（`RecastNavMesh` 节重复） | 去重 |
| `hygiene-13` | 已挂起删除已验证安全：`Directory.Build.props`、2 个计划 `.md`、`ComputeShaderLandscapeTempLayer.cpp/.h` | 提交这些删除 |
| `critic-3` | `PCGPlugins.uplugin` 强启用 `Landmass` 但代码零引用 | 移除 `Landmass` 依赖项 |
| `critic-4` | `PCGPlugins.uplugin` 源码插件却标 `"Installed": true`（应为 false，其他源码插件均 false） | 改 `false` 或删该键 |

### 5.4 根目录杂物与 Build.cs 依赖

| ID | 问题 | 动作 |
| --- | --- | --- |
| `hygiene-6,7,8` | `docs/`（仅生成的 bleve 搜索索引二进制）、`ShaderTest/`（无引用的调试草稿）、`Tools/`（6 个跟踪文件混 14 个草稿脚本/日志） | 分别评估：`docs/`、`ShaderTest/` 移出跟踪；`Tools/` 分离跟踪与草稿 |
| `shaders-25` | `Tools/_vine_diff.txt`（65KB 陈旧 `VVVoxel.usf` scratch diff） | 删除 |
| `critic-2` | `ComputeShaderGenerator.Build.cs`：未用 `Foliage` 依赖 + `CoreUObject`/`Engine` 在 Public 与 Private 重复 | 精简依赖 |
| `pcgep-16` | `AssetHandlerPlugin`：9 个未用模块依赖、1 个未用插件依赖 | 精简（注：`AssetHandlerPlugin` 本身**非死插件**，被 `EUW_AssetDebug` 引用且 `EnabledByDefault=true`） |
| `pcgep-20` `gse-30` | `PCGEditorProcess.Build.cs` `Core` ×3、`Blutility` ×2；`GeometryScriptExtraEditor.Build.cs` 4 个模块各列两遍 | 去重 |
| `mcp-21` | `MCPUnrealTests` 模块每次编辑器启动都 `PostEngineInit` 加载（无 target 限定） | 限定为测试/开发构建加载 |

## 六、去重后工作量估算

| 优先级 | 内容 | 去重后唯一量 | 决策依赖 |
| --- | --- | --- | --- |
| P0 | 前置安全（提交 WIP、明确嵌套仓库权威） | — | 无（立即做） |
| P0 | 仓库卫生大项（`Plugins/Temp` 4.3GB + `mcp-unreal` 26MB + `.7z` 9MB + 断裂 junction） | ~4.35GB | 无（低风险） |
| P1 | 冗余死代码（1.1–1.5，不含 WIP/兼容路径） | ~6,000–7,500 行 C++ + ~1,000 行 HLSL | 逐条 Content 引用确认 |
| P1 | 重复代码提取共享（净减） | ~1,000–1,300 行 | 无 |
| P1 | 上提到父类 | ~350–450 行 | 无 |
| P2 | 多余兼容性内容（2.1 三条 CPU 路径） | ~1,600 行 | **需产品决策**：是否放弃 CPU 回退 |
| P2 | Epic 模板变体 `Variant_Strategy`/`Variant_TwinStick` | ~3,500 行 + Content | **需产品决策**：是否保留 demo |

## 七、建议执行顺序

1. **P0 安全门**：提交/暂存未跟踪 WIP 源码；决定 `Plugins/PCGPlugins` 与 `mcp-unreal` 的仓库权威并统一。
2. **P0 空间回收**：删 `Plugins/Temp`、断裂 junction、`.7z`；处理 `mcp-unreal` 克隆。风险极低，先见效。
3. **P1 整文件死代码**（1.2）：整类/整文件删除最安全、收益最大，逐个删并编译验证。
4. **P1 迁移死代码**（1.1，不含兼容路径）：删死 CPU 线预处理、step-log、readback 助手、无消费缓存。
5. **P1 死 API/UPROPERTY/shader**（1.3、1.4）：每条先做 Content 二进制扫描；`gse-9`（high）单独处理并重存 BP。
6. **P1 重复提取 + 上提父类**（四、三）：先删逐字节重复头，再收拢助手，最后上提场景代理/捕获类。
7. **P1 配置卫生**（5.3、5.4）：提交已挂起删除、修死配置、精简 Build.cs、修 `.uplugin`。
8. **P2 决策项**：与团队确认后处理兼容路径（2.1）与模板变体。

每步单独提交 + 编译验证；uasset 变更走编辑器重定向；序列化 UPROPERTY（`gse-9`、`xmodule-10`、`pcgep-12`）改动需加载旧资产验证不静默丢值。

## 附录 A：负向 / 需保留发现

- `xmodule-16`（负向）：CPU 空间殖民 / CPU 后处理路径是**活跃 CVar fallback**，非死代码——已在第二章标注需决策。
- `pcgep`：`AssetHandlerPlugin` **非死插件**，与 `PCGEditorProcess` 无功能重叠（视口 clay-material override vs PCG/landscape 工具），被 `EUW_AssetDebug.uasset` 引用且默认启用。
- `aitool`：commandlet 的 `.npy` dump 被 `Plugins/AITool/Tools/compare_golden.py` 消费（golden 测试），非死代码。
- `game`：游戏模块与插件源码**无跨模块重复**（grep 命中均为 UHT 中间产物）。
- 约束四列出的被 Content 引用的反射符号一律保留。

## 附录 B：溯源

全部约 205 条已验证发现的原始记录（含逐条 grep 调用点计数、Content 二进制扫描结果、file:line 证据、风险与修正动作）保存在审计工作流 journal：

```text
.claude/.../subagents/workflows/wf_0ca8c293-27e/journal.jsonl
```

发现按维度编号：`csg-*`（ComputeShaderGenerator）、`gse-*`（GeometryScriptExtraEditor）、`pcgep-*`（PCGEditorProcess+AssetHandler）、`mcp-*`（MCPUnreal）、`aitool-*`（AITool）、`game-*`（Source）、`shaders-*`（Shaders）、`hierarchy-*`（类继承/上提）、`xmodule-*`（跨模块重复）、`hygiene-*` + `critic-*`（仓库卫生）。
