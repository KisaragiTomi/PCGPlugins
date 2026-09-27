# GPU 三角形 Buffer 统一评估

[返回文档索引](index.md)

让项目里的 GPU 三角形产出（`meshgenerator` 的 direct mesh、`road`、`vine`）共用同一条
「GPU 三角形 → StaticMesh」的 readback / 保存路径，而不是给每条产出各写一套兼容接口。

状态：**direct + road 已统一并落地**（见 [实现结果](#实现结果已实施)）；`vine` 待叶子化后接入。buffer 集做成
**描述符驱动、可扩展**——后续加 buffer 不用改 readback/save。下文保留评估过程作为背景，实际落地以「实现结果」为准。

第二轮规划（全生产者 readback 统一 + 双 sink + vine 叶子收尾，含对抗评审修正）见
[`Plugins/PCGPlugins/Docs/GpuTriangleUnified_Plan.md`](../Plugins/PCGPlugins/Docs/GpuTriangleUnified_Plan.md)。

## 结论速览

- `meshgenerator` **已经有**提交 GPU 三角形到渲染管线的方法：`AComputeShaderMeshGenerator::SubmitBoxSceneTrianglesToRenderPipeline`（`ComputeShaderMeshGenerator.h:791`）。它走的 `UCSDirectTriangleMeshComponent` 和 `road` 的 `URoadMeshComponent` **共用同一个基类** `UCSGpuMeshComponent` / `FCSGpuMeshSceneProxy`，buffer 布局字节一致。所以「把 road 的渲染方法弄过来」这件事在渲染侧已经做完了，不需要再搬。
- 真正没统一的是 **readback → `StaticMesh`** 那一段：它现在只挂在 `CSDirect` 这个叶子上（`ReadbackMeshSync` → `SaveDirectGPUMeshToStaticMesh`），`road` 和 `vine` 用不了。
- 统一是「把 buffer 布局和 readback 下沉到基类」而非「加兼容接口」——两个现存叶子布局已经一致，这条路可行。
- 难度分三档：`CSDirect` 已完成（参考实现）；`road` 低成本（同布局，缺 readback + soup/indexed 计数差异）；`vine` 高成本（还没有 GPU-resident 叶子，buffer 布局分叉，CPU 重算法线）。

## 实现结果（已实施）

状态：**已落地并通过验证**——编译通过；`DirectGPUMesh.SaveStaticMesh` 自动化测试 `Result={Success}`（走新链路存出 144 顶点 / 48 三角形，本地空间还原）；对角评审 0 confirmed bug。改动横跨 `ComputeShaderGenerator` + `PCGEditorProcess` 两模块与两个 `.usf`。

- **描述符驱动的可扩展 buffer 集**：每个 stream 用一条 `FCSGpuStreamDesc`（`CSGpuMeshTypes.h`）描述 —— role / 元素格式 / 每顶点元素数 / `CountSource`（PerVertex·PerIndex·Fixed）/ VF 绑定 / SRV 格式 / readback 语义。基类持 `TArray<TUniquePtr<FCSGpuStreamRuntime>>`，按描述符统一分配、绑 VF、回读。**加一个新 buffer = 叶子 `RegisterStreams()` 里多一次 `AddStream(...)`**，alloc / VF 绑定 / readback / save 一行不用改（本次核心诉求）。
- **下沉到基类**：buffer 分配、VF 绑定、拆卸、整条 CPU readback 全在 `FCSGpuMeshSceneProxy` / `UCSGpuMeshComponent`；叶子只实现 `RegisterStreams()` + `BuildGeometry()` 两个纯虚。`InitGpuGeometry`/`ReleaseGpuGeometry` 降级为基类编排器，`CreateRenderThreadResources` 调用顺序不变；拆卸仍是 VF 先于 buffer。
- **计数解耦**：新增基类持有的 2-uint `MeshCounters`（`[0]=vertexCount [1]=indexCount`），与 5-uint `IndirectArgs` 分开。soup（`CSDirectMesh.usf` `BuildIndirectArgsCS`）写 `V==I`；road（`RoadBuilder.usf` `FinalizeCS`）持久化本就由 `AllocStrip` 累加的 `RWCounters[0]/[1]`。readback 顶点流按 `vertexCount`、索引流按 `indexCount` 分别定长。
- **共享存盘**：`CSGpuMeshSave::SaveGpuMeshComponentToStaticMesh`（editor-only，吃通用 `UCSGpuMeshComponent*` + material + transform）。`AComputeShaderMeshGenerator::SaveDirectGPUMeshToStaticMesh` 变薄封装；road 新增 `URoadMeshComponent::SaveToStaticMesh`。`FCSDirectMeshCPUData` → `FCSGpuMeshCPUData`。
- **空间约定**：direct 存世界空间 soup（`bConvertToActorLocalSpace=true`）；road 存组件本地空间（默认 `false`）。
- 编译期唯一坑：MSVC 给 move-only registry 合成拷贝赋值 → 显式 `= delete` 基类拷贝操作。

## 数据流图

三条产出 → 各自叶子 SceneProxy → 共享基类 → 一处 readback/存盘 → `UStaticMesh` 的整体关系见
[`Plugins/PCGPlugins/Docs/GpuTriangleBuffer_Unification_Flow.svg`](../Plugins/PCGPlugins/Docs/GpuTriangleBuffer_Unification_Flow.svg)。
图中青色=共享核心（基类持有的 buffer 集 + readback + 存盘，现已下沉落地），橙色虚线=`vine` 缺口（待叶子化）。

## 三条产出路径现状

| 产出 | 组件 / 代理 | 模块 | 是否继承基类 | 有 readback/save 吗 |
| --- | --- | --- | --- | --- |
| meshgenerator direct mesh | `UCSDirectTriangleMeshComponent` / `FCSDirectTriangleMeshSceneProxy` | `ComputeShaderGenerator` | 是 | **有**（参考实现） |
| road | `URoadMeshComponent` / `FRoadMeshSceneProxy` | `PCGEditorProcess` | 是 | 无 |
| vine | 无 GPU-resident 叶子；走 `UDynamicMeshComponent` | `GeometryScriptExtraEditor` | 否 | 走 CPU 全量回读 → `UDynamicMesh` |

- 基类：`FCSGpuMeshSceneProxy`（`CSGpuMeshSceneProxy.h:24`）拥有 `FLocalVertexFactory`、`FPooledVertexBuffer`/`FPooledIndexBuffer` wrapper、`FDrawDesc`（`:70`，选择 direct draw 还是 `DrawIndexedIndirect`）。**pooled buffer 由各叶子自己持有**，基类不持有；readback 也不在基类。
- `CSDirect` 的存 mesh 链路：`SaveDirectGPUMeshToStaticMesh`（`ComputeShaderMeshGenerator.cpp:4242`）→ `UCSDirectTriangleMeshComponent::ReadbackMeshSync`（`CSDirectTriangleMeshComponent.cpp:50`）→ `BuildDirectGPUMeshDescription`（`ComputeShaderMeshGenerator.cpp:284`）→ `UE::AssetUtils::CreateStaticMeshAsset`。

## Buffer 布局对比

| Buffer | road / CSDirect（GPU-resident 叶子） | vine（UDynamicMesh 路径） |
| --- | --- | --- |
| Positions | `Buffer<float>`，`N*3` float，stride 12，SRV `PF_R32_FLOAT` | `StructuredBuffer<float4>`，`N` × `FVector4f`（xyz, w=1） |
| Tangents/Normals | `Buffer<uint>`，`N*2` uint，`PF_R8G8B8A8_SNORM`，2× `VET_PackedNormal` | GPU 上无 —— CPU `RecomputeNormals` |
| TexCoords | `Buffer<float>`，`N*2` float，`PF_G32R32F` | `StructuredBuffer<float2>` |
| Colors | `Buffer<uint>`，`N` uint，`PF_R8G8B8A8` | 无 |
| Indices | `Buffer<uint>` + `IndexBuffer` usage | `StructuredBuffer<uint>` |
| IndirectArgs | `CreateIndirectDesc(uint32, 5)` — `DrawIndexedIndirect` | 无 |
| 计数来源 | GPU 决定（`Args[0]` 经 indirect 回读） | CPU 已知（`PathPointCount*ProfileCount`） |
| 持久性 | 持久 pooled buffer，每帧绘制 | 瞬态 RDG buffer，回读一次即丢 |

- road / CSDirect 布局字节一致，唯一差别：`CSDirect` 是 triangle soup，index buffer 用 identity 且按 `MaxVertices` 分配；`road` 是正经 indexed mesh，顶点按 `MaxVertices`、索引按独立的 `MaxIndices` 分配（`RoadMeshSceneProxy.cpp:28-49`）。
- vine 的 GPU 输出是三个瞬态 `StructuredBuffer`（`GeometryEditorActor.cpp:3480-3482`：`float4` 顶点 / `float2` UV / `uint` 索引），没有 tangent、color、indirect args；生成后立刻 `FRHIGPUBufferReadback` 全量回读（`:3810-3849`），CPU 端 `BuildDynamicMeshFromGPUVineOutput`（`:4152`）建 `UDynamicMesh` 并 CPU 重算法线。

## 计数模型：soup vs indexed（关键坑）

`DrawIndexedIndirect` 的 5 个 uint = `[IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation]`，`Args[0]` 是 **index 数，没有 vertex 数**。

- `CSDirect` 是 triangle soup：每顶点唯一、identity index，所以 `vertexCount == indexCount == Args[0]`。`EnqueueMeshReadback` 用**同一个** `VertexCount` 同时读 position 和 index（`CSDirectTriangleMeshSceneProxy.cpp:116-119`）。
- `road` 是 indexed mesh：`V ≠ I`，且 GPU 真实顶点数不在 indirect args 里。想精确回读顶点流，要么额外读一个 vertex counter，要么读满 `MaxVertices` capacity（会带出未引用顶点）。
- 所以 `CSDirect` 现有的 readback 是 **soup 专用**，不能原样复用到 road。统一 readback 必须把「顶点流长度」和「索引流长度」解耦。

## 统一难度分档

| 产出 | 难度 | 主要缺口 |
| --- | --- | --- |
| CSDirect | 已完成 | 无（参考实现） |
| road | 低—中 | 缺基类 readback；soup/indexed 计数差异；indexed 需要 vertex counter |
| vine | 高 | 还没有 GPU-resident 叶子；buffer 布局分叉；CPU 重算法线需补 GPU 切线 pass |

## 落地方案（已实施，对照原计划）

1. ~~把标准 6-buffer 布局提到基类~~ → 实际做成**描述符驱动的 buffer 集**（`AddStandardTriangleStreams()` 注册标准 7 项：Position/Tangent/TexCoord/Color/Index/IndirectArgs + MeshCounters），直接在 `FCSGpuMeshSceneProxy` 上，未另设中间类。比「固定 6-buffer」更可扩展。
2. `ReadbackMeshSync` + `EnqueueCountersReadback` / `EnqueueMeshReadback` 提到基类，产出 `FCSGpuMeshCPUData`。计数解耦落实为**独立 2-uint `MeshCounters` buffer**（非「args 第 6 个 slot」）：soup 写 `V==I`，road 的 `FinalizeCS` 持久化已有的 `RWCounters[0]/[1]`。
3. `SaveDirectGPUMeshToStaticMesh` 核心抽成 `CSGpuMeshSave::SaveGpuMeshComponentToStaticMesh`（吃通用组件 + material + 空间 flag），`BuildDirectGPUMeshDescription` → `CSGpuMeshSave::BuildGpuMeshDescription`。direct 与 road 各自的 owner 调它，一处实现。
4. vine（**未做，后续**）：给它一个继承基类的 `UVineGpuMeshComponent` 叶子（见 mem `vinegenerate-gpu-task` / `gpu-resident-mesh-rendering`），把 `float4` 结构化 buffer + CPU 重算法线换成标准 stream 集 + GPU 切线/法线 pass，即可免费拿到 save-to-StaticMesh。

## 坑清单

1. **soup(1 计数) vs indexed(2 计数)**：indirect args 只带 index 数，indexed mesh（road）需要额外 vertex counter 才能精确回读顶点流。road 的主要坑。**已定：road 要存盘，故此项为必做项**（build pass 需写入顶点数）。
2. **vine 是最深的坑**：没有 GPU-resident 叶子，buffer 布局分叉（`float4`、无 tangent/color/indirect），且靠 CPU 重算法线——上基类前需要先补 GPU 切线/法线 pass。好在这已是排期中的独立任务。
3. **空间约定**：`CSDirect` 是世界空间 + absolute identity transform；`road`/`vine` 是局部空间 + 正常 `LocalToWorld`。save 的 `bConvertToActorLocalSpace` 转换必须按叶子区分，否则存出的 mesh 位置错。
4. **下沉改动面**：buffer 下沉会同时动到 `road` + `CSDirect` 两个现存叶子的 `InitGpuGeometry`，范围可控（只有 2 个），但要回归两者渲染。
5. **单材质槽假设**：当前 save 只建 1 个 material slot（`ComputeShaderMeshGenerator.cpp:4313-4314`），多 section / 多材质 mesh 不支持。
6. **基类引入 readback 依赖**：下沉会把 `FRHIGPUBufferReadback` 依赖引入基类（模块内已有，无新增外部依赖）。

落地状态：坑 1（计数解耦 + `MeshCounters`）、3（空间约定按叶子区分）、4（两叶子回归，direct 已过自动化测试）、6 均已在实现中处理；坑 2（vine）、5（多材质）仍为限制项。

对角评审复核出的两个「潜在但非现网」项（均判为可接受，无需改）：

7. **road 溢出时的顶点尾巴**：若 road 几何超过保守的 `MaxVertices` 估算，`AllocStrip` 会在容量检查前自增 `RWCounters[0]`，`FinalizeCS` 报 `vertexCount=MaxVertices`，readback 带出未写入的尾部顶点（未初始化 VRAM）。**内存安全、输出正确**：`ClearCS` 清零 index buffer，被拒绝的 strip 没写索引，那些垃圾顶点只被退化 `(0,0,0)` 三角形引用 → `BuildGpuMeshDescription` 跳过、StaticMesh 构建期压缩掉。保守估算下溢出本不该发生。要更保险可在 `ClearCS` 一并清零顶点流。
8. **未来「半语义」叶子**：若以后某叶子只注册部分 readback 语义（如只要 Position+Index、省掉 TangentBasis/TexCoord）再存盘，那几个 CPU 数组会 `SetNumUninitialized` 但不填，`IsValid()` 只查长度会放行 → 存出垃圾法线/UV。当前两叶子都用 `AddStandardTriangleStreams`（四语义齐全），**不可复现**；且 `CSGpuMeshSave` 会把非有限法线换成面法线、重算切线、清零非有限 UV，能吸收大部分。给未来半语义叶子加保护时，可让 save 前校验语义齐全，或对缺失语义开 `bEnableRecomputeNormals/Tangents`。

## 已定方向

- `road` 也要能存成 `StaticMesh`（不只共用基类布局）→ 为 road 补 vertex counter 是必做项（坑 1），readback 下沉到基类。

## Open Questions

- vine 叶子化排期是否与本次统一合并推进。
- road 存盘目前仅 build + 评审验证，尚未跑过一次真机 road save（direct 已过自动化测试）；是否补一个确定性 road fixture 的自动化测试。
- 现状已知限制：vertex color 不回读、只存单套 UV、单材质槽（见坑 5/8）。若要把 color / 多 UV 存进 StaticMesh，按可扩展设计加对应 stream 语义 + `FCSGpuMeshCPUData` 成员即可。
