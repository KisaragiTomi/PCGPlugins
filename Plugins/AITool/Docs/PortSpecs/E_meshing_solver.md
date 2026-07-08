# E. 对偶 Marching Cubes 网格提取与 PCG 稀疏求解器移植规格

本规格覆盖 NKSR 推理路径中的两个模块，供纯 C++ (CPU) 移植使用，移植工程师不阅读原始源码：

1. `field.extract_dual_mesh(mise_iter=1)` 全流程（dual grid 构建、dual cube graph、MISE 细化、Marching Cubes、trim）。
2. `sparse_solve.solve_pcg` CPU 求解器（symmetric-block Jacobi-PCG）及其 CSR 辅助函数。

源码依据（本文所有行为均直接取自这些文件）：

- `NKSR/package/csrc/meshing/meshing.h`、`grid_builder.h`、`inds_cpu.cpp`、`mc_cpu.cpp`、`mc_data.h`
- `NKSR/package/csrc/common/iter_util.h`
- `NKSR/package/csrc/sparse_solve/bind.cpp`、`solve_cpu.cpp`
- `NKSR/package/nksr/fields/base_field.py`、`nksr/meshing.py`、`nksr/solver.py`、`nksr/utils.py`、`nksr/fields/kernel_field.py`、`nksr/fields/layer_field.py`、`nksr/configs.py`

不移植：`extract_primal_mesh`、CUDA 路径、MarchingCubes/PCGSolver 的 backward（autograd）、颜色场（`texture_field`，推理路径上为 `None`）、`grid_upsample > 1` 分支（默认 1）、chunked 重建。

## 0. 调用链与超参

```python
# nksr/__init__.py reconstruct() + base_field.py，ks.pth 配置
# hparams(ks): voxel_size=0.1, tree_depth=4, adaptive_depth=2, udf.enabled=True,
#              solver.pos_weight=1e4, solver.normal_weight=1e4
field = reconstructor.reconstruct(xyz, normal, detail_level=D)
#   -> KernelField.solve(...)                 # 内部最终调用 solve_pcg（见 §11）
#   -> field.set_mask_field(NeuralField(udf_svh, udf_decoder))   # ks: udf.enabled=True
#      mask_field.set_level_set(2 * hparams.voxel_size)          # = 0.2
#   -> field.set_scale(global_scale)          # detail_level 决定，见任务 A/F 规格
mesh = field.extract_dual_mesh(mise_iter=1)   # grid_upsample=1, max_depth=100,
                                              # trim=True, max_points=-1
```

PCG 求解器超参（`reconstruct` 默认值，经 `KernelField.solver_config` 传入）：

```text
solver_max_iter = 2000      # max_iter
solver_tol      = 1.0e-5    # tol（相对残差）
res_fix         = False     # solver.py _solve 固定传 False
verbose         = False
```

## 1. 通用约定：稀疏体素网格与坐标

### 1.1 网格变换

每层网格 `SparseIndexGrid` 由 `(voxelSize, voxelOrigin)` 两个 double 标量定义（各向同性、三轴同 origin）。SVH 第 `d` 层（`d=0` 最细）：

```text
voxelSize(d)  = voxel_size * 2^d          # ks: voxel_size = 0.1
voxelOrigin(d)= 0.5 * voxel_size * 2^d    # svh.py get_grid_voxel_size_origin
```

坐标映射（`SparseFeatureIndexGrid::setTransform`，double 精度计算）：

```text
world_to_grid: g = (w - voxelOrigin) / voxelSize      # 逐分量
grid_to_world: w = g * voxelSize + voxelOrigin
```

整数体素坐标 `ijk` 的**体素中心**位于 `grid_to_world(ijk) = (ijk + 0.5) * voxel_size * 2^d`（因 origin 为半格偏移），体素角点对齐 `voxelSize` 的整数倍。`points_in_active_voxel(p)` 的判定：`round(world_to_grid(p))` 逐分量四舍五入到最近整数坐标后查活跃性（`PointsInGrid.cpp`）。

### 1.2 体素索引（voxel index）

每个网格给所有活跃体素分配 0 基连续索引。原实现由 nanovdb ValueIndex grid 定义（`voxelIndex(acc, ijk) = acc.getValue(ijk) - 1`，非活跃返回 `-1`），迭代顺序为 nanovdb 树序。

**移植约定**：C++ 端自定义一个确定性顺序即可（推荐：按 `(i, j, k)` 字典序升序，i 为主键、k 变化最快），但同一网格的索引约定必须在整条管线内全局一致——`active_grid_coords()` 第 `n` 行、特征数组第 `n` 行、解向量第 `n` 个元素、graph 构建、`ijk_to_index` 全部指同一个体素。索引顺序改变只影响输出顶点/三角形的排列次序，不影响网格几何。实现建议：`TMap<FIntVector, int64>`（ijk→index）+ `TArray<FIntVector>`（index→ijk）。

### 1.3 数据类型

| 数据 | 类型 |
| --- | --- |
| 体素整数坐标 | `int32` 三元组 |
| 世界坐标 / SDF 值 / 顶点 | `float32`（变换系数用 double 算再转 float） |
| graph、face、顶点索引 | `int64` |
| CSR `a_p` / `a_j` | `int32` |
| CSR `a_x`、向量 `b`、`inv_diag` | `float32` |

## 2. extract_dual_mesh 总流程

`base_field.py extract_dual_mesh`，直译伪代码（`depth = svh.depth = 4`，`max_depth=100` 不起裁剪作用）：

```cpp
// 1) 每层压平（flatten）
TArray<Grid> flattened;                        // 长度 = depth
for (int d = 0; d < depth; ++d)
    flattened.Add(BuildFlattenedGrid(svh.grids[d],
        d > 0 ? &svh.grids[d - 1] : nullptr,   // childGrid = 更细一层
        /*conforming=*/ d != depth - 1));      // 最粗层 non-conforming

// 2) 联合对偶格 + 对偶 cube graph
Grid dualGrid = BuildJointDualGrid(flattened);              // §4
Tensor dmcGraph = DualCubeGraph(flattened, dualGrid);       // (M,8) int64, §5

// 3) DMC 顶点 = 各层活跃体素中心（跳过空层，顺序 d=0..depth-1）
Tensor dmcVertices = ConcatOverNonEmpty(flattened,
    [](Grid& g){ return GridToWorld(g, g.ActiveGridCoordsFloat()); });   // (P,3) float
if (field.scale != 1.0) dmcVertices *= field.scale;

// 4) 采样 SDF：f_bar(x) = f(x / scale) - level_set（主场 level_set = 0）
Tensor dmcValue = EvaluateFBar(field, dmcVertices);         // (P,) float

// 5) MISE 细化 mise_iter 次（默认调用为 1 次），§7
for (int it = 0; it < miseIter; ++it)
    MiseRefine(dmcGraph, dmcVertices, dmcValue, field);

// 6) Marching Cubes，§8
auto [V, F] = MarchingCubesCPU(dmcGraph, dmcVertices, dmcValue);

// 7) trim：mask_field.f_bar(V) < 0 的顶点保留，§9
if (trim) ApplyVertexMask(V, F, EvaluateFBar(maskField, V) < 0.f);

return { V, F };   // mesh.v (V,3) float32, mesh.f (T,3) int64
```

要点：`dmcGraph` 每行是一个 MC cube，8 列存 `dmcVertices` 的行号；cube 的 8 个角样本就是 primal 体素中心的 `(位置, SDF 值)`。

## 3. buildFlattenedGrid（每层压平）

输入：`thisGrid`（第 `d` 层）、可选 `childGrid`（第 `d-1` 层，更细）、`conforming`。输出新网格，`voxelSize/voxelOrigin` 与 `thisGrid` 相同。语义：把"更细层已经覆盖"的体素从本层剔除，使每个空间区域只由最细的活跃层代表；非最粗层再做八分体（octant）补全，保证与粗一层对齐。

```cpp
// childMask[vi]: thisGrid 第 vi 个体素是否含活跃 child
TArray<bool> childMask; childMask.SetNumZeroed(thisGrid.NumVoxels());
if (childGrid)
    for ((coord v, int64 vi) : thisGrid.ActiveVoxels())
        for (coord c : OctChildren(v * 2))          // c ∈ v*2 + {0,1}^3，见下
            if (childGrid->IsActive(c)) { childMask[vi] = true; break; }

TSet<coord> newVoxels;
if (!conforming) {
    // 最粗层：仅移除有 child 的体素
    for ((coord v, int64 vi) : thisGrid.ActiveVoxels())
        if (!childMask[vi]) newVoxels.Add(v);
} else {
    // 其它层：对每个活跃体素，加入其所在八分体的全部 8 个兄弟
    for ((coord v, int64 vi) : thisGrid.ActiveVoxels())
        for (coord s : OctChildren(v)) {            // 八分体 = floor-even(v) + {0,1}^3
            int64 si = thisGrid.IjkToIndex(s);      // 非活跃返回 -1
            if (si == -1 || !childMask[si]) newVoxels.Add(s);
        }
}
return Grid(newVoxels, thisGrid.voxelSize, thisGrid.voxelOrigin);
```

`OctChildren(p)` 枚举（`iter_util.h OctChildrenIterator`）：

```cpp
// center 每分量向下取偶：cx = (px >> 1) << 1（算术右移，负数也向下取整）
// count = 0..7: dz = count % 2; dy = (count / 2) % 2; dx = count / 4
// 产出 center + (dx, dy, dz)，即枚举顺序 z 最快、x 最慢
```

注意：conforming 分支会**新增**原网格中不存在的体素（补全八分体），这是有意的。

## 4. buildJointDualGrid（联合对偶格）

输入：全部 `flattened` 层（层 0 必须存在，取其 `voxelSize/voxelOrigin` 记作 `vs0/org0`；ks 中 `org0 = 0.5*vs0`）。输出对偶网格：

```text
dualVoxelSize   = vs0
dualVoxelOrigin = org0 - 0.5 * vs0      # ks 中 = 0，对偶格点落在 primal 角点上
```

算法（`grid_builder.h buildJointDualGrid`）：对每层 `l` 的每个活跃体素 `ijk`，把它的 8 个角投到对偶格并激活：

```cpp
TSet<coord> dualVoxels;
for (int l = 0; l < depth; ++l)
    for (coord ijk : flattened[l].ActiveVoxels())
        for (int ax : {-1, +1}) for (int ay : {-1, +1}) for (int az : {-1, +1}) {
            dvec3 w  = (dvec3(ijk) + 0.5 * dvec3(ax, ay, az)) * vs_l + org_l; // 角点世界坐标
            coord dc = Round((w - dualOrigin) / vs0);                          // 逐分量 round
            dualVoxels.Add(dc);
        }
```

几何结论（用于自检）：层 `l` 体素 `ijk` 的 8 个角映射到对偶坐标 `2^l * ijk + {0, 2^l}^3`，即 stride `2^l` 的格点。对偶格只含各层体素的**角**格点；粗体素面/棱内部的格点只有在相邻细层体素贡献时才活跃——这正是多分辨率 DMC 的自适应来源。

## 5. dualCubeGraph（对偶 cube graph）

输出 `(M, 8) int64`：每个对偶格点（= 一个 MC cube）的 8 个角槽位（slot），值为 primal 体素的**全局索引**（各层体素索引按层拼接：层 `l` 的体素 `vi` 全局索引 = `baseIdx[l] + vi`，`baseIdx` 是各非空层 `numVoxels` 的前缀和，层序 0..depth-1，空层/缺层跳过且不计入）。与 §2 第 3 步 `dmcVertices` 的拼接顺序严格一致。

槽位语义：对偶格点 `D` 的 cube 覆盖世界区间 `[(D-0.5)*vs0, (D+0.5)*vs0] + dualOrigin`；slot `k` 对应角 `cubeRelTable[k]`（§12.1），即位于 `D + cubeRelTable[k] - (1,1,1)*0.5`（对偶坐标）处的 primal 体素中心。

```cpp
Tensor graph = Full({nCorner, 8}, -1, int64);   // nCorner = dualGrid.NumVoxels()
int64 baseIdx = 0;
for (int l = 0; l < depth; ++l) {
    if (flattened[l].NumVoxels() <= 0) continue;
    int Stride = 1 << l;                        // 仅实例化 1/2/4/8，l>3 抛错
    for ((coord p, int64 pi) : flattened[l].ActiveVoxels())
        for (auto it = CubeFaceIterator(p * Stride, Stride); it.IsValid(); ++it) {
            int64 vi = dualGrid.IjkToIndex(it.Coord());
            if (vi == -1) continue;
            for (int a = 0; a < it.AccCount(); ++a)
                graph[vi][it.AccInds(a)] = pi + baseIdx;
        }
    baseIdx += flattened[l].NumVoxels();
}
// 只保留 8 个槽位全部填满的行（行序保持不变）
return graph.RowsWhere([](row r){ return None(r == -1); });
```

`CubeFaceIterator(base, Stride)` 枚举 stride 立方体表面的全部对偶格点，并给出该格点上本 primal 体素占据的槽位（`iter_util.h`）。枚举顺序：先 8 个角、再 12 条棱各 `Stride-1` 个内点、再 6 个面各 `(Stride-1)^2` 个内点。`Stride=1` 时只有 8 个角。

```cpp
// 角（count = 0..7）：
//   coord = base + cornerAxisTable[count] * Stride;  槽位数 1
//   slot  = cornerAccIndsTable[count]
// 棱（idx = count-8; edgeIdx = idx/(Stride-1); inner = idx%(Stride-1)）：
//   coord[edgeAxisTable[e][2]] = edgeAxisTable[e][0] * Stride;
//   coord[edgeAxisTable[e][3]] = edgeAxisTable[e][1] * Stride;
//   coord[edgeAxisTable[e][4]] = inner + 1;   coord += base;  槽位数 2
//   slots = edgeAccIndsTable[e][0..1]
// 面（idx = count-8-12*(Stride-1); faceIdx = idx/((Stride-1)^2); inner = idx%((Stride-1)^2)）：
//   coord[faceAxisTable[f][1]] = faceAxisTable[f][0] * Stride;
//   coord[faceAxisTable[f][2]] = inner % (Stride-1) + 1;
//   coord[faceAxisTable[f][3]] = inner / (Stride-1) + 1;
//   coord += base;  槽位数 4;  slots = faceAccIndsTable[f][0..3]
```

八个方向表见 §12.2，必须逐字复制。填充顺序说明：同一 `graph[vi][slot]` 可能被不同层多次写入（细层在前、粗层在后），**后写覆盖先写**——保持层循环 `l` 升序即可复现原行为。

## 6. DMC 顶点与 SDF 采样

- `dmcVertices`：各非空 `flattened` 层（升序）活跃体素中心的世界坐标（float32），`w = (ijk + 0.5) * vs_l`（ks 的 origin 约定下）。行序 = 全局体素索引序（§5 的 `baseIdx` 约定）。
- scale：`field.scale = global_scale`（由 `detail_level` 换算，见任务 A/F）。顶点先乘 `scale` 得到输入点云坐标系；`evaluate_f_bar` 内部再除以 `scale` 回到模型坐标系后求场值，最后减 `level_set`（主场为 0）。
- 场值 `f` 的计算（kernel evaluation）属于其它任务的规格；本模块只把它当作黑盒 `float f_bar(vec3 world)`。

## 7. MISE 细化（`utils.py subdivide_cube_indices`）

每次迭代：先筛出跨零 cube，再压缩顶点表，然后每 cube 一分为八，最后对**全部新顶点表**重新求场值。

```cpp
// (a) 筛选：cube 8 角符号不全同才保留（> 0 为正，<= 0 记为负侧）
BitArray sign  = dmcValue.Gather(dmcGraph) > 0;              // (M,8)
BitArray keep  = !(AllTrue(sign, dim=1) || AllFalse(sign, dim=1));
dmcGraph = dmcGraph[keep];

// (b) 顶点压缩：unq = 图中出现过的顶点索引，升序去重；graph 重映射为 unq 中的位置
auto [unq, inv] = UniqueSorted(dmcGraph.Flatten());          // inv 与原元素一一对应
dmcGraph    = inv.View({-1, 8});
dmcVertices = dmcVertices.IndexSelect(unq);

// (c) 细分
[dmcGraph, dmcVertices] = SubdivideCubeIndices(dmcGraph, dmcVertices);

// (d) 重新采样（顶点均为纯位置函数，可对旧顶点复用缓存值，数值等价）
dmcValue = EvaluateFBar(field, dmcVertices);
```

### 7.1 SubdivideCubeIndices

输入 `graph (M,8) int64`、`verts (P,3) float`。输出 `(8M, 8)` 与新顶点表。思路：每个 cube 按角 0..7 生成 8 个子 cube；子 cube 在角 `i` 处，其角 `j` 的顶点索引记 `NG[i][j]`（每项是长度 M 的列向量）。共享的棱中点/面心跨 cube 去重。

```text
角点:  NG[v][v] = graph[:, v]                    (v = 0..7)

棱组（3 组，每组 4 条平行棱，顺序必须逐字保留）:
  组1: [0,4] [1,5] [3,7] [2,6]
  组2: [0,1] [3,2] [4,5] [7,6]
  组3: [0,3] [1,2] [4,7] [5,6]
每组处理:
  pairs = concat_按组内棱序( stack(graph[:,e0], graph[:,e1], dim=1) )   # (4M, 2)，保持 (e0,e1) 原方向
  (uniqPairs, invMap) = UniqueRowsSorted(pairs)      # 行字典序升序去重 + 逆映射
  mid = (verts[uniqPairs[:,0]] + verts[uniqPairs[:,1]]) / 2
  deg = uniqPairs[:,0] != uniqPairs[:,1]             # 退化棱（两端同点）不加新顶点
  追加 mid[deg] 到顶点表；newIdx[r] = deg[r] ? (顶点表内新位置) : uniqPairs[r,0]
  perElem = newIdx[invMap]，按棱序均分 4 段；对第 g 条棱 [e0,e1]:
      NG[e0][e1] = NG[e1][e0] = 段 g

面组（3 组，每组 2 个对面，角环顺序必须逐字保留）:
  组1: [0,1,5,4] [3,2,6,7]
  组2: [1,2,6,5] [0,3,7,4]
  组3: [0,1,2,3] [4,5,6,7]
每组处理:
  quads = concat( stack(graph[:,c0..c3], dim=1) )    # (2M, 4)
  (uniqQuads, invMap) = UniqueRowsSorted(quads)
  ctr = (v[c0]+v[c1]+v[c2]+v[c3]) / 4
  deg = !(四列全相等)；追加 ctr[deg]；newIdx 同棱的规则
  perElem 均分 2 段；对面 [f0,f1,f2,f3]（段 g）:
      for i in 0..3: NG[f[i]][f[(i+2) % 4]] = 段 g   # 子 cube 在 f[i]，对角写入

体心: ctr8 = (Σ_{i=0..7} verts[graph[:,i]]) / 8，全部追加为新顶点（不去重不判退化）
  对 cur = 0..7, diag = {6,7,4,5,2,3,0,1}[cur]:  NG[cur][diag] = 体心索引列

输出: newGraph = 按 i=0..7 依次拼接 stack(NG[i][0..7], dim=1)  →  (8M, 8)
      newVerts = [旧顶点, 棱组1, 棱组2, 棱组3, 面组1, 面组2, 面组3, 体心]  按此顺序拼接
```

关键细节：

- `UniqueRowsSorted` 语义 = `torch.unique(dim=0, return_inverse=True)`：输出行按字典序升序，`invMap[i]` 给出输入第 `i` 行在输出中的位置。
- 棱方向一致性：同一空间棱在相邻 cube 里的 `(e0,e1)` 索引对方向相同（每组内 4 条棱同向），因此不需排序即可去重；**不要**擅自对 pair 排序（虽然多数情况下无害，但保持原样最安全）。
- 退化（塌缩）棱/面来自多层 DMC 中粗细混合 cube，必须按上面的规则复用旧顶点索引。

## 8. MarchingCubesCPU

输入：`cubeCornerInds (M,8) int64`、`cornerPos (N,3) float32`、`cornerValue (N,) float32`。输出：`vertices (V,3) float32`、`faces (T,3) int64`（另有 `vertIdx` 仅供 backward，不移植）。

### 8.1 cube type 与角/棱约定

角 `i` 的相对位置 = `cubeRelTable[i]`（§12.1，标准 Bourke 顺序：0..3 底环、4..7 顶环）。

```cpp
int cubeType = 0;                                  // bit i 置位 ⟺ sdf[i] < 0（严格小于）
for (int i = 0; i < 8; ++i) if (sdf[i] < 0) cubeType |= (1 << i);
```

棱 0..11 的端点对（插值方向 `p1 → p2`，`fill_vert_list` 顺序，逐字保留）：

```text
edge:  0      1      2      3      4      5      6      7      8      9      10     11
pair: (0,1)  (1,2)  (2,3)  (3,0)  (4,5)  (5,6)  (6,7)  (7,4)  (0,4)  (1,5)  (2,6)  (3,7)
```

### 8.2 棱上顶点插值（`sdf_interp`）

```cpp
vec3 SdfInterp(vec3 p1, vec3 p2, float v1, float v2) {
    if (fabsf(v1) < 1.0e-5f) return p1;            // 端点即零点
    if (fabsf(v2) < 1.0e-5f) return p2;
    if (fabsf(v1 - v2) < 1.0e-5f) return p1;       // 退化：取 p1
    float w2 = (0.f - v1) / (v2 - v1);
    return p1 * (1.f - w2) + p2 * w2;              // 线性插值，逐分量
}
```

### 8.3 主流程

```cpp
// pass 1: 每 cube 的输出顶点数（= 3 * 三角形数）
for (int64 c = 0; c < M; ++c) {
    float sdf[8]; for (int i = 0; i < 8; ++i) sdf[i] = cornerValue[inds[c][i]];
    vertCount[c] = numVertsTable[GetCubeType(sdf)];
}
// exclusive prefix sum: base[c] = Σ_{k<c} vertCount[k]；总三角形数 T = Σ/3
// pass 2: 生成三角形
for (int64 c = 0; c < M; ++c) {
    读取 8 角的 pointIds/sdf/pos；int ct = GetCubeType(sdf);
    int edgeCfg = edgeTable[ct];
    if (edgeCfg == 0) continue;
    vec3 vertList[12];
    for (int e = 0; e < 12; ++e)
        if (edgeCfg & (1 << e))
            vertList[e] = SdfInterp(pos[pair[e].a], pos[pair[e].b],
                                    sdf[pair[e].a], sdf[pair[e].b]);
    for (int i = 0; triangleTable[ct][i] != -1; i += 3) {
        int64 tri = base[c] / 3 + i / 3;
        for (int vi = 0; vi < 3; ++vi) {
            int e = triangleTable[ct][i + vi];
            triPos[tri][vi] = vertList[e];                     // 顶点位置
            int64 a = pointIds[e2iTable[e][0]];                // 合并键：棱的两个全局角索引
            int64 b = pointIds[e2iTable[e][1]];
            if (a < b) Swap(a, b);                             // 保证 a >= b（降序）
            triKey[tri][vi] = { a, b };
        }
    }
}
```

### 8.4 顶点合并

```cpp
// 对 (T*3, 2) 的 triKey 做行去重（字典序升序，语义同 torch.unique_dim(dim=0)）
// uniqKeys: (V,2)；invMap: (T*3,) 每个三角形角 → 顶点 id
faces    = invMap.View({T, 3});
vertices = Zeros({V, 3});
for (int64 t = 0; t < T * 3; ++t) vertices[invMap[t]] = triPos.Flat3()[t];  // 重复覆盖
```

说明：同一空间棱在相邻 cube 中的插值方向相反，位置在代数上相同、浮点上可能差 1 ulp；"最后写入者胜"即可，与原实现一致（原实现用 `index_put_`）。三角形三顶点顺序按 `triangleTable` 给出的次序输出，绕向由查表保证全网格一致（内部 = `f < 0` 一侧）。

`e2iTable` 与 §8.1 的插值端点表几乎相同，但 **edge 10 为 `{6,2}`**（插值表为 `(2,6)`）——由于合并键随后做了降序规整，语义等价，但按 §12.1 逐字复制即可避免歧义。

边界情况：`M == 0` 或 `T == 0` 时返回空 `vertices/faces`。

## 9. 网格修剪（trim）

`vert_mask = mask_field.evaluate_f_bar(V) < 0.0`（`< 0` 保留）。ks.pth 配置 `udf.enabled=True`，mask field 为 `NeuralField(udf_svh, udf_decoder)` 且 `level_set = 2 * voxel_size = 0.2`，其求值属于神经场任务的规格；若移植目标配置 `udf.enabled=False`，则 mask 为 `LayerField`：

```cpp
// layer_field.py：inside_depth = hparams.adaptive_depth（ks = 2）
// f(x) = grids[inside_depth-1].points_in_active_voxel(x) ? -1 : +1   （§1.1 round 判定）
// f_bar = f - 0（LayerField 未设 level_set）
```

`apply_vertex_mask`（`utils.py`）：

```cpp
map[i] = keep[i] ? (保留顶点中的新序号) : -1;      // 序号按原顶点顺序压缩
V = V[keep];
F = F.Map(map);                                    // 逐元素重映射
F = F.RowsWhere(全部 3 个索引 != -1);               // 任一顶点被删则删除整个三角形
```

## 10. 输出坐标空间

`mesh.v` 为 float32 世界坐标，与用户输入 `xyz` 同一坐标系（`extract_dual_mesh` 中顶点已乘 `field.scale`；MC 插值不改变坐标系）。`mesh.f` 为 `(T,3) int64`，0 基顶点索引。

## 11. PCG 稀疏求解器（solve_pcg_cpu）

### 11.1 矩阵组织（`solver.py SparseMatrix` + `solve_cpu.cpp`）

线性系统 `A x = b`，`A` 为对称正定，按 SVH 深度分块（ks: `n_block = 4`）。仅存**上三角块** `(di, dj), di <= dj`；对角块 `(d,d)` 本身是完整对称子矩阵、只存一份。每块为独立 CSR：

```text
block_size[d] = grids[d].num_voxels               # 缺层为 0
block_ptr[d]  = Σ_{k<d} block_size[k]             # 长度 n_block+1 的前缀和
块 (di,dj): 尺寸 (block_size[di], block_size[dj])
  a_p: (rows+1,) int32   # CSR 行指针，由 ind2ptr(a_i_coo, rows) 得到
  a_j: (nnz,)   int32    # 列索引（块内局部索引）
  a_x: (nnz,)   float32  # 值
```

COO→CSR 的输入 `a_i`（行索引）按行主序升序排列（来自 `torch.where` 的行主扫描，见 `kernel_field.py`；矩阵装配属任务 D 的规格）。`ind2ptr`（`solve_cpu.cpp dispatch_ind2ptr_cpu`）：

```cpp
// 输入 ind: 升序行索引 (E,)；输出 ptr: (M+1,)
if (E == 0) { ptr 全 0; return; }
for (i = 0; i <= ind[0]; ++i) ptr[i] = 0;
for (i = 0; i < E - 1; ++i)
    for (idx = ind[i]; idx < ind[i + 1]; ++idx) ptr[idx + 1] = i + 1;
for (i = ind[E - 1] + 1; i <= M; ++i) ptr[i] = E;
```

（`ptr2ind` 为逆操作：`out[e] = i for e in [ptr[i], ptr[i+1])`，本路径仅 QGMatrix 非融合分支使用，融合模式 `fused_mode=True` 默认路径不需要。）

Jacobi 预条件子：每个对角块取 `inv_diag_blk = 1.0f / a_x[a_i == a_j]`（COO 中行==列的元素，天然按行升序、每行恰一个），再按 `d = 0..n_block-1` 顺序拼接成 `(N,) float32` 的 `inv_diag_A`。右端 `b`：各层 rhs 向量按 `d = 0..n_block-1` 拼接（缺层跳过）。解向量 `x` 同序拆回各层。

### 11.2 对称分块乘 `y = A * v`

```cpp
void SymblkMatmul(Vec& y, const Vec& v) {
    y.SetZero();
    for (int i = 0; i < nBlock; ++i)
        for (int j = 0; j < nBlock; ++j) {
            Seg yi = y.Segment(blockPtr[i], blockPtr[i+1] - blockPtr[i]);
            Seg vj = v.Segment(blockPtr[j], blockPtr[j+1] - blockPtr[j]);
            if (HasBlock(i, j))      yi += Csr(i, j) * vj;           // 直接块
            else if (HasBlock(j, i)) yi += CsrTransposed(j, i) * vj; // 下三角用转置
        }
}
// CSR 乘: yi[r] += Σ_{k=ap[r]..ap[r+1]-1} ax[k] * vj[aj[k]]
// 转置乘: 对块 (j,i) 的每个元素 (r, aj[k], ax[k]): yi[aj[k]] += ax[k] * vj[r]
```

### 11.3 PCG 主循环（float32，直译 `solve_pcg_cpu`）

```cpp
// 输入: 块矩阵、b (N,), invDiag (N,), tol, maxIter, resFix(=false)
int   sqrtN = (int)ceil(sqrt((double)N));
float bNorm = Norm2(b);                 // L2 范数
float atol  = tol * bNorm;              // 绝对残差阈值（相对判据）

Vec x = 0, p = 0, z = 0, q = 0;
Vec r = b;                              // x=0 起步，r = b - A*0 = b
int iters = 0; float rho = 0.f, rho1;

while (maxIter < 0 || iters < maxIter) {
    z = invDiag ⊙ r;                    // 预条件（逐元素乘）
    rho1 = rho;  rho = Dot(r, z);
    if (iters == 0) p = z;
    else            p = z + (rho / rho1) * p;
    SymblkMatmul(q, p);
    float alpha = rho / Dot(p, q);
    x += alpha * p;
    if (resFix && (iters + 1) % sqrtN == 0) { SymblkMatmul(q, x); r = b - q; }
    else                                      r -= alpha * q;
    ++iters;
    if (Norm2(r) <= atol) break;        // 收敛判据（每轮末尾检查）
}
return { x, iters };                    // iters == maxIter 仅告警，结果照常使用
```

与 Python 参数对应：`tol = solver_tol = 1e-5`，`max_iter = solver_max_iter = 2000`，`res_fix = False`（`solver.py _solve` 硬编码），无最小迭代数、无 stagnation 检测。注意除零情形（`rho1`、`Dot(p,q)`、`bNorm==0`）原实现不做保护——若 `b` 全零会得到 NaN；移植可选择在 `bNorm == 0` 时直接返回 `x = 0`（标注为移植增强）。

## 12. 查表数据清单

### 12.1 MC 表（`csrc/meshing/mc_data.h`，逐字复制）

| 表 | 维度 | 说明 | 抽查值 |
| --- | --- | --- | --- |
| `edgeTable` | `int[256]` | cubeType → 12 bit 棱激活掩码 | `[0]=0x0, [1]=0x109, [255]=0x0` |
| `triangleTable` | `int[256][16]` | cubeType → 棱 id 三元组序列，`-1` 结尾 | `[0]` 全 `-1`；`[1]={0,8,3,-1×13}` |
| `numVertsTable` | `int[256]` | cubeType → 输出顶点数（3 的倍数，≤15） | `[0]=0, [1]=3, [255]=0` |

三个 256 项大表从 `mc_data.h`（前 163 行）原样拷贝进 C++ 源文件即可，不要手抄。小表全文如下：

```cpp
int cubeRelTable[8][3] = {                  // MC 角 i 的相对偏移（底环 0-3 逆时针，顶环 4-7）
    {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
    {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1} };
int e2iTable[12][2] = {                     // 棱 → 两角（顶点合并键；注意 edge 10 = {6,2}）
    {0,1}, {1,2}, {2,3}, {0,3}, {4,5}, {5,6}, {6,7}, {7,4},
    {0,4}, {1,5}, {6,2}, {3,7} };
```

### 12.2 CubeFaceIterator 方向/槽位表（`csrc/common/iter_util.h`，逐字复制）

```cpp
int faceAxisTable[6][4] = {                 // {fix_side, fix_axis, iter_axis0, iter_axis1}
    {0,0,1,2}, {1,0,1,2}, {0,1,2,0}, {1,1,2,0}, {0,2,0,1}, {1,2,0,1} };
int faceAccIndsTable[6][4] = {              // 面内点占据的 4 个槽位
    {1,2,5,6}, {0,3,4,7}, {2,3,6,7}, {0,1,5,4}, {4,5,6,7}, {0,1,2,3} };
int edgeAxisTable[12][5] = {                // {side_a, side_b, axis_a, axis_b, iter_axis}
    {0,0,1,2,0}, {0,1,1,2,0}, {1,0,1,2,0}, {1,1,1,2,0},
    {0,0,2,0,1}, {0,1,2,0,1}, {1,0,2,0,1}, {1,1,2,0,1},
    {0,0,0,1,2}, {0,1,0,1,2}, {1,0,0,1,2}, {1,1,0,1,2} };
int edgeAccIndsTable[12][2] = {             // 棱内点占据的 2 个槽位
    {6,7}, {2,3}, {4,5}, {0,1}, {5,6}, {4,7}, {1,2}, {0,3},
    {2,6}, {1,5}, {3,7}, {0,4} };
int cornerAxisTable[8][3] = {               // 角枚举顺序（z 最快、x 最慢，≠ cubeRelTable！）
    {0,0,0}, {0,0,1}, {0,1,0}, {0,1,1}, {1,0,0}, {1,0,1}, {1,1,0}, {1,1,1} };
int cornerAccIndsTable[8] = { 6, 2, 5, 1, 7, 3, 4, 0 };   // 角占据的 1 个槽位
```

### 12.3 MISE 细分常量（§7.1，逐字保留顺序）

```text
棱组:   [[0,4],[1,5],[3,7],[2,6]]  [[0,1],[3,2],[4,5],[7,6]]  [[0,3],[1,2],[4,7],[5,6]]
面组:   [[0,1,5,4],[3,2,6,7]]      [[1,2,6,5],[0,3,7,4]]      [[0,1,2,3],[4,5,6,7]]
体心对角: cur 0..7 → diag {6,7,4,5,2,3,0,1}
```

## 13. 边界情况与移植风险

- **cubeRelTable 与 cornerAxisTable 是两套不同的角序**：前者是 MC cube 槽位约定（xy 底环），后者只是 `CubeFaceIterator` 的枚举顺序（z 最快）；两者通过 `cornerAccIndsTable` 关联。混用会得到看似合理却拓扑错乱的网格。
- 符号判定不对称：cube type 用 `sdf < 0`，MISE 筛选用 `value > 0`；`sdf == 0` 在 cube type 中算外侧、在 MISE 中算负侧。保持原样。
- `OctChildrenIterator` 的 floor-to-even 必须用算术右移语义（负坐标向下取整），`(-3 >> 1) << 1 == -4`。
- `buildJointDualGrid` 的 `round`（四舍五入到最近整数）在角点正好落在半格时敏感；ks 的 origin 约定下角点总是精确整数（见 §4 自检），用 double 计算可避免抖动。
- graph 行、`dmcVertices` 行、`baseIdx` 三者的层序（0..depth-1、跳过空层）必须一致，否则 cube 角索引全部错位。
- 去重原语（`UniqueRowsSorted`/`UniqueSorted`）必须输出**升序 + 逆映射**语义；顶点合并与 MISE 都依赖它。
- 求解器全程 float32；`b` 范数为 0 时原实现产生 NaN（实际数据不会发生，但空输入需在上层挡掉）。
- MC 并行化（原实现 `at::parallel_for` 两遍扫描 + 前缀和）是可选优化；串行实现结果一致。
- `mise_iter` 每次迭代 cube 数 ×8（先经跨零筛选），mise_iter=1 时峰值内存约为筛后 cube 数的 8 倍行数，注意预留。
