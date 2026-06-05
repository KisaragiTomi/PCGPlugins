# EnhancedHairCards Groom-Style Editor Plan

本计划用于把 `EnhancedHairCards` 的转换结果从“普通 Actor Blueprint 体验”升级为“类似原生 Groom 的自定义资产编辑器体验”。目标不是立刻替换现有渲染逻辑，而是先把资产形态、编辑器界面、预览和多部件管理设计稳定下来。

## Current State

当前实现已经具备 groom card 数据转换和运行时组件基础，但编辑体验仍然是普通蓝图。

| Area | Current Behavior | Key Files |
| --- | --- | --- |
| 转换入口 | `ConvertGroomToEnhancedHairCardsBlueprint()` 生成 `UBlueprint` | `Plugins/EnhancedHairCards/Source/EnhancedHairCardsEditor/Private/EnhancedHairCardsConverterLibrary.cpp` |
| 转换产物 | Actor BP，包含 `SceneRoot`、`OriginalGroomCards`、多个 `UEnhancedHairCardsComponent` | 同上 |
| 多部件 | `SelectedCards` 循环创建多个 Enhanced component | `EnhancedHairCardsConverterLibrary.cpp` |
| 组件数据 | `SourceMesh`、Group/LOD、material、guide curves | `EnhancedHairCardsComponent.h`、`EnhancedHairCardsDatas.h` |
| 渲染/调试 | `FEnhancedHairCardsSceneProxy` 读取 StaticMesh LOD0，支持 guide debug lines | `EnhancedHairCardsSceneProxy.cpp` |
| Editor UI | 没有自定义 asset editor，双击结果会打开普通 Blueprint Editor | `EnhancedHairCardsEditorModule.cpp` |

## Target UX

转换后的资产应像原生 `UGroomAsset` 一样打开独立编辑器，而不是普通蓝图编辑器。

- Content Browser 中显示为 `Enhanced Hair Cards` 类型资产。
- 双击资产打开自定义 editor toolkit。
- 主界面包含 preview viewport 和 Groom 风格 tab。
- 支持查看源 Groom、card parts、材质、guide/debug、preview 设置。
- 保留“导出/生成 Actor Blueprint”能力，作为关卡放置和旧流程兼容路径。

## Architecture Decision

采用“新增自定义资产 + 自定义 Asset Editor”的方案。

不建议只做 Blueprint Details 面板美化。Details customization 可以改善组件属性展示，但无法改变双击资产进入普通 Blueprint Editor 的事实，也很难做到原生 Groom 那种 preview viewport + multi-tab 体验。

| Option | Result | Decision |
| --- | --- | --- |
| 美化 `UEnhancedHairCardsComponent` Details | 仍然打开普通 Blueprint Editor | 只作为后续辅助 |
| 新增 `UEnhancedHairCardsAsset` | 可拥有自定义编辑器、资产图标、preview viewport | 采用 |
| 直接复用/继承 `UGroomAsset` | 与原生 Groom pipeline 耦合过深 | 不采用 |

## Data Model

新增运行时可序列化资产类，放在 `EnhancedHairCards` Runtime 模块，Editor 模块只负责界面和转换工具。

| Type | Module | Purpose |
| --- | --- | --- |
| `UEnhancedHairCardsAsset` | `EnhancedHairCards` | 保存转换后的所有 groom card 部件数据 |
| `FEnhancedHairCardsPart` | `EnhancedHairCards` | 单个 card mesh/part 的 source、settings、material、guides |
| `FEnhancedHairCardsPreviewSettings` | `EnhancedHairCards` | 预览开关、显示模式、debug 默认值 |
| `FEnhancedHairCardsAssetEditorToolkit` | `EnhancedHairCardsEditor` | 独立资产编辑器 |
| `SEnhancedHairCardsEditorViewport` | `EnhancedHairCardsEditor` | Preview viewport |

建议字段：

| Field | Type | Meaning |
| --- | --- | --- |
| `SourceGroom` | `TObjectPtr<UGroomAsset>` | 原始 Groom，用于引用、预览和重建 |
| `Parts` | `TArray<FEnhancedHairCardsPart>` | 转换出的多个 card 部件 |
| `SourceGroupIndex` | `int32` | 源 Groom group |
| `SourceLODIndex` | `int32` | 源 cards LOD |
| `SourceCardsDescriptionIndex` | `int32` | `GetHairGroupsCards()` 中的原始 description index |
| `SourceMesh` | `TObjectPtr<UStaticMesh>` | imported card mesh |
| `Material` | `TObjectPtr<UMaterialInterface>` | 该部件材质 |
| `CardSettings` | `FEnhancedHairCardsSettings` | 当前组件使用的 cards 设置 |
| `GuideCurves` | `TArray<FEnhancedHairCardsGuideCurve>` | 从 cards guide bulk data 或 mesh fallback 得到的 guide |

`FEnhancedHairCardsPart` 不保存独立 `TextureSlots` 参数。Card mesh 只负责几何、材质引用和 guide/debug 数据；贴图由材质参数或源 Groom 参考数据承担，避免把每个 card part 变成一套额外的贴图配置。

## Editor Layout

参考原生 Groom Editor 的体验，但只实现当前插件真正需要的页面。

| Tab | Content | Notes |
| --- | --- | --- |
| `Render` | Preview viewport | 显示 `OriginalGroomCards` 和 Enhanced preview components |
| `Cards` | Parts list/details | 每个 part 一行，显示 mesh、group、LOD、guide count |
| `Material` | Material reference and override | 不暴露 per-card texture slot 参数 |
| `Guides` | Guide debug settings | 开关、颜色、线宽、root tick |
| `Preview` | Preview scene/component settings | 显示模式、原始 Groom/Enhanced toggle |

Toolbar 建议：

| Command | Effect |
| --- | --- |
| `Rebuild From Groom` | 从 `SourceGroom` 重新收集 card descriptions |
| `Toggle Original Groom` | 显示/隐藏原始 `UGroomComponent` |
| `Toggle Enhanced Cards` | 显示/隐藏 Enhanced components |
| `Toggle Guides` | 开关 guide debug |
| `Export Blueprint` | 生成当前兼容 Actor Blueprint |

## Implementation Plan

### Phase 1: Asset Layer

1. 新增 `UEnhancedHairCardsAsset` 和 `FEnhancedHairCardsPart`。
2. 把当前 `PopulateComponentFromCardDescription()` 中的“从 Groom CardDesc 抽数据”拆成可复用 helper。
3. 新增 `ConvertGroomToEnhancedHairCardsAsset()`，生成 `UEnhancedHairCardsAsset`。
4. 保留现有 `ConvertGroomToEnhancedHairCardsBlueprint()`，但内部可以改为先生成/读取 asset 数据，再导出 BP。

### Phase 2: Content Browser Integration

1. 在 Editor 模块注册 Enhanced asset 类型。
2. UE 5.7 优先检查是否使用 `UAssetDefinition`；如果当前工程模块更适合旧路径，则使用 `FAssetTypeActions_Base`。
3. 添加资产显示名称、颜色、分类和双击打开逻辑。
4. 确保新资产保存到 `/EnhancedHairCards/...` 路径时能被 Asset Registry 正确发现。

### Phase 3: Custom Asset Editor

1. 新增 `FEnhancedHairCardsAssetEditorToolkit`。
2. 创建 tab layout：`Render`、`Cards`、`Material`、`Guides`、`Preview`。
3. 使用 `PropertyEditor` 创建多个 DetailsView，分别绑定 asset、selected part、preview settings。
4. 创建 preview scene，临时生成：
   - `UGroomComponent` for `SourceGroom`
   - one `UEnhancedHairCardsComponent` per `Part`
5. Details 改动后刷新 preview components，并 `MarkPackageDirty()`。

### Phase 4: Preview and Component Sync

1. 为 `UEnhancedHairCardsComponent` 增加从 `FEnhancedHairCardsPart` 填充的 helper。
2. Preview 中支持多部件显示/隐藏。
3. Guide debug 使用 asset 中的 `GuideCurves`，继续保持“有存储曲线优先，mesh fallback 其次”。
4. 保持当前默认策略：原始 Groom 负责真实物理/主预览，Enhanced 负责 overlay/debug；如果用户打开 `bRenderCardsMesh`，才显示 Enhanced mesh path。

### Phase 5: Blueprint Export Compatibility

1. 添加 `Export Blueprint` 命令。
2. 生成和当前一致的 Actor BP：
   - `SceneRoot`
   - `OriginalGroomCards`
   - one `UEnhancedHairCardsComponent` per part
3. 保证 commandlet 支持两个目标：

```text
-run=EnhancedHairCardsConvertGroom -Groom=/Game/Hair/Groom -Output=/EnhancedHairCards/Hair/EHC_Asset
-run=EnhancedHairCardsConvertGroom -Groom=/Game/Hair/Groom -Output=/EnhancedHairCards/Hair/BP_Hair -Blueprint
```

### Phase 6: Validation

1. 编译 Runtime 和 Editor 模块。
2. 用一个含多个 imported Groom Cards 的 GroomAsset 测试转换。
3. 验证 Content Browser 双击打开自定义 editor，而不是 Blueprint Editor。
4. 验证 Cards tab 中每个部件的 mesh、group、LOD、material、guide count 正确。
5. 验证 viewport 中原始 Groom、Enhanced mesh、guide debug 可以独立开关。
6. 验证导出的兼容 BP 与当前旧流程结果一致。

## Build Command

优先使用窄构建，减少无关模块干扰。

```powershell
& "$env:UE_ROOT\Engine\Build\BatchFiles\Build.bat" `
  UETest574_2Editor Win64 Development `
  UETest574_2.uproject `
  -WaitMutex -NoHotReload `
  -Module=EnhancedHairCards `
  -Module=EnhancedHairCardsEditor `
  -NoUBA -MaxParallelActions=1
```

## Risks

| Risk | Mitigation |
| --- | --- |
| 原生 Groom editor 私有实现较多，直接复制成本高 | 只参考 tab/viewport 架构，不依赖私有类 |
| 当前转换 helper 绑定 component 模板 | 先抽出 part 数据填充，再由 asset/editor/BP 共用 |
| `UAssetDefinition` 与旧 `AssetTypeActions` API 选择 | 先检查 UE 5.7 本地模块用法，再落地 |
| Preview 组件生命周期复杂 | 使用 transient preview scene，编辑器关闭时释放 |
| 多部件属性编辑容易脏数据不同步 | 所有 UI 修改走 asset 数据，再统一重建 preview |

## Acceptance Criteria

- 转换源 Groom 后得到 `UEnhancedHairCardsAsset`。
- 双击该资产打开 EnhancedHairCards 自定义编辑器。
- 编辑器布局包含 viewport 和 Groom 风格的 Cards/Material/Guides/Preview tab。
- 多个 card 部件在 Cards tab 中可见，并能选中查看单独设置。
- Preview 中可以切换原始 Groom、Enhanced mesh path、guide debug。
- 旧的 Actor Blueprint 导出路径仍可用。

## Open Questions

- 新资产是否需要支持直接拖入关卡生成 preview actor，还是只通过 `Export Blueprint` 放置。
- `bRenderCardsMesh` 在新资产中是否仍默认 false，还是在 editor preview 中单独提供显示 override。
- 是否需要把现有 `Plugins/EnhancedHairCards/PLAN.MD` 迁移/合并到根目录计划，还是保留为历史开发记录。
