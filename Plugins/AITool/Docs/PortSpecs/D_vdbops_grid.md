# NKSR 移植规格 (D: vdbops 索引网格语义 + 哈希替代设计)

本文档定稿 `_C._CpuIndexGrid`（C++ 类 `SparseFeatureIndexGrid<nanovdb::HostBuffer>`）的全部推理路径语义：world↔voxel 坐标变换、稀疏索引网格 (index grid) 的拓扑构建与索引分配顺序、每个 CPU kernel 的逐元素公式，以及用「开放寻址哈希表 + 有序坐标数组」替代 nanovdb 的设计。移植工程师只依据本文写代码，不读原始源码。伪代码可一比一翻译为 C++。

源码基准：

- `NKSR/package/csrc/vdbops/SparseFeatureIndexGrid.h`, `PythonBindings.cpp`, `Kernels.h`
- `NKSR/package/csrc/vdbops/kernels/cpu/*.cpp`（10 个文件）
- `NKSR/package/csrc/vdbops/utils/`：`VoxelCoordTransform.h`, `IndexGridBuilders.h`, `ActiveVoxelIterator.h`, 4 个插值迭代器
- `NKSR/package/csrc/vdbops/autograd/*.h`（只取 forward 语义）
- nanovdb（vendored, `package/external/openvdb/nanovdb/`）：`NanoVDB.h`, `util/GridBuilder.h`, `util/IndexGridBuilder.h` —— 只用于确定取整规则与索引分配顺序，C++ 端整体不移植 nanovdb，用 [§5](#5-哈希替代设计) 的结构替代。

## 0. 移植范围

推理路径（见 A 规格）实际调用的方法，全部必须移植：

| 方法 | 推理路径调用点 |
| --- | --- |
| 构造 `(vox_size, vox_origin)` | `svh.py` 每层 `SparseIndexGrid(vs_d, 0.5*vs_d)` |
| `build_from_pointcloud_nearest_voxels` | `svh.build_point_splatting`（每层用同一份输入点） |
| `build_from_ijk_coords(coords, pad_min, pad_max)` | UNet decoder 结构构建（pad 恒为 `(0,0,0)/(0,0,0)`） |
| `coarsened_grid(2)` | UNet 下采样结构 |
| `subdivided_grid(2, mask)` | UNet 上采样结构、`extract_dual_mesh` 稠密化 |
| `dual_grid()` | meshing 用 dual 角点网格 |
| `active_grid_coords` / `ijk_to_index` / `num_voxels` | 到处 |
| `grid_to_world` / `world_to_grid` | 特征位置、点量化 |
| `splat_trilinear(points, data, return_counts)` | 法线 splat（`return_counts=True` 与 `False` 都出现） |
| `sample_trilinear(points, data, return_grad)` | interpolator、UNet 特征插值 |
| `sample_bezier(points, data, return_grad)` | kernel field / neural field SDF 求值（含 `return_grad=True`） |
| `subdivide` (= UpsampleGridNearest forward) | UNet nearest 上采样特征 |
| `max_pool` (= DownsampleGridMaxPool forward) | UNet max 池化特征 |
| `points_in_active_voxel` | 仅 `LayerField`（ks 配置不触发，仍建议移植，代码极短） |

不移植：

| 项 | 原因 |
| --- | --- |
| 全部 backward kernel（`*Grad`, `*GradGrad`, `SplatIntoGridBezier`, `UpsampleGridNearestGrad`, `DownsampleGridMaxPoolGrad`, `TransformPointsToGridGrad`/`Inv...Grad`） | 只在 autograd backward 中调用，推理不触发。注意 `sample_*` 的 `return_grad=True` 属于 **forward**（空间梯度），必须移植 |
| CUDA kernel、`_CudaIndexGrid`、`to_cuda`/`to_cpu`、`PytorchDeviceBuffer` | 纯 CPU 移植 |
| `buildFromPointsWithPadding`（`build_from_pointcloud`） | 只被 `build_iterative_coarsening` 使用，ks 路径用 `build_point_splatting`；语义在 [§3.2](#32-拓扑构建) 顺带给出（与 `build_from_ijk_coords` 同构） |
| `buildPaddedHierarchyFromPoints` | 全库无调用 |
| `set_origin` / `set_voxel_size` / `voxelSize`/`origin` 之外的 setter 路径 | 推理不改变已建网格的 transform |
| nanovdb 树结构本身（leaf/internal/root 节点、GridHandle） | 用哈希表替代，见 [§5](#5-哈希替代设计) |

数据类型约定：推理路径所有点/特征 tensor 为 `float32` 连续 row-major；`ijk` 坐标 tensor 为 `int32`（Python 侧 `.int()`）或 `int64`（`active_grid_coords` 输出恒为 `int64`）；index 输出恒为 `int64`。

## 1. 坐标约定 (VoxelCoordTransform)

### 1.1 变换定义

`VoxelCoordTransform` 是三轴同参数的一维仿射：`g = x * scale + translate`。它同时保存 double 与 float 两份参数：

```cpp
struct FVoxelCoordTransform {
    float  ScaleF, TranslateF;   // 构造时由 double 强制转换 (float)Scale, (float)Translate
    double ScaleD, TranslateD;
    // apply(x)    = x * scale + translate      （world → grid）
    // applyInv(g) = (g - translate) / scale    （grid → world）
};
```

关键精度规则（必须一字不差遵守）：

- 参数在**构造时**用 double 算好，再截断出 float 副本。kernel 按 tensor dtype 选副本；NKSR 路径全是 `float32`，即所有逐点变换都用 **float 副本** 计算。
- `apply` 必须写成 `x * scale + translate`，**不能**写成 `(x - origin) / voxSize`——数学等价但 float 舍入不同，会在 voxel 边界处改变 `round`/`floor` 结果。
- `applyInv` 必须写成 `(g - translate) / scale`，**不能**写成 `g * voxSize + origin`。

### 1.2 primal / dual transform

`SparseFeatureIndexGrid::setTransform(voxSize, voxOrigin)`（double 计算）：

```cpp
mVoxSize          = voxSize;
mPrimalTransform  = { scale = 1.0 / voxSize, translate = -voxOrigin / voxSize };
mDualTransform    = { scale = 1.0 / voxSize, translate = -voxOrigin / voxSize + 0.5 };
```

由此：

- primal 网格坐标：`g = (x - origin) / vs`。整数格点 `i` 的 world 位置 = `origin + i * vs`（voxel 中心）。voxel `i` 在 `round` 量化下覆盖 world 区间 `[origin + (i-0.5)*vs, origin + (i+0.5)*vs)`。
- dual 网格坐标：`g_dual = (x - origin)/vs + 0.5`。dual 整数格点 `j` 的 world 位置 = `origin + (j - 0.5) * vs`，即 primal voxel 的**角点**（primal voxel `i` 的 8 个角 = dual 格点 `{i, i+1}³`）。
- `voxelOrigin()` 读回：`applyInv(0) = -translate/scale = voxOrigin`（double 副本）。

### 1.3 每层 voxel size 与 origin（NKSR 用法）

`svh.get_grid_voxel_size_origin(depth)`：

```python
vs_d     = voxel_size * 2**depth        # voxel_size 为最细层尺寸（ks: 0.1 * 输入尺度换算，见 A 规格）
origin_d = 0.5 * vs_d
```

层间关系（`SparseFeatureIndexGrid` 内部公式，`b`=branch factor，`s`=subdiv factor）：

```cpp
// coarsened_grid(b):  vs_c = b * vs;   origin_c = origin + (b-1) * vs * 0.5
// subdivided_grid(s): vs_f = vs / s;   origin_f = origin - (s-1) * vs_f * 0.5
```

`b = s = 2` 时与 `get_grid_voxel_size_origin` 自洽：`(vs, 0.5*vs)` 粗化后 = `(2vs, vs) = (vs_1, 0.5*vs_1)`。粗 voxel `c` 的 world 中心正好是其 `2³` 个细 voxel 中心的平均。

### 1.4 取整规则

nanovdb 的两种取整都是**逐分量**、返回 `int32`：

```cpp
floor(x) = int32( std::floor(x) );          // Vec3::floor
round(x) = int32( std::floor(x + 0.5) );    // Vec3::round —— 半格上取（round-half-up）
```

注意 `round` **不是** `std::round`（half-away-from-zero）也不是银行家舍入：`round(-0.5) = floor(0.0) = 0`，`round(2.5) = 3`，`round(-2.5) = -2`。全部 kernel 与拓扑构建都用这两个定义。

各操作使用的取整：

| 操作 | 取整 |
| --- | --- |
| 拓扑构建（点 splat 建格）、`points_in_active_voxel` | `round(primal(x))` |
| trilinear 插值 stencil 基点 | `floor(g)` |
| bezier 插值 stencil 基点 | `round(g)` |
| fine→coarse 坐标 | `floor( double(ijk) / b )`（double 除法后 floor，负数正确） |
| upsample 中 fine→coarse | `floor( float(ijk) / f )`（源码用 ScalarT=float 除法；建议 C++ 用整数 floor-div `FMath::FloorToInt` 等价实现，注意负数） |

## 2. IndexGrid 语义

### 2.1 核心抽象

一个 index grid 是映射 `active ijk → 唯一 value index` 的稀疏结构：

- nanovdb 中 index 是 **1-based**：`getValue(ijk)` 对 active voxel 返回 `1..N`，对 inactive 返回 `0`（0 是 background 槽）。
- 全部 kernel 通过统一辅助函数换成 0-based：

```cpp
int64 voxelIndex(grid, ijk) = getValue(ijk) - 1;   // active: 0..N-1；inactive: -1
```

- `num_voxels()` = N（active voxel 总数）。`valueArraySize()` = N+1（含 background 槽，未暴露给 Python，可不实现）。
- 所有特征 tensor 的第 0 维按此 0-based index 对齐：`data[index] = voxel 的特征行`。

### 2.2 索引分配顺序（nanovdb 原始顺序）

原实现建格流程：`GridBuilder<int32>` 逐点 `setValue`（永不生成 tile，所有 active voxel 都落在 8³ leaf 中）→ `IndexGridBuilder(grid, includeInactive=false, includeStats=false)` 给 active voxel 依树遍历顺序分配 `1..N`。该遍历顺序等价于以下比较器下的升序（`i,j,k` 为 int32，`>>` 为算术右移）：

```cpp
// nanovdb 顺序比较器（层级：root tile 4096³ → upper 局部 32³ → lower 局部 16³ → leaf 局部 8³）
// 每一级都是 i 优先、j 次之、k 最后的字典序
bool NanoVdbLess(FIntVector a, FIntVector b) {
    #define CMP(x, y) if ((x) != (y)) return (x) < (y)
    CMP(a.X >> 12, b.X >> 12); CMP(a.Y >> 12, b.Y >> 12); CMP(a.Z >> 12, b.Z >> 12);  // root: 4096 块，有符号字典序
    CMP((a.X >> 7) & 31, (b.X >> 7) & 31); CMP((a.Y >> 7) & 31, (b.Y >> 7) & 31); CMP((a.Z >> 7) & 31, (b.Z >> 7) & 31);  // upper 内 lower 槽位
    CMP((a.X >> 3) & 15, (b.X >> 3) & 15); CMP((a.Y >> 3) & 15, (b.Y >> 3) & 15); CMP((a.Z >> 3) & 15, (b.Z >> 3) & 15);  // lower 内 leaf 槽位
    CMP(a.X & 7, b.X & 7); CMP(a.Y & 7, b.Y & 7); CMP(a.Z & 7, b.Z & 7);              // leaf 内 voxel 槽位
    return false;
    #undef CMP
}
```

要点：

- 这**不是**全局 `(i,j,k)` 字典序。例如 `(0,0,9)` 与 `(1,0,0)` 同属一个 leaf 邻域时，`(1,0,0)` 的 leaf 槽位在前 → index 更小。
- 但它是纯拓扑的确定性全序：与插入顺序、点顺序无关，只由 active 集合决定。
- 顺序**只需自洽**即可正确（见 [§5.2](#52-顺序依赖分析)）；只有和 Python 参考实现做逐张量 bit 级对拍时才需要复刻此比较器。

`ActiveVoxelIterator` 语义：按上述顺序枚举 `(ijk, index)`，index 严格递增。带模板参数 `Offset=-1` 时产出 0-based index。因此 **`active_grid_coords()` 的第 `r` 行 = index 为 `r` 的 voxel 的 ijk**，与 `ijk_to_index` 严格互逆——这是整个系统最重要的不变量。

### 2.3 拓扑构建操作

全部构建先把 active 集合收集齐，再一次性按 [§2.2](#22-索引分配顺序nanovdb-原始顺序)（或 §5 的替代顺序）分配 index。伪代码中 `Activate(ijk)` 表示插入 active 集合（幂等）。

```cpp
// build_from_pointcloud_nearest_voxels(points [N,3] float32)  —— ks 路径主构建
// 等价形式：对每点激活 trilinear stencil floor(g) + {0,1}³
for (p : points) {
    Vec3f g = PrimalTx.apply(p);                       // float 变换
    for (sx : {-0.5f, 0.5f}) for (sy : {-0.5f, 0.5f}) for (sz : {-0.5f, 0.5f})
        Activate(round(g + Vec3f(sx, sy, sz)));        // round = floor(x+0.5)
}

// build_from_ijk_coords(coords [N,3] int, padMin, padMax)      —— UNet decoder；pad 恒 (0,0,0)/(0,0,0)
for (c : coords)
    for (di : padMin.x..padMax.x) for (dj : padMin.y..padMax.y) for (dk : padMin.z..padMax.z)
        Activate(c + (di, dj, dk));

// build_from_pointcloud(points, padMin, padMax)                —— 不在 ks 路径；结构同上，c = round(PrimalTx.apply(p))

// coarsened_grid(b=2)：新 transform 见 §1.3
for ((ijk, _) : fineGrid) Activate( floor(double(ijk) / b) );   // 逐分量 double 除后 floor

// subdivided_grid(s=2, mask)：mask 为空 tensor 或 [numCoarseVoxels] bool/int
for ((ijk, idx) : coarseGrid) {                                 // idx 为 0-based coarse index
    if (mask.Num() > 0 && !mask[idx]) continue;
    for (i : 0..s-1) for (j : 0..s-1) for (k : 0..s-1)
        Activate(ijk * s + (i, j, k));
}

// dual_grid()：pad [0,1]³ + 交换 transform
for ((ijk, _) : primalGrid)
    for (di : 0..1) for (dj : 0..1) for (dk : 0..1)
        Activate(ijk + (di, dj, dk));
// dual 网格对象的 PrimalTransform = 父的 DualTransform；DualTransform = 父的 PrimalTransform
// dual 网格 voxSize / voxelOrigin 数值与父相同（构造时传入父的 (vs, origin) 再被覆盖 transform）
```

### 2.4 `_CpuIndexGrid` 方法总表

| 方法 | 输入 → 输出（shape, dtype） | 语义 |
| --- | --- | --- |
| `__init__(vox_size, vox_origin, device_index=-1)` | double, double | 建空网格 + `setTransform`（[§1.2](#12-primal--dual-transform)） |
| `build_from_pointcloud_nearest_voxels(points)` | `[N,3] f32` | [§2.3](#23-拓扑构建操作)；要求 N>0 |
| `build_from_ijk_coords(coords, pad_min, pad_max)` | `[N,3] int`, 两个 int3 | [§2.3](#23-拓扑构建操作) |
| `num_voxels()` | → int64 | active voxel 数 N |
| `voxel_size()` / `origin()` | → double | `mVoxSize` / `-translate/scale`（double 副本） |
| `active_grid_coords()` | → `[N,3] i64` | `out[index] = ijk`，index 升序即 [§2.2](#22-索引分配顺序nanovdb-原始顺序) 顺序 |
| `ijk_to_index(ijk)` | `[M,3] int` → `[M] i64` | active → 0-based index；inactive → `-1` |
| `points_in_active_voxel(pts)` | `[M,3] f32` → `[M] bool` | `isActive(round(PrimalTx(p)))` |
| `world_to_grid(pts)` | `[M,3] f32` → 同形 | 逐点 `apply`（float），[§4.1](#41-transformpointtogrid) |
| `grid_to_world(g)` | `[M,3] f32` → `[M,3]` | 逐点 `applyInv`（float）；注意输出恒被 reshape 成 `(-1,3)` |
| `splat_trilinear(points, data, return_counts=False)` | `[N,3] f32`, `[N,...,D] f32` | [§4.5](#45-splatintogridtrilinear-含-counts)；返回 `[V,...,D]` 或 `([V,...,D],[V])` |
| `sample_trilinear(points, data, return_grad=False)` | `[M,3] f32`, `[V,...,D] f32` | [§4.6](#46-samplegridtrilinear--grad)；返回 `[M,...,D]` 或 `([M,...,D],[M,...,D,3])` |
| `sample_bezier(points, data, return_grad=False)` | 同上 | [§4.7](#47-samplegridbezier--grad) |
| `coarsened_grid(b=2)` / `subdivided_grid(s=2, mask=None)` / `dual_grid()` | → 新网格对象 | [§2.3](#23-拓扑构建操作) |
| `subdivide(fine_grid, factor, coarse_data)` | 网格, uint, `[Vc,...,D] f32` → `[Vf,...,D]` | [§4.8](#48-upsamplegridnearest)（self 为 coarse） |
| `max_pool(coarse_grid, factor, fine_data)` | 网格, uint, `[Vf,...,D] f32` → `[Vc,...,D]` | [§4.9](#49-downsamplegridmaxpool)（self 为 fine） |

校验约束（原实现 TORCH_CHECK，移植为 check/ensure）：points 必须 `[N,3]` 且 N>0；`sample_*` 的 `data.size(0) == num_voxels()`；`splat` 的 `points.size(0) == data.size(0)`；多维特征 `data [N, d1, d2, ...]` 一律先 flatten 成 `[N, D]`（`D = d1*d2*...`）计算，结果 reshape 回原尾部形状。

## 3. 插值迭代器（stencil 精确定义）

### 3.1 Trilinear（8 邻域）

```cpp
// 输入 g（grid 坐标, float）
IntVector base = floor(g);                 // 逐分量
Vec3f uvw = g - Vec3f(base);               // ∈ [0,1)^3
// 枚举顺序 n = 0..7：di = (n>>2)&1, dj = (n>>1)&1, dk = n&1
// 即 (0,0,0),(0,0,1),(0,1,0),(0,1,1),(1,0,0),(1,0,1),(1,1,0),(1,1,1) —— k 最快，i 最慢
// 权重（u=uvw.x, v=uvw.y, w=uvw.z; a?b:c 表示 d?=1 取 b 否则取 a）：
W[n] = (di ? u : 1-u) * (dj ? v : 1-v) * (dk ? w : 1-w);
// 邻域坐标 = base + (di, dj, dk)；权重和恒为 1（全 active 时插值是仿射精确的）
```

### 3.2 Trilinear + 空间梯度

每个邻域产出 `Vec4 (W, dW/du, dW/dv, dW/dw)`，即权重对 grid 坐标的偏导。展开表（`U0=1-u, V0=1-v, W0=1-w`）：

| n=(di,dj,dk) | W | ∂W/∂u | ∂W/∂v | ∂W/∂w |
| --- | --- | --- | --- | --- |
| (0,0,0) | `U0*V0*W0` | `-V0*W0` | `-U0*W0` | `-U0*V0` |
| (0,0,1) | `U0*V0*w`  | `-V0*w`  | `-U0*w`  | `+U0*V0` |
| (0,1,0) | `U0*v*W0`  | `-v*W0`  | `+U0*W0` | `-U0*v`  |
| (0,1,1) | `U0*v*w`   | `-v*w`   | `+U0*w`  | `+U0*v`  |
| (1,0,0) | `u*V0*W0`  | `+V0*W0` | `-u*W0`  | `-u*V0`  |
| (1,0,1) | `u*V0*w`   | `+V0*w`  | `-u*w`   | `+u*V0`  |
| (1,1,0) | `u*v*W0`   | `+v*W0`  | `+u*W0`  | `-u*v`   |
| (1,1,1) | `u*v*w`    | `+v*w`   | `+u*w`   | `+u*v`   |

world 空间梯度 = grid 梯度 × `scale`（= `1/voxSize` 的 float 副本），在 kernel 内逐项乘（[§4.6](#46-samplegridtrilinear--grad)）。

### 3.3 Bezier（27 邻域，二次样条式 bump）

```cpp
IntVector base = round(g);                 // floor(g + 0.5) 逐分量
Vec3f uvw = g - Vec3f(base);               // ∈ [-0.5, 0.5]
// 枚举顺序 n = 0..26：dz = n%3 - 1（最快）, dy = (n/3)%3 - 1, dx = n/9 - 1（最慢）
// 每维偏移 ∈ {-1, 0, +1}；权重：
W[n] = B(uvw.x - dx) * B(uvw.y - dy) * B(uvw.z - dz);
```

一维基函数与导数（分段边界按左闭右开判断，源码用 `x < ±0.5 / ±1.5` 级联比较）：

```cpp
float B(float x) {           // 支撑域 (-1.5, 1.5)
    if (x >= -1.5f && x < -0.5f) return (x + 1.5f) * (x + 1.5f);
    if (x >= -0.5f && x <  0.5f) return -2.f * x * x + 1.5f;
    if (x >=  0.5f && x <  1.5f) return (x - 1.5f) * (x - 1.5f);
    return 0.f;
}
float dB(float x) {
    if (x >= -1.5f && x < -0.5f) return 2.f * x + 3.f;
    if (x >= -0.5f && x <  0.5f) return -4.f * x;
    if (x >=  0.5f && x <  1.5f) return 2.f * x - 3.f;
    return 0.f;
}
```

关键事实：

- `uvw - d ∈ [-1.5, 1.5]`，27 个权重一般全非零；参数落在 `x = -0.5` 等分段点上时按上述左闭右开取值。
- **权重未归一化**：每维 `B(u+1)+B(u)+B(u-1) ≡ 2`，三维权重总和恒为 8。这是有意的（NKSR 的 kernel/网络在此尺度上训练），移植时**不得**归一化。
- 带梯度版本产出 `Vec4 (Bx*By*Bz, dBx*By*Bz, Bx*dBy*Bz, Bx*By*dBz)`（对 grid 坐标）。

## 4. kernels/cpu 逐算子规格

统一约定：`Tx` = 该网格对象的 `mPrimalTransform` float 副本；`Idx(ijk)` = 0-based index 或 -1；所有累加输出先零初始化（`torch::zeros` 语义）；外层循环按行序 `pi = 0..N-1` 单线程顺序执行（决定 float 累加顺序）；stencil 内层按 [§3](#3-插值迭代器stencil-精确定义) 的枚举顺序；特征维内层 `j = 0..D-1`。

### 4.1 TransformPointToGrid

`world_to_grid`：`out[i] = pts[i] * scale + translate`（逐分量，float）。
`grid_to_world`：`out[i] = (g[i] - translate) / scale`（逐分量，float；除法不是乘倒数）。
输出 `torch::empty` 后全量覆盖，无零初始化。`grid_to_world` 的返回值形状恒为 `[M,3]`（源码 `reshape_as` 的是展平视图）；`world_to_grid` 返回原输入形状。

### 4.2 PointsInGrid

```cpp
for (pi : 0..M-1) outMask[pi] = IsActive( round(Tx.apply(pts[pi])) );
```

### 4.3 IjkToIndex

```cpp
for (ci : 0..M-1) outIndex[ci] = Idx( IntVector(coords[ci]) );   // inactive → -1
```

### 4.4 ActiveGridCoords

```cpp
for ((ijk, idx) : ActiveVoxels())          // idx 0-based
    out[idx] = int64(ijk);                 // out: [N,3] int64
```

### 4.5 SplatIntoGridTrilinear（含 counts）

输入 `points [N,3]`, `data [N,D]`（已 flatten）。输出 `outGrid [V,D]` 零初始化；`return_counts` 时另有 `outCounts [V]`（float，与 points 同 dtype）零初始化。

```cpp
for (pi : 0..N-1) {
    Vec3f g = Tx.apply(points[pi]);
    for ((ijk, w) : TrilinearStencil(g)) {             // §3.1 顺序
        int64 idx = Idx(ijk);
        if (idx < 0) continue;                          // 越界/未激活：贡献直接丢弃，不重归一化
        for (j : 0..D-1) outGrid[idx][j] += w * data[pi][j];
        outCounts[idx] += 1.f;                          // 仅 with-counts 版本；+1 与权重无关
    }
}
```

注意 `counts` 统计的是「(点, active stencil 格) 命中次数」而非权重和。V = `num_voxels()`；输出 reshape 回 `[V, *data 尾部形状]`。

### 4.6 SampleGridTrilinear（± grad）

输入 `points [M,3]`, `gridData [V,D]`。输出 `outFeat [M,D]` 零初始化；`return_grad` 时另有 `outGrad [M,D,3]` 零初始化。

```cpp
for (pi : 0..M-1) {
    Vec3f g = Tx.apply(points[pi]);
    for ((ijk, w4) : TrilinearStencilWithGrad(g)) {    // 无 grad 版用 §3.1 只取 w4[0]
        int64 idx = Idx(ijk);
        if (idx < 0) continue;                          // 缺失邻域按 0 处理（无重归一化）
        for (j : 0..D-1) {
            outFeat[pi][j] += w4[0] * gridData[idx][j];
            for (dim : 0..2) outGrad[pi][j][dim] += w4[dim+1] * gridData[idx][j] * scaleF;  // world 梯度
        }
    }
}
```

`scaleF` = transform 的 float `scale`（=`1/voxSize`）。返回 shape：`feat` reshape 回 `[M, *data 尾部]`；`grad` 为其后再接一维 3。

### 4.7 SampleGridBezier（± grad）

与 [§4.6](#46-samplegridtrilinear--grad) 结构完全相同，仅把 stencil 换成 [§3.3](#33-bezier27-邻域二次样条式-bump) 的 27 邻域（`base = round(g)`、权重 `B`、导数 `dB`），梯度同样乘 `scaleF`。

### 4.8 UpsampleGridNearest

绑定名 `subdivide(self=coarse, fine, f, coarseData)`。输入 `coarseData [Vc,D]`，输出 `outFine [Vf,D]` 零初始化。要求 `coarseData.size(0) == coarse.num_voxels()`。

```cpp
for ((fineIjk, fineIdx) : fineGrid.ActiveVoxels()) {
    IntVector coarseIjk = FloorDiv(fineIjk, f);        // 逐分量 floor(ijk / f)，负数向下取整
    int64 cIdx = coarseGrid.Idx(coarseIjk);
    if (cIdx < 0) continue;                             // 粗格缺失 → fine 行保持 0
    for (j : 0..D-1) outFine[fineIdx][j] = coarseData[cIdx][j];
}
```

### 4.9 DownsampleGridMaxPool

绑定名 `max_pool(self=fine, coarse, f, fineData)`。输入 `fineData [Vf,D]`，输出 `outCoarse [Vc,D]`（原实现 `torch::empty` 后由 kernel 全行初始化）。要求 `fineData.size(0) == fine.num_voxels()`。

```cpp
for ((coarseIjk, cIdx) : coarseGrid.ActiveVoxels()) {
    for (j : 0..D-1) outCoarse[cIdx][j] = -INFINITY;
    for (i : 0..f-1) for (j2 : 0..f-1) for (k : 0..f-1) {
        int64 fIdx = fineGrid.Idx(coarseIjk * f + (i, j2, k));
        if (fIdx < 0) continue;
        for (j : 0..D-1) outCoarse[cIdx][j] = max(outCoarse[cIdx][j], fineData[fIdx][j]);
    }
}
```

边界情形：某 coarse voxel 若无任何 active fine 子格，该行保持 `-inf`。推理路径的 coarse 网格总由该 fine 网格 `coarsened_grid` 得到（`nn/modules.py` 中 `max_pool` 缺省 `coarse_grid = self.coarsened_grid(pool_factor)`），每个 coarse voxel 至少 1 个子格，`-inf` 不会出现；但替代实现应保留该语义。

### 4.10 autograd 包装层的 shape 语义（forward）

- 多维特征 `data [N, d1, ..., dm]`：先 `view/reshape` 为 `[N, D]`，kernel 结束后 reshape 回 `[out0, d1, ..., dm]`（`sample` 的 grad 再接 `,3`）。
- `splat` 输出行数 = `num_voxels()`；`sample` 输出行数 = 点数；`subdivide`/`max_pool` 输出行数 = 目标网格 voxel 数。
- 所有输出 dtype/设备与输入特征一致（推理为 CPU float32）。

## 5. 哈希替代设计

### 5.1 推荐数据结构

```cpp
struct FSparseIndexGrid {
    FVoxelCoordTransform Primal, Dual;   // §1.2
    double VoxSize;
    TArray<FIntVector>   Coords;         // Coords[index] = ijk；按选定全序升序排列
    THashMap             IjkToIdx;       // 开放寻址：packed ijk → int32/int64 index
};

// key 打包（坐标范围约 ±2^20，NKSR 场景远小于此）：
uint64 Pack(FIntVector v) {
    return (uint64(uint32(v.X) & 0x1FFFFF) << 42)
         | (uint64(uint32(v.Y) & 0x1FFFFF) << 21)
         |  uint64(uint32(v.Z) & 0x1FFFFF);
}
// 哈希：splitmix64(Pack(v))；表容量取 2 的幂 ≥ 2N，线性探测；空槽哨兵 key = UINT64_MAX（或独立 occupied 位）
```

构建协议（所有 [§2.3](#23-拓扑构建操作) 的构建都走同一条路）：

1. 用临时哈希集合收集 active ijk（`Activate` 幂等去重）。
2. 导出到 `Coords` 数组，按选定比较器排序。
3. 按序赋 index `0..N-1`，重建 `IjkToIdx`。

推荐比较器：**有符号 `(i,j,k)` 字典序**（简单、稳定、与插入顺序无关）。若需与 Python/nanovdb 参考输出做逐张量对拍，编译期或运行期切换为 [§2.2](#22-索引分配顺序nanovdb-原始顺序) 的 `NanoVdbLess` 即可获得 bit 级同序。

### 5.2 顺序依赖分析

哪些地方依赖迭代顺序、替代方案如何自洽：

| 依赖点 | 依赖性质 | 自洽条件 |
| --- | --- | --- |
| `active_grid_coords()[r]` ↔ 特征第 `r` 行 | 硬契约 | `Coords[index]` 与 `IjkToIdx` 严格互逆即可，任意全序都成立 |
| `ijk_to_index` | 硬契约 | 同上 |
| UNet 卷积 kernel map（其它任务）通过 `ijk_to_index(邻域坐标)` gather | 只依赖映射一致性 | 全序任选，纯置换不影响数值 |
| `subdivided_grid(mask)` 的 mask 下标 | mask 按 **coarse index** 索引 | mask 由同一网格的特征/坐标顺序产生，自动一致 |
| `subdivide`/`max_pool` 跨网格 | 通过 `Idx(ijk)` 查目标行，不假设两格顺序对齐 | 任意全序 |
| `splat`/`sample` 的 float 累加顺序 | 数值 ULP 级 | 保持「外层点序 = 输入行序、内层 stencil 序 = §3 枚举序、单线程」即可与参考实现同序；若并行化，结果有 ULP 级差异 |
| meshing（其它任务）读 dual/primal `active_grid_coords` + `ijk_to_index` | 同硬契约 | 任意全序；顶点/三角形输出顺序随之置换，几何不变 |
| 与 Python 中间张量对拍 | 顺序敏感 | 用 `NanoVdbLess` 复刻顺序；或对拍前双方都按 `(i,j,k)` 字典序重排（用 `active_grid_coords` 建置换） |

结论：除对拍外没有任何下游依赖 nanovdb 的特定顺序；替代实现只需保证「每个网格对象内 `Coords`/`IjkToIdx`/特征行三者用同一全序」这一不变量。

### 5.3 实现注意

- `Idx` 查询在 sample/splat 内是最热路径（每点 8 或 27 次），开放寻址 + 打包 key 明显优于 `TMap<FIntVector,...>`；也可为每次 sample 缓存上一次命中槽位（nanovdb accessor 的局部性优化），非必需。
- 坐标可能为负（origin>0 且点云跨原点），`Pack` 的 `uint32` 截断 + 21bit 掩码对 ±2^20 内负数是单射，勿改成有符号移位。
- 排序赋 index 后网格不可变（原实现同样是 build 后只读）；`set_origin`/`set_voxel_size` 只改 transform 不改拓扑。
- 空网格：`num_voxels()==0` 时 `active_grid_coords` 返回 `[0,3]`；kernel 层原实现直接禁止空输入（TORCH_CHECK），移植保持同样的前置校验。

## 6. Risks / Open Questions

- `round = floor(x+0.5)` 与 `std::round` 在 `x = n - 0.5`（n 整数）处结果不同；点恰好落在 voxel 边界时会改变拓扑与 stencil 归属。必须用 `floor(x+0.5)`。
- transform 必须以 `x*scale+translate` / `(g-translate)/scale` 的 float 形式计算（[§1.1](#11-变换定义)），任何代数等价重写都可能在边界翻转 `round`/`floor`。
- Bezier 权重和为 8（未归一化），trilinear 权重和为 1；缺失邻域一律按 0 贡献、不重归一化。误加归一化会系统性改变 SDF 幅值。
- `max_pool` 输出行在无 active 子格时为 `-inf`（推理路径不会触发，但不要改成 0）。
- 与 Python 参考做逐张量对拍时特征行是置换关系（nanovdb 顺序 ≠ 字典序），需先按 ijk 对齐或启用 `NanoVdbLess`。
