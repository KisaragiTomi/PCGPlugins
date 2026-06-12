# GenerateVines 迁移清单（预览，未执行复制）

源工程：`d:\MyWork\UnrealProject\UETest574_2\Plugins\PCGPlugins`
目标工程：`D:\KeLuwang_PC-KeLuWang_1851\Lycoris_main\Plugins\PCGPlugins`

> 本文件只列「打算复制什么」，尚未真正动文件。请先确认范围。

---

## 0. 关键背景：目标插件是同名旧版本

目标 `Lycoris_main\Plugins\PCGPlugins` 已经存在一份**较旧的 PCGPlugins**，不是空目录。两边差异很大：

| 维度 | 源（UETest574_2） | 目标（Lycoris_main，旧） |
| --- | --- | --- |
| GenerateVines 逻辑位置 | 已合并进 `GeometryEditorActor.cpp/.h`（`AVineContainer::GenerateVines`） | 仍是独立的 `GenerateVines.h`（旧架构） |
| 源码文件数 | 多（含 GPUSkeletalTree、MeshFill、CliffGenerate 等新增模块） | 少（缺一批 .cpp/.h） |
| Shader 数 | 22 个 usf/ush | 17 个（缺 VVVoxel/VinePerlinNoise/VoxelCavitySpan/Connectivity 等） |
| SpaceColonization 测试内容 | 较全（含 UVTest 材质、额外网格） | 较少 |

含义：这不是「往空目录里放新东西」，而是**用源端的新版 GenerateVines 体系覆盖目标的旧版**。直接覆盖会改变目标插件的编译单元和资源，需要你确认是否接受覆盖。

---

## 1. 数据流 / 模块关系（先看懂耦合，再决定怎么搬）

GenerateVines 不是一个独立文件，而是一条贯穿三层的链路：

```
AVineContainer (GeometryEditorActor.h/.cpp)        ← 蓝图可调用入口 GenerateVines()
   │ 继承
AComputeShaderMeshGenerator (ComputeShaderMeshGenerator.h/.cpp)  ← 体素/三角缓存基类
   │ 依赖
SpaceColonization GPU shaders + VVVoxel GPU 管线   ← .usf/.ush（Shaders/Private）
   │ 输出
测试关卡 L_TestWorld + BP_VineSource + 材质/网格    ← Content/SpaceColonization
```

调用顺序（运行态）：

```
GenerateVineAction()
  └─ GenerateVines(ExtrudeScale, Result)
       ├─ 构建 Bounds / 体素输入
       ├─ EnsureTriangleCache（基类的三角缓存）
       ├─ SpaceColonizationWithScales()  ── 调 SpaceColonizationQueue.usf 的 6 个 CS
       ├─ 生成 TubeLines
       └─ VisVine(GPU/CPU)  ── 调 VVVoxel.usf 的 8 个 CS（含 VinePerlinNoise.ush）
```

结论：**逻辑无法只搬一个 GenerateVines 文件**。最小可运行集合 = `AVineContainer` 所在文件 + 其基类 + SpaceColonization/VVVoxel 两套 shader + 测试 Content。

---

## 2. 必须复制的 C++ 源码

### 2a. GenerateVines 逻辑本体（模块 GeometryScriptExtraEditor）
GenerateVines 已并入 `AVineContainer`，与同文件里的其它 Actor 逻辑无法干净拆分，建议整文件搬：

- `Source\GeometryScriptExtraEditor\Private\GeometryEditorActor.cpp`（含 GenerateVines + SpaceColonization GPU shader 声明 + VVVoxel 管线，约 7569 行）
- `Source\GeometryScriptExtraEditor\Public\GeometryEditorActor.h`
- `Source\GeometryScriptExtraEditor\Private\FoliageConverter.cpp`（GenerateVines 的植被互转，强依赖 AVineContainer）
- `Source\GeometryScriptExtraEditor\Public\FoliageConverter.h`
- `Source\GeometryScriptExtraEditor\Public\GeometryGenerate.h`（SpaceColonization 计时统计声明）

> 注意：`GeometryEditorActor.cpp` 顶部注释写明 shader 声明是「moved from GenerateVines.cpp」。目标旧版仍有独立 `GenerateVines.h`，覆盖后要确认旧 `GenerateVines.h` 是否还被别处引用（否则会有重复符号或悬空 include）。

### 2b. 基类与 GPU 缓存（模块 ComputeShaderGenerator）
- `Source\ComputeShaderGenerator\Private\ComputeShaderMeshGenerator.cpp`
- `Source\ComputeShaderGenerator\Public\ComputeShaderMeshGenerator.h`
- `Source\ComputeShaderGenerator\Public\ComputeShaderDebugParams.h`（VisVine 调试参数结构，被 .h 引用）

### 2c. 视口叠加 UI（模块 PCGEditorProcess，可选）
GenerateVines 的视口按钮/叠加层，非生成逻辑必需，但属于「相关逻辑」：

- `Source\PCGEditorProcess\Private\SVineContainerViewportOverlay.cpp` / `.h`
- `Source\PCGEditorProcess\Private\VineContainerViewportOverlay.cpp` / `.h`

---

## 3. 必须复制的 Shader（Shaders\Private）

GenerateVines 直接走到的：

- `SpaceColonizationQueue.usf`（SC 的 6 个 CS：Init/MarkSources/BuildNeighbors/ResetProposals/Propose/Commit）
- `VVVoxel.usf`（VisVine 的 8 个 CS）
- `VinePerlinNoise.ush`（被 VVVoxel.usf include，**必带**）
- `VineVisualization.usf`（vine 可视化）

间接/可能相关（基类体素用到，目标旧版缺失）：

- `VoxelCavitySpan.usf`（体素腔体扫描，基类 BasicFunction 体系用）
- `Connectivity.usf`（连通性，目标缺）
- `SparseTileDispatch.ush`（稀疏调度 helper，目标缺）

> 建议：2/3 节列的 shader 至少把前 4 个 vine 专用的带上；后 3 个视目标是否报缺再补。

---

## 4. 测试关卡 + 蓝图 + 材质（Content\SpaceColonization）

源端整个 `Content\SpaceColonization` 目录，对比目标旧版的差异（★ = 目标缺失，需新增）：

**根目录**
- `L_TestWorld.umap` ← 测试关卡（目标已有旧版，会覆盖）
- `BP_VineSource.uasset` ← vine 源点蓝图
- `SMF_PlaneVine_FoliageType.uasset` / `SMF_Target_FoliageType.uasset` / `SMF_TubeVine_FoliageType.uasset`
- ★ `Cylinder.uasset` / `NewBlueprint.uasset` / `SM_MERGED_InstancedFoliageActor_0.uasset`（目标缺）

**Material/**
- `C_VineData.uasset`、`M_Color.uasset`、`M_FoliageColor.uasset`
- `MI_Color_{Plane,Target,Tube}.uasset`、`MI_FoliageColor_{Plane,Target,Tube}.uasset`
- ★ `M_UVTest.uasset` / `M_UVTest_Inst.uasset` / `M_UVTest_Inst1.uasset` / `MI_FoliageColor_Tube1.uasset`（目标缺）

**Mesh/**
- `Cone / Cube / Sphere / SM_PlaneVineCube / SM_TargetCube / SM_TubeVineCube`（基本一致）

**CSSWData/**（浅水测试数据，疑似与 vine 无直接关系，按需）
- `MI_CSSW_Decal_8141441` / `MI_CSSW_Water_8141441` / `SM_CSSW_Water_8141441` / `T_CSSW_DepthWet_8141441` / `T_CSSW_VelHeight_8141441`
- 目标旧版的 `BlurPrinet\EUW_FoliageConverterCore.uasset` 在源端没有，复制时不要误删目标已有的它。

---

## 5. 需要你拍板的事项

1. **覆盖策略**：目标是旧版同名插件。是整目录覆盖（彻底变成源端版本），还是只挑 vine 相关文件增量覆盖（可能与旧版其它模块编译冲突）？
2. **C++ 范围**：GenerateVines 已并入大文件 `GeometryEditorActor.cpp`，无法只搬「vine 部分」。是否接受整文件 + 基类一起搬？
3. **旧 `GenerateVines.h`**：目标 `Source\...\Public\GenerateVines.h` 是旧架构残留，覆盖后是否删除它（否则可能重复定义）？
4. **CSSWData / 浅水数据**：是否一并带（看起来与 vine 弱相关）？
5. **shader 第二梯队**（VoxelCavitySpan/Connectivity/SparseTileDispatch）：先带还是等编译报缺再补？

确认以上后我再执行真正的复制。