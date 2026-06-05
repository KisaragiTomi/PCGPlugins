# EnhancedHairCards 当前进度

本文记录 `Plugins/EnhancedHairCards` 截至当前的实现状态，重点说明“尽可能继承原版 Groom Cards sim，同时保留自定义渲染”已经做到哪里、还差什么。

## 目标

- 保留 `EnhancedHairCards` 自己的渲染入口：
  - `UEnhancedHairCardsComponent`
  - `FEnhancedHairCardsSceneProxy`
  - `FEnhancedHairCardsVertexFactory`
  - 自定义材质路径和现有属性兼容逻辑
- 尽量复用 UE 原版 `UGroomComponent` 的 Groom Cards 模拟/变形结果：
  - 原版 Niagara guide simulation
  - 原版 cards deformation buffer
  - 原版 cards normal/tangent deformation结果

## 当前结论

当前版本已经从“并排跑一个 Source Groom 只作参考”推进到“自定义组件优先绑定原版 Groom 的 cards deformed buffer 来渲染”。

这意味着：

- 模拟和 cards 顶点变形尽量来自原版 `UGroomComponent`
- 最终 MeshBatch、材质、渲染入口仍然是 `EnhancedHairCards`
- 如果原版 buffer 当前不可用，仍然回退到插件自己的静态 mesh / guide preview 路径

## 已完成

### 1. 原版 sim 资源接线

`UEnhancedHairCardsComponent` 已新增：

| 字段 | 作用 |
| --- | --- |
| `SourceGroomComponent` | 指向原版 `UGroomComponent` |
| `bUseSourceGroomSimulation` | 是否优先使用原版 Groom Cards deformation 结果 |

运行时逻辑：

- 组件注册时会优先尝试自动查找同 Actor 上的 `UGroomComponent`
- `AEnhancedHairCardsActor` 在重建时会把生成的 `SourceGroomSimulationComponent` 显式回填给每个 `UEnhancedHairCardsComponent`
- 导出 Blueprint 时，每个 Enhanced 组件模板也会显式引用 `OriginalGroomCards`

### 2. 自定义渲染绑定原版 cards deformation

`FEnhancedHairCardsSceneProxy` 已加入一条优先路径：

- 如果 `SourceGroomComponent` 可用
- 且对应 `GroupIndex/LODIndex` 的 `FHairGroupInstance::Cards.LODs[LOD]` 有效
- 且 `RestResource` / `DeformedResource` / SRV / index buffer 已经就绪

则 SceneProxy 直接绑定原版资源：

- `Cards.DeformedResource->Current`
- `Cards.DeformedResource->Previous`
- `Cards.DeformedResource->DeformedNormalBuffer`
- `Cards.RestResource->UVsBuffer`
- `Cards.RestResource->MaterialsBuffer`
- `Cards.RestResource->VertexColorsBuffer`（有的话）
- `Cards.RestResource->RestIndexBuffer`

这时仍然使用 `EnhancedHairCardsVertexFactory` 和 Enhanced 的 MeshBatch 提交流程，不切回引擎 `FHairCardsVertexFactory`。

### 3. 回退路径仍保留

如果原版 Groom cards buffer 还没准备好，或者当前 group/LOD 不匹配，仍然保留原来的插件路径：

- `SourceMesh` LOD0 提取几何
- 本地 guide debug
- 本地 preview dynamics / guide simulation

这样不会因为原版资源暂时无效而直接失去显示。

### 4. 资源状态切换检测

`UEnhancedHairCardsComponent` 会持续检查 Source Groom cards resource 是否从“不可用”变成“可用”：

- 状态变化时自动 `MarkRenderStateDirty()`
- 这样首次注册时如果还只能走回退路径，后续原版 Groom buffer 初始化完成后可以切回原版 sim 驱动路径

### 5. Source Groom 可见性保护

`FEnhancedHairCardsPreviewSettings` 新增：

| 字段 | 默认值 | 作用 |
| --- | --- | --- |
| `bKeepSourceGroomVisibleForSimulation` | `true` | 当 Enhanced 组件使用 Source Groom sim 时，保持 Source Groom 可见并参与原版 HairStrands cards pipeline |

这个开关已经接到：

- `AEnhancedHairCardsActor::ApplyAssetPreviewSettings()`
- `FEnhancedHairCardsAssetEditorToolkit::ApplyPreviewVisibility()`
- Blueprint 导出里的 `OriginalGroomCards` 模板可见性

原因是 UE 原版 cards deformation 由 GroomManager 在 HairStrands 渲染流程里处理。直接隐藏 `UGroomComponent` 或关掉 main pass，可能让它不再进入 `View.HairCardsMeshElements`，从而导致 cards deformation buffer 不再按帧更新。

## 本轮已改动文件

| 文件 | 说明 |
| --- | --- |
| `Source/EnhancedHairCards/Public/EnhancedHairCardsComponent.h` | 新增 Source Groom 接线字段和接口 |
| `Source/EnhancedHairCards/Private/EnhancedHairCardsComponent.cpp` | 自动查找 Groom、资源可用性检测、tick 切换逻辑 |
| `Source/EnhancedHairCards/Private/EnhancedHairCardsSceneProxy.h` | 新增 Source Groom cards resource 绑定入口 |
| `Source/EnhancedHairCards/Private/EnhancedHairCardsSceneProxy.cpp` | 优先绑定原版 cards deformed/rest resource 渲染 |
| `Source/EnhancedHairCards/Public/EnhancedHairCardsAsset.h` | 新增 `bKeepSourceGroomVisibleForSimulation` |
| `Source/EnhancedHairCards/Private/EnhancedHairCardsActor.cpp` | 先创建 Source Groom，再把引用传给每个 Enhanced 组件，并按 sim 需求保持 Source Groom 可见 |
| `Source/EnhancedHairCardsEditor/Private/EnhancedHairCardsAssetEditorToolkit.cpp` | 编辑器预览同步 Source Groom 可见性保护 |
| `Source/EnhancedHairCardsEditor/Private/EnhancedHairCardsConverterLibrary.cpp` | 导出 Blueprint 时写入 `SourceGroomComponent` 模板引用，并保持 `OriginalGroomCards` 默认可见以驱动 sim |

## 当前限制

### 1. 仍依赖原版 Groom 进入可见/处理集合

当前实现是“复用原版已经算好的 cards deformation buffer”，不是“主动替原版跑 deformation pass”。

因此如果原版 `UGroomComponent` 完全不参与当帧 HairStrands 处理，cards deformed buffer 可能不会更新。

这也是为什么现在更稳的用法仍然是：

- 保留一个 `SourceGroomSimulationComponent`
- 让它继续走原版 Groom pipeline
- 保持 `bKeepSourceGroomVisibleForSimulation=true`
- Enhanced 组件只接管最终自定义渲染

### 2. 还没有主动触发原版 RDG deformation pass

目前没有在 `EnhancedHairCards` 自己的渲染路径里直接调用：

- `AddHairCardsDeformationPass(...)`
- `AddHairCardsRBFInterpolationPass(...)`

也没有自己组装完整的 GroomManager 级实例处理流程。

所以现在属于：

- 已共享原版 sim/deformation 结果
- 但还没有做到“完全脱离原版 Groom 可见渲染，也能独立驱动原版 deformation”

### 3. 组/LOD 匹配依赖 Source 索引

当前绑定依赖：

- `SourceGroupIndex`
- `SourceLODIndex`

如果资产重建、Groom LOD 策略变化、或者导出的 part 与运行时实际使用的 Groom LOD 不一致，可能需要继续补更稳的映射策略。

## 编译状态

本轮核心改动和后续小清理已经做过窄范围编译，结果：

```text
Build.bat UETest574_2Editor Win64 Development
  D:/MyWork/UnrealProject/UETest574_2/UETest574_2.uproject
  -Module=EnhancedHairCards
  -Module=EnhancedHairCardsEditor
```

最近结果：

- 编译通过
- UHT 通过
- `EnhancedHairCards` / `EnhancedHairCardsEditor` 均编译并链接通过
- 此前完整编译中仍有 `EnhancedHairCardsSceneProxy.cpp` 里已有的 RHI deprecated warning

注意：

- 用户中断前最后两处小改动已经重新编译确认
- 这两处改动是删除未使用 helper，并把“可用”判断恢复为更严格的 SRV/index buffer 就绪检查
- 新增的 Source Groom 可见性保护也已经重新编译确认

## Smoke 验证

已用现有 Groom 资产跑过一次 Blueprint 导出 smoke：

```text
UnrealEditor-Cmd.exe UETest574_2.uproject
  -run=EnhancedHairCardsConvertGroom
  -Groom=/Game/Hair/MainChar_01_Hair_005/MainCharSuit_005_Guide_1
  -Output=/Game/Developers/Codex/BP_EHC_SourceGroomSmoke
  -Blueprint
  -Overwrite
```

结果：

- `EnhancedHairCards export created ... using 3 card mesh(es), copied 287 guide curve(s)`
- 导出路径按现有逻辑重定向到 `/Game/Hair/MainChar_01_Hair_005/BP_EHC_SourceGroomSmoke`
- 临时 smoke 资产已经删除

注意：commandlet 进程返回 `1`，但原因是项目启动时 `HttpListener` 无法绑定 `127.0.0.1:8090`；导出日志本身显示 EnhancedHairCards Blueprint 创建成功。

## 推荐的当前使用方式

现阶段如果目标是“尽可能继承原版 sim，同时保留自定义渲染”，推荐这样使用：

1. 保留 `SourceGroomSimulationComponent`
2. `SetUseCards(true)`，让它继续跑原版 Groom Cards pipeline
3. `UEnhancedHairCardsComponent` 打开 `bUseSourceGroomSimulation`
4. 最终显示使用 Enhanced 组件的自定义材质 / 自定义渲染
5. 保持 `bKeepSourceGroomVisibleForSimulation=true`，避免 Source Groom 被预览隐藏后停止更新 cards deformation buffer

## 下一步

如果要继续往前推，优先级建议如下：

1. 进编辑器实测：
   - `bKeepSourceGroomVisibleForSimulation=true` 时，Enhanced 是否稳定跟随原版 sim
   - 关闭该开关并隐藏 Source Groom 时，deformed buffer 是否停止更新
2. 如果仍希望隐藏 Source Groom 且保持原版 deformation，再进入下一阶段：
   - 在 Enhanced 渲染侧主动接入原版 `AddHairCardsDeformationPass`
   - 或做一个“仅仿真/仅 deformation”的隐藏 Groom 驱动方案
