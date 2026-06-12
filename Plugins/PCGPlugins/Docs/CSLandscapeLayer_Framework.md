# Compute Shader 地形图层编辑框架

> 梳理 PCGPlugins 中「基于 Compute Shader 的 Landscape 图层编辑」整套框架的模块关系、
> 数据结构、数据流与已知架构问题。本文为架构说明，不含逐行代码。

## 1. 模块分层

三个 Source 模块自下而上协作：

| 层 | 模块 | 职责 |
|---|---|---|
| 数据读取层 | `GeometryScriptExtraEditor` | 把 Landscape 高度图读成纹理数据（`FReadLandscapeData`） |
| CS 调度层 | `ComputeShaderGenerator` | 底层 `FGlobalShader` 封装、`.usf` 入口、运行时图层 |
| 编辑器图层层 | `PCGEditorProcess` | 编辑器 Edit Layer Actor，接入引擎 Edit Layer Merge 管线 |

```
GeometryScriptExtraEditor   读取：Landscape 高度 -> 纹理
   │  FReadLandscapeData / ULandscapeExtra::CreateLandscapeTextureData
   ▼
ComputeShaderGenerator      调度：FGlobalShader + .usf；运行时图层
   │  FCSReadLandscapeData / FCS* shaders
   ▼
PCGEditorProcess            接入：ILandscapeEditLayerRenderer + UCSLandscapeEditLayer
```

## 2. 图层 Actor（框架核心，四者并列）

| Actor | 模块 | 环境 | 定位 | CS 入口 |
|---|---|---|---|---|
| `ACSLandscape` / `ACSLandscapeRiver` | PCGEditorProcess | 编辑器 | 高度复制粘贴、河床雕刻、河流模拟 | `CSLandscape.usf :: CSLandscapeFunction` |
| `ACSLandscapeTempLayer` | PCGEditorProcess | 编辑器 | 纯非破坏临时改动 | `CSLandscape.usf :: CSLandscapeFunction` |
| `ACSLandscapeLayer` | PCGEditorProcess | 编辑器 | 材质驱动 + 噪声生成混合 | `LandscapeLayer.usf :: CSLandscapeLayerFunction` |
| `ACSRuntimeLandscapeLayer` / `...Manager` | ComputeShaderGenerator | 运行时 | 运行时噪声/侵蚀/河床/自定义高度场，多层叠加 | `LandscapeLayer.usf :: CSRuntimeLandscapeLayerFunction` |

接入方式两条路线：

- 编辑器三层：实现 `ILandscapeEditLayerRenderer`，统一经
  `UCSLandscapeEditLayer`（继承 `ULandscapeEditLayerProcedural`）挂进引擎 Edit Layer Merge 管线。
- 运行时层：继承 `ACSRangeGenerator`，靠 `Tick` 驱动，不走 Edit Layer。

## 3. 统一数据流（编辑器三层共享）

```
[1] 读取    Landscape 高度 -> FReadLandscapeData（纹理 + UV/世界范围 + Transform）
              ULandscapeExtra::CreateLandscapeTextureData
      │
[2] 生成    按 Source/Generation 模式产出目标高度/Alpha 到 RT
              Flat / 噪声 / 噪声+侵蚀 / 外部RT / 材质纹理驱动
      │
[3] 混合    CS：原始高度 × 目标高度 × Alpha × 衰减(Falloff)，按 BlendMode 输出 RT_Result
      │
[4a] 预览   写入 Merge 管线 scratch RT（RenderLayer 回调中 ApplyResultToCombined）
              非破坏，删 Actor 即还原
      │
[4b] 提交   CommitToLandscape：FScopedSetLandscapeEditingLayer 把 RT_Result
              永久烘进高度图，再 RequestLayersContentUpdate
```

关键状态位：

- `bHasResult` / `bHasLayerResult` / `bHasRuntimeResult`：控制 `RenderLayer` 是否参与合批。
- `OwnedEditLayerGuid`：绑定该 Actor 在 Landscape 上独占的 Edit Layer。
- 编辑器层用 `PersistentResult`(UTexture2D) 序列化结果，跨存档保留。

## 4. 底层 CS 资产（地形相关仅 2 个 .usf）

| .usf | 入口 | 使用者 |
|---|---|---|
| `CSLandscape.usf` | `CSLandscapeFunction` | `ACSLandscape`、`ACSLandscapeTempLayer` 共用 |
| `LandscapeLayer.usf` | `CSLandscapeLayerFunction` | `ACSLandscapeLayer`（编辑器） |
| `LandscapeLayer.usf` | `CSRuntimeLandscapeLayerFunction` | `ACSRuntimeLandscapeLayer`（运行时） |

数据载体：`FReadLandscapeData`（编辑器侧，GeometryScriptExtraEditor）与
`FCSReadLandscapeData`（运行时侧，ComputeShaderGenerator）为同一概念的两份定义。

## 5. 已知架构问题

1. **四 Actor 高度重复**：`RenderLayer` / `GetRendererStateInfo` / `GetRenderItems` /
   `EnsureEditLayer` / `CommitToLandscape` 这套 Edit Layer 接入逻辑在三个编辑器 Actor 中平行复制，
   差异只在「生成」与「BlendMode」。
2. **数据结构分裂**：`FReadLandscapeData` 与 `FCSReadLandscapeData` 字段几乎一致却各存一份，
   跨模块需重复转换。
3. **BlendMode 枚举重复三套**：`ETempLayerBlendMode` / `ELandscapeLayerBlendMode` /
   `ERuntimeLayerBlendMode`，语义大量重叠。

## 6. CS 里如何「添加 landscape layer」

这里的「添加图层」分两件事：① 在 Landscape 上**新建一条 Edit Layer 槽位**（注册阶段）；
② 让 CS 算出的结果**进入这条图层**（写入阶段）。编辑器三层和运行时层走的是两条完全不同的路。

### 6.1 编辑器侧：新建 Edit Layer 槽位

调用入口都是 `ALandscape::CreateLayer`，区别只在是否绑定自定义 EditLayer 类：

```
ACSLandscape / ACSLandscapeTempLayer
   Landscape->CreateLayer(LayerName, UCSLandscapeEditLayer::StaticClass())
       └─ 绑定自定义类，使该层能反向找回拥有它的 Actor

ACSLandscapeLayer
   Landscape->CreateLayer(UsedLayerName)
       └─ 普通图层（不绑定自定义类）
```

通用步骤（以 `ACSLandscape::EnsureEditLayer` 为代表）：

```
[1] FindLandscape()                         拿到世界里的 ALandscape
[2] 若 OwnedEditLayerGuid 已有效且仍能 GetLayerIndex 命中 → 直接复用，跳过创建
       否则 Invalidate，准备重建（防止存档回来后 GUID 失效）
[3] LayerName = "CS_<ActorName>"            每个 Actor 独占一条命名图层
[4] NewIdx = Landscape->CreateLayer(LayerName, UCSLandscapeEditLayer::StaticClass())
[5] OwnedEditLayerGuid = GetLayerConst(NewIdx)->EditLayer->GetGuid()
       记录 GUID，作为该 Actor 与这条图层的唯一绑定
```

- 命名策略：`ACSLandscape` 用 `CS_<ActorName>`；`ACSLandscapeLayer` 在未指定 `LayerName` 时
  退化为 `PCG_TempLayer_<现有图层数>`。
- `ACSLandscapeLayer` 额外用 `bLayerCreated` / `LayerIndex` / `LayerGuid` 三个状态记账，
  并提供 `RemoveLandscapeLayer`（`Landscape->DeleteLayer(LayerIndex)`）做配对清理。

### 6.2 自定义类的作用：让 Merge 管线找回 Actor

`UCSLandscapeEditLayer`（继承 `ULandscapeEditLayerProcedural`）是「图层」与「Actor」之间的桥。
引擎在合批时调用 `GetEditLayerRendererStates`，它按 **GUID 反查**对应 Actor：

```
GetEditLayerRendererStates(MergeContext):
    MyGuid = GetGuid()                       当前这条 Edit Layer 的 GUID
    遍历世界中的 ACSLandscape / ACSLandscapeLayer / ACSLandscapeTempLayer：
        若 Actor->OwnedEditLayerGuid == MyGuid:
            把 Actor（ILandscapeEditLayerRenderer）登记为该层的 Renderer
    返回 States    →  引擎随后回调 Actor 的 RenderLayer 把 CS 结果写进合批 RT
```

即：CreateLayer 建槽 → 记录 GUID → 自定义类按 GUID 把 Actor 挂回管线，
之后 `RenderLayer` 才有机会把 CS 输出注入这条图层。

### 6.3 写入图层：预览 vs 提交

- **预览（非破坏）**：置 `bHasResult/bHasLayerResult=true` → `RequestLandscapeUpdate`
  触发 `RequestLayersContentUpdateForceAll`，引擎重跑 Merge，在 `RenderLayer` 回调里把
  CS 结果叠进该 Edit Layer 的 scratch RT。删 Actor 即还原。
- **提交（永久）**：`CommitToLandscape` 把 RT 读回 CPU 高度数据后，用
  `FScopedSetLandscapeEditingLayer(Landscape, TargetLayerGuid, ...)` 把当前编辑目标
  切到这条图层，再 `LandscapeEdit.SetHeightData(...)` 把高度永久烘进去：

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

## 7. 重构方案（编辑器三层，排除运行时）

> 范围已锁定：仅 `PCGEditorProcess` 内的编辑器三层
> （`ACSLandscape`/`ACSLandscapeRiver`、`ACSLandscapeTempLayer`、`ACSLandscapeLayer`）。
> 运行时层（`ACSRuntimeLandscapeLayer`）本轮不动——它在另一模块、不走 Edit Layer，
> 机制无重叠，强行并入只会增加耦合。

### 7.1 关键依赖约束

模块依赖单向：`PCGEditorProcess` → `ComputeShaderGenerator` → `GeometryScriptExtraEditor`。
所有「下沉到公共层」的类型必须放在被依赖方，编辑器三层才能共享：

```
共享结构 / 单一枚举   放 ComputeShaderGenerator（或 PCGPluginsShared 头）
                       └─ 编辑器三层可见；不反向依赖编辑器模块
基类 ACSLandscapeEditLayerBase   留在 PCGEditorProcess（只服务编辑器三层）
```

### 7.2 三项改造

| # | 改造 | 现状 | 目标 |
|---|---|---|---|
| A | 合并数据结构 | `FReadLandscapeData`（GeometryScriptExtraEditor）与 `FCSReadLandscapeData`（ComputeShaderGenerator）13 字段逐字相同 | 保留 `FCSReadLandscapeData` 一份，删 `FReadLandscapeData`；`ULandscapeExtra::CreateLandscapeTextureData` 改用前者 |
| B | 单一 BlendMode | `ETempLayerBlendMode` / `ELandscapeLayerBlendMode` 两套（运行时第三套本轮不动） | 收敛为一个枚举，取并集 `Alpha / Override / Additive / Subtract / Multiply / MaterialDrive`，下沉到共享层 |
| C | 公共基类 | 三层平行复制 Edit Layer 接入 + Commit + RT | 抽 `ACSLandscapeEditLayerBase`，子类只重写「生成」「混合」钩子 |

### 7.3 基类 `ACSLandscapeEditLayerBase` 形态

```
ACSLandscapeEditLayerBase  (继承 AActor + ILandscapeEditLayerRenderer)
  承载（不再各写一遍）：
    · OwnedEditLayerGuid / FindLandscape / EnsureEditLayer / RemoveEditLayer
    · CommitToLandscape（FScopedSetLandscapeEditingLayer + SetHeightData 流程）
    · RT 管理（EnsureRTs / 尺寸对齐 / 预览写入合批 RT）
    · 四个 renderer 接口默认实现：GetRendererStateInfo / GetRenderItems
      / GetRenderFlags / RenderLayer（RenderLayer 内部调用下方钩子）
  子类只重写两个钩子：
    · GenerateTargetHeight()   生成阶段：产出目标高度/Alpha 到 RT
    · BlendIntoResult()        混合阶段：按 BlendMode 输出 RT_Result
```

子类落点：

- `ACSLandscapeTempLayer`、`ACSLandscapeLayer` 直接继承基类，仅实现两钩子。
- `ACSLandscape`/`ACSLandscapeRiver` 含复制粘贴/河床/河流模拟等专有逻辑，
  作为基类子类后这些逻辑保留为子类扩展（不下沉到基类）。

### 7.4 待定决策点

1. **基类是否纳入 `ACSLandscape` 体系**：全纳入（河流逻辑留子类）/ 仅纳入较纯的两层 /
   全纳入并重排继承链。影响改动面与回归风险。
2. **结构体保留命名**：建议保留 `FCSReadLandscapeData`（在底层模块，下沉成本最低）。
3. **BlendMode 旧枚举值兼容**：合并后枚举序号变化会影响已存档 Actor，
   需确认是否要做数值映射或可接受重设。