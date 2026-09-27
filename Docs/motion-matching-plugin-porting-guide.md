# Motion Matching 与动画预测插件移植说明

[返回文档索引](index.md)

记录从 `D:\KeLuwang_PC-KeLuWang_1851\Lycoris_main\Plugins` 移植 Motion Matching / 动画预测相关插件到本工程（`UETest574_2`，UE 5.7.4）的范围、约束与验证结果，并给出以 Epic Game Animation Sample 作为复现底座的接入步骤。

## 移植范围

只移植与 Motion Matching、轨迹预测、学习式动画生成直接相关的三个插件，外加一个被动依赖。

| 插件 | 目标路径 | 文件数 | 体积 | 提供的能力 |
| --- | --- | ---: | ---: | --- |
| `PoseSearch` | `Plugins/Animation/PoseSearch` | 241 | 8 MB | 项目化定制版 Motion Matching 核心：AABB Tree、显式烘焙、5 个自定义特征通道、组成本、跳转冷却、Play Rate 平滑 |
| `CustomAnimationTools` | `Plugins/CustomAnimationTools` | 566 | 620.8 MB | 轨迹预测（`LyPoseSearchTrajectory*`）、Neurava NNE 学习式 MM、PALM / Phase Neural Network、CATS BlendSpace Montage、动画批处理工具 |
| `LycorisCustomAnimationNode` | `Plugins/LycorisCustomAnimationNode` | 39 | 0.4 MB | `MMSteering`、`MMOrientationWarping`、`MMOffsetRootBone`、`PredictIk` 动画节点 |
| `MotionTrajectory` | `Plugins/Animation/MotionTrajectory` | 8 | 25 KB | 引擎插件项目化副本，提供 `UCharacterTrajectoryComponent`，GASP 需要 |

复制时排除 `Intermediate/`、`Binaries/`、`DerivedDataCache/`、`Saved/`，源文件数与源工程逐一对齐。

未移植 `CATSMontageVisualizationSystem`（Montage 可视化，非 Motion Matching 主链路）。`ABP_Locomotion_TP` 引用了它，若后续要复刻该 ABP 需要补上。

## 引擎兼容性

源工程运行在自编译引擎 `D:\KeLuwang_PC-KeLuWang_1851`，本工程运行在 `D:\UnrealEngine-5.7.4-release`。已逐文件哈希比对两侧的动画相关引擎插件：

| 引擎插件 | 源引擎文件数 | 目标引擎文件数 | 差异 |
| --- | ---: | ---: | ---: |
| `Animation/BlendStack` | 27 | 27 | 0 |
| `Chooser` | 191 | 191 | 0 |
| `Animation/AnimationWarping` | 43 | 43 | 0 |
| `Experimental/Animation/MotionTrajectory` | 7 | 7 | 0 |
| `Animation/PoseSearch` | 195 | 195 | 0 |

结论：这些上游插件在两个引擎中完全一致，项目化的 `PoseSearch` 可直接落到本工程，不需要额外的引擎侧补丁。

## 关键约束：引擎插件不能引用项目模块

`PoseSearch` 被项目化后，`/Script/PoseSearch` 变成**项目模块**。UBT 的模块层级规则是 `Project -> Engine Programs -> Engine Plugins -> Engine`，因此任何**引擎插件**再去引用 `PoseSearch` 都会直接构建失败：

```text
Module 'MotionTrajectory' (Engine Plugins) should not reference module 'PoseSearch' (Project).
Hierarchy is Project -> Engine Programs -> Engine Plugins -> Engine.
Errors validating plugins or modules.
```

目标引擎中依赖 `PoseSearch` 模块的引擎插件共 5 个，启用其中任何一个都会触发同样的错误：

| 引擎插件 | 默认启用 | 本工程处理 |
| --- | --- | --- |
| `MotionTrajectory` | 否 | 已项目化复制到 `Plugins/Animation/MotionTrajectory` |
| `Mover` | 否 | 未启用 |
| `MoverAnimNext` | 否 | 未启用 |
| `MovieScenePoseSearchTracks` | 否 | 未启用 |
| `UAFPoseSearch` | 否 | 未启用 |

有两种解法，本工程采用第一种：

1. **把该引擎插件也复制进项目**。项目插件引用项目模块是合法的。`MotionTrajectory` 只有 8 个文件，代价极低，且不改动引擎。
2. **打补丁的 UnrealBuildTool**。源工程自带该方案，位于 `Plugins/Animation/PoseSearch/Tools/UBTSwitch/`，用 `allowlist.json` 放行指定引擎插件。当前放行名单为 `Mover`、`MoverAnimNext`、`UAFPoseSearch`，不含 `MotionTrajectory`。该方案会替换引擎的 UBT，影响该引擎下所有工程。

## PoseSearch 的反射身份

`Plugins/Animation/PoseSearch/Tools/PoseSearchSwitch/` 支持两种模式，当前处于 `EngineCompat`：

| 模式 | 运行时模块 | 反射包 | 对外部资产的影响 |
| --- | --- | --- | --- |
| `EngineCompat`（当前） | `PoseSearch` | `/Script/PoseSearch` | 与引擎原版同名，GASP 等外部资产可直接加载 |
| `CustomIsolated` | `CustomPoseSearch` | `/Script/CustomPoseSearch` | 与引擎版共存互不冲突，但引用 `/Script/PoseSearch` 的外部资产会失效 |

复现 GASP 必须保持 `EngineCompat`，否则 GASP 的 Schema、Database、AnimBP 会全部丢引用。

## .uproject 变更

在 `UETest574_2.uproject` 的 `Plugins` 数组追加以下条目（原有条目未改动）：

```jsonc
{ "Name": "PoseSearch",                 "Enabled": true },  // 项目化定制版，覆盖引擎同名插件
{ "Name": "CustomAnimationTools",       "Enabled": true },  // 轨迹预测 + Neurava
{ "Name": "LycorisCustomAnimationNode", "Enabled": true },  // MM 动画节点
{ "Name": "Chooser",                    "Enabled": true },  // PoseSearch 依赖，多 Database 语义筛选
{ "Name": "BlendStack",                 "Enabled": true },  // PoseSearch 依赖
{ "Name": "AnimationWarping",           "Enabled": true },  // Orientation Warping
{ "Name": "MotionWarping",              "Enabled": true },  // CustomAnimationTools 依赖
{ "Name": "MotionTrajectory",           "Enabled": true },  // 项目化副本，GASP 轨迹组件
{ "Name": "AnimationLocomotionLibrary", "Enabled": true },  // Distance Matching
{ "Name": "IKRig",                      "Enabled": true },  // CustomAnimationTools 依赖
{ "Name": "GameplayAbilities",          "Enabled": true },  // CustomAnimationTools 依赖
{ "Name": "NNERuntimeORT",              "Enabled": true },  // Neurava 推理后端
```

`NNERuntimeORT` 是必需项：Neurava 通过 `UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"))` 取推理后端，缺失时学习式路径无法初始化。`NNE` 本身是引擎 Runtime 模块（`Engine/Source/Runtime/NNE`），不需要在插件列表中启用。

## 验证结果

已验证项：

- 编译：`UETest574_2Editor Win64 Development` 通过，`PoseSearch`、`PoseSearchEditor`、`CustomAnimationToolsRuntime`、`CustomAnimationToolsEditor`、`LycorisCustomAnimationNode` 等 DLL 均生成在各自插件的 `Binaries/Win64` 下。
- 插件挂载：编辑器日志确认三个插件以项目插件身份挂载，且项目版 `PoseSearch` 正确覆盖引擎版。

```text
LogPluginManager: Display: By default, prioritizing project plugin (.../UETest574_2/Plugins/Animation/PoseSearch/PoseSearch.uplugin)
                  over the corresponding engine version (../../../Engine/Plugins/Animation/PoseSearch/PoseSearch.uplugin).
LogPluginManager: Mounting Project plugin PoseSearch
LogPluginManager: Mounting Project plugin CustomAnimationTools
LogPluginManager: Mounting Project plugin LycorisCustomAnimationNode
```

- 自动化测试：`CustomAnimationTools.SpringMovement` 全部 **21 个测试通过，0 失败**。

未验证项：未启动 PIE，未做任何动画质量或性能实测。

## 已通过的自动化测试

轨迹预测（“动画预测”）能力已在本工程实测通过，不依赖任何角色资产。11 个轨迹测试全部 `Success`：

```text
Trajectory.Determinism                                 # 固定步长下轨迹可重复
Trajectory.VelocityIntegration                         # 速度积分正确性
Trajectory.AirborneGravity                             # 空中重力处理
Trajectory.WorldCollisionModePolicy                    # 世界碰撞模式策略
Trajectory.FacingOffset                                # 朝向偏移
Trajectory.ExternalVelocityKeepsSpringState            # Root Motion 接管时保留弹簧状态
Trajectory.ExternalFacingReset                         # 外部朝向接管后的复位
Trajectory.ExternalIdleDoesNotFeedBack                 # 静止时外部状态不回灌
Trajectory.AirborneExternalVelocityDoesNotFeedBack     # 空中外部速度不回灌
Trajectory.AirborneExternalFacingDoesNotFeedBack       # 空中外部朝向不回灌
Trajectory.ExternallyDrivenGroundedFacingReanchorsWholeSpring  # 地面外部朝向重锚整段弹簧
```

另有 10 个弹簧移动模型测试（`AccelerationBudget`、`DoubleCriticalSpringFloat`、`Smoothing`、`SplitUpdateMatchesCombined` 等）同样通过。

复现方式：

```bash
"D:\UnrealEngine-5.7.4-release\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\MyProject\UnrealProject\UETest574_2\UETest574_2.uproject" -ExecCmds="Automation RunTests CustomAnimationTools.SpringMovement" -TestExit="Automation Test Queue Empty" -unattended -nopause -nosplash -nullrhi -skipcompile
```

不要加 `-NoShaderCompile`。该标志会让 `FUnrealEdMisc::OnInit` 在 `FShaderCompilerStats::GetTotalShadersCompiled()` 处触发 `Null assigned to TNotNull` 崩溃，与本次移植无关。

## GASP 案例复现

选定的复现底座是 Epic 官方 Game Animation Sample（业界标准 Motion Matching 案例，含完整 MM 数据库与 Manny 动画库）。

当前状态：**本机没有该工程**。已确认扫描范围：

- `D:\`、`C:\` 全盘 `.uproject` 枚举，无 Game Animation Sample。
- Epic Launcher Vault Cache（`C:\ProgramData\Epic\EpicGamesLauncher\VaultCache`）只有 `Lyra_5.5`。
- 源工程文档 `Tools/UAFPoseSearch_Projectization_Notes.md` 提到的 `D:\Doc\Unreal Projects\GameAnimationSample5__7` 路径不存在。

需要先由使用者从 Fab 下载 Game Animation Sample（需 Epic 账号登录，约 4 GB）。下载后的接入步骤：

1. 用 UE 5.7 打开 GASP 工程，`Content` 全选后 Migrate 到本工程 `Content/`；或直接复制 `Content/` 目录。
2. 确认 GASP 依赖的插件在本工程 `.uproject` 中已启用。`PoseSearch`、`Chooser`、`BlendStack`、`AnimationWarping`、`MotionTrajectory`、`AnimationLocomotionLibrary` 已启用。
3. 首次打开会触发 PoseSearch Database 重建派生数据。项目版 `PoseSearch` 的 `bUseExplicitBaking` 默认开启，若报烘焙数据失效，需要在 Database 资产上手动重新烘焙。
4. 跑通 GASP 原版 locomotion 后，再把 Lycoris 定制能力逐项接上：
   - 用 `LyPoseSearchTrajectoryComponent` 替换 GASP 的 `UCharacterTrajectoryComponent`，对比轨迹形状与响应。
   - 在 AnimGraph 中接入 `MMSteering`、`MMOrientationWarping`、`MMOffsetRootBone`、`PredictIk`。
   - Schema 中启用自定义特征通道（`FKVelocity` 是源工程唯一确认在生产 Schema 中使用的）。
   - Neurava 学习式路径需要重新训练的模型，GASP 骨骼与源工程 `MainChar_01` 不同，插件自带模型不能直接复用。

## 为什么不能直接复刻源工程案例

源工程的生产案例 `ABP_MainChar`、`ABP_Locomotion_TP` 都引用了游戏模块 `/Script/Lycoris`：

```text
ABP_Locomotion_TP  -> /Script/Lycoris, /Script/CATSMontageVisualizationRuntime, /Script/LycorisCustomAnimationNode, ...
ABP_MainChar       -> /Script/Lycoris, /Script/LycorisCustomAnimationNode, ...
```

`/Script/Lycoris` 是源工程 `Source/Lycoris` 游戏模块，不是插件，且依赖 `SMSystem`、`ExternalStateTree`、`CommonGame`、`Wwise`、`Steamworks` 等一整套插件。直接搬运 ABP 只会得到一堆断引用。

同时生产资产的规模也不适合搬运：

| 目录 | 体积 | 文件数 |
| --- | ---: | ---: |
| `Content/Characters/ABPTemplate` | 507.9 MB | 268 |
| `Content/Animations/MotionMatchingTest` | 393.8 MB | 496 |
| `Content/Characters/MainChar/MainChar_01` | 10.4 GB | 4299 |

所有生产 Schema 都绑定 `SKEL_MainChar_01`，位于上表第三行的 10.4 GB 目录内。

插件自带的 `Content/LearnMotionMatching/PSD_LearnMM` 也不是自包含的：它引用 `/Game/Developers/SYB/Collections/MainCharAnimUbi/` 下约 40 个动画，而该目录在源工程中实际只有 4 个文件，其余引用在源工程中已经是断的。

## 已知警告

编译期出现、不影响本次构建的警告：

- `StructUtils` 插件在 5.5 已废弃，`CustomAnimationTools` 与 `LycorisCustomAnimationNode` 仍依赖它。下个引擎版本前需要迁移。
- `FPoseSearchQueryTrajectory` / `FPoseSearchQueryTrajectorySample` 已废弃，应改用 `FTransformTrajectory` / `FTransformTrajectorySample`。集中出现在 `CustomAnimationToolsRuntime/Public/FunctionLibrary/NeuravaLibrary.h` 与对应 `.cpp`。
- `UE::PoseSearch::FSearchIndex::CompareAlignedPoses` 已废弃，见 `PoseSearchDatabase.cpp`。
- MSVC 工具链使用 14.38.33145，UBT 建议 14.44.35207。

## 未决问题

- Game Animation Sample 尚未下载，GASP 案例复现未开始。
- Neurava 模型与 GASP 骨骼不匹配，学习式路径能否在 GASP 上复现待定。
- Neurava 的 NNE 推理未做端到端验证，只确认了 `NNERuntimeORT` 能加载并枚举到本机 GPU。
- 启动时 `LogClass` 报告 `CustomAnimationToolsRuntime`、`LycorisCustomAnimationNode` 等模块存在多处 UPROPERTY 未初始化（如 `FAnimNode_MMOffsetRootBone::MMRefAnimInstance`、`FAnimationOverlayerParams::*`）。属于源工程既有代码质量问题，不影响本次构建与测试。

## 移植过程中的构建故障

移植期间本工程原有的 `PCGPlugins` 出现过一串与 Motion Matching 无关的构建故障，处理路径记录如下，便于以后复现：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `Module 'MotionTrajectory' (Engine Plugins) should not reference module 'PoseSearch' (Project)` | `PoseSearch` 项目化后引擎插件无法引用它 | 把 `MotionTrajectory` 也复制进项目 |
| `MSB0001: Invalid node id specified` | MSBuild 节点复用守护进程残留 | 设 `MSBUILDDISABLENODEREUSE=1`，清理空闲 MSBuild 节点 |
| `IOException: file being used by another process` | 上一次失败构建残留的 UBT 进程占用中间文件 | 结束残留 UBT 进程后重建 |
| `ComputeShaderGenerator` `LNK2011: 未链接预编译对象` | 引擎侧 RTTI 版共享 PCH 缓存失效（`bUseRTTI = true` 的模块才会用到该变体） | 删除 `Engine/Intermediate/Build/Win64/x64/UnrealEditor/Development/UnrealEd/SharedPCH.UnrealEd.Project.RTTI.ValApi.ValExpApi.Cpp20.h.*` 让其重建 |
| `PCGEditorProcess` 53 个 `LNK2019` 未解析符号 | UBT 的 `Makefile.bin` / `DependencyCache.bin` 认为目标文件是最新的，但磁盘上已被清掉，导致只链接不编译 | 删除这两个缓存文件后重建 |

注意：编辑器启动时若发现模块过期会自动触发一次后台编译。该自动编译中断后会留下不一致的中间产物，是上面多数故障的源头。用命令行验证时建议加 `-skipcompile`。
