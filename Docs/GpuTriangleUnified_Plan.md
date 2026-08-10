# GPU 三角形统一规划：可拓展储存 + 单一 readback

承接项目文档 [`gpu-triangle-buffer-unification.md`](../../../doc/gpu-triangle-buffer-unification.md)（第一轮：direct + road 并入 `CSGpuMesh` 基座）。本文规划第二轮：把 `AComputeShaderMeshGenerator` 与 vine 剩余的散装 GPU→CPU 回读收进一套分层 readback，把 build/save 归一到统一 CPU 快照，并给 vine 叶子迁移收尾。方案经四路代码盘点 + 三路对抗评审（结论均为 sound-with-changes），评审修正已并入本文；所有行号为 2026-07-24 现状。

## 结论

- **能统一**。储存侧的可拓展目标已由描述符驱动的基座达成（加一个 buffer = 叶子 `RegisterStreams()` 里多一次 `AddStream(...)`，分配/VF 绑定/回读遍历 registry 不改）；真正要补的是：一个通用 readback 底层（L0）、一个 DynamicMesh sink（sink B）、vine 叶子的 M1→M3 收尾。
- **储存不强并成一种**。评审确认项目里有三种各有其存在理由的 GPU 储存形态（见下表），统一的是**回读与落地方式**，不是强迫所有生产者改用同一种 buffer。
- **Blueprint 边界类型保留**。`FCSTriangleMeshData` / `FCSSurfaceVoxelData` 是 `BlueprintType` USTRUCT、被 UFUNCTION 与 RT 重上传路径钉死；`FCSGpuMeshCPUData` 是非反射结构体，二者不能互并。统一发生在 readback 层与 sink 层，BP 类型只做单向适配。

## 架构图

三种储存形态 → 双层 readback → 双 sink 的整体关系见
[`GpuTriangleUnified_Architecture.svg`](GpuTriangleUnified_Architecture.svg)。
绿色网格 = 描述符驱动储存（核心）；紫色点纹 = 统一 readback 层；琥珀条纹 = actor 持有的 retained pooled 集；灰色斜纹 = M3 待删的 vine 遗留路径。

## 现状盘点

### 储存形态（3 种，全部保留）

| 形态 | 持有者 / 生命周期 | 实例 | 回读方式 |
| --- | --- | --- | --- |
| (a) proxy 描述符流 | `FCSGpuMeshSceneProxy`，随渲染状态重建 | road / direct / vine 叶子的 7 标准流（`AddStandardTriangleStreams`，`CSGpuMeshSceneProxy.cpp:129`） | L1 `ReadbackMeshSync`（counted 两段） |
| (b) 瞬态过程 buffer | 单个 `FRDGBuilder` 图内创建即消费 | box-scene 三角汤（`ComputeShaderMeshGenerator.cpp:2370`）、landscape 三角（`:5746`）、vine 旧输出 | 必须在图内 `AddCopies`（L0 的 RT 半侧） |
| (c) actor 持有 retained pooled | actor 成员，跨帧存活，非 UPROPERTY | `FCSSurfaceVoxelGPUBuffers LastSurfaceVoxelGPUBuffers`（`ComputeShaderMeshGenerator.h:213-236`） | pooled，可 counted 两段 |

### readback 站点（15 处 `FRHIGPUBufferReadback`，全在 ComputeShaderGenerator + GeometryScriptExtraEditor）

| 类别 | 站点 | 处置 |
| --- | --- | --- |
| A 并入统一层 | 基座 `ReadbackMeshSync`（`CSGpuMeshComponent.cpp:43`）；box-scene 三角汤（`ComputeShaderMeshGenerator.cpp:2355`）；landscape 三角（`:5733`）；vine 旧三元组 Vertex/UV/Index（`GeometryEditorActor.cpp:3271`，**冻结到 M3**） | P0 重建 / P1 迁移 / M3 删除 |
| B 形态不同、L0 可服务 | 表面体素 5-buffer 批（`:2597`）；点采样器 points+counts；SC 队列 3 结果回读 | P1 机会性迁 L0（capacity 单段模式） |
| C 明确排除 | SC 逐迭代 debug readback 家族（`GeometryEditorActor.cpp:6101+`，`if constexpr` 门控）；GDF 三站点（视图扩展渲染图内，**不可 flush**，自带 async pump）；heightmap/RT 纹理路径 | 不动；GDF 若future 统一需 async 变体 |
| 死代码 | `ReadbackResolvedStaticMeshTriangleRequestSync`（`ComputeShaderMeshGenerator.cpp:1755-1943`，全插件零调用） | **P0 直接删除，不迁移** |

### build / save 现状

- sink A 已统一：`ReadbackMeshSync` → `CSGpuMeshSave::SaveGpuMeshComponentToStaticMesh`（空路径默认 `<level>/result/SM_<owner>`，`bSaveAsset=false` 只标 dirty），三个叶子共用。
- vine 旧 build 独立：readback 三元组 → `BuildDynamicMeshFromGPUVineOutput`（`GeometryEditorActor.cpp:3674`）→ `AppendMesh`；save 走 `SaveStaticmesh`（`:5213`，含 `AutoResult/SM_<Actor>_<timecode>` 命名 + tagged `AStaticMeshActor` 生成/挂接/隐藏的 actor 生命周期包装）。
- CPU 侧已有 `CSMeshBuild::BuildDynamicMeshFromCSTriangleData`（soup+法线，无 UV/色）；与 vine build 特性集不同（后者有 UV 覆盖、逐三角 MaterialID、A,C,B 绕序翻转、法线重算），**不合并**，sink B 另立。

## 目标设计

### S1 储存：描述符拓展点

加**非回读**流今天已是 one-touch（`AllocateStreamsAndBindVF` 全描述符驱动，`AuxVertex` 走安全默认分支）。加**回读语义**今天要摸 5 处（enum、`FCSGpuMeshCPUData` 成员/Reset/IsValid、数组 sizing、decode switch、save 消费），sink B 落地后变 6 处——P1 一并收敛为 **per-semantic decode registry**（语义 → {stride 校验, 解码函数, 目标数组} 单表项），并让 `FCSGpuMeshCPUData::IsValid()` 对缺失流 optional-aware（为将来"半语义"叶子留门）。

### R1 readback：L0 双侧 API + L1 重建

```cpp
// L0: ComputeShaderGenerator/Public/CSGpuReadback.h（P0 新增, COMPUTESHADERGENERATOR_API）
struct FCSGpuReadbackRequest
{
	FRDGBufferRef Buffer;          // 或 TRefCountPtr<FRDGPooledBuffer>（counted 两段模式仅限 pooled）
	uint64 NumBytes;               // 拷贝字节数; uint64->uint32 溢出守卫收进层内
	ERHIAccess FinalAccess;        // 拷完恢复的访问态; 缺省按 FinalAccessForRole(Role) 推导
	const TCHAR* DebugName;        // 保留逐 buffer 名, RenderDoc 可辨识
};
// RT 半侧: 在调用者自己的 GraphBuilder 里插拷贝 pass —— 瞬态 buffer 只能走这条
void AddCopies(FRDGBuilder&, TArrayView<FCSGpuReadbackRequest>, FCSGpuReadbackBatch& Out);
// GT 半侧: flush + lock + memcpy + RAII 释放; '返回即 consume 完成' 的阻塞契约不变
bool Resolve(FCSGpuReadbackBatch&, TFunctionRef<void(int32 /*Index*/, const void*, uint64)> Consume);
```

固化的评审裁决（缺一即错）：

- **请求必须携带 `FinalAccess`**：Index 流须恢复 `VertexOrIndexBuffer`、顶点流 `VertexOrIndexBuffer|SRVMask`、counters 留 `CopySrc`——通用 `(buffer, bytes)` 签名会把 pooled 流留在 `CopySrc`，回读后组件再也画不出来。`FinalAccessForRole()` helper 从原 P3 提前到 P0 进 `CSGpuMeshTypes.h`（现状三份拷贝：基座/vine/road proxy）。
- **两种模式都要**：capacity 整拷单段（瞬态图内 buffer、CPU 已知计数的 vine/landscape）与 counted 两段（仅 pooled；先读 2-uint counters 再按计数拷流）。只写两段式则三个迁移目标一个都接不上。
- **flush 下限 3 不是 2**（现状 5）：pre-capture flush（`CSGpuMeshComponent.cpp:54`）是 proxy 重建栅栏，`SetBuildInput` 的同步 save 契约依赖它，必须保留；只有 4 次 enqueue/consume flush 可两两合并。真实主成本是两次 `SubmitAndBlockUntilGPUIdle` 全 GPU 排空——如实说明，勿把 flush 数当头条收益。
- **L0 拥有 readback 对象生命周期**（RAII，杜绝现状每站点 3-5 份 delete-on-failure 重复），并成为尺寸计算与 `(bReadback && CpuSemantic != None)` 过滤的**单一权威**（现状组件/proxy 各一份，靠隐式注册顺序契约对齐）。

L1 = `ReadbackMeshSync` 重建于 L0 之上，对外签名与 `CpuSemantic` 解包契约不变（`CSGpuMeshSave.cpp:221` 唯一调用点无感）。

### B1 sink 归一

- `FCSGpuMeshCPUData` = **叶子专属**统一快照（非反射，不进 BP/UFUNCTION）。
- sink A 已有（→ StaticMesh）。sink B 新增 `CSMeshBuild::BuildDynamicMeshFromGpuMeshData(Data, MaterialID, bReverseOrientation, bRecomputeNormals)` → `UDynamicMesh`：须带绕序翻转（vine 旧路 append A,C,B）、逐三角 MaterialID、退化索引过滤、法线重算开关（snorm8 解包法线 vs CPU 平滑法线是画质差异，须显式选择）。
- BP 边界：`FCSTriangleMeshData` / `FCSSurfaceVoxelData` 字段不动，加单向 converter 适配。

## 阶段计划

| 阶段 | 内容 | 验收门 |
| --- | --- | --- |
| P0a | 落地 L0（双侧 API + `FinalAccessForRole`）；L1 在 L0 上**字节等价**重建（flush 模式不变）；删死代码 readback | build 绿 + `DirectGPUMesh.SaveStaticMesh` / `RoadMesh.SaveStaticMesh` 双测试绿（此时 MeshGen 手写路径保留为独立 oracle） |
| P0b | flush 5→3 收敛（enqueue+block+lock 合入单渲染命令） | 同上双测试绿 |
| P1 | sink B + decode registry + `IsValid` optional-aware；MeshGen 三角汤/体素/landscape readback 迁 L0 + BP converter（**vine 旧路径冻结不迁**） | 双测试绿 + `CSDirectMeshSaveTests.cpp:55` 的 soup-vs-save parity 断言保持（两侧不得同时依赖同一 L0 字节数学，见风险） |
| P2·M1 | vine 叶子目视验证（**先回退 `VVVoxel.usf:1356` 的 DIAG V ramp**）：形状/拓扑/光照 vs 旧路，隐藏其一防 Z-fight | 编辑器目视 + 无 RDG 报错 |
| P2·M2 | GPU V-scan（分段扫描写 V 进叶子 TexCoord 流，替代 CPU `RecomputeVineOutputUVs`；环周长解析算，摆脱输出顶点依赖）；**新增叶子-旧路 parity 自动化测试**（位置/索引/UV 容差比对）作为出口门 | parity 测试绿（注意 legacy V 在 ProfileCount==3 才是有效基准） |
| P2·M3 | save 切换到叶子（**移植** `SaveStaticmesh` 的 AutoResult 命名 + tagged actor 生成/挂接/隐藏包装）；删旧 readback 三元组 + `BuildDynamicMeshFromGPUVineOutput` + `RecomputeVineOutputUVs` + SegmentMeta 重建；`SurfaceTargets` 统计与 `StageCenter` 调试画线单独处置（L0 debug 模式或明示放弃）；CPU prep 收敛为单份 bundle | 双 save 测试 + vine parity 测试绿 + 编辑器目视 |
| P3 | 剩余常量/打包格式/访问状态收进 `CSGpuMeshTypes.h` | build 绿 |

**vine 冻结规则**（评审最重要修正）：vine 旧 readback 与旧 build 是 M1/M2 的对照基准（旁路并行渲染即为此设计），**不进 P0/P1 迁移名单**——迁了既污染基准又在 M3 被整体删除，纯浪费。

## 已接受缺口（不在本轮修）

- 顶点色不回读、save 硬编码白色（`CSGpuMeshSave.cpp:143`）；vine 叶子写了 ColorUAV 但两侧都不消费，parity 成立。
- vine 叶子 `CastShadow=false`（间接绘制 vs VSM 的 GPU-Scene 限制）且无碰撞——替换 DynamicMesh 渲染后阴影/碰撞行为改变，需产品侧确认。
- 单材质槽、binormal sign 恒 1、碰撞恒 `CTF_UseComplexAsSimple`、单 UV 通道回读。

## 风险

- **oracle 独立性**：`CSDirectMeshSaveTests` 用 soup readback 对照 save 路径；若两侧同期迁 L0，共享字节数学 bug 会自我抵消。P1 迁移必须晚于 P0 全绿。
- proxy 裸指针跨渲染命令捕获仅因游戏线程全程阻塞而安全；任何 async 化都须改为句柄/重取。
- `GetMeshReadbackDescs` 与 `EnqueueMeshReadback` 的注册顺序契约是隐式的——L0 化时改为 (desc, readback) 成对传递，让错序在结构上不可能。
- unity/jumbo 构建同名符号坑（`CSMeshBuild` 的 `CSMB_` 前缀教训）：新共享 TU 里的 helper 必须唯一命名。
- vine 叶子全部工作**未提交**（插件仓库仅 clean 单提交 `5a05569`）；M1/M2 期间先落一个 checkpoint 提交。
- 体素 CPU sanitize 会压缩数组（CPU 计数 ≠ GPU 计数，`ComputeShaderMeshGenerator.cpp:3050-3122`）；GPU-resident 消费者看到的是未 sanitize 数据——迁移时不得静默删 sanitize。
- retained-pooled 写入靠 flush 栅栏保证竞态安全（`:2608-2613`）；L0 的 RT 半侧必须允许在拷贝 enqueue 旁执行调用者代码，保住该写入位置。

## Open Questions

- `CaptureLandscapeTrianglesGPU` 回读后仅作为 `InitialTriangleData` 重上传——直接做 GPU-resident 交接（类比 `LastSurfaceVoxelGPUBuffers`）删掉 readback，还是先迁 L0？
- 体素大数组 readback 是否降级为 lazy/debug-only（counter-only 快路），`StoreSurfaceVoxelTextureData` 与调试画线按需触发？
- 是否立项 `FCSTriangleSoupGPUBuffers` retained 镜像，让纹理数据路径与诊断共享一次 build？
- vine 阴影：接受无阴影，还是给叶子加非 indirect 的 shadow-only 绘制路径？
