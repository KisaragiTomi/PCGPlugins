# Compute Shader 地形图层编辑框架

> 梳理 PCGPlugins 中「基于 Compute Shader 的 Landscape 图层编辑」整套框架的模块关系、
> 数据结构、数据流与已知架构问题。本文为架构说明，不含逐行代码。

## 1. 模块分层

三个 Source 模块自下而上协作：

| 层 | 模块 | 职责 |
|---|---|---|
| 数据读取层 | `GeometryScriptExtraEditor` | 把 Landscape 高度图读成纹理数据（统一用 `FCSReadLandscapeData`） |
| CS 调度层 | `ComputeShaderGenerator` | 底层 `FGlobalShader` 封装、`.usf` 入口、运行时图层、共享结构 + 单一 BlendMode 枚举 |
| 编辑器图层层 | `PCGEditorProcess` | 编辑器 Edit Layer Actor，接入引擎 Edit Layer Merge 管线 |

```
GeometryScriptExtraEditor   读取：Landscape 高度 -> 纹理
   │  ULandscapeExtra::CreateLandscapeTextureData -> FCSReadLandscapeData
   ▼
ComputeShaderGenerator      调度：FGlobalShader + .usf；运行时图层
   │  FCSReadLandscapeData（唯一结构）/ ECSLandscapeBlendMode（唯一枚举）/ FCS* shaders
   ▼
PCGEditorProcess            接入：ACSLandscapeEditLayerBase + ILandscapeEditLayerRenderer + UCSLandscapeEditLayer
```

## 2. 图层 Actor（框架核心，四者并列）

| Actor | 模块 | 环境 | 定位 | CS 入口 |
|---|---|---|---|---|
| `ACSLandscape` / `ACSLandscapeRiver` | PCGEditorProcess | 编辑器 | 高度复制粘贴、河床雕刻、河流模拟 | `CSLandscape.usf :: CSLandscapeFunction` |
| `ACSLandscapeTempLayer` | PCGEditorProcess | 编辑器 | 纯非破坏临时改动 | `CSLandscape.usf :: CSLandscapeFunction` |
| `ACSLandscapeLayer` | PCGEditorProcess | 编辑器 | 材质驱动 + 噪声生成混合 | `LandscapeLayer.usf :: CSLandscapeLayerFunction` |
| `ACSRuntimeLandscapeLayer` / `...Manager` | ComputeShaderGenerator | 运行时 | 运行时噪声/侵蚀/河床/自定义高度场，多层叠加 | `LandscapeLayer.usf :: CSRuntimeLandscapeLayerFunction` |

接入方式两条路线：

- 编辑器三层：统一继承公共基类 `ACSLandscapeEditLayerBase`（其本身实现
  `ILandscapeEditLayerRenderer`），经 `UCSLandscapeEditLayer`（继承
  `ULandscapeEditLayerProcedural`）挂进引擎 Edit Layer Merge 管线。子类只重写
  `RenderLayer` 及自身的生成/混合逻辑。
- 运行时层：继承 `ACSRangeGenerator`，靠 `Tick` 驱动，不走 Edit Layer。

## 3. 统一数据流（编辑器三层共享）

```
[1] 读取    Landscape 高度 -> FCSReadLandscapeData（纹理 + UV/世界范围 + Transform）
              ULandscapeExtra::CreateLandscapeTextureData
      │
[2] 生成    按 Source/Generation 模式产出目标高度/Alpha 到 RT
              Flat / 噪声 / 噪声+侵蚀 / 外部RT / 材质纹理驱动
      │
[3] 混合    CS：原始高度 × 目标高度 × Alpha × 衰减(Falloff)，按 ECSLandscapeBlendMode 输出 RT_Result
      │
[4a] 预览   写入 Merge 管线 scratch RT（RenderLayer 回调中各子类的混合写入）
              非破坏，删 Actor 即还原
      │
[4b] 提交   CommitToLandscape -> 基类 BakeResultToLandscape：
              FScopedSetLandscapeEditingLayer 把 RT 永久烘进高度图，再 RequestLayersContentUpdate
```

关键状态位：

- `bHasResult`（已下沉到基类，三层统一）：控制 `RenderLayer` 是否参与合批。
- `OwnedEditLayerGuid`（已下沉到基类）：绑定该 Actor 在 Landscape 上独占的 Edit Layer。
- `ACSLandscape` 用 `PersistentResult`(UTexture2D) 序列化结果，跨存档保留。

## 4. 底层 CS 资产（地形相关仅 2 个 .usf）

| .usf | 入口 | 使用者 |
|---|---|---|
| `CSLandscape.usf` | `CSLandscapeFunction` | `ACSLandscape`、`ACSLandscapeTempLayer` 共用 |
| `LandscapeLayer.usf` | `CSLandscapeLayerFunction` | `ACSLandscapeLayer`（编辑器） |
| `LandscapeLayer.usf` | `CSRuntimeLandscapeLayerFunction` | `ACSRuntimeLandscapeLayer`（运行时） |

数据载体：编辑器侧与运行时侧已统一为单一结构 `FCSReadLandscapeData`
（定义在 ComputeShaderGenerator），`ULandscapeExtra::CreateLandscapeTextureData` 直接产出它，
旧的 `FReadLandscapeData` 已删除。

## 5. 已落地的重构（编辑器三层）

1. **公共基类下沉**：`ACSLandscapeEditLayerBase` 承载全部 Edit Layer 接入逻辑
   （`OwnedEditLayerGuid` / `FindLandscape` / `EnsureEditLayer` / `RemoveEditLayer` /
   `RequestLandscapeUpdate` / `BakeResultToLandscape` / 四个 renderer 接口样板 / 生命周期清理）。
   三个编辑器 Actor 改为继承它，子类只保留 `RenderLayer` 与自身生成/混合逻辑。
2. **数据结构合并**：`FReadLandscapeData` 已删除，全局统一 `FCSReadLandscapeData` 一份。
3. **BlendMode 枚举合并**：旧的 `ETempLayerBlendMode` / `ELandscapeLayerBlendMode` 已删除，
   统一为单一枚举 `ECSLandscapeBlendMode`（`Alpha=0 / Override=1 / Additive=2 /
   Subtract=3 / Multiply=4 / MaterialDrive=5`），下沉到 ComputeShaderGenerator，
   `.usf` 内整数分支已对齐新序号。

## 6. CS 里如何「添加 landscape layer」

这里的「添加图层」分两件事：① 在 Landscape 上**新建一条 Edit Layer 槽位**（注册阶段）；
② 让 CS 算出的结果**进入这条图层**（写入阶段）。编辑器三层和运行时层走的是两条完全不同的路。

### 6.1 编辑器侧：新建 Edit Layer 槽位（已统一）

三层现在走同一条路径——基类 `ACSLandscapeEditLayerBase::EnsureEditLayer`，
都绑定自定义类 `UCSLandscapeEditLayer`：

```
ACSLandscape / ACSLandscapeTempLayer / ACSLandscapeLayer
   Landscape->CreateLayer("CS_<ActorName>", UCSLandscapeEditLayer::StaticClass())
       └─ 统一绑定自定义类，使该层能反向找回拥有它的 Actor
```

通用步骤（基类 `EnsureEditLayer`，三层共用）：

```
[1] FindLandscape()                         拿到世界里的 ALandscape
[2] 若 OwnedEditLayerGuid 已有效且仍能 GetLayerIndex 命中 → 直接复用，跳过创建
       否则 Invalidate，准备重建（防止存档回来后 GUID 失效）
[3] LayerName = "CS_<ActorName>"            每个 Actor 独占一条命名图层
[4] NewIdx = Landscape->CreateLayer(LayerName, UCSLandscapeEditLayer::StaticClass())
[5] OwnedEditLayerGuid = GetLayerConst(NewIdx)->EditLayer->GetGuid()
       记录 GUID，作为该 Actor 与这条图层的唯一绑定
```

- 命名策略统一为 `CS_<ActorName>`。
- 配对清理统一走基类 `RemoveEditLayer`（按 `OwnedEditLayerGuid` 查 `GetLayerIndex`
  再 `DeleteLayer`），并挂在 `EndPlay(Destroyed)` / `Destroyed()` 生命周期里。
- `ACSLandscapeLayer` 原先那套 `bLayerCreated` / `LayerIndex` / `LayerGuid` 记账机制
  已废弃；其 `CreateLandscapeLayer` / `RemoveLandscapeLayer` 蓝图接口保留，但内部改为
  转调基类的 `EnsureEditLayer` / `RemoveEditLayer`。

### 6.2 自定义类的作用：让 Merge 管线找回 Actor

`UCSLandscapeEditLayer`（继承 `ULandscapeEditLayerProcedural`）是「图层」与「Actor」之间的桥。
引擎在合批时调用 `GetEditLayerRendererStates`，它按 **GUID 反查**对应 Actor：

```
GetEditLayerRendererStates(MergeContext):
    MyGuid = GetGuid()                       当前这条 Edit Layer 的 GUID
    遍历世界中的 ACSLandscapeEditLayerBase（一种基类即可覆盖三层）：
        若 Actor->OwnedEditLayerGuid == MyGuid:
            把 Actor（ILandscapeEditLayerRenderer）登记为该层的 Renderer
    返回 States    →  引擎随后回调 Actor 的 RenderLayer 把 CS 结果写进合批 RT
```

即：CreateLayer 建槽 → 记录 GUID → 自定义类按 GUID 把 Actor 挂回管线，
之后 `RenderLayer` 才有机会把 CS 输出注入这条图层。统一基类后，反查只需遍历
`ACSLandscapeEditLayerBase` 一种类型，并通过 friend 关系读取其 `OwnedEditLayerGuid`。

### 6.3 写入图层：预览 vs 提交

- **预览（非破坏）**：置 `bHasResult=true` → `RequestLandscapeUpdate`
  触发 `RequestLayersContentUpdateForceAll`，引擎重跑 Merge，在 `RenderLayer` 回调里把
  CS 结果叠进该 Edit Layer 的 scratch RT。删 Actor 即还原。
- **提交（永久）**：`CommitToLandscape` 统一转调基类 `BakeResultToLandscape`，把 RT 读回
  CPU 高度数据后，用 `FScopedSetLandscapeEditingLayer(Landscape, TargetLayerGuid, ...)`
  把当前编辑目标切到持久层（index 0），再 `LandscapeEdit.SetHeightData(...)` 永久烘进去：

```
ClearEditLayer(LayerIndex, Heightmap)                先清掉该层旧内容
FScopedSetLandscapeEditingLayer Scope(Landscape, TargetLayerGuid, [重建回调]):
    LandscapeEdit.SetHeightData(ReadRange, HeightData...)   写进当前编辑图层
```

`FScopedSetLandscapeEditingLayer` 的关键作用：在作用域内把「当前编辑图层」临时指向
`TargetLayerGuid`，所有 `SetHeightData` 只落在这条图层，作用域结束自动触发内容更新。

### 6.4 运行时侧：不走 Edit Layer，靠 Manager 维护图层栈

运行时层（`ACSRuntimeLandscapeLayer`）**不调用 `CreateLayer`**，也不进 Merge 管线。
「添加图层」体现在 Manager 持有的数组里：

```
ACSRuntimeLandscapeLayerManager:
    AddLayer(Layer)     → ActiveLayers.Add(Layer)     注册一层
    RemoveLayer(Layer)  → ActiveLayers.Remove(Layer)
    RunAllLayers()      → 依序跑各层 FullRuntimePipeline，多层结果叠加
    ClearAllLayers()
```

每层自身的流程是 `ReadLandscapeData → GenerateLayerData → BlendLayer → CommitToLandscape`，
全程在 `Tick` 里用 RDG 调度 `LandscapeLayer.usf :: CSRuntimeLandscapeLayerFunction`，
与编辑器侧的 Edit Layer 注册机制无关。

## 7. 重构实施记录（编辑器三层，已完成）

> 范围：仅 `PCGEditorProcess` 内的编辑器三层
> （`ACSLandscape`/`ACSLandscapeRiver`、`ACSLandscapeTempLayer`、`ACSLandscapeLayer`）。
> 运行时层（`ACSRuntimeLandscapeLayer`）未动——它在另一模块、不走 Edit Layer，机制无重叠。

### 7.1 关键依赖约束

模块依赖单向：`PCGEditorProcess` → `ComputeShaderGenerator` → `GeometryScriptExtraEditor`。
所有「下沉到公共层」的类型必须放在被依赖方，编辑器三层才能共享：

```
共享结构 / 单一枚举   放 ComputeShaderGenerator（或 PCGPluginsShared 头）
                       └─ 编辑器三层可见；不反向依赖编辑器模块
基类 ACSLandscapeEditLayerBase   留在 PCGEditorProcess（只服务编辑器三层）
```

### 7.2 三项改造（均已落地）

| # | 改造 | 结果 |
|---|---|---|
| A | 合并数据结构 | 已删 `FReadLandscapeData`，全局统一 `FCSReadLandscapeData`；`ULandscapeExtra::CreateLandscapeTextureData` 直接产出它 |
| B | 单一 BlendMode | 已删 `ETempLayerBlendMode` / `ELandscapeLayerBlendMode`，统一为 `ECSLandscapeBlendMode`（`Alpha=0/Override=1/Additive=2/Subtract=3/Multiply=4/MaterialDrive=5`），下沉到 ComputeShaderGenerator，`.usf` 整数分支已对齐 |
| C | 公共基类 | 已抽出 `ACSLandscapeEditLayerBase`，三层继承之，子类只保留 `RenderLayer` 与自身生成/混合逻辑 |

### 7.3 基类 `ACSLandscapeEditLayerBase` 实际形态

```
ACSLandscapeEditLayerBase  (UCLASS(Abstract)，继承 AActor + ILandscapeEditLayerRenderer)
  承载（不再各写一遍）：
    · OwnedEditLayerGuid / FindLandscape / EnsureEditLayer / RemoveEditLayer
    · RequestLandscapeUpdate
    · BakeResultToLandscape（FScopedSetLandscapeEditingLayer + SetHeightData，统一提交到持久层 index 0）
    · 生命周期清理（EndPlay(Destroyed) / Destroyed 自动 RemoveEditLayer）
    · 四个 renderer 接口默认实现：GetEditLayerRendererDebugName / GetRendererStateInfo
      / GetRenderItems / GetRenderFlags
    · bHasResult 状态位
    · friend class UCSLandscapeEditLayer（供桥接类按 GUID 反查）
  子类虚钩子（仅两个）：
    · ShouldSupportHeightmap()    默认 true；ACSLandscape 重写为返回 bAffectHeightmap
    · IsLayerEnabledForMerge()    默认随 bHasResult；ACSLandscapeTempLayer 重写为恒 true
  RenderLayer 不在基类实现（各子类的合批贡献不同，留各自重写）。
```

子类落点：

- 三层都直接继承基类。`ACSLandscapeTempLayer`、`ACSLandscapeLayer` 仅保留 `RenderLayer`
  和自身的 RT/生成/混合逻辑。
- `ACSLandscape`/`ACSLandscapeRiver` 含复制粘贴/河床/河流模拟等专有逻辑（`PersistentResult`、
  `ApplyResultToCombined`、`SaveResultToPersistent`/`RestoreResultFromPersistent` 等），
  作为基类子类后这些逻辑保留为子类扩展（不下沉到基类）。

### 7.4 决策落定

1. **基类纳入范围**：全纳入——三层统一继承 `ACSLandscapeEditLayerBase`，河流逻辑留 `ACSLandscape` 子类扩展。
2. **结构体命名**：保留 `FCSReadLandscapeData`（底层模块，下沉成本最低）。
3. **接入机制彻底统一**：三层全部改用 `UCSLandscapeEditLayer` + `OwnedEditLayerGuid`，
   `ACSLandscapeLayer` 旧的 `LayerIndex` 机制已废弃。
4. **BlendMode 枚举序号变化**：合并后序号与旧枚举不同，已存档 Actor 的 BlendMode 值需重设
   （未做数值映射，按重构后语义重新选择即可）。已编译验证通过。