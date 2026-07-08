# EnhancedHairCards Groom Card 路径支持源 StaticMesh 顶点色与多 UV 方案

目标：`UEnhancedHairCardsComponent` 走 groom card 路径（`bUseSourceGroomSimulation` + 有效 `SourceGroomComponent`）渲染时，材质能拿到源 StaticMesh（cards 描述里的 `ImportedMesh` / `Part.SourceMesh`）的顶点色与全部 UV 通道，且 TexCoord 语义与 StaticMesh 路径一致。

引擎：源码构建 UE 5.7.4（`D:/UnrealEngine-5.7.4-release`）。

## 现状与数据流

`FEnhancedHairCardsSceneProxy` 有两条互斥的数据路径：

| | StaticMesh 路径 | Groom Card 路径 |
| --- | --- | --- |
| 触发条件 | `bUsingSourceGroomCardsResource == false` | `BindSourceGroomCardsResource` 绑定成功（[EnhancedHairCardsSceneProxy.cpp:424](Plugins/EnhancedHairCards/Source/EnhancedHairCards/Private/EnhancedHairCardsSceneProxy.cpp:424)） |
| 位置/法线 | 自建缓冲，CPU guide 变形 | 引擎 `FHairCardsDeformedResource`（groom 模拟驱动） |
| UV 来源 | `SourceMesh` RenderData LOD0，槽位 `0..N-1` = 源 UV0..UV(N-1)，N 取 `NumUVChannels`（≤8） | `FHairCardsRestResource::UVsBuffer`，槽位 0 = RootUV，槽位 `1..N` = 源 UV0..UV(N-1)（`HAIR_CARD_UV_ROOTUV=0`，`HAIR_CARD_UV_CARDUV=1`） |
| 顶点色 | `ColorVertexBuffer` 打包 `FColor`/`PF_R8G8B8A8` 上传；mesh 无顶点色时关闭 0x80 flag，shader 输出白 | `Header.bVertexColor` 时绑 `VertexColorsBuffer`；否则 `MaterialsBuffer` 仅作占位绑定，flag 关闭后 shader 不取样、输出白 |
| 顶点数/顺序 | RenderData 顶点（构建时分裂/重排） | groom bulk 顶点 = ImportedMesh **MeshDescription** 的 VertexID 顺序 |

引擎侧结论（已核对 5.7.4 源码，`HairCardsBuilder.cpp` `InternalImportGeometry_WithImportedGuides`，约 950 行起）：

- cards 导入**已支持**多 UV 与顶点色：`Header.NumUVs = 源UV数 + 1`（+1 是 RootUV），`Header.bVertexColor` 取决于 MeshDescription 是否带 Color 属性；每顶点属性取该顶点第 0 个 vertex instance（“Assume no actual duplicated data”）。
- 缓冲格式：UVs 为 float4（每元素打包 2 套 UV），VertexColors 为 `PF_R8G8B8A8`；插件 shader 以 `Buffer<float4>` 读取均兼容。
- bulk 是构建缓存（DDC）。旧引擎/旧版本构建的 groom 资产可能没有顶点色或只有部分 UV，需要触发 cards 重建才会带上。

### 已修复（顶点色部分）

1. **顶点色回退错误**：groom 路径 `bVertexColor == false` 时曾绑 `MaterialsBuffer` 当颜色读（乱码）。现由 `FEnhancedHairCardsVertexBuffers::bHasVertexColor` 驱动 0x80 flag，shader 不取样、输出白；占位 SRV 仅保证绑定合法。
2. **SM 路径 R/B 互换**：曾以 RGBA 顺序 float4 上传顶点色，而 shader 的 `FMANUALFETCH_COLOR_COMPONENT_SWIZZLE` 在所有平台均为 `.bgra`，导致 R/B 对调。现改为与引擎 `FHairCardsRestResource` 相同的 `FColor`/`PF_R8G8B8A8` 打包（`PackColorSRV`），swizzle 后通道正确，且每顶点从 16B 降到 4B。
3. **0x80 恒置位**：`FEnhancedHairCardsSettings::PackFlags` 不再恒置 `EHairCardsVFlags_VertexColor`，由 `CreateEnhancedHairCardsVFUniformBuffer` 按实际缓冲状态置位。

改动文件：`EnhancedHairCardsVertexFactory.h`（`bHasVertexColor`）、`EnhancedHairCardsDatas.h`（`PackFlags`）、`EnhancedHairCardsVertexFactory.cpp`（flag 置位）、`EnhancedHairCardsSceneProxy.cpp`（两条路径的绑定与 `PackColorSRV` 上传）、`EnhancedHairCardsVertexFactory.ush`（flag 门控取色，默认白）。

### 已修复（UV 部分）

统一为"材质 TexCoord[k] = 源 mesh UV[k]"，groom / SM 两条路径一致。经 4 维度多智能体对拍验证（对照 5.7.4 引擎源码），修掉一个步长边界 bug 后无残留缺陷。

1. **UV 槽位语义统一**：新增 `UVSlotOffset`（groom=1 跳过 RootUV、SM=0）与 `NumUVSlots`（缓冲实际槽位数=步长基准）。`GetTexCoords` 用 `Slot = UVSlotOffset + CoordIndex`、fetch `float4[Slot>>1]` 的 `(Slot&1)==0 ? .xy : .zw`——经推导与引擎自身 reader（`ReadCoordIndex = CoordIndex+1` + 反相 `.zw/.xy`）对 groom 与 SM 缓冲布局逐槽等价。
2. **`GetRootUVsAtlasUVs` 统一**：RootUV 仅 groom（slot 0）有值、SM 路径为 0；AtlasUV 两路统一 = 源 UV0（slot `UVSlotOffset`）。
3. **PS 端覆盖改兜底**：无条件 `SetUV(0, HairPrimitiveUV)` 改为 `#elif`——材质声明顶点 UV 时 TexCoord 全走纯源 UV（不含 AtlasV 反转），仅材质无顶点 UV 时用 AtlasUV 兜底 slot 0。
4. **`NumUVChannels` 跟随源 mesh**：转换器 `GetSourceMeshNumUVChannels` 从 `ImportedMesh` LOD0 `GetNumTexCoords()` 读取（clamp 1..8），替换写死的 2；并新增 `ValidateGroomCardsAttributes`，cards bulk 的 UV 数/顶点色少于源 mesh 时输出重建提示。
5. **步长 clamp 边界 bug（对拍发现并修复）**：`NumUVSlots` 是 shader 每顶点 UV fetch 步长的除数，最初误 clamp 到 `ENHANCED_HAIR_CARDS_MAX_UV(8)`。当源 mesh 有 8 个 UV 通道时 `Header.NumUVs = 9`，引擎按步长 `ceil(9/2)=5` 打包，clamp 后 shader 用 `ceil(8/2)=4` → 除第 0 顶点外全部读错 float4、并丢第 8 通道。修复：`NumUVSlots` 取真实 `Header.NumUVs`（不下 clamp），uniform 侧 clamp 上限放宽到 `MAX_UV+1`（含 RootUV 槽）；仅 `NumUVs`（材质暴露的源 UV 数）仍 cap 到 8。

改动文件：`EnhancedHairCardsVertexFactory.h`（`NumUVSlots`/`UVSlotOffset`）、`EnhancedHairCardsVertexFactory.cpp`（uniform 装配 + 步长 clamp）、`EnhancedHairCardsSceneProxy.cpp`（两路 UV 槽位设置）、`EnhancedHairCardsVertexFactory.ush`（`GetTexCoords`/`GetRootUVsAtlasUVs`/PS 兜底）、`EnhancedHairCardsConverterLibrary.cpp`（UV 数读取 + 校验）。

## 方案对比

| 方案 | 思路 | 优点 | 缺点 |
| --- | --- | --- | --- |
| A（推荐） | 沿用引擎 groom cards bulk 数据，插件统一槽位语义 + 修正顶点色回退 | 改动小（shader/proxy/converter），cooked 可用，不加资产体积 | 依赖 groom cards 重建才有旧资产的色/UV；受 instance0 采样限制（UV 接缝丢失） |
| B（二期可选） | 编辑器按 MeshDescription VertexID 顺序烘焙 UV/顶点色进 `FEnhancedHairCardsPart`，运行时替换两个 SRV | 不依赖 groom 重建；布局自定义；可自选属性聚合方式 | 新增序列化数据（每顶点 8B/UV + 4B 色）；mesh 重导入需重烘焙；实现量大 |
| C（不做） | 改引擎 `HairCardsBuilder` | 可获得 RenderData 级接缝精度 | 5.7.4 已支持所需功能，引擎改动维护成本不值 |

推荐先落地 A；只有当出现“groom 资产无法重建 / 需要 UV 接缝精度”时再叠加 B。

## 方案 A 详细设计（已实现）

UV 部分已按下述设计落地并编译通过。与初稿的唯一偏差：PS 端 `SetUV(0, HairPrimitiveUV)` 不是"删除"，而是改成 `#elif`——仅当材质**未声明**顶点 texcoord 时兜底（详见下）。

### Uniform / Shader

`FEnhancedHairCardsVFParameters` 增加槽位偏移与槽位数，`NumUVs` 语义改为**源 UV 数**：

```cpp
SHADER_PARAMETER(uint32, NumUVs)        // 源 mesh UV 通道数（不含 RootUV）
SHADER_PARAMETER(uint32, UVSlotOffset)  // groom 路径 = 1（跳过 RootUV 槽位），SM 路径 = 0
SHADER_PARAMETER(uint32, NumUVSlots)    // 缓冲实际槽位数，用于 fetch stride = ceil(NumUVSlots/2)
```

`EnhancedHairCardsVertexFactory.ush` 改动：

```hlsl
// GetTexCoords: 材质 TexCoord[k] 恒等于源 mesh UV[k]
const uint NumFetchUVs = EnhancedDivRoundUp2(EnhancedHairCardsVF.NumUVSlots);
const uint Slot = EnhancedHairCardsVF.UVSlotOffset + CoordIndex;
if (CoordIndex < EnhancedHairCardsVF.NumUVs)
{
    const float4 PackedUVs = EnhancedHairCardsVF.UVsBuffer[Input.VertexId * NumFetchUVs + (Slot >> 1u)];
    OutTexCoords[CoordIndex] = (Slot & 1u) == 0 ? PackedUVs.xy : PackedUVs.zw;
}

// GetRootUVsAtlasUVs: RootUV 仅 groom 路径有真值；AtlasUV 统一 = 源 UV0
// groom 路径: RootUV = 槽位0.xy；SM 路径: RootUV = 0
// 顶点色: Flags 增加 "无顶点色" 位，置位时直接输出 half4(1,1,1,1)，不再 fetch
```

`VertexFactoryGetInterpolantsVSToPS` 里把原来无条件的 `SetUV(Interpolants, 0, HairPrimitiveUV)` 改为 `#elif NUM_TEX_COORD_INTERPOLATORS`——材质声明了顶点 texcoord 时 TexCoord 全部来自 `GetTexCoords`（纯源 UV，不含 AtlasV 反转），只在材质**没有**顶点 texcoord 时才用 AtlasUV 兜底 slot 0。这样 TexCoord[k] 两条路径一致，同时保留引擎式的安全默认；RootUV 继续经 `HairPrimitiveRootUV`（Hair Attributes 节点）暴露，AtlasV 反转（0x40）仍作用在 `HairPrimitiveUV`（不污染 TexCoord）。

### SceneProxy

`BindSourceGroomCardsResource`：

- `NumUVSlots = BulkData.GetNumUVs()`（真实 `Header.NumUVs`，**不下 clamp 到 8**，它是 shader 步长基准）、`UVSlotOffset = 1`、`NumUVs = clamp(NumUVSlots - 1, 1, 8)`（仅材质暴露的源 UV 数 cap 到 8）。uniform 侧 `NumUVSlots` clamp 上限 `MAX_UV + 1`（含 RootUV 槽）。
- 顶点色：已按"已修复"一节落地（flag 门控 + 占位绑定），UV 改动无需再动。
- SM 路径 `UVSlotOffset = 0`、`NumUVSlots = NumUVs`（≤8），上传逻辑不变。

### 转换器 / 资产

`PopulatePartFromCardDescription`（EnhancedHairCardsConverterLibrary.cpp）：

- `NumUVChannels` 从 `ImportedMesh->GetRenderData()->LODResources[0]` 的 `GetNumTexCoords()` 读取，替换写死的 2。
- 转换时校验 groom cards bulk：`Header.NumUVs - 1` 与源 mesh UV 数不符、或源 mesh 有顶点色但 `bVertexColor == false` 时，输出 Warning 提示重建该 cards LOD（编辑器内修改 CardsSourceDescription 或重导 ImportedMesh 触发；5.7.4 构建器会自动带上顶点色与全部 UV）。

### 兼容性注意

- SM 路径 Hair Attributes 的 RootUV/AtlasUV 语义变化：原先 RootUV = 源 UV0、AtlasUV = 源 UV1，统一后 RootUV = 0、AtlasUV = 源 UV0。依赖旧行为的材质需检查（目前项目内 groom cards 材质主要走 TexCoord，影响面小，落地前用引用查找确认）。
- `bInvertAtlasV`（Flags 0x40）作用对象随 AtlasUV 一起变为源 UV0。
- 引擎导入按顶点 instance0 取 UV/色：同一位置顶点带多套 UV（接缝）会丢失。卡片网格通常每张卡独立成岛，实际影响小；若某资产确实需要接缝，转 B 方案。

## 方案 B 概要（二期备选）

编辑器侧烘焙，运行时替换 groom 路径的 UVs/VertexColors 两个 SRV（位置/法线仍用 groom Deformed 缓冲）：

```cpp
// FEnhancedHairCardsPart 新增字段（编辑器烘焙、随资产序列化）
UPROPERTY() int32 BakedVertexCount = 0;         // 须等于 groom cards bulk 顶点数，否则忽略
UPROPERTY() int32 BakedNumUVChannels = 0;       // 烘焙 UV 通道数
UPROPERTY() TArray<FVector2f> BakedUVs;         // VertexCount * NumUVChannels，槽位0 即源 UV0
UPROPERTY() TArray<FColor> BakedVertexColors;   // VertexCount；空 = 无顶点色
```

关键约束：烘焙遍历必须与引擎导入一致——对 `ImportedMesh` 的 MeshDescription 按 `VertexID` 顺序、每顶点取 instance0（或在此处做更好的多 instance 聚合），保证与 groom 顶点一一对应；mesh 重导入后需重新烘焙（转换器里做顶点数校验，失配即回退方案 A 行为）。

## 验证

1. 同一 groom 资产分别走 SM 路径与 groom 路径，材质用 VertexColor 与 TexCoord0/1/2 直出 BaseColor 对比，两路径渲染一致。
2. 无顶点色的旧 groom 资产：groom 路径 VertexColor 显示纯白（不再是乱码）。
3. Hair Attributes 节点 RootUV 在 groom 路径仍有效（卡片根部渐变图验证）。
4. `NumUVChannels > 2` 的源 mesh：TexCoord2+ 采样正确。

## 开放问题

- SM 路径旧 RootUV/AtlasUV 语义是否有材质在依赖，需要落地前全局排查确认。
- 转换器提示重建后，是否要在插件内直接提供一键重建 groom cards 的按钮（调 `UGroomAsset` 构建接口），还是仅提示手动操作。
