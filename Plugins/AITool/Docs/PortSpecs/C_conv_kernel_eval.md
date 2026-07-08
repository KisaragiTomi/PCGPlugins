# C — csrc 稀疏卷积 + kernel_eval CPU 算子移植规格

本文档规定 NKSR 推理路径中 `csrc/conv` 与 `csrc/kernel_eval` 两个 native 模块的纯 C++ (CPU) 移植规格。移植工程师只依据本文档实现，不阅读原始源码。所有伪代码可一比一翻译为 C++。数值全部使用 `float`（原实现 `AT_DISPATCH_FLOATING_TYPES`，推理时输入均为 float32；conv 前向甚至硬编码 `float`）。

来源文件（仅供溯源，不需要阅读）：
`csrc/conv/{conv.h, bind.cpp, convolution_cpu.cpp, kmap_cpu.cpp}`、
`csrc/kernel_eval/{bind.cpp, keval.h, keval_cpu.cpp, kbuild_cpu.cpp, matrixb_cpu.cpp, rhs_cpu.cpp, qgbuild_cpu.cpp, qgbuild_cuda.cu}`、
`csrc/vdbops/{SparseFeatureIndexGrid.h, utils/VoxelCoordTransform.h, utils/Utils.h, utils/ActiveVoxelIterator.h}`、
`csrc/common/iter_util.h`。

## 0. 覆盖范围与调用位置

推理路径（`Reconstructor.reconstruct` 默认参数：`fused_mode=True`、`geometry='kernel'`、`feature='normal'`）实际调用的 native 算子：

| 算子 | 推理中的调用点 | 本文小节 |
| --- | --- | --- |
| `convolution_kernel_map` | `nn/modules.py: Conv3d._compute_conv_args`（UNet 每个稀疏卷积层） | §2.1 |
| `sparse_convolution`（forward） | `nn/modules.py: Conv3d.forward` | §2.3 |
| `kernel_evaluation`（forward） | `fields/kernel_field.py: evaluate_f_depth`（field 求值 / dual mesh 提取时反复调用） | §3.2 |
| `build_coo_indexer` | `kernel_field.py: KernelField.solve` | §3.3 |
| `matrix_building`（forward） | `KernelField.solve`（GᵀG 与 QᵀQ 两次） | §3.5 |
| `k_building`（forward） | `KernelField.solve`（对角块正则项，`reg_weight=1.0`） | §3.6 |
| `rhs_evaluation`（forward） | `KernelField.solve`（右端项 Qᵀn） | §3.7 |

不移植（在推理默认路径之外）：

- 所有 backward / autograd 分支（`convolution_backward_cpu`、`kernel_grad_evaluation_bwd` 及各 `*Backward` dispatcher）。
- `qg_building` 与 `csr_matrix_multiplication`：仅被 `solve_non_fused`（`fused_mode=False`）使用；两者原库 CPU 版直接 `throw`（`qgbuild_cpu.cpp`、`CsrMatrixMultiplication::forward` 的 CPU 分支）。§3.8 给出按 CUDA 语义整理的参考规格，标注为可选。
- `csrc/common/math_util.h`：本路径完全未使用（`keval.h` 中的 include 已被注释，仅 `pcproc/cuda_kdtree.cuh` 引用）。无需移植其中任何内容。
- sensor / 颜色 / 可视化 / 训练相关算子。

## 1. 公共约定（全部算子共享，必须一字不差遵守）

### 1.1 体素索引 (voxel index) 与全局顺序

- 每个稀疏网格 (grid) 是一组激活的整数体素坐标 `ijk ∈ Z³`。每个激活体素有唯一索引 `offset ∈ [0, numVoxels)`。
- 原实现中该索引来自 nanovdb ValueIndex grid：`voxelIndex(acc, ijk) = acc.getValue(ijk) - 1`（1-based 内部值减 1）。顺序本身由 nanovdb 构建过程决定，**数值上没有任何算子依赖具体是哪种顺序**——唯一硬性要求是：同一个 grid 的所有 per-voxel 张量（feature、`grid_kernel`、`gridAlpha`/solution、`active_grid_coords()` 坐标表、`indexMap` 行号、conv 的输入/输出特征行号）必须共享同一顺序，并在整条流水线中保持不变。
- 移植建议实现：构建 grid 时生成 `coords: int32[M][3]`（体素坐标表，行号即 offset）+ 哈希表 `HashMap<Coord, int64> ijkToOffset`（未激活坐标查询返回 -1）。下文伪代码用两个原语表达：
  - `isActive(ijk) -> bool`
  - `voxelIndex(ijk) -> int64`（前置条件 `isActive`；等价于哈希表查询）
- 注意：本工程其它任务（grid 构建规格）必须生成与此一致的 `coords`/哈希表；只要 A/B/C/D 各任务共用同一 grid 对象即可。

### 1.2 坐标变换 VoxelCoordTransform（半格偏移約定）

均匀标量仿射（三轴同参数）：

```cpp
// world -> grid(局部体素坐标)
apply(x)    = x * scale + translate;        // 逐分量
// grid -> world
applyInv(x) = (x - translate) / scale;
```

每个 grid 由 `voxel_size w` 和 `voxel_origin tx` 定义两套变换（`SparseFeatureIndexGrid::setTransform`）：

```cpp
primal: scale = 1.0 / w, translate = -tx / w;         // 体素中心在 world = tx + ijk * w
dual:   scale = 1.0 / w, translate = -tx / w + 0.5;   // 相对 primal 半格偏移(+0.5 局部单位)
```

- 本文所有 kernel_eval 算子使用的都是各 grid 的 **primalTransform**（bind 层传入 `grid->primalTransform()`）。
- `transform.scale<float>()` 即 `1/w`，在梯度公式中作为链式法则因子（局部导数 → world 导数）。
- 关键精度细节：原实现内部同时存 float 与 double 两份 scale/translate；float 路径取 float 成员。移植用 `float scale = (float)(1.0 / w); float translate = (float)(-tx / w);` 一次转换后全程 float 即可。

### 1.3 NNIterator<N> —— 邻域枚举顺序（核心约定）

对中心整数坐标 `c` 枚举 N×N×N 邻域（N 为奇数），**x 为最外层、z 为最内层**（x-major）：

```cpp
// count ∈ [0, N^3)，双向映射：
// CountFromDelta: delta 分量 ∈ [-N/2, N/2]
count = (dx + N/2) * N*N + (dy + N/2) * N + (dz + N/2);
// DeltaFromCount:
dz = count % N          - N/2;
dy = (count / N) % N    - N/2;
dx = count / (N*N)      - N/2;
// 遍历: for count = 0 .. N^3-1: coord = c + DeltaFromCount(count)
```

- 浮点中心的取整（`nanovdb::Vec3<float>::round`）：**逐分量 `floor(x + 0.5)`**（half-up，非 banker's rounding，非 `rint`）。所有 "以点为中心找 3³ 邻域" 均先 `c = round(p_local)` 再枚举。
- 本文用到 `NNIterator<3>`（27 邻域，kernel 求值）与 `NNIterator<5>` 的 `CountFromDelta`（125 槽位索引，`indexMap` 列号）。

### 1.4 Bezier 基函数（二次样条，支撑 (-1.5, 1.5)）

```cpp
// 一维基函数, x 为局部体素坐标差 (点局部坐标 - 体素整数坐标)
float bezier_1dim(float x) {
    if (x >= -1.5f && x < -0.5f) return (x + 1.5f) * (x + 1.5f);
    if (x >= -0.5f && x <  0.5f) return -2.0f * x * x + 1.5f;
    if (x >=  0.5f && x <  1.5f) return (x - 1.5f) * (x - 1.5f);
    return 0.0f;                                   // |x| >= 1.5 (含 x = ±1.5? 见下)
}
```

分段边界必须与上式的开闭一致（源码用 `x < 边界` 级联判断）：`x = -1.5` 落入第一段（值 0）、`x = -0.5` 落入第二段（值 1.0）、`x = 0.5` 落入第三段（值 1.0）、`x = 1.5` 返回 0。峰值 `bezier_1dim(0) = 1.5`（= 2 × 标准二次 B-spline）。

```cpp
float bezier_grad_1dim(float x) {                  // d/dx bezier_1dim
    if (x >= -1.5f && x < -0.5f) return 2.0f * x + 3.0f;
    if (x >= -0.5f && x <  0.5f) return -4.0f * x;
    if (x >=  0.5f && x <  1.5f) return 2.0f * x - 3.0f;
    return 0.0f;
}
```

### 1.5 核函数求值原语 kernel_grad_evaluation_fwd

所有 kernel_eval 算子的公共内核。给定 "点侧" 特征 `pKernel[P][K]`、"格侧" 特征 `cKernel[M][K]`、点侧特征对 world 坐标的梯度 `gradKernelQuery[P][K][3]`（可为空张量，size(0)==0），以及局部坐标差 `(pcx, pcy, pcz)`（= 点局部坐标 − 体素整数坐标）：

```cpp
// 输入: offset(格侧行号), pi(点侧行号), scale = transform.scale = 1/w
// 输出: f(核值), gradF[3](核值对点 world 坐标的梯度, 仅 grad=true 时有效)
void kernel_grad_evaluation_fwd(
        int64 offset, int64 pi, float scale,
        float pcx, float pcy, float pcz,
        const float* pKernel, const float* cKernel,   // [*, K]
        const float* gradKernelQuery,                 // [P, K, 3] 或空
        bool grad,
        float& f, float gradF[3]) {

    float bx = bezier_1dim(pcx), by = bezier_1dim(pcy), bz = bezier_1dim(pcz);
    float bezierKernel = bx * by * bz;

    float dpKernel = 1.0f;                            // K == 0 时退化为纯 Bezier 核
    if (K > 0) {
        dpKernel = 0.0f;
        for (int64 k = 0; k < K; ++k)
            dpKernel += cKernel[offset][k] * pKernel[pi][k];  // 普通乘加, 无 fma
    }
    f = bezierKernel * dpKernel;

    if (!grad) return;

    float dbx = bezier_grad_1dim(pcx) * scale;        // 链式: 局部导数 * (1/w) = world 导数
    float dby = bezier_grad_1dim(pcy) * scale;
    float dbz = bezier_grad_1dim(pcz) * scale;
    float db[3] = { dbx * by * bz, bx * dby * bz, bx * by * dbz };

    for (int dim = 0; dim < 3; ++dim) {
        float gradDot = 0.0f;
        if (K > 0 && gradKernelQuery_size0 > 0) {     // 两个条件都要: 特征维>0 且梯度张量非空
            for (int64 k = 0; k < K; ++k)
                gradDot += cKernel[offset][k] * gradKernelQuery[pi][k][dim];
        }
        gradF[dim] = bezierKernel * gradDot + dpKernel * db[dim];
    }
}
```

即核函数 `K(x, v) = B((x_local − v)) · ⟨φ(x), ψ_v⟩`，其中 `B(t) = bezier(t.x)·bezier(t.y)·bezier(t.z)`，`φ` 为点侧特征（K 维），`ψ` 为格侧特征；`K == 0` 时点积项恒为 1。`gradF = ∇ₓK`（对 world 坐标），包含 Bezier 部分的解析梯度与特征部分的链式梯度（`gradKernelQuery = ∇ₓφ(x)`，空张量时视为 0，即 `approx_kernel_grad` 模式）。

### 1.6 盒重叠测试 has_overlap

```cpp
bool has_overlap(const float aMin[3], const float aMax[3],
                 const float bMin[3], const float bMax[3]) {
    return aMax[0] >= bMin[0] && bMax[0] >= aMin[0]
        && aMax[1] >= bMin[1] && bMax[1] >= aMin[1]
        && aMax[2] >= bMin[2] && bMax[2] >= aMin[2];   // 闭区间, >= 含边界
}
```

## 2. conv 模块 —— 稀疏卷积

### 2.1 convolution_kernel_map（kmap 构建）

签名与约束：`kmap = convolution_kernel_map(source_grid, target_grid, kernel)`，要求 `source.voxelSize() <= target.voxelSize()`（source 为细/同级，target 为粗/同级），`kernel > 0`。

```cpp
// 输出: kmap int32[targetNumVoxels][kernel^3], 值为 source 体素 offset 或 -1
int stride       = (int) round(target.voxelSize() / source.voxelSize());  // std::round
int kernelVolume = kernel * kernel * kernel;
int kernelStart  = (int) floor(-kernel / 2.0 + 1);   // kernel=3 -> -1; kernel=2 -> 0

for (每个 target 激活体素 (tCoord, tIdx)) {          // tIdx = target 的 voxel offset
    Coord sCoord = tCoord * stride;                  // 逐分量乘
    int kIdx = 0;
    for (int kx = kernelStart; kx < kernelStart + kernel; ++kx)
      for (int ky = kernelStart; ky < kernelStart + kernel; ++ky)
        for (int kz = kernelStart; kz < kernelStart + kernel; ++kz, ++kIdx) {
            Coord sOffset = sCoord + Coord(kx, ky, kz);
            kmap[tIdx][kIdx] = source.isActive(sOffset) ? source.voxelIndex(sOffset) : -1;
        }
}
```

关键点：

- 偏移枚举顺序 = x 最外、z 最内（与 §1.3 一致）；`kernel=3` 时 `kIdx=13` 恰是中心偏移 `(0,0,0)`（conv 前向的中心优化依赖此性质）。
- 偏移作用在 **source 的体素坐标系**上：`sCoord = tCoord * stride + (kx,ky,kz)`。偶数 kernel 时 `kernelStart=0`（偏移集 `{0..kernel-1}`，非对称）。
- 逐 target 体素独立，可并行。

### 2.2 Python 侧 kmap → (nbmap, nbsizes) 变换（必须在 C++ 中复刻）

`Conv3d._compute_conv_args`（`nn/modules.py`）把 kmap 重排成 conv kernel 消耗的紧凑格式。语义如下（等价实现即可，但**顺序必须一致**）：

```cpp
// 输入: kmap[T][KV] (T=target voxels, KV=kernel^3)
// 概念上转置成 kmapT[KV][T] 后:
//   nbsizes[k] = kmapT 第 k 行中 != -1 的个数           (int32[KV])
//   nbmap      = 按 (k 升序, 行内 t 升序) 收集所有有效项 (int32[L][2], L=Σ nbsizes)
//                nbmap[l][0] = kmap[t][k]  (source 体素 offset)
//                nbmap[l][1] = t           (target 体素 offset)
for (int k = 0, l = 0; k < KV; ++k)
    for (int t = 0; t < T; ++t)
        if (kmap[t][k] != -1) { nbmap[l][0] = kmap[t][k]; nbmap[l][1] = t; nbsizes[k] = ...; ++l; }
```

即 nbmap 中条目按 kernel 偏移分组连续存放，组内按 target 体素序号升序；组 k 的长度是 `nbsizes[k]`。

调用侧组织（`Conv3d.forward`）：

- 非转置卷积：`kmap = convolution_kernel_map(in_grid, out_grid, ks)`，`shape = (in.numVoxels, out.numVoxels)`。
- 转置卷积：`kmap = convolution_kernel_map(out_grid, in_grid, ks)`（source/target 对调，即 source=输出细网格、target=输入粗网格），`shape = (out.numVoxels, in.numVoxels)`。此时 `nbmap[l][0]` 是 **out_grid**（细）offset、`nbmap[l][1]` 是 **in_grid**（粗）offset。
- 同 grid 同 depth 的 kmap 以 `(depth, kernel_size)` 为 key 缓存复用。
- `kernel_size==1 && stride==1` 特例：不建 kmap，直接 `out = in_feat.matmul(kernel)`（此时权重形状是 `[Cin][Cout]` 而非 `[1][Cin][Cout]`）。

### 2.3 convolution_forward（gather–GEMM–scatter）

签名：`convolution_forward_cpu(in_feat, out_feat, kernel, neighbor_map, neighbor_offset, transpose)`。

张量布局（全部行优先连续）：

| 张量 | 形状 | 说明 |
| --- | --- | --- |
| `in_feat` | `float[Nin][Cin]` | 输入特征，行号 = 输入 grid 体素 offset |
| `out_feat` | `float[Nout][Cout]` | 输出特征，调用前按 0 初始化；`Nout = transpose ? shape[0] : shape[1]`（两种情况均 = 输出 grid 的 numVoxels） |
| `kernel` | `float[KV][Cin][Cout]` | **权重维序 [kernel_volume][in_channels][out_channels]**；`kernel[i]` 是第 i 个偏移的 `[Cin][Cout]` 矩阵 |
| `neighbor_map` | `int32[L][2]` | §2.2 的 nbmap |
| `neighbor_offset` | `int32[KV]` | §2.2 的 nbsizes |

gather / scatter 的索引方向由 `transpose` 决定（`kmap` 指向 nbmap 的扁平 int 指针）：

```cpp
// gather: 从 in_feat 取 n_k 行到连续 buffer
void gather(int n_k, int c, const float* in_feat, float* buf,
            const int* kmap /*本组 nbmap 起点*/, bool transpose) {
    for (int i = 0; i < n_k; ++i) {
        int in_pos = kmap[2*i + (transpose ? 1 : 0)];   // 非转置取 nbmap[i][0], 转置取 [1]
        if (in_pos < 0) continue;                       // 防御(实际不会出现)
        for (int j = 0; j < c; ++j) buf[i*c + j] = in_feat[in_pos*c + j];
    }
}
// scatter: buffer 累加回 out_feat (+=, 非覆盖)
void scatter(int n_k, int c, const float* buf, float* out_feat,
             const int* kmap, bool transpose) {
    for (int i = 0; i < n_k; ++i) {
        int out_pos = kmap[2*i + (transpose ? 0 : 1)];  // 非转置取 nbmap[i][1], 转置取 [0]
        if (out_pos < 0) continue;
        for (int j = 0; j < c; ++j) out_feat[out_pos*c + j] += buf[i*c + j];
    }
}
```

前向主体：

```cpp
void convolution_forward(...) {
    require(Cin == kernel.size(1));
    out_feat.zero();
    int KV = kernel.size(0);

    // 中心优化: KV 为奇数且 Nout == Nin (同 grid stride=1 卷积) 时,
    // 中心偏移 (kIdx = KV/2) 是恒等映射, 直接一次 GEMM 完成:
    bool centerFlag = (KV % 2 == 1) && (out_feat.rows == in_feat.rows);
    if (centerFlag)
        out_feat = in_feat * kernel[KV/2];              // [Nin,Cin]x[Cin,Cout], 覆盖写(此前为0)

    // 临时 buffer 尺寸 = 各组(centerFlag 时排除中心组) nbsizes 的最大值, 至少 1
    int bufRows = centerFlag
        ? max(1, max(nbsizes[0 .. KV/2-1] ∪ nbsizes[KV/2+1 .. KV-1]))
        : max(nbsizes[0 .. KV-1]);
    float* inBuf  = new float[bufRows * Cin];
    float* outBuf = new float[bufRows * Cout];

    int cur = 0;                                        // nbmap 扁平 int 游标(单位: int, 每条目2个)
    for (int i = 0; i < KV; ++i) {
        if (centerFlag && i == KV/2) { cur += 2 * nbsizes[i]; continue; }
        if (nbsizes[i] == 0) continue;
        int n = nbsizes[i];
        gather (n, Cin,  in_feat.data, inBuf,  nbmap.data + cur, transpose);
        // GEMM: outBuf[n][Cout] = inBuf[n][Cin] * kernel[i][Cin][Cout]
        matmul(outBuf, inBuf, kernel[i], n, Cin, Cout);
        scatter(n, Cout, outBuf, out_feat.data, nbmap.data + cur, transpose);
        cur += 2 * nbsizes[i];
    }
}
```

要点：

- 转置卷积**不转置权重矩阵**，只交换 gather/scatter 的索引列；权重仍是 `[KV][Cin][Cout]`，`Cin` = 该层输入通道。
- `centerFlag` 只看行数相等 + KV 奇数；命中时中心组的 nbmap 条目被跳过（游标仍要步进）。
- scatter 目标行可重复 ⇒ 若并行化，须按输出行归约或对 i 循环内的 scatter 保持串行；原实现只在通道维开 OpenMP（无竞争）。
- bias 在 Python 层加（`out_feature += bias`，逐行广播），移植时并入本层实现即可。

## 3. kernel_eval 模块

### 3.1 公共输入约定

- 点坐标 `pts/query/pos_xyz/normal_xyz`：`float[P][3]` world 坐标；进入算子后先 `p_local = transform.apply(pts[pi])`。
- 点侧核特征 `pts_kernel/query_kernel`：`float[P][K]`（K 可为 0，推理默认由 interpolator 生成 K>0 特征；`evaluate_kernel` 无 interpolator 时给 `[P][0]`）。
- 格侧核特征 `grid_kernel`：`float[M][K]`，M = grid.numVoxels（推理时 = `features.basis_features[d]`）。
- 点侧特征梯度 `grad_kernel_pts`：`float[P][K][3]` 或空 `[0][*][3]`（空即忽略该项，见 §1.5）。
- 所有 "点 × 体素" 配对都通过同一模式产生：`c = round(p_local)`，枚举 `NNIterator<3>` 的 27 个邻域坐标，跳过非激活体素；局部坐标差 `pc = p_local - (float)coord`，分量必然落在 `[-1.5, 1.5]`。

### 3.2 kernel_evaluation（field 求值）

绑定签名：`(outFunc, outGradFunc) = kernel_evaluation(grid, query, query_kernel, grid_kernel, grid_alpha, grad_kernel_query, grad=false)`。`grid_alpha: float[M]` 是求解得到的每体素系数（`solutions[d]`）。输出 `outFunc: float[P]`；`grad=true` 时 `outGradFunc: float[P][3]`，否则形状 `[0][3]` 不写。

```cpp
for (int64 pi = 0; pi < P; ++pi) {                     // 可按 pi 并行, 无写冲突
    float3 p = transform.apply(query[pi]);
    float  func = 0; float3 dfunc = {0,0,0};
    for (Coord it : NN3(round(p))) {                   // §1.3 顺序
        if (!isActive(it)) continue;
        int64 offset = voxelIndex(it);
        float kiv; float gradKiv[3];
        kernel_grad_evaluation_fwd(offset, pi, scale,
            p[0]-it[0], p[1]-it[1], p[2]-it[2],
            queryKernel, gridKernel, gradKernelQuery, grad, kiv, gradKiv);
        func  += gridAlpha[offset] * kiv;
        if (grad) dfunc += gridAlpha[offset] * gradKiv;
    }
    outFunc[pi] = func;
    if (grad) outGradFunc[pi] = dfunc;
}
```

即 `f(x) = Σ_{v∈27邻域∩激活} α_v · K(x, v)`。多层 field 的求和（对 depth 累加）在 Python 层完成，不属于本算子。

### 3.3 build_coo_indexer（125 槽位 COO 索引器）

签名：`indexer = build_coo_indexer(grid_i, grid_j)`，输出 `int32[Mi][125]`，初值全 -1。语义：对 grid_i 的每个体素 i，标出 grid_j 中与 i 的核支撑可能重叠的体素 j，槽位号是 j 相对 "i 映射到 j 坐标系" 的 5³ 邻域编号。

```cpp
Tensor indexer = full({Mi, 125}, -1, int32);
const float3 primalRange = {2.5f, 2.5f, 2.5f};         // 局部体素单位

for (每个 grid_i 激活体素 (iCoord, iIdx)) {
    float3 iPrimal = (float3) iCoord;
    float3 world   = Ti.applyInv(iPrimal);             // i 体素中心 world
    float3 jc      = Tj.apply(world);                  // 该中心在 j 局部坐标
    for (NNIterator<5> jt(jc); jt.isValid(); ++jt) {   // 中心 = round(jc), 125 个
        if (!gridJ.isActive(*jt)) continue;
        float3 jPrimal = (float3)(*jt);
        // 世界空间盒重叠测试: 各自 ±2.5 体素(局部)映射回 world
        if (!has_overlap(Ti.applyInv(iPrimal - primalRange), Ti.applyInv(iPrimal + primalRange),
                         Tj.applyInv(jPrimal - primalRange), Tj.applyInv(jPrimal + primalRange)))
            continue;
        indexer[iIdx][jt.getCount()] = gridJ.voxelIndex(*jt);   // 槽位号 = NN5 枚举序号
    }
}
```

注意：

- 计算全程 float（源码 `NNIterator<5, float>`）。`applyInv(iPrimal ± 2.5)` 因 scale>0 保序，min/max 无需再排序。
- 槽位号与 §1.3 的 `CountFromDelta(delta) = (dx+2)*25 + (dy+2)*5 + (dz+2)` 一一对应，`delta = jCoord - round(jc)`。

### 3.4 indexMap 的构造（Python 层逻辑，需复刻）

`KernelField.solve` 把 `indexer` 转成 `indexMap`（后续三个算子的输入）与块 COO 结构：

```cpp
// 输入: indexer int32[Mi][125]
// 1) 收集有效槽位, 行优先扫描顺序 (i 升序, 槽位升序) —— 等价 torch.where(indexer != -1)
//    d_inds[e]  = i 行号, dd_inds[e] = indexer 里存的 j offset, e = 0..E-1
// 2) indexMap = indexer 的副本(CPU 用 int64), 把每个有效槽位的值替换为其枚举序号 e:
//    indexMap[i][slot] = e   (有效);  = -1 (无效, 保持)
// E = 有效槽位总数 = 该 (d, dd) 块的 COO 非零元个数 (numEntries)
```

- `d_inds/dd_inds` 是该块 COO 的 (row=grid_i offset, col=grid_j offset)；`E = numEntries` 传给 `matrix_building/k_building` 用于分配输出 `outMatrix: float[E]`（零初始化）。
- CPU 路径强制 `indexMap` 为 int64（源码：`device=="cpu"` 时 `mat_indexer_type = torch.long`）。
- 块遍历顺序（求解器组装约定，见任务 D）：`d` 从 depth-1 递减到 0，`dd` 从 depth-1 递减到 `d`（只装上三角块，`dd >= d`，即 grid_i = 细层 grids[d]、grid_j = 粗/同层 grids[dd]）。

### 3.5 matrix_building（GᵀG 与 QᵀQ 组装）

绑定签名：`out = matrix_building(grid_i, grid_j, pts_pos, pts_kernel_i, pts_kernel_j, i_kernel, j_kernel, grad_pts_kernel_pos_i, grad_pts_kernel_pos_j, index_map, grad, num_entries)[0]`。输出 `outMatrix: float[numEntries]`，零初始化后累加。

- `grad=false`（GᵀG，位置约束）：累加 `K(x_k, v_i) · K(x_k, v_j)`；此时两个 grad 张量传 `[0][0][3]` 空。
- `grad=true`（QᵀQ，法向约束）：累加 `∇ₓK(x_k, v_i) · ∇ₓK(x_k, v_j)`（3 维点积）。

```cpp
for (int64 pi = 0; pi < P; ++pi) {                     // 每点独立计算, 但对 outMatrix 累加有竞争,
    float3 pI = Ti.apply(ptsPos[pi]);                  // 并行时需 atomic 或分块归约
    for (Coord it : NN3(round(pI))) {                  // i 侧 27 邻域
        if (!gridI.isActive(it)) continue;
        int64 offsetI = gridI.voxelIndex(it);
        float kiF; float gradKiF[3];
        kernel_grad_evaluation_fwd(offsetI, pi, Ti.scale,
            pI[0]-it[0], pI[1]-it[1], pI[2]-it[2],
            ptsKernelI, iKernel, gradPtsKernelPosI, grad, kiF, gradKiF);

        float3 pJ = Tj.apply(ptsPos[pi]);
        for (Coord jt : NN3(round(pJ))) {              // j 侧 27 邻域
            if (!gridJ.isActive(jt)) continue;
            int64 offsetJ = gridJ.voxelIndex(jt);
            float kjF; float gradKjF[3];
            kernel_grad_evaluation_fwd(offsetJ, pi, Tj.scale,
                pJ[0]-jt[0], pJ[1]-jt[1], pJ[2]-jt[2],
                ptsKernelJ, jKernel, gradPtsKernelPosJ, grad, kjF, gradKjF);

            float outVal = grad ? dot3(gradKiF, gradKjF) : (kiF * kjF);

            // 槽位: 把 i 坐标映射进 j 坐标系并取整(floor(x+0.5)), 求 5^3 槽位号
            Coord iC = round(Tj.apply(Ti.applyInv((float3)it)));
            int   slot = CountFromDelta5(jt - iC);      // §1.3, N=5
            int64 e = indexMap[offsetI][slot];          // 必然 >= 0 (由 §3.3/§3.4 保证; 建议 assert)
            outMatrix[e] += outVal;
        }
    }
}
```

要点：

- `offsetJ` 参与 `kernel_grad_evaluation_fwd`（作为格侧行号），也是槽位查询的隐含列——但 `outMatrix` 索引只经 `indexMap[offsetI][slot]`。
- 同一 (i, j) 单元被多个点累加；同一块内既有 i 行也覆盖 j 的所有邻近列，块本身非对称三角（对称性在求解器层利用：只装 `dd >= d` 的块，任务 D 负责镜像）。
- 边界点（`pc` 恰为 ±1.5）产生 f=0 的贡献仍会执行 `+= 0`，槽位保证有效。
- GᵀG 调用传入的点集可能经 nystrom 下采样（`d >= nystrom_min_depth` 时；默认 `nystrom_min_depth=100` 实际不触发），QᵀQ 的点集固定为 `normal_xyz`（各层体素中心拼接）。

### 3.6 k_building（对角块正则项 K(i,j)）

绑定签名：`out = k_building(grid, kernel, index_map, num_entries)[0]`，仅用于 `d == dd` 的对角块（grid_i == grid_j，同一 transform）。输出 `outMatrix: float[numEntries]` 零初始化。

```cpp
for (每个激活体素 (iCoord, offsetI)) {
    for (Coord jt : NN3(iCoord)) {                     // 整数中心, 27 邻域
        if (!isActive(jt)) continue;
        Coord diff = jt - iCoord;                      // 各分量 ∈ {-1,0,1}
        int64 offsetJ = voxelIndex(jt);
        // K(i,j) = B(diff) * <kernel[offsetJ], kernel[offsetI]>
        // 注意实参顺序: 格侧行号 = offsetJ, 点侧行号 = offsetI, 两侧特征都是同一 kernel 张量
        float ijF; float dummy[3];
        kernel_grad_evaluation_fwd(offsetJ, offsetI, T.scale,
            (float)diff[0], (float)diff[1], (float)diff[2],
            kernel, kernel, /*gradKernelQuery=*/empty, /*grad=*/false, ijF, dummy);

        int   slot = CountFromDelta5(diff);            // 同 grid: iC == iCoord
        int64 e = indexMap[offsetI][slot];
        outMatrix[e] += ijF;                           // 每个 (i,j) 恰好加一次
    }
}
```

`diff` 各分量为整数，`bezier_1dim(0)=1.5`、`bezier_1dim(±1)=0.25` ⇒ `B(diff) = Π 1.5^{[d=0]}·0.25^{[|d|=1]}`（如 `B(0,0,0)=3.375`、`B(1,0,0)=0.5625`、`B(1,1,1)=0.015625`）。推理中 `reg_weight = 1.0`，即 `lhs += 1.0 * k_building(...)`。

### 3.7 rhs_evaluation（右端项 Qᵀn）

绑定签名：`out = rhs_evaluation(grid, pts, pts_kernel, grid_kernel, grad_kernel_pts, pts_data)[0]`。`pts_data: float[P][3]`（推理传 `normal_value = -各层法向特征`，负号在 Python 层）。输出 `outRhs: float[M]`（M = grid.numVoxels）零初始化。**强制 grad=true 求梯度**。

```cpp
for (int64 pi = 0; pi < P; ++pi) {                     // outRhs[offset] 累加有竞争
    float3 p = T.apply(pts[pi]);
    float3 n = { ptsData[pi][0], ptsData[pi][1], ptsData[pi][2] };
    for (Coord it : NN3(round(p))) {
        if (!isActive(it)) continue;
        int64 offset = voxelIndex(it);
        float kiv; float gradKiv[3];
        kernel_grad_evaluation_fwd(offset, pi, T.scale,
            p[0]-it[0], p[1]-it[1], p[2]-it[2],
            ptsKernel, gridKernel, gradKernelPts, /*grad=*/true, kiv, gradKiv);
        outRhs[offset] += dot3(n, gradKiv);            // rhs_v += n_k · ∇ₓK(x_k, v)
    }
}
```

Python 层再乘标量 `normal_weight`。推理默认超参（`__init__.py`）：`pos_weight = hparams.solver.pos_weight / N_pts`，`normal_weight = hparams.solver.normal_weight / N_normal * voxel_size²`，`reg_weight = 1.0`（hparams 数值属任务 A/权重清单范围）。

### 3.8 qg_building 与 csr_matrix_multiplication（不移植，参考规格）

仅 `solve_non_fused`（`fused_mode=False`，非默认）使用；原库 CPU 版直接抛异常，纯 CPU 移植不需要。若将来实现，按 CUDA 语义：

- `qg_building(grid, pts, pts_kernel, grid_kernel, grad_kernel_pts, grad)` → `(qg, indexer)`：`indexer int32[P][27]` 初值 -1；对每点枚举 NN3(round(p_local))，激活体素处 `indexer[pi][it.count] = offset`；`grad=false` 时 `qg float[P][27] = K(x,v)`，`grad=true` 时 `qg float[P][27][3] = ∇ₓK`。（`qg` 用 `empty` 分配，无效槽位为垃圾值，消费方只读有效槽位。）
- `csr_matrix_multiplication`：两个按点分组的 CSR 稀疏矩阵做 AᵀB，行组 batch = 点号，`out[indexMap[colI][slot(colJ 相对 colI 的 5³ delta)] ] += Σ value_i · value_j`（值为标量或 3 维点积，与 §3.5 的 slot 计算完全相同）。

## 4. 数值与实现注意

- 全程 float32；累加顺序会引入 1e-6 量级差异，验收时用相对误差而非逐位相等。`dpKernel` 用普通乘加（源码注释掉了 fmaf）。
- `round` 一律 `floor(x + 0.5)`（§1.3）；`stride` 用 `std::round`（§2.1）；`iC` 槽位取整也是 `floor(x+0.5)`（§3.5）。
- 除 conv 的 GEMM 外全部是标量循环；建议并行化维度：kmap/keval 按体素或点（无冲突），matrix_building/rhs 按点分块 + 线程私有累加或 atomic。
- 越界防御：`indexMap[...] == -1` 理论上不可达（§3.3 的 2.5 范围盒测试覆盖 1.5+1.5 支撑），移植时加 `check(e >= 0)` 便于捕获 grid 构建不一致。
- 空输入：`P == 0` 或 grid 为空时各算子返回全零输出（原实现循环自然跳过）。

## 5. 验收对拍建议

对每个算子用 Python 原库（CPU device）生成小规模随机夹具：随机点云 → `SparseFeatureHierarchy` → 抓取各算子的输入/输出张量存 `.npz`，C++ 侧读入比对（rtol=1e-4, atol=1e-5）。重点回归用例：

- kmap：kernel=3/stride=1（同 grid）、kernel=2/stride=2（下采样）、转置方向。
- conv：centerFlag 命中与不命中、某些 `nbsizes[i]==0`、转置卷积。
- kernel_eval 系列：K=0（纯 Bezier）、K>0、`grad_kernel_*` 空与非空、点落在体素边界 x.5 处（round half-up 敏感点）。
