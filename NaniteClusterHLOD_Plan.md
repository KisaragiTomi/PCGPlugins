# Nanite 截面 HLOD

用 Nanite 自带的 cluster 层级给远处的一群大中型物体生成简易 HLOD：每个源网格按一个与切换距离挂钩的误差阈值（CutError）取出一个截面，合并成一份网格。小物体归 foliage instance 管理，不进 HLOD。

2026-09-22 已实现，见 [实现清单](#实现清单)：截面抽取、WP 构建器、关卡内试验台、外部可见性剔除、烘焙成带贴图的 Nanite 静态网格（BakeHLOD）。自动化测试 5/5，测试关卡 `/PCGPlugins/NaniteCutHLOD/L_NaniteCutHLODTest` 多角度截图验收过（见 [画面验收](#画面验收)），但还没在真实的 World Partition 关卡里构建过 HLOD。文中的引擎结论都对照 5.7.4 源码（`D:\UnrealEngine-5.7.4-release`）核实，行号为 2026-09-21～22 的现状；标「推断」或「待核实」的条目还没有实测。

## 结论

- **可行，而且 HLOD 这个用途正好避开了截面抽取最麻烦的两个问题。**
  - 与视角无关：截面只由 CutError 决定，没有任何相机参数，一份几何所有角度共用。
  - 流送压力小：HLOD 要的是粗截面，每个网格最粗的几层在随资产加载、一直常驻的 root 页里。粗截面常常只用到 root 页（推断，由自检计数逐源确认）。
- **切换几乎看不出来。** CutError 按切换距离算，HLOD 出现那一刻的几何就是 Nanite 在那个距离本来会画的样子，误差不超过约 1 像素。
- **外面看不见的整块删掉。** 合并之后再跑一遍外部可见性剔除：从切换距离上的一圈视点看，从来没当过最近面的三角形删除 —— 封闭房间里的东西、蛇形山洞深处、互相穿插的内部面、贴地的底面。测试关卡里封闭后屋 1,644 → 0、隧道深处三个物体全删，洞口与透过门窗看得见的物体留着。
- **最大限制是每个物体的下限。** Nanite 只在单个网格内部简化，最粗到一个根 cluster（≤128 三角形），不跨物体合并。小物体已经归 foliage，进 HLOD 的都是大中型物体，这个下限可以接受。
- **引擎现成的 `Nanite::StreamOutData` 不能直接用**，插件里另写了一套遍历与写出（`UCSNaniteCutOps`），见 [截面抽取算子](#截面抽取算子)。
- **接入 World Partition 走 `UHLODBuilder` 子类**（`UCSNaniteCutHLODBuilder`），引擎构建时直接给出切换距离 `MinVisibleDistance`。

## 范围

| 源 | 处理 |
| --- | --- |
| 开了 Nanite 的 `UStaticMeshComponent` | 按 CutError 取截面，合并进 HLOD |
| 非 foliage 的 ISM / HISM | 每个实例一个源，按实例变换写出 |
| 没有 Nanite 数据的静态网格 | 跳过，状态 `NotNanite`，构建日志里列出；试验台显示 HLOD 时不藏它（HLOD 里没有它的替身） |
| foliage 实例（小物体） | 不进 HLOD，构建器遇到 `UFoliageInstancedStaticMeshComponent` 直接跳过 |
| actor 或组件带排除标签（默认 `NaniteCutHLOD_Exclude`） | 不进 HLOD。构建器与试验台共用 `UCSNaniteCutOps::MakeSourcesFromComponents`，规则不会分叉 |
| 形状保持设为 `Voxelize` 的网格 | 跳过（`Unsupported`）：远处几层是体素，没有三角形 |
| Nanite Assembly、多根资源 | 跳过（`Unsupported`）：部件变换与多根的读取路径没验过 |

## 原理

### 截面与 CutError

Nanite 把每个网格建成一张 cluster 层级图（DAG），没有「LOD 编号」，只有误差。给定一个网格局部空间的阈值 CutError（单位 cm），截面由两条规则决定：

```hlsl
// NaniteStreamOut.usf:456 —— 父级误差比阈值大，才往下走
bShouldVisitChild = StreamOutCutError < HierarchyNodeSlice.MaxParentLODError;

// NaniteStreamOut.usf:559-560 —— 自身误差比阈值小就输出；更细的数据没加载时被迫输出
bool bSmallEnoughToDraw = StreamOutCutError > Cluster.LODError;
bool bVisible = bSmallEnoughToDraw || (Cluster.Flags & NANITE_CLUSTER_FLAG_STREAMING_LEAF);
```

- 根 cluster 组的父级误差是 `1e10`（`ClusterDAG.cpp:842`），所以任何 CutError 至少取到根。层级里的误差存成 f16（`NaniteDataDecode.ush:693-694`），算子把 CutError 钳到 65000 以下，免得连根都进不去。
- CutError 越大截面越粗，粗截面是细截面在同一张图上的合并：2 倍 CutError 的结果就是 Nanite 在 2 倍距离上会画的样子。
- 规则里没有相机参数，截面与视角无关。

### 距离换算 CutError

Nanite 渲染时，透视视图下一个 cluster「够细」的条件是（`NaniteClusterCulling.usf:320-322`、`NaniteShared.cpp:92-94`）：

```text
LODScale = 0.5 × P[1][1] × ViewSizeY / r.Nanite.MaxPixelsPerEdge
够细     ⇔ d > UniformScale × LODError × LODScale        // d ≈ 相机到 cluster 包围球的距离
         ⇔ LODError < d / (LODScale × UniformScale)
```

HLOD 的 CutError 因此取（`UCSNaniteCutOps::CutErrorForScreenError` 给的是除以缩放之前的世界值）：

```text
CutError_local = PixelError × MinVisibleDistance / (LODScale_ref × InstanceMinScale)
```

- `PixelError`：允许的屏幕误差，Nanite 默认 1 像素（`r.Nanite.MaxPixelsPerEdge` 默认 1）。
- `LODScale_ref`：参考视图。沿用引擎 Simplify 构建器假定的 1920×1080、水平 FOV 90°（`HLODBuilderMeshSimplify.cpp:82-85`），此时 `LODScale_ref = 960`。目标分辨率更高时按比例放大。
- `InstanceMinScale`：实例三个轴缩放的最小值。误差在网格局部空间，Nanite 同样取最小轴缩放（`NaniteClusterCulling.usf:321`）。
- `MinVisibleDistance`：WP 取的是源网格层的 `LoadingRange`（`RuntimeSpatialHashHLOD.cpp:228`、`WorldPartitionRuntimeHashSetHLODGeneration.cpp:386`）。源单元格在这个距离外才卸载、HLOD 才显示，格内物体此时离相机至少这么远（推断：按单元格包围盒量）。

1 像素、缩放为 1 时：

| 切换距离 | CutError |
| --- | --- |
| 100 m | 10.4 cm |
| 256 m | 26.7 cm |
| 512 m | 53.3 cm |
| 1 km | 104 cm |

### 为什么切换时几乎看不出来

HLOD 在 `MinVisibleDistance` 及更远处显示。在这些距离上，Nanite 画源物体用的误差阈值都不小于上面算出的 CutError，所以 HLOD 的几何不比 Nanite 本来画的粗，两者的差异在 1 像素量级；材质也是源材质本身。Simplify 构建器是重新简化并烘焙材质，几何和材质都换了。

推断：HLOD 输出如果不是 Nanite 网格，阴影与光照走另一条渲染路径，切换时仍可能有轻微明暗差，需要实测。

## 设计

### 管线

```text
UCSNaniteCutHLODBuilder::Build(Context, SourceComponents)      // 编辑器，WP 构建 HLOD 时调用
  ├─ 过滤源组件                 // 跳过 foliage；组件材质覆盖逐槽带上
  ├─ CutError_world = CutErrorForScreenError(Context.MinVisibleDistance, PixelError, 参考视图)
  ├─ 每个组件 / 每个 ISM 实例一个源：CutError = CutError_world / InstanceMinScale
  ├─ UCSNaniteCutOps::AppendNaniteCuts(临时 UCSMesh, Sources)
  │    ├─ 游戏线程：校验（NotNanite / Unsupported）、变换与镜像标记
  │    ├─ 渲染线程：新登记的资源先落地，读出层级偏移等运行时状态（NotReady）
  │    ├─ 计数趟（按 cluster 预算分趟）：逐层遍历 → 叶切片展开 → 挑选 → 记录，回读用到的那段
  │    ├─ CPU：定状态（Complete / Incomplete / Skipped）、按稳定键排序、算写出偏移、并材质
  │    └─ 扩容 + 写出趟：逐 cluster 解码位置 / 法线 / 切线 / UV / 颜色 / 索引 / 材质号
  ├─ UCSMeshVisibilityOps::CullHiddenTriangles(视点距离 = MinVisibleDistance)   // bCullHidden，默认开
  └─ 回读 → 丢掉没人引用的顶点 → 以 WorldPosition 为枢轴建 FMeshDescription → 静态网格（AssetsOuter）→ UStaticMeshComponent
```

### 截面抽取算子

现有的 `UCSMeshOps::CopyFromStaticMesh` 读 `RenderData->LODResources[i]`（`CSMeshOps.cpp:576-577`），对 Nanite 网格拿到的是 fallback。fallback 是构建期先在层级图上切一刀、再额外简化一遍（`NaniteBuilder.cpp:423-429`），与 Nanite 截面接近但不相等，而且每个资产只有一份，所以另写了 `UCSNaniteCutOps`（独立的函数库，没并进 `UCSMeshOps`）。

**不直接用引擎的 `Nanite::StreamOutData`**（`NaniteStreamOut.h:44`，Nanite 光追用它按 `r.RayTracing.Nanite.CutError` 建 BLAS）：

1. 没有 `RENDERER_API`，头文件在 `Renderer/Private`，插件链接不到。
2. 用 PrimitiveId 从 GPU Scene 取层级偏移（`NaniteStreamOut.usf:439`、`:446`），要求网格作为组件注册在场景里。
3. 只写位置、索引和各材质段范围（`NaniteStreamOut.usf:379-392`），没有法线、UV、颜色，填不满 `UCSMesh` 的流。

插件里照它的结构写的这一套，各部件的来源：

| 部件 | 来源 |
| --- | --- |
| 页数据与层级 buffer | `Nanite::GStreamingManager` 的 `GetClusterPageDataSRV` / `GetHierarchySRV`（`ENGINE_API`） |
| 层级偏移、驻留状态 | `FResources` 的 public 字段 `HierarchyOffset`、`NumHierarchyNodes`、`NumClusters`、`PersistentHash` |
| 层级遍历 | 自写的逐层遍历（每层一个算派发参数的单线程 pass + 一个间接派发 pass），节点切片用 `NaniteDataDecode.ush` 的 `GetHierarchyNodeSlice` 解 |
| 位置与索引 | `NaniteDataDecode.ush` 的 `DecodePosition`、`DecodeTriangleIndices` |
| 法线 / 切线 / 颜色 / UV | `NaniteAttributeDecode.ush` 的 `GetRawAttributeData<3>` |
| 逐三角材质号 | `GetRelativeMaterialIndex`；它就是静态网格的材质槽（`NaniteResources.cpp:2365-2370`），经重映射表换成目标材质表下标 |
| 照抄的私有部分 | `FNaniteGlobalShader` 的编译环境（`NaniteShared.h:372`：`VF_SUPPORTS_PRIMITIVE_SCENE_DATA`、DXC、HLSL 2021） |

- **不经过 GPU Scene**：请求里直接填 `FResources::HierarchyOffset`。Nanite 场景代理写进 GPU Scene 的也是这个值（`NaniteResources.cpp:262`），它由流送管理器在资产加入时分配（`NaniteStreamingManager.cpp:738`）。资产渲染资源就绪就能解，场景里不需要有组件。
- **页数据布局宏不用管**：`NANITE_VOXEL_DATA` / `NANITE_ASSEMBLY_DATA` 由着色器编译器对所有着色器统一设置（`ShaderCompiler.cpp:4159-4160`）。
- **容量由 GPU 决定**：计数趟把头部、逐源统计与逐 cluster 记录写进一个 buffer，先回读头部与统计，再只读用到的那段记录；按结果 `EnsureCapacitySync` 后跑写出趟。计数趟按 cluster 总数分趟（每趟 ≤ 2M），候选与记录 buffer 按资源自身的 cluster 总数定上限，节点队列按 `NumHierarchyNodes` 定；溢出说明层级不是预想的树，整批作废（`Failed`）。
- **自检计数**：「误差不够小、只因带 `STREAMING_LEAF` 才被输出、且不是 `FULL_LEAF`」的 cluster 数。为 0 说明结果就是该 CutError 的精确截面（`Complete`），否则 `Incomplete`。`FULL_LEAF` 不算：CutError = 0 时最细一级正是靠 `STREAMING_LEAF` 选出来的。
- **顶点按 cluster 各存一份**：cluster 边界上的顶点重复，但位置逐位相同。需要共享顶点时跑 `WeldVertices`；HLOD 构建器走提交 MeshDescription 的完整构建，按属性相等合并顶点，顺带焊掉。
- **切线**：有显式切线时原样用。没有时 Nanite 渲染是逐三角现算（`NaniteAttributeDecode.ush:615`），逐顶点的流装不下，写出趟把顶点所在各三角的 UV 切线加起来再正交化；退化时用 `GetNaniteFallbackTangent`。单点版 `GetRawAttributeData<1>` 会无条件覆盖显式切线（`:610`），所以解码用三个角填同一顶点的 `<3>` 版。
- **镜像**：变换行列式为负时自动翻转绕序，切线空间手性跟着翻（测试验证：镜像前后绕序与法线的关系一致）。
- **确定性**：计数趟的记录顺序由原子分配决定，CPU 按 (请求号, 稳定键) 排序后再定写出偏移。稳定键 = (节点号, 子切片号, 切片内序号)，与页驻留在哪个槽位无关。测试验证同样的输入写出逐位相同的网格。

### 流送与驻留

- root 页（每页最大 32 KB，`NANITE_ROOT_PAGE_GPU_SIZE`）随资产加载、一直常驻（`NaniteResources.h:412`）。截面只用到 root 页时，自检计数天然为 0。
- **新登记的资源**：`FStreamingManager::Add` 当场分好层级偏移与根页槽位，数据却要等下一次 `BeginAsyncUpdate` 里的 `ProcessNewResources` 才上传（`NaniteStreamingManager.cpp:2320`）。算子在渲染线程上自己推一次 `BeginAsyncUpdate` / `EndAsyncUpdate`（都是 `ENGINE_API`）让它落地；推完仍有待处理的新资源时一律当 `NotReady`。测试里新建、从没被渲染过的网格能直接解，就是这一步在起作用。
- **更细的流送页**：页流送那部分按帧号门控（`NaniteStreamingManager.cpp:2325`），要真正过帧才会推进，`Build` 期间推不动。
  - `PrefetchResource` 只请求前 16 个流送页（`MAX_RESOURCE_PREFETCH_PAGES`，`NaniteStreamingManager.cpp:33`），大网格不够，没用它。
  - `bRequestMissingPages` 用 `RequestNanitePages` 显式请求不完整资源的全部流送页：先写 `PersistentHash`，后跟若干 `(PageIndex << 1) | 续接位`，高位是优先级（`NaniteStreamingManager.cpp:2222-2243`）。这次的结果不变，编辑器过几帧再抽一次才完整；总量受 `r.Nanite.Streaming.StreamingPoolSize` 限制。
  - 这次怎么写由 `IncompletePolicy` 决定：`UseAvailable` 照写驻留着的（缺页处偏粗，截面仍无缝），`SkipSource` 整个源不写。

### HLOD 构建器

- `UCSNaniteCutHLODBuilder` 继承 `UHLODBuilder`（`HLODBuilder.h:110`），放在 Editor 模块 `PCGEditorProcess`。引擎自带的 MeshMerge / MeshSimplify / MeshApproximate / Instancing 构建器都在 `WorldPartitionHLODUtilities` 插件里这样继承。
- 用法：HLOD Layer 资产里 Layer Type 选 Custom，HLOD Builder Class 选 `CSNaniteCutHLODBuilder`。需要真 RHI；`-nullrhi` 下记错误、不产出组件。
- `UCSNaniteCutHLODBuilderSettings`：`PixelError`、`ReferenceScreenWidth`、`ReferenceHorizontalFOV`、`bEnableNaniteOutput`、`IncompletePolicy`、`bRequestMissingPages`（默认开）、`ExcludeTag`、`bCullHidden`（默认开）与 `CullOptions`。
  - `IsReusingSourceMaterials` 返回 true（`HLODBuilder.h:41`）：HLOD 直接引用源材质，材质按组件上的覆盖逐槽取。
  - `ComputeHLODHash` 纳入上面这些设置和一个算法版本号（加剔除时升到 2），改了才会触发重建。
- `RequiresWarmup`（`HLODBuilder.h:142`）：输出开了 Nanite 时返回 true，否则 false。
- 输出：静态网格建在 `AssetsOuter` 里、去掉 `RF_Public | RF_Standalone`，以 `WorldPosition` 为枢轴烘到局部空间，组件放在 `WorldPosition`（与 MeshMerge 构建器同一做法）；UV 组数按源网格实际用到的声明（最多 4 组）。WP 之后会统一把组件改成静态、关碰撞、给网格改名（`WorldPartitionHLODUtilities.cpp:920-1007`）。

### 外部可见性剔除

`UCSMeshVisibilityOps::CullHiddenTriangles`（`CSMeshVisibilityCull.h`），对任意 `UCSMesh` 可用；构建器与试验台都在截面合并之后、排材质 section 之前调它（开关 `bCullHidden`，默认开）。

- **视点**：包围球外，仰角带内按斐波那契球面均布 `NumDirections` 个方向（默认 512），每个方向一张透视（视点在切换距离上，小于包围球半径 × 1.5 时按后者）+ 一张正交（无穷远），每张 `Resolution`² 像素（默认 1024），视图恰好框住包围球。
- **判定**：GPU 软件光栅，不走图形管线，直接读常驻流。每张视图一趟 `InterlockedMin` 深度，一趟用同一段代码重算深度、与缓冲逐位相等就标"可见"。两趟同一个入口，深度的计算指令完全相同，逐位相等才靠得住。所有面按双面处理（背面也遮挡、也能被看见）。
- **保守**：与可见三角共享顶点的一并留下（`bKeepNeighbors`），补细长三角被像素中心漏采的针眼。掠射角下共边的两个面深度会打平，少数其实看不见的三角被判可见 —— 只会多留，不会多删（构建器测试：5 个立方体的底面 40 个三角删掉 25 个）。
- **压实**：回读逐三角标记，CPU 排出保留列表（升序，相对顺序不变），GPU 收进临时 buffer 再抄回；索引与逐三角材质号一起搬，顶点不动。构建器建 MeshDescription 前再在 CPU 上丢掉没人引用的顶点。
- **只算 HLOD 自己的遮挡**：地形、没进 HLOD 的物体不参与。所以仰角下限默认 0°（相对包围球心的水平面），等于把地面当作挡住了下方视线；从山谷仰视高处的 HLOD（屋檐底面之类）要调成负值。
- **采样不是证明**：只从很窄的角度才看得见的东西（深屋里透过小窗）可能漏采，方向与分辨率越高越稳。测试关卡的隧道：洞口是斜着穿出端面的，开口在 y 向被拉长到约 ±4.6 m，贴着第一道弯内侧的直线视线能擦到 x≈±6 m 的洞壁 —— 那里的三角确实看得见、被留下；中段 ±4.8 m 里只剩紧挨它们的 8 个邻居，深处三个物体一个三角都不剩。
- 耗时：测试关卡 49,516 三角、1,024 张视图，GPU 约 0.13 s（首次含着色器约 3.6 s）。

### 关卡内试验台

`ACSNaniteCutHLODActor`（子蓝图 `BP_NaniteCutHLOD`）不依赖 World Partition：`GatherBox` 里包围盒中心落在盒内的静态网格组件进 HLOD（筛选同构建器），`BuildHLOD` 抽取 + 剔除 + 显示，`ShowHLOD` / `ShowSources` 来回切。

- 显示 HLOD 时只藏真进了 HLOD 的源（写出了三角形）；非 Nanite、被 `SkipSource` 跳过、失败的源在 HLOD 里没有替身，留着显示。一个 actor 只要有一个组件进了 HLOD 就整个藏（与 WP 一致，替换单位是 actor）。编辑器里是临时隐藏（不改属性、不进存盘），运行时是 `HiddenInGame`。
- HLOD 网格是 GPU 常驻数据、不存盘：关卡重新加载后要再 `BuildHLOD`。
- `CountHLODTrianglesInBoxes`：诊断用，数 HLOD 里重心落在各个世界盒子里的三角形。验收剔除就靠它框住外面看不见的区域。
- 模板关卡自带的 `SM_SkySphere` 中心在原点，会落进任何罩住原点的收集盒 —— 它不是 Nanite，报 `NotNanite`、留着显示。

### 烘焙成资产（BakeHLOD）

试验台的 `BakeHLOD` 把截面落地成资产：重新分 UV，用源材质把 BaseColor / Normal / Roughness 烘进 2048（`BakeTextureSize`）的图集，存成开 Nanite 的静态网格，生成一个 StaticMeshActor 挂在 HLOD actor 下面。实现在 `CSNaniteCutBake`（编辑器专用，依赖引擎的 `MaterialBaking` / `MaterialUtilities`，即 MeshMerge 那一套）。

```text
BakeHLOD()
  ├─ 收集 + 抽取截面：材质表按源不去重（bUniqueMaterialsPerSource），逐三角材质号 → 源
  ├─ 外部可见性剔除（同 BuildHLOD）
  └─ CSNaniteCutBake::Bake
       ├─ 回读（世界空间；副法线符号另从切线流原样读，回读接口把它丢了）
       ├─ 新 UV：FStaticMeshOperations::GenerateUV（Legacy = 按源 UV0 的岛重新打包，失败退回 AutoUV）
       ├─ CPU 光栅"纹素 → 三角"表
       ├─ 所有 (源, 材质) 一次 IMaterialBakingModule::BakeMaterials（单输出：同一套整图集渲染目标，各画各的三角）
       ├─ 缝隙外扩 16 圈，岛之间的空白填中性值
       ├─ FMaterialUtilities::CreateFlattenMaterialInstance（BaseFlattenMaterial）→ T_<名>_BaseColor / _Normal / _Roughness + MI_<名>
       └─ 静态网格 SM_<名>：UV0 = 图集，保留源的法线切线，开 Nanite + 显式切线
```

- **材质里的"其它信息"逐源喂**：MaterialBaking 里 WorldPosition 是烘焙网格的顶点位置 × `FPrimitiveData.LocalToWorld`，ObjectPosition 是 `WorldBounds` 的中心，ActorPosition、包围盒、自定义图元数据也都来自 `FPrimitiveData`，顶点色与各组 UV 来自烘焙网格。所以按源拆开烘，每个源给它自己的 ActorPosition / 包围盒 / 自定义图元数据。引擎 MeshMerge 这里只给默认值，用了 ObjectPosition 的材质合并后全都一个样。
- **位置给世界空间、LocalToWorld 给单位矩阵**：MaterialBaking 把图集平面（z = 0）上的点先乘 LocalToWorld 的逆、再由着色器乘回去；给源的真实变换时，带非均匀缩放的源 z 会落成 ±ε，负的一半被近裁剪面裁掉 —— 实测随机丢了 21% 的纹素（726,504 个要补），改成单位矩阵后只剩 22 个。代价：LocalPosition / ObjectOrientation / 转到局部空间的变换按世界空间算（与 MeshMerge 相同）。
- **法线贴图烘在源的切线空间里**：MaterialBaking 原样输出材质的 Normal（切线空间）。输出网格保留源的切线（构建设置关掉重算法线切线），Nanite 打开显式切线（否则 Nanite 按 UV 在像素里现推切线），渲染时的切线系与烘焙时一致，不用逐纹素换切线空间。世界空间法线的材质由 `bTangentSpaceNormal` 换到切线空间。
- **构建设置**：静态网格默认会重算法线切线、生成光照 UV、半精度 UV；这里全部关掉并开全精度 UV —— 2048 的图集在 0.5～1 之间半精度只剩约一个纹素的精度。
- **颜色空间**：烘焙前切到线性烘焙（粗糙度更准），烘完按扁平化材质默认贴图的 sRGB 标志换算，再恢复原状态。顶点色流里是渲染用的 sRGB 字节，喂给 MeshDescription 前先解码，MaterialBaking 再编回同样的字节。
- **资产与挂载**（MeshBoolean 同款）：放在关卡同级 `AutoResult/`，名字 `HLOD_<actor 标签>_<8 位稳定编号>`（编号存在 actor 上，复制出来的 actor 拿新的），重烘就地覆盖；只标脏不写盘。StaticMeshActor 带 `CSNaniteCutBakedHLOD` 标签挂在 HLOD actor 下面，重烘时只替换带标签的；收集源时跳过挂在 HLOD actor 下面的 actor。
- **显存：用单输出路径**：`BakeMaterials(..., FBakeOutput&)` 把所有条目画进同一套整图集大小的渲染目标（BaseColor / Normal / Roughness 各一张 2048²，共 48 MB），渲染目标不进池子、烘完随 GC 释放；图集各块互不重叠，画完就是合成好的结果，不用再按"纹素 → 三角"表拼。多输出路径（`TArray<FBakeOutput>&`）按"尺寸 + 格式"把渲染目标永久挂进模块的池子（`TStrongObjectPtr`，只有模块关闭时才 `CleanupRenderTargets`），之前逐源矩形的尺寸各不相同，实测烘一次就多挂 94 张、485 MB 显存直到关编辑器。换成单输出后覆盖率与补的边缘纹素完全一致（84.2% / 22 个），烘焙 6.4 s → 2.6 s，烘完显存回落到烘焙前以下。缓存的材质代理本来就在下一次 GC 前释放。
- **显示切换**：`ShowHLOD` 有烘焙结果时显示它（BuildHLOD 之后显示 GPU 截面），`ShowSources` 藏起来；关卡加载时默认显示源、烘焙结果临时隐藏，免得两份叠在一起。
- **没覆盖的**：ISM 的 PerInstanceRandom / 逐实例自定义数据（烘焙时不是实例化顶点工厂）；Time、相机相关节点（CameraVector / ReflectionVector 退化成顶点法线）；WPO（烘的是未偏移的几何）；半透明 / 遮罩材质按不透明烘。

### 输出形态

| 形态 | 远处表现 | 适用 |
| --- | --- | --- |
| 普通静态网格（构建器默认） | 固定几何，距离越远三角形越小于像素 | 单级 HLOD、距离跨度不大 |
| 开 Nanite 的静态网格（`bEnableNaniteOutput`） | 在截面上重建层级，继续连续 LOD；需要预热 | 距离跨度大 |
| 运行时 `UCSMesh`（直接调算子） | 由 `UCSMeshRenderComponent` 画，非 Nanite 路径 | 运行时生成、不走 WP 的内容 |

多级 HLOD：第 k 级取 `2^k` 倍的切换距离和 CutError。各级都是同一张层级图上的截面，天然嵌套。

## 限制

- **每个物体的下限**：每个网格的层级一直简化到只剩一个 cluster 才停（`ClusterDAG.cpp:698-709`），根 cluster ≤128 三角形（`NANITE_MAX_CLUSTER_TRIANGLES`）。N 个物体的 HLOD 至少有 N 个根 cluster。
- **不跨物体合并**：相邻物体不焊接，缝宽在 CutError 以内，与 Nanite 在同距离上的表现相同。物体之间的内部面、互相穿插的部分由外部可见性剔除删掉看不见的那部分。
- **材质**：沿用源材质，合并后每种材质一个 draw（静态网格按材质分 section；`UCSMesh` 要跑 `BuildMaterialSections`）。材质种类多时 draw 多，烘焙类构建器能压到 1 个。
- **`Voxelize` 形状保持**（`EngineTypes.h:3031-3032`，5.7 给植被用）：远处几层是体素 cluster，没有三角形。
- **世界位置偏移（WPO）**：源材质带 WPO 时 HLOD 也会跑，远处可能要关（待定）。

## 与引擎自带构建器的对比

| 构建器 | 几何 | 材质 | 实例 / draw | 运行时依赖源资产 |
| --- | --- | --- | --- | --- |
| Instancing | ISM 复用源网格，Nanite 源继续按距离选截面 | 源材质（`HLODBuilderInstancing.h:30`） | 实例数不变 | 是 |
| MeshSimplify | CPU 简化，屏幕尺寸由 `MinVisibleDistance` 推出（`HLODBuilderMeshSimplify.cpp:103`） | 烘焙到 `HLODMaterial` | 一份网格 | 否 |
| `CSNaniteCutHLODBuilder` | 各源的 Nanite 截面合并 | 源材质 | 一份网格，每种材质一个 draw | 否（只在构建时需要） |

MeshMerge、MeshApproximate 未逐项核实，不在表内。

值得用它的前提是：痛点在实例数，或者远处不想保留源资产。如果痛点只是远处三角形多，Nanite 本身已经处理了，用 Instancing 就够。

## 实现清单

| 文件 | 内容 |
| --- | --- |
| `Source/ComputeShaderGenerator/Public/CSNaniteCut.h` | `UCSNaniteCutOps`（`AppendNaniteCuts` / `AppendNaniteCut` / `CutErrorForScreenError` / `RequestNaniteResidency`）与源、选项、结果结构体 |
| `Source/ComputeShaderGenerator/Private/CSNaniteCut.cpp` | 着色器类、驻留落地、分趟计数与回读、排序与写出计划、写出趟 |
| `Shaders/Private/CSNaniteCut.usf` | 6 个 kernel：`InitTraversalCS`、`PrepareNodeArgsCS`、`NodeCullCS`、`PrepareClusterArgsCS`、`ClusterSelectCS`、`WriteClustersCS` |
| `Shaders/Private/CSNaniteCutLayout.ush` | C++ 与 .usf 共用的布局常量（输出 buffer 槽位、统计字段、记录字段、写出标志） |
| `Source/PCGEditorProcess/Public/CSNaniteCutHLODBuilder.h`、`Private/CSNaniteCutHLODBuilder.cpp` | `UCSNaniteCutHLODBuilder` 与设置类 |
| `Source/ComputeShaderGenerator/Public/CSMeshVisibilityCull.h`、`Private/CSMeshVisibilityCull.cpp`、`Shaders/Private/CSMeshVisibilityCull.usf` | 外部可见性剔除 `UCSMeshVisibilityOps` |
| `Source/ComputeShaderGenerator/Public/CSNaniteCutHLODActor.h`、`Private/CSNaniteCutHLODActor.cpp` | 关卡内试验台 `ACSNaniteCutHLODActor` |
| `Source/ComputeShaderGenerator/Public/CSNaniteCutBake.h`、`Private/CSNaniteCutBake.cpp` | 烘焙成资产：新 UV、逐源图元数据的材质烘焙（MaterialBaking 单输出路径）、扁平化材质、Nanite 静态网格（编辑器专用；Build.cs 编辑器目标加了 MaterialBaking / MaterialUtilities） |
| `Source/ComputeShaderGenerator/Private/CSGpuMeshSceneProxy.cpp` | `SubmitGpuBufferDraw` 补了线框视图模式（原来整块画成黑色实体），所有 GPU 网格受益 |
| `Source/ComputeShaderGenerator/Private/Tests/CSNaniteCutTests.cpp` | `NaniteCut.CutErrorForScreenError`、`NaniteCut.Extract`、`NaniteCut.HLODActor` |
| `Source/ComputeShaderGenerator/Private/Tests/CSMeshVisibilityCullTests.cpp` | `MeshVisibility.CullHidden` |
| `Source/PCGEditorProcess/Private/Tests/CSNaniteCutHLODBuilderTests.cpp` | `NaniteCutHLOD.Build` |
| `Content/NaniteCutHLOD/` | 测试关卡 `L_NaniteCutHLODTest`、子蓝图 `BP_NaniteCutHLOD`、`Meshes/SM_NCH_*`（含山洞、房子）、`Materials/MI_NCH_*` 与三种验证烘焙的材质 `M_NCH_WorldChecker` / `M_NCH_ObjectTint` / `M_NCH_GridNormal`；`AutoResult/` 是 BakeHLOD 的产物 |
| `Scripts/NaniteCutHLODSetup.py` | 生成上面这些内容（可重跑；`NCH_REBUILD=<网格名>` 重新生成某个网格） |
| `Scripts/NaniteCutHLODShots.py`、`Scripts/NaniteCutHLODCompose.py` | 多角度出图 + 区域计数；系统 Python 合成对照图与像素判据 |

## 验证

2026-09-22 实跑，5 条全过：

```bash
UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests PCGPlugins.ComputeShaderGenerator.NaniteCut+PCGPlugins.ComputeShaderGenerator.MeshVisibility+PCGPlugins.PCGEditorProcess.NaniteCutHLOD; Quit" -unattended -nosplash -stdout -AbsLog=<log>
```

| 测试 | 验了什么 | 实测 |
| --- | --- | --- |
| `NaniteCut.CutErrorForScreenError` | 换算与 Nanite 的 LODScale 一致 | 960 cm 处 1 像素 = 1 cm |
| `NaniteCut.Extract` | 16×16 起伏网格（512 三角、两材质）：CutError 0 的三角数等于源网格；极大 CutError 只剩根；递增单调不增；包围盒；确定性；追加；镜像；非 Nanite | Nanite 构建出 7 个 cluster；CutError 0 取到 4 个叶 cluster、512 三角、被迫 0（`Complete`）；镜像前后绕序与法线的关系一致 |
| `NaniteCut.HLODActor` | 试验台：盒内收集、排除标签、盒外不收、非 Nanite 的不藏（回归）、ShowHLOD / ShowSources / ClearHLOD | 3 个源（1 个非 Nanite 留着显示），1024 三角 |
| `MeshVisibility.CullHidden` | 封闭外壳里包一个盒子、外面另放一个：全方向视点删掉被包住的 12 个、留 24 个，材质号跟着压实；仰角 ≥ 0° 时底面也删 | 36 → 24；36 → 20 |
| `NaniteCutHLOD.Build` | 经 `UHLODBuilder::Build` 入口：两个组件（一个镜像）+ ISM 三个实例进 HLOD，foliage 与带排除标签的（actor / 组件各一）跳过；输出位置、outer、标志、包围盒；预热；关 / 开剔除各建一次 | 引擎 Cube 是 48 三角，HLOD 240 三角 = 5 个立方体；开剔除 215（底面删掉大半） |

不能加 `-nullrhi`：抽取在 GPU 上。还可以补的判据：大网格，用一个 root 页装不下的网格验 `Incomplete` 与 `bRequestMissingPages` 过几帧后转 `Complete`（要用 latent 命令跨帧）。

### 画面验收

测试关卡 `L_NaniteCutHLODTest`：6×6 格子群（5 种 Nanite 网格、3 个镜像、4 个红色的带排除标签）+ 蛇形山洞（30 × 18 × 9 m 实心岩块，两个整波的圆截面隧道，洞口、第一道弯、深处各放物体）+ 房子（12 × 8 × 4.5 m，墙 / 地板 / 屋顶 25 cm，前屋开一门两窗、后屋完全封闭，各放物体）+ 七块互相穿插的石头。切换距离 80 m。

```bash
UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<插件>/Scripts/NaniteCutHLODSetup.py" -unattended -nosplash -stdout -AbsLog=<log>
UnrealEditor-Cmd.exe <uproject> /PCGPlugins/NaniteCutHLOD/L_NaniteCutHLODTest -ExecCmds="py <插件>/Scripts/NaniteCutHLODShots.py" -unattended -nosplash -stdout -AbsLog=<log>
python <插件>/Scripts/NaniteCutHLODCompose.py
```

出图脚本的显存：这个工程里一个常驻渲染状态的 1080p SceneCapture 约 210 MB（TSR / Lumen / VSM / 体积云的逐视图历史 + 每视图 3 × 16 MB 的光追场景缓冲），同时挂 11 个就是 +2.3 GB。脚本现在线框拍完即销毁、实景每 4 个一批，编辑器峰值工作集 6 GB → 4.3 GB。存关卡时场景里不能有临时捕获 —— 旧版在捕获还挂着时存了关卡，11 个 SceneCapture2D 连同 1080p 渲染目标进了 .umap（已清掉）。

出图在 `Saved/NaniteCutHLODShots/`：`compare_<机位>.png`（原始 | HLOD | 差异热图 | 原始重拍的噪声底）、`wire_<机位>.png`（同一份截面剔除关 / 开的 X 光线框）、`compare_overview.png`、`compare_stats.json`。

SceneCapture 统一关 Lumen 与距离场 AO：HLOD 显示组件是 GPU 常驻网格，没有 Lumen 卡片也没有距离场，开着两组画面的间接光本来就不同。X 光只拿 HLOD 比：Nanite 的线框会写深度、大平面填黑，不透视。

2026-09-22 结果：53 个源、全精度 378,362 三角 → 截面 49,516 → 剔除后 40,919（10.8%）。

| 盒子里的 HLOD 三角（`CountHLODTrianglesInBoxes`） | 剔除关 | 剔除开 |
| --- | --- | --- |
| 后屋内部（封闭，含内壁） | 1,644 | 0 |
| 后屋 3 个物体（各自的包围盒） | 1,839 | 0 |
| 隧道深处 3 个物体 | 1,015 | 0 |
| 隧道中段 x = ±4.8 m（含洞壁） | 1,661 | 8（紧挨可见洞壁的一圈邻居，见上文） |
| 第一道弯 2 个物体 | 633 | 478（从拉长的洞口斜着看得见一部分） |
| 洞口 2 个物体 | 634 | 587 |
| 前屋内部（一门两窗） | 1,039 | 869 |
| 前屋 2 个物体 | 1,009 | 841 |

**烘焙 HLOD（开 Nanite 的静态网格 + 2048 贴图）vs 原始**（出图脚本现在比的是这一组）：

| 机位 | 差异 > 16/255 | 覆盖 IoU | 噪声底 | 说明 |
| --- | --- | --- | --- | --- |
| switch_front / switch_high / far_side（最近物体 93～134 m） | 6.1% / 4.4% / 4.7% | ≥ 0.985 | 0.4～1.0% | 轮廓 13～16%、内部 2.7～4.4%：图集约 3.5 cm/纹素，凹凸面的细节光影变平 |
| mid（80 m 群中心） | 3.1% | 0.994 | 0.5% | |
| house（19 m） | 7.9% | 0.994 | 0.15% | 大头是原始自己的毛病：房子正面恰好落在 WorldChecker 150 cm 格子的分界面上，`floor()` 随逐像素精度噪声来回翻，原始画面里是一片抖动的斜带（原始重拍也在变）；烘焙逐纹素只算一次，干净地落在一侧 |
| cave（33 m） / pile（11 m） | 8.5% / 3.4% | ≥ 0.981 | ≤ 0.2% | |
| cave_mouth（8 m，截面误差 16.6 像素） | 20.4% | 0.947 | 0.03% | 远在设计距离以内：一个纹素铺约 7 个像素，GridNormal 的细碎花纹糊掉，花纹与颜色对得上 |

以下是早先 GPU 截面（没烘焙，直接用源材质画）的对比：远景（最近物体 93～134 m，截面误差 ≤ 0.86 像素）两边肉眼看不出差别，从外面看任何机位都没有少东西：覆盖掩码 IoU ≥ 0.987，差异 > 16/255 的像素 2.4～3.3%，其中轮廓像素约 11%、内部约 2%；同一设置重拍的噪声底 0.5～1.0%。差异是真的，但落在轮廓 ≤ 1 像素的偏移与凹凸面的明暗细节上（HLOD 的阴影投射体是固定截面，Nanite 的阴影按阴影图分辨率另取截面）。

## Open Questions

- 还没在真实 WP 关卡里构建过 HLOD。目标关卡是否都走 World Partition 也待确认（工程里的模板关卡是，`Content/__ExternalActors__/`）；不走 WP 的内容需要自己按距离切换，算子本身可以直接用。
- 参考分辨率与 FOV：沿用引擎的 1080p / 90°，还是按实际目标分辨率（4K 时 CutError 减半）。
- 输出开不开 Nanite；开的话，Nanite 在合并后的网格上重建层级时能否跨物体简化（未核实）。
- Nanite Assembly 源：场景外遍历需要 `FResources::AssemblyTransformOffset`，路径未核实，目前跳过。
- 材质种类多的组，draw 数能否接受；需要时再加材质合并。
- 没有 Nanite 数据的源目前直接跳过；要不要退回读它的普通 LOD（`CopyFromStaticMesh` 那条路现在只能替换、不能追加）。
- 可见性剔除只算 HLOD 自己的遮挡。要不要把地形 / 没进 HLOD 的大件当遮挡物喂进深度趟（能删掉更多贴地部分，也能放开仰角下限）。
- 可见性采样的默认密度（512 方向 × 1024 像素）够不够：窄开口后面的东西会不会漏采，需要在真实场景里看有没有"切到 HLOD 少了东西"。

## 源码位置

文中只写文件名，完整路径如下（引擎根 `D:\UnrealEngine-5.7.4-release` 下）：

| 文件 | 目录 |
| --- | --- |
| `NaniteStreamOut.usf`、`NaniteClusterCulling.usf`、`NaniteHierarchyTraversal.ush`、`NaniteDataDecode.ush`、`NaniteAttributeDecode.ush` | `Engine/Shaders/Private/Nanite/` |
| `NaniteDefinitions.h` | `Engine/Shaders/Shared/` |
| `NaniteStreamOut.h`、`NaniteShared.h`、`NaniteShared.cpp`、`NaniteRayTracing.cpp` | `Engine/Source/Runtime/Renderer/Private/Nanite/` |
| `NaniteResources.h`、`NaniteStreamingManager.h` | `Engine/Source/Runtime/Engine/Public/Rendering/` |
| `NaniteResources.cpp`、`NaniteStreamingManager.cpp` | `Engine/Source/Runtime/Engine/Private/Rendering/` |
| `ShaderCompiler.cpp` | `Engine/Source/Runtime/Engine/Private/ShaderCompiler/` |
| `ClusterDAG.cpp`、`NaniteBuilder.cpp` | `Engine/Source/Developer/NaniteBuilder/Private/` |
| `EngineTypes.h` | `Engine/Source/Runtime/Engine/Classes/Engine/` |
| `HLODBuilder.h` | `Engine/Source/Runtime/Engine/Public/WorldPartition/HLOD/` |
| `RuntimeSpatialHashHLOD.cpp` | `Engine/Source/Runtime/Engine/Private/WorldPartition/RuntimeSpatialHash/` |
| `WorldPartitionRuntimeHashSetHLODGeneration.cpp` | `Engine/Source/Runtime/Engine/Private/WorldPartition/RuntimeHashSet/` |
| `HLODBuilderInstancing.h` | `Engine/Plugins/Editor/WorldPartitionHLODUtilities/Source/Public/WorldPartition/HLOD/Builders/` |
| `HLODBuilderMeshSimplify.cpp` | `Engine/Plugins/Editor/WorldPartitionHLODUtilities/Source/Private/WorldPartition/HLOD/Builders/` |
| `WorldPartitionHLODUtilities.cpp` | `Engine/Plugins/Editor/WorldPartitionHLODUtilities/Source/Private/WorldPartition/HLOD/Utilities/` |
| `CSMeshOps.cpp` | 本插件 `Source/ComputeShaderGenerator/Private/` |
