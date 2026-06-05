# Hair Mesh Rendering — UE 插件开发计划

> 基于 SIGGRAPH 2024 论文 *"Real-Time Hair Rendering with Hair Meshes"* (Bhokare et al.)

---

## 一、论文核心思想

Hair Mesh 是一种体积结构：从头皮网格（scalp mesh）向外逐层挤出（extrude），形成若干 **bundle**。每个 bundle 内部通过重心坐标插值生成大量发丝（strand），再施加程序化造型（styling）产生发丝间的几何变化。

### 关键优势
| 对比项 | 传统 LHS（线性发丝蒙皮） | Hair Mesh 方案 |
|--------|--------------------------|----------------|
| 存储 | 300K 发丝 ≈ 340 MB | 5 张纹理 ≈ 39 KB |
| 渲染耗时 (RTX 4090) | 18 ms | 1.8 ms（无 LOD） |
| 动画 | 需逐帧更新 guide hair + 权重 | 仅更新 hair mesh 顶点 |

### 五张纹理
| 纹理 | 维度 | 内容 |
|-------|------|------|
| Hair Mesh Texture | 3D | 物体空间顶点位置（含 quadratic Bézier 中间切片） |
| UV Texture | 2D | 根层 uv 造型坐标 |
| W Texture | 3D | 各层 w 造型坐标 |
| U Direction Texture | 3D | 物体空间造型方向 û |
| V Direction Texture | 3D | 物体空间造型方向 v̂ |

### GPU 渲染管线（三级并行）
1. **Bundle 级** → Task Shader：决定每个 bundle 生成多少发丝、LOD 剔除
2. **Strand 级** → Mesh Shader：每个线程组处理一根发丝
3. **Vertex 级** → SIMD 线程：每个线程计算 n 个顶点（采样纹理 + 造型扰动）

### LOD 策略
- **Strand LOD**：用渐进式蓝噪声样本集逐步减少发丝数量，同时增大发丝宽度补偿
- **Vertex LOD**：顶点数保持 2^e + 1，通过 morphing 平滑过渡

---

## 二、UE 插件架构

```
HairMeshRendering/
├── HairMeshRendering.uplugin
├── PLAN.MD
├── Content/
├── Shaders/
│   └── Private/
│       ├── HairMeshCommon.usf         # 数据结构、Bézier 求值、纹理采样、Hash
│       ├── HairMeshStyling.usf        # Curl/Wave/Frizz/Clump + 5 种衰减曲线
│       ├── HairMeshGeneration.usf     # Compute Shader: 发丝生成 + LOD + 造型
│       └── HairMeshRaster.usf         # VS/PS: 双高光 Kajiya-Kay 着色
└── Source/
    ├── HairMeshRendering/             # Runtime 模块
    │   ├── HairMeshRendering.Build.cs
    │   ├── Private/
    │   │   ├── HairMeshRenderingModule.cpp
    │   │   ├── HairMeshAsset.cpp
    │   │   ├── HairMeshTextureBuilder.cpp  # 5 张纹理 CPU 构建（完整实现）
    │   │   ├── HairMeshComponent.cpp
    │   │   ├── HairMeshSceneProxy.cpp      # GPU 资源 + RDG 调度
    │   │   ├── HairMeshShaders.cpp
    │   │   └── HairMeshStylingParams.cpp
    │   └── Public/
    │       ├── HairMeshRenderingModule.h
    │       ├── HairMeshAsset.h
    │       ├── HairMeshTextureBuilder.h
    │       ├── HairMeshComponent.h
    │       ├── HairMeshSceneProxy.h
    │       ├── HairMeshShaders.h           # CS/VS/PS Global Shader 声明
    │       └── HairMeshStylingParams.h
    └── HairMeshRenderingEditor/       # Editor 模块
        ├── HairMeshRenderingEditor.Build.cs
        ├── Private/
        │   ├── HairMeshRenderingEditorModule.cpp
        │   ├── HairMeshImporter.cpp        # OBJ + SimpleFormat 导入
        │   └── HairMeshAssetFactory.cpp    # 资产创建 + .obj 导入工厂
        └── Public/
            ├── HairMeshRenderingEditorModule.h
            ├── HairMeshImporter.h
            └── HairMeshAssetFactory.h
```

---

## 三、分阶段开发计划

### Phase 0：基础骨架 ✅
**目标**：插件可编译、可加载

- [x] 创建插件目录结构和 `.uplugin`
- [x] `HairMeshRendering.Build.cs` — 模块依赖声明
- [x] `HairMeshRenderingModule.h/.cpp` — 模块启动/关闭、注册 Shader 目录
- [x] `HairMeshAsset`、`HairMeshComponent`、`HairMeshStylingParams` 类声明
- [x] `HairMeshSceneProxy`、`HairMeshTextureBuilder` 骨架

### Phase 1：Hair Mesh 资产与纹理构建 ✅
**目标**：能从网格数据生成 5 张 GPU 纹理

- [x] `UHairMeshAsset`
  - 存储：scalp mesh 拓扑（顶点、面、UV）、extrusion 层数据、每层顶点位置
  - 序列化：标准 UObject 序列化
  - 编辑器预览：显示 hair mesh wireframe
- [x] `FHairMeshTextureBuilder`
  - 从 `UHairMeshAsset` 构建 5 张纹理
  - Root slice 布局：每个 quad face 占 2×2 texel，triangle 复制一个顶点
  - Catmull-Rom → cubic Bézier → quadratic Bézier 转换
  - 中间切片插入（每对 layer slice 之间 3 个中间切片）
  - 计算每个顶点的 û、v̂ 方向（Eq.1-4，面积加权平均）
  - 输出：CPU 端 FLinearColor 数组 → RHI 纹理上传
- [ ] `FHairMeshImporter`
  - 从 FBX/OBJ 导入 scalp mesh + extrusion 层
  - 自动识别层结构（按命名约定或顶点组）
  - 验证拓扑合法性（quad/tri only）

### Phase 2：GPU 发丝生成管线 ✅
**目标**：在 GPU 上实时生成并光栅化发丝

- [x] **方案选择：Mesh Shader vs Compute Shader**
  - UE 5.7 的 Mesh Shader 支持情况评估
  - 若 Mesh Shader 不可用，退回 Compute Shader + IndirectDraw
  - 推荐：先用 Compute Shader 实现（兼容性好），后续迁移 Mesh Shader

- [x] `HairMeshGeneration.usf`
  - Halton 序列发根放置（渐进式低差异序列）
  - 3D 纹理采样生成发丝控制点（trilinear → quadratic Bézier 求值）
  - 程序化造型：Curl / Wave / Frizz（在 Compute Shader 中实现）
  - 输出：camera-facing triangle strip（每根发丝 2·VertsPerStrand 顶点）
  - Tangent 计算：Eq.5-6 端点反射
  - SIMD 对齐：THREADGROUP_SIZE_X = 64

- [x] `HairMeshRaster.usf` — VS/PS，Kajiya-Kay 简化着色

- [x] `FHairMeshSceneProxy`
  - 继承 `FPrimitiveSceneProxy`
  - RDG Pass 注册：在 BasePass 或自定义 Pass 中调度 Compute/Mesh Shader
  - 管理 GPU Buffer：顶点输出、索引输出、indirect args
  - 动态更新 Hair Mesh Texture（动画时）

### Phase 3：程序化造型系统 ✅
**目标**：通过参数控制发丝变化

- [x] `FHairMeshStylingParams`
  - 参数集：curl（卷曲）、wave（波浪）、frizz（毛躁）、clump（聚簇）、length variation
  - 每个参数：幅度、频率、随机种子、沿发丝衰减曲线
  - 支持 per-bundle override

- [ ] `HairMeshStyling.usf`
  - uvw 坐标系计算：采样 UV/W/U/V Texture，构建 M_p* 变换矩阵
  - 造型扰动在纹理空间计算，幅度在物体空间定义（不随 mesh 缩放）
  - 各造型函数独立可叠加
  - ˆw = normalize(û × v̂) 保证正交

- [ ] `HairMeshCommon.usf`
  - 纹理采样辅助函数
  - Bézier 求值函数
  - 坐标变换工具

### Phase 4：LOD 系统 ✅
**目标**：远距离自动降低复杂度，平滑过渡

- [x] **Strand LOD**
  - 渐进式蓝噪声样本集（Sample Elimination 生成）
  - 按 L = min(1, hR/(d·tan(θ)·α)) 计算细节级别
  - 每个 bundle 加随机偏移 δ 避免同步跳变
  - 发丝宽度补偿：thickness × min(1/L, N_i)

- [ ] **Vertex LOD**
  - 顶点数始终为 2^e + 1
  - Morphing 过渡：奇数索引顶点向相邻偶数顶点中心移动（Eq.11）
  - 切线方向同步 morphing（两套切线混合）
  - 过渡窗口参数 λ 控制平滑度

- [ ] Task Shader / CPU 预处理
  - 每个 bundle 独立计算 LOD 级别
  - 输出 indirect dispatch/draw args

### Phase 5：着色与集成 ✅
**目标**：视觉效果达到可用水准

- [x] `HairMeshRaster.usf`（原 HairMeshShading.usf，合并为 VS/PS）
  - Marschner 简化模型（R + TT + TRT）
  - 切线方向：相邻顶点差分（Eq.5-6 处理端点）
  - MSAA 支持（8× MSAA）
  - 透明度排序 / OIT 策略

- [ ] UE 渲染集成
  - 与 UE 光照系统对接（方向光、点光、IBL）
  - 阴影：Deep Opacity Map 或简化方案
  - 运动模糊支持
  - 与 Groom 系统的兼容性评估

### Phase 6：编辑器工具 ✅
- [x] `HairMeshRenderingEditor` 编辑器模块
- [x] `UHairMeshImporter` — OBJ 导入 + SimpleFormat 导入
- [x] `UHairMeshAssetFactory` — Content Browser 右键创建资产
- [x] `UHairMeshAssetImportFactory` — .obj 文件拖拽导入
- [ ] Hair Mesh 资产编辑器（可视化 bundle 结构 + 造型预览）— 后续扩展
- [ ] 从现有 Groom 资产转换 — 后续扩展

---

## 四、技术风险与决策点

| 风险 | 影响 | 缓解策略 |
|------|------|----------|
| UE 5.7 Mesh Shader API 不完善 | 无法用 Task/Mesh Shader | 先用 Compute Shader + Indirect Draw 实现 |
| 3D Volume Texture 动态更新性能 | 动画帧率下降 | 仅更新变化的切片；考虑 Compute Shader 写入 |
| 造型函数高频 → 顶点不足 | 视觉瑕疵 | Vertex LOD 保证足够的最小顶点数 |
| 透明排序（OIT） | 渲染伪影 | 评估 UE 已有的 Hair OIT 方案 |
| Hair Mesh 导入工作流 | 美术难以使用 | 先支持简单 OBJ 格式，后续对接 DCC 导出器 |

---

## 五、参考资源

- **论文**: Bhokare et al., "Real-Time Hair Rendering with Hair Meshes", SIGGRAPH 2024
- **Hair Mesh 原始论文**: Yuksel et al., "Hair Meshes", SIGGRAPH Asia 2009
- **Quadratic Bézier 近似**: Truong et al., "Quadratic Approximation of Cubic Curves", HPG 2020
- **蓝噪声采样**: Yuksel, "Sample Elimination for Generating Poisson Disk Sample Sets", EG 2015
- **UE 参考**: Epic Games Groom 系统源码、Niagara Mesh Shader 示例
