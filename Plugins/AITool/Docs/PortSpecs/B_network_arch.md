# NKSR 移植规格 B：网络架构与权重键（ks 配置）

本文档规定 NKSR 推理路径中神经网络部分的完整结构、每层前向数学与 `state_dict` 权重键，供纯 C++ (CPU) 移植使用。移植工程师只依据本文档实现，不阅读原始 Python 源码。

覆盖范围：`nksr.Reconstructor(device)`（默认 `config='ks'`，权重 `ks.pth`）→ `reconstruct(xyz, normal, detail_level=D)` 的网络前向与其输出如何进入 kernel field 求解。稀疏体素结构（IndexGrid）与底层算子（`build_point_splatting`、`ijk_to_index`、`sample_trilinear`、`max_pool`、`subdivide` 等）的精确语义见任务 C 规格；kernel field 求解与 dual mesh 提取见任务 D/E 规格。

源码依据：`package/nksr/__init__.py`、`nn/modules.py`、`nn/unet.py`、`nn/encdec.py`、`interpolator.py`、`configs.py`、`csrc/conv/kmap_cpu.cpp`、`csrc/conv/convolution_cpu.cpp`、`svh.py`、`fields/kernel_field.py`、`fields/neural_field.py`（交叉验证：`models/nksr_net.py`）。

## 1. ks 超参数（决定网络结构的全部数值）

`configs.py` 中 `__configs__['ks']`（无 parent，直接生效）：

```yaml
feature: "normal"          # 编码器输入附加特征 = 点法线 → PointEncoder dim=6
geometry: "kernel"         # 走 KernelField 分支（interpolators 存在，无 sdf_decoder）
voxel_size: 0.1            # 最细层体素边长（世界单位）
kernel_dim: 4              # basis/theta 特征维度（basis_channels、interpolator theta_dim）
tree_depth: 4              # 层级数 D=4，depth 0（最细）..3（最粗）
adaptive_depth: 2          # normal head 只在 depth 0,1 评估；法线约束点取 depth 0,1
unet: { f_maps: 32 }       # UNet 基础通道数
udf: { enabled: true }     # 启用 udf_heads (16ch) 与 udf_decoder（mask field）
interpolator: { n_hidden: 2, hidden_dim: 16 }   # 每层 MLP 插值器结构
solver: { pos_weight: 1.0e4, normal_weight: 1.0e4 }
density_range: [1.0, 20.0] # detail_level → 密度缩放范围
```

`NKSRNetwork.__init__` 中的派生常量：

```text
encoder            = PointEncoder(dim=6, c_dim=32, hidden_dim=32, n_blocks=3)
interpolators[d]   = MLPFeatureInterpolator(theta_dim=4, n_hidden=2, hidden_dim=16), d ∈ {0,1,2,3}
unet               = SparseStructureNet(in_channels=32, num_blocks=4, basis_channels=4,
                                        normal_channels=3, f_maps=32, udf_branch_dim=16,
                                        order='gcr', num_groups=8, pooling='max',
                                        upsample='nearest', neck_type='dense', neck_expand=1)
udf_decoder        = MultiscalePointDecoder(c_each_dim=16, multiscale_depths=4,
                                            out_init=0.5, coords_depths=[2,3],
                                            p_dim=3, out_dim=1, hidden_size=32,
                                            n_blocks=2, aggregation='cat')
```

UNet 通道表 `n_features = [in] + [f_maps * 2^k] = [32, 32, 64, 128, 256]`。

## 2. 数据布局总约定

- 所有特征张量为 `float32`，行主序（row-major），形状 `[N, C]`：N = 当前稀疏网格的活跃体素数（或点数），C = 通道数。
- **特征行序 = 该 IndexGrid 的体素索引序**（由 NanoVDB 活跃体素遍历顺序决定，见任务 C）。同一 grid 上的所有算子共享此顺序；跨 grid 搬运一律通过 `ijk_to_index` 完成，绝不假设两个 grid 顺序一致。
- `nn.Linear`：权重 `weight` 形状 `[out, in]`，`y = x @ weight^T + bias`。即 `y[o] = Σ_i x[i] * weight[o][i] + bias[o]`。
- 稀疏卷积核 `Conv3d.kernel`：形状 `[K^3, C_in, C_out]`（K=3 时 `[27, C_in, C_out]`）；K=1 时为二维 `[C_in, C_out]`。注意这与 torch 常规 conv 布局不同，**已经是 in×out 方向，使用时不需要转置**：`out_row = in_row @ kernel[k]`。
- SVH（SparseFeatureHierarchy）：depth d 的 grid 体素边长 `voxel_size * 2^d`，origin（grid 坐标 0 的世界位置，即体素中心）= `0.5 * voxel_size * 2^d`。体素角点对齐世界原点；`world_to_grid(x) = x / vs_d − 0.5`（以体素中心为整数格点，最终以任务 C 规格为准）。

## 3. 基础层前向数学

### 3.1 ResnetBlockFC（pre-activation 残差块）

构造 `ResnetBlockFC(size_in, size_out=None, size_h=None)`：`size_out` 缺省 = `size_in`；`size_h` 缺省 = `min(size_in, size_out)`。子层：`fc_0: Linear(size_in, size_h)`、`fc_1: Linear(size_h, size_out)`、`shortcut: Linear(size_in, size_out, bias=False)`（仅当 `size_in != size_out` 存在，否则恒等）。

```text
net = fc_0(relu(x))        // 注意：ReLU 在 fc_0 之前（pre-activation）
dx  = fc_1(relu(net))
out = (shortcut ? shortcut(x) : x) + dx
```

### 3.2 GroupNorm（稀疏版，全局统计）

`nn.GroupNorm(num_groups=8, num_channels=C, eps=1e-5, affine=True)`，参数 `weight[C]`、`bias[C]`。输入 `[N, C]` 先转置 reshape 为 `[1, C, N]` 再做标准 GroupNorm，等价数学：

- 通道被均分为 G=8 组，组 g 覆盖通道 `[g*C/8, (g+1)*C/8)`。
- 每组的 `μ_g`、`σ²_g` 在 **该组全部通道 × 全部 N 个体素** 上联合计算（batch=1，整张稀疏张量共享统计）。方差为有偏估计（除以元素个数 `n = N*C/8`，不是 n−1）。
- `y[n][c] = (x[n][c] − μ_g) / sqrt(σ²_g + 1e-5) * weight[c] + bias[c]`。

`SparseConvBlock` 中 order='gcr'，'g' 位于 'c' 之前 → GroupNorm 的 `num_channels` = 该 block 的 **输入** 通道数。ks 路径中所有 GN 通道数 ≥ 32 且被 8 整除，`num_groups` 恒为 8（`num_channels < num_groups` 时退化为 1 组的规则在 ks 中不触发）。

边界情况：N=0 时统计为 NaN；ks 推理路径在调用 head 前已保证 N>0（见 5.3 提前终止规则），移植时可 assert N>0。

### 3.3 Activation

order='gcr' 只用 `ReLU`（`max(x, 0)`，inplace 与否不影响数值）。`LeakyReLU(0.1)`（order 含 'l'）在 ks 中不出现——不移植。

### 3.4 Conv3d（稀疏卷积，ks 中只有 K=3/stride=1 与 K=1/stride=1）

构造 `Conv3d(in_channels, out_channels, kernel_size, stride=1, bias, transposed=False)`。ks 路径中：

- 所有 K=3 卷积均为 stride=1、同 grid、同 depth（in/out 活跃体素集合相同）、`bias=False`（order 含 'g'）。
- 所有 K=1 卷积（heads 的 `OutConv` 与 `OneConv0`）直接做矩阵乘：`out = in @ kernel (+ bias)`，kernel 形状 `[C_in, C_out]`。
- `transposed=True`（upsample='deconv'）与 stride=2（pooling='conv'）分支在 ks 中不出现——不移植。

**Kernel offset 枚举顺序**（`kmap_cpu.cpp`，与任务 C 规格一致，此处为权威定义）：

```cpp
kernelStart = floor(-K/2.0 + 1);            // K=3 → -1
kIdx = 0;
for (kx = kernelStart; kx < kernelStart+K; ++kx)      // x 最外层
  for (ky = kernelStart; ky < kernelStart+K; ++ky)    // y 中间
    for (kz = kernelStart; kz < kernelStart+K; ++kz, ++kIdx)  // z 最内层
      offset[kIdx] = (kx, ky, kz);
// K=3: kIdx = (kx+1)*9 + (ky+1)*3 + (kz+1)；中心 (0,0,0) 的 kIdx = 13 = 27/2
```

**卷积语义（stride=1、同 grid）**：offset 加在**输入坐标**上（相关 correlation 方向，非翻转卷积）：

```text
对每个输出体素 o（grid 坐标 c_o，体素索引同输入 grid）：
  out[o][co] = Σ_{k=0..26} Σ_{ci} in[ idx(c_o + offset[k]) ][ci] * kernel[k][ci][co]
  （c_o + offset[k] 非活跃体素时该项跳过；bias 存在时最后加 bias[co]）
```

参考实现（原版 gather–matmul–scatter，逐 offset 桶分组）可直接展开为上式，两者数学等价；原实现对中心桶 k=13 用 `out = in @ kernel[13]` 整体矩阵乘加速（条件：K^3 为奇数且 in/out 行数相同），移植时按直接求和实现即可。

kmap 结构（若按原实现分桶）：`nbmap` 为 `[M, 2]` int32，每行 `(input_voxel_idx, output_voxel_idx)`，按 kIdx 升序分桶、桶内按输出体素索引升序；`nbsizes[k]` 为桶大小。缓存键 `(depth, kernel_size)`（同一 svh 同 depth 的 K=3 kmap 可复用；推理结束后 `clear_svh_kernel_maps()` 清空）。

### 3.5 MaxPooling（kernel_size=2）

输入 depth d → 输出 depth d+1。ks 中输出 grid 恒已存在（encoder svh 由 point splatting 独立构建各层）。逐通道：

```text
out[c_coarse][ch] = max over f ∈ (2*c_coarse + {0,1}^3) 且 f 在细 grid 活跃 的 in[f][ch]
无任何活跃子体素的粗体素 → 结果为 -inf，随后统一替换为 0.0
```

注意：粗 grid 与细 grid 由 splatting 独立生成，**存在粗体素无细子体素的情况**（必须实现 −inf→0 替换）。

### 3.6 Upsampling（scale_factor=2, mode='nearest'）

输入 depth d → 输出 depth d−1。ks 解码路径中输出 grid 不存在，需现场构建：`out_grid = in_grid.subdivided_grid(2, mask)`，即只对 `mask=true` 的输入体素生成其 2×2×2 子体素（mask 为按输入体素索引序的 bool 向量；首次调用外部传入，见 5.3）。然后：

```text
out[f][ch] = in[ parent(f) ][ch]，parent(f) = floor(f / 2) 逐分量（负坐标向下取整）
```

mode='trilinear' 分支不移植。

### 3.7 SparseZeroPadding（跨 grid 零填充/重排）

同 depth，将特征从 in_grid 搬到 out_grid（行序换成 out_grid 的体素索引序）：

```text
out = zeros(out_grid.num_voxels, C)
for j in 0..out_grid.num_voxels-1:
    i = in_grid.ijk_to_index(out_grid.active_grid_coords()[j])
    if (i != -1) out[j] = in[i];
```

支持一维 bool/byte 向量（用于 upsample_mask 的搬运，填充值为 0/false）。若 in_grid 与 out_grid 是同一对象则直接返回原特征。

## 4. PointEncoder（逐点 PointNet + 体素池化）

构造：`fc_pos: Linear(6, 64)`；`blocks: 3 × ResnetBlockFC(64, 32)`（size_h=32，均含 shortcut `[32,64]`）；`fc_c: Linear(32, 32)`。调用 `encoder(xyz, normal, svh, depth=0)`，grid = 最细层 grid。

```text
g     = grid.world_to_grid(xyz)                  // [N,3] float，体素中心为整数
vid   = grid.ijk_to_index(round(g))              // torch.round = 银行家舍入（half-to-even）！
local = (g + 0.5) mod 1                          // ∈ [0,1)，体素内相对坐标（0 = 体素下角）
                                                 // mod 为 Python 语义：结果非负 = x − floor(x)
mask  = (vid != -1)；丢弃 mask=false 的点（vid、local、normal 同步过滤）

h = fc_pos(concat[local(3), normal(3)])          // [n,64]，顺序：先 local 后 normal
h = blocks[0](h)                                 // [n,32]
for b in {blocks[1], blocks[2]}:
    pooled = scatter_max(h, vid, dim_size=num_voxels)   // 逐通道：每个体素内所有点取 max
    h = b(concat[h(32), pooled[vid](32)])               // [n,64] → [n,32]
c = fc_c(h)                                      // [n,32]
out = scatter_mean(c, vid, dim_size=num_voxels)  // [num_voxels,32]；无点体素 → 全 0
```

`scatter_max` 空桶值不会被读取（只经 `pooled[vid]` 回读非空桶）；`scatter_mean` 空桶必须输出 0（splatting 会激活点周围 8 邻域体素，无点体素普遍存在）。

## 5. SparseStructureNet（UNet 主干）

### 5.1 构造与子模块命名（决定权重键）

子模块通过 `add_module(名字, ...)` 挂到 `ModuleList` 上，**名字含负数后缀**，权重键原样出现：

| 容器 | 成员名（顺序即前向顺序） | 构成 |
| --- | --- | --- |
| `encoders` | `Enc0..Enc3` | `SparseDoubleConv(encoder=True)`，Enc0 无 pooling，Enc1..3 前置 `MaxPool(2)` |
| `decoders` | `Dec-2, Dec-3, Dec-4` | `SparseDoubleConv(encoder=False)` |
| `upsamplers` | `Up-2, Up-3, Up-4` | `Upsampling(2,'nearest')`，无参数 |
| `struct_heads` | `Struct-1..Struct-4` | `SparseHead(C→3)` |
| `udf_heads` | `UDF-1..UDF-4` | `SparseHead(C→16)` |
| `normal_heads` | `Normal-1..Normal-4` | `SparseHead(C→3)` |
| `basis_heads` | `Basis-1..Basis-4` | `SparseHead(C→4, enhanced=True)` |
| `padding` | — | `SparseZeroPadding`，无参数 |

名字后缀 `-j` ↔ 特征 depth = `4 − j`（`Struct-1` 在 depth 3 / 256ch，`Struct-4` 在 depth 0 / 32ch）。

`SparseConvBlock(in, out, order='gcr', groups=8, K=3)` 内部成员名固定为 `GroupNorm`（作用于 in 通道）、`Conv`（`Conv3d(in,out,K,1,bias=False)`）、`ReLU`，前向按 GN → Conv → ReLU。

`SparseDoubleConv` 通道规则：

- encoder 侧：`conv1_out = max(out/2, in)`；`SingleConv1: in→conv1_out`，`SingleConv2: conv1_out→out`。Enc1..3 在 SingleConv1 前有 `MaxPool`。
- decoder 侧：`SingleConv1: in→out`，`SingleConv2: out→out`。

`SparseHead`（非 enhanced）：`SingleConv: SparseConvBlock(C→C, K=3)` + `OutConv: Conv3d(C→out, K=1, bias=True)`。
`SparseHead`（enhanced，仅 basis_heads）：`SingleConv: C→C (K=3)` → `SingleConv2: C→mid (K=3)` → `OneConv0: mid→mid (K=1, SparseConvBlock 形式，Conv 无 bias)` → `OutConv: Conv3d(mid→4, K=1, bias=True)`，其中 `mid = min(64, C)`。

### 5.2 encoder 路径

输入：`feat [n_vox_d0, 32]`（PointEncoder 输出），`enc_svh`（4 层 grid 均已由 splatting 构建），初始 `feat_depth=0`。

```text
enc_feats = {}
feat = Enc0(feat, enc_svh, 0)          // depth 不变
enc_feats[0] = feat                    // 32ch
for k in 1..3:
    feat = Enck(feat, enc_svh, k-1)    // 内部 MaxPool: depth k-1 → k，然后两个 conv block
    enc_feats[k] = feat                // 64/128/256 ch
// 结束时 feat_depth = 3
```

### 5.3 neck 与 decoder 路径（含结构剪枝）

创建两个空 SVH（与 enc_svh 同 voxel_size/depth）：`dec_svh`（剪枝后网格，最终返回给 KernelField）与 `tmp_svh`（剪枝前网格，最终作为 udf_svh 返回）。

**neck（neck_type='dense', neck_expand=1 → padding 0）**：取 `enc_svh.grids[3]` 活跃坐标的逐轴 min/max，生成稠密长方体坐标集 `{(x,y,z) : min ≤ · ≤ max}`（含端点；生成序 x 外层、z 最内层，但行序最终由 grid 自身索引决定），`tmp_svh.build_from_grid_coords(3, coords)`。`dec_main = ZeroPad(enc_feats[3], enc_grid3 → tmp_grid3)`。

**主循环**（it = 0..3，d = 3−it；heads 按 `Struct-(it+1)` 等选取）：

```text
for it in 0..3:
    d = 3 - it
    if it > 0:
        // 上采样：输入是 dec_svh.grids[d+1] 上的特征（已剪枝、已重排）
        // tmp_svh.grids[d] 现场构建 = up_mask 为 true 的体素的 2×2×2 子体素
        dec_main = UpsampleNearest(dec_main, in=dec_svh@d+1, out=tmp_svh@d, mask=up_mask)
        enc_skip = ZeroPad(enc_feats[d], enc_svh@d → tmp_svh@d)
        dec_main = concat[enc_skip, dec_main]        // 通道顺序：encoder skip 在前！
        dec_main = Dec-(it+1)(dec_main, tmp_svh, d)  // 384→128 / 192→64 / 96→32

    struct = Struct-(it+1)(dec_main, tmp_svh@d)      // [n_tmp, 3] 原始 logits
    res.structure_features[d] = struct               // 推理下游不消费，可只留 decision
    res.udf_features[d] = UDF-(it+1)(dec_main, tmp_svh@d)   // [n_tmp, 16]，挂在 tmp grid 上

    decision = argmax(struct, dim=1)                 // 0=NON_EXIST, 1=EXIST_STOP, 2=EXIST_CONTINUE
                                                     // 平手取最小下标
    exist = (decision != 0)
    if none(exist): break

    dec_coords = tmp_svh.grids[d].active_grid_coords()[exist]   // 行序 = tmp grid 索引序
    dec_svh.build_from_grid_coords(d, dec_coords)
    dec_main = ZeroPad(dec_main, tmp_svh@d → dec_svh@d)          // 重排+裁剪到剪枝网格
    up_mask  = ZeroPad(decision == 2, tmp_svh@d → dec_svh@d)     // bool 向量

    if d < adaptive_depth (=2):                                  // 即 d ∈ {0,1}
        res.normal_features[d] = Normal-(it+1)(dec_main, dec_svh@d)  // [n_dec, 3]
    res.basis_features[d] = Basis-(it+1)(dec_main, dec_svh@d)        // [n_dec, 4]

    if none(up_mask): break
```

循环后 `populate_empty`：未写入的 depth 填 `[0, dim]` 空张量（structure 3 / normal 3 / basis 4 / udf 16）。返回 `(res, dec_svh, tmp_svh)`。

关键事实：

- `struct`/`udf` 特征在 **tmp grid（剪枝前）** 上；`normal`/`basis` 特征在 **dec grid（剪枝后）** 上。
- 第一轮 (it=0, d=3) 的 heads 在稠密 neck 网格上评估；`Normal-1`、`Normal-2` 的权重存在于 checkpoint，但因 `d < 2` 条件在推理中**从不评估**（仍需能被加载或显式忽略）。
- upsample 的 mask 用 `decision==2`（CONTINUE），剪枝用 `decision!=0`。
- 训练路径 `gt_decoder_svh` 分支不移植。

## 6. MultiscalePointDecoder（udf_decoder，mask field 用）

构造（第 1 节参数）：`c_dim = 16*4 = 64`；`fc_p: Linear(6, 32)`；`fc_c: 2 × Linear(64, 32)`；`blocks: 2 × ResnetBlockFC(32)`（无 shortcut）；`fc_out: Linear(32, 1)`。查询点 `xyz [M,3]`（世界坐标），svh = `tmp_svh`（udf_svh），特征 = `res.udf_features`：

```text
// 位置编码：coords_depths = [2, 3]（升序拼接）
for did in [2, 3]:
    vs = 0.1 * 2^did
    p_did = (xyz mod vs) / vs - 0.5        // Python mod 语义，∈ [-0.5, 0.5)；注意用世界坐标直接取模
p = concat[p_2, p_3]                       // [M,6]

// 多尺度特征：全部 4 层
for did in 0..3:
    c_did = (tmp_svh.grids[did] == null) ? zeros(M,16)
            : grids[did].sample_trilinear(xyz, udf_features[did])   // 零填充式三线性采样（任务 C）
c = concat[c_0, c_1, c_2, c_3]             // [M,64]

net = fc_p(p)
for i in 0..1:
    net = net + fc_c[i](c)
    net = blocks[i](net)
out = fc_out(relu(net))                    // [M,1]，UDF 值（世界单位距离）
```

`NeuralField(svh=tmp_svh, decoder=udf_decoder, features=udf_features)`，`set_level_set(2 * voxel_size = 0.2)`。该 field 作为 mask field 供 mesh 裁剪（任务 D/E）；若其需要梯度，采用数值差分：`interval = 0.01 * voxel_size = 0.001`，中心差分 `(f(x+δe_i) − f(x−δe_i)) / (2δ)`。

`geometry='neural'` 的 `sdf_decoder` 分支不移植。

## 7. MLPFeatureInterpolator（kernel theta 插值器，每 depth 一个）

`interpolators[str(d)].mlp = MLPWithGrad(n_inputs=4, n_outputs=4, n_layers=2, n_units=16, nonlinear=ReLU, n_fourier=0)`。`layers` 为 `nn.Sequential`，索引即权重键：

```text
layers.0: Linear(4, 16)     → ReLU (layers.1，无参)
layers.2: Linear(16, 16)    → ReLU (layers.3)
layers.4: Linear(16, 16)    → ReLU (layers.5)
layers.6: Linear(16, 4)     // 输出层，无激活
```

`n_fourier=0` → 无 `tpB` 参数、无 Fourier 分支（不移植）。

调用：`interpolate(queries, grid, features[d], grad)`：

```text
(v, J_v) = grid.sample_trilinear(queries, features[d], return_grad=grad)
           // v: [M,4] 三线性采样值；J_v: [M,4,3] 采样值对世界坐标的梯度（任务 C）
(theta, J) = mlp(v, J_v)   // 前向 + 链式法则同步传播雅可比
```

MLPWithGrad 的雅可比传播（`J` 形状 `[M, C, 3]`，按 Sequential 逐层）：

```text
线性层（偶数索引）: x ← x @ W^T + b;   J ← W @ J        // W: [out,in]，J: [M,out,3]
ReLU（奇数索引）  : x ← relu(x);       J ← 1[x != 0] ⊙ J // 用激活后的 x：x==0 处梯度取 0，逐元素广播到 3 列
```

输出 `(theta [M,4], J [M,4,3])`。`approx_kernel_grad=False`（reconstruct 默认）→ 走完整梯度；True 分支不移植。

## 8. 推理调用序列（reconstruct → 网络 → field）

`reconstruct(xyz, normal, detail_level=D)` 其余参数取默认：`sensor=None, voxel_size=None, chunk_size=-1, approx_kernel_grad=False, solver_max_iter=2000, solver_tol=1e-5, nystrom_min_depth=100, fused_mode=True, preprocess_fn=None`。chunk / sensor / preprocess 分支不移植。

```text
// 1. detail_level → 全局缩放（density_range=[1,20] 非空才执行）
vox_ijk    = unique( floor(xyz / 0.1) 逐分量取整为 int64 )   // torch.div(…, rounding_mode='floor')
cur_density    = N / |vox_ijk|
target_density = max(1.0 + 19.0 * (1.0 - D), 0.01)
global_scale   = sqrt(target_density / cur_density)
if global_scale != 1.0: xyz = xyz / global_scale             // normal 不缩放（保持单位向量）

// 2. 结构构建 + 网络前向
svh = SparseFeatureHierarchy(voxel_size=0.1, depth=4)
svh.build_point_splatting(xyz)                               // 任务 C：4 层各自 nearest-voxels splatting
feat0 = encoder(xyz, normal, svh, 0)                         // [n_vox_d0, 32]
(res, dec_svh, tmp_svh) = unet(feat0, svh, adaptive_depth=2)

// 3. KernelField（任务 D 消费）
field = KernelField(svh=dec_svh, interpolator=interpolators,
                    features=res.basis_features,             // {d: [n_dec_d, 4]}
                    approx_kernel_grad=false, solver_max_iter=2000, solver_tol=1e-5)
// balanced_kernel=False → grid_kernel[d] = res.basis_features[d]（原样，即体素上的 theta 值不再过 MLP）
// 查询点处 theta：evaluate_kernel(x, d) = MLP_d( trilinear(grid_d, basis_features[d], x) )（第 7 节）

normal_xyz   = concat[dec_svh 体素中心 @ depth 0, @ depth 1]  // 顺序：depth 0 在前
normal_value = concat[res.normal_features[0], res.normal_features[1]]  // 同序
field.solve(pos_xyz      = xyz,
            normal_xyz   = normal_xyz,
            normal_value = -normal_value,                    // 注意取负！
            pos_weight   = 1e4 / N,
            normal_weight= 1e4 / M * 0.1^2,                  // M = normal_xyz 行数；×voxel_size²
            reg_weight   = 1.0,
            nystrom_min_depth = 100)                         // > tree_depth → Nystrom 低秩近似不触发

// 4. mask field（第 6 节）
mask_field = NeuralField(tmp_svh, udf_decoder, res.udf_features); mask_field.set_level_set(0.2)
field.set_mask_field(mask_field)
field.clear_svh_kernel_maps()          // 释放卷积 kmap 缓存
field.set_scale(global_scale)          // 提取网格时顶点坐标 × global_scale 还原（任务 E）
```

## 9. state_dict 权重键清单

checkpoint `ks.pth`（HF URL）本身即 `{'state_dict': {…}}`，键与下表完全一致（无 `network.` 前缀；本地 lightning `.ckpt` 才需剥离 `network.` 前缀）。无 EMA、无 BN running stats、无任何 buffer；全部为 `float32` Parameter；`load_state_dict` 为 strict。**任何权重都不需要 transpose/reshape**（Linear 按 `[out,in]` 使用，conv kernel 按 `[27,Cin,Cout]` / `[Cin,Cout]` 使用）。

### 9.1 encoder / interpolators / udf_decoder

| 键 | 形状 |
| --- | --- |
| `encoder.fc_pos.weight` / `.bias` | `[64,6]` / `[64]` |
| `encoder.blocks.{0,1,2}.fc_0.weight` / `.bias` | `[32,64]` / `[32]` |
| `encoder.blocks.{0,1,2}.fc_1.weight` / `.bias` | `[32,32]` / `[32]` |
| `encoder.blocks.{0,1,2}.shortcut.weight` | `[32,64]`（无 bias） |
| `encoder.fc_c.weight` / `.bias` | `[32,32]` / `[32]` |
| `interpolators.{0..3}.mlp.layers.0.weight` / `.bias` | `[16,4]` / `[16]` |
| `interpolators.{0..3}.mlp.layers.2.weight` / `.bias` | `[16,16]` / `[16]` |
| `interpolators.{0..3}.mlp.layers.4.weight` / `.bias` | `[16,16]` / `[16]` |
| `interpolators.{0..3}.mlp.layers.6.weight` / `.bias` | `[4,16]` / `[4]` |
| `udf_decoder.fc_c.{0,1}.weight` / `.bias` | `[32,64]` / `[32]` |
| `udf_decoder.fc_p.weight` / `.bias` | `[32,6]` / `[32]` |
| `udf_decoder.blocks.{0,1}.fc_0.weight` / `.bias` | `[32,32]` / `[32]` |
| `udf_decoder.blocks.{0,1}.fc_1.weight` / `.bias` | `[32,32]` / `[32]` |
| `udf_decoder.fc_out.weight` / `.bias` | `[1,32]` / `[1]` |

（`udf_decoder.blocks` 无 shortcut：size_in==size_out=32。）

### 9.2 unet.encoders / unet.decoders

每个 `SingleConv{1,2}` 含 `GroupNorm.weight`、`GroupNorm.bias`（形状 `[C_gn]`）与 `Conv.kernel`（形状 `[27,Cin,Cout]`，无 bias）。

| 前缀 | SingleConv1 GN / Conv | SingleConv2 GN / Conv |
| --- | --- | --- |
| `unet.encoders.Enc0.` | `[32]` / `[27,32,32]` | `[32]` / `[27,32,32]` |
| `unet.encoders.Enc1.` | `[32]` / `[27,32,32]` | `[32]` / `[27,32,64]` |
| `unet.encoders.Enc2.` | `[64]` / `[27,64,64]` | `[64]` / `[27,64,128]` |
| `unet.encoders.Enc3.` | `[128]` / `[27,128,128]` | `[128]` / `[27,128,256]` |
| `unet.decoders.Dec-2.` | `[384]` / `[27,384,128]` | `[128]` / `[27,128,128]` |
| `unet.decoders.Dec-3.` | `[192]` / `[27,192,64]` | `[64]` / `[27,64,64]` |
| `unet.decoders.Dec-4.` | `[96]` / `[27,96,32]` | `[32]` / `[27,32,32]` |

示例完整键：`unet.encoders.Enc0.SingleConv1.GroupNorm.weight`、`unet.encoders.Enc0.SingleConv1.Conv.kernel`。

### 9.3 unet 四组 heads（j ∈ {1..4}，C = {256,128,64,32}，对应 depth = 4−j）

非 enhanced heads（`Struct-j` out=3、`UDF-j` out=16、`Normal-j` out=3）：

| 键（`unet.{struct,udf,normal}_heads.{Struct,UDF,Normal}-j.` 前缀） | 形状 |
| --- | --- |
| `SingleConv.GroupNorm.weight` / `.bias` | `[C]` |
| `SingleConv.Conv.kernel` | `[27,C,C]` |
| `OutConv.kernel` | `[C,out]`（二维，K=1） |
| `OutConv.bias` | `[out]` |

enhanced heads（`Basis-j`，out=4，`mid = min(64,C)` = {64,64,64,32}）：

| 键（`unet.basis_heads.Basis-j.` 前缀） | 形状 |
| --- | --- |
| `SingleConv.GroupNorm.weight` / `.bias` | `[C]` |
| `SingleConv.Conv.kernel` | `[27,C,C]` |
| `SingleConv2.GroupNorm.weight` / `.bias` | `[C]` |
| `SingleConv2.Conv.kernel` | `[27,C,mid]` |
| `OneConv0.GroupNorm.weight` / `.bias` | `[mid]` |
| `OneConv0.Conv.kernel` | `[mid,mid]`（二维，K=1，无 bias） |
| `OutConv.kernel` / `OutConv.bias` | `[mid,4]` / `[4]` |

## 10. 数值细节与边界情况（必须一字不差遵守）

- `torch.round` 是**银行家舍入**（half-to-even）：PointEncoder 的 `round(g).int()` 在 g 恰为 .5 时与 C++ `std::round` 不同。用 `std::nearbyint`（FE_TONEAREST）或显式 half-to-even 实现。
- Python/torch 的 `%` 对正除数恒返回非负：`x mod m = x − floor(x/m)*m`。C++ `fmod` 语义不同，必须替换（PointEncoder local 坐标、MultiscalePointDecoder 位置编码）。
- 密度体素化用 `floor(xyz / 0.1)`（floor 除法，负坐标向下取整），再按三元组去重计数。
- `argmax` 平手取最小下标。
- GroupNorm 统计是**全稀疏张量全局**的：不得分块/分 tile 计算，否则结果偏移。方差用有偏（除 n）。
- decoder 拼接通道顺序固定为 `[encoder skip, upsampled decoder]`。
- 卷积 offset 加在输入坐标上（`in[c_out + δ]`），枚举序 x 外 / z 内、中心 kIdx=13。
- MaxPool 空粗体素 −inf → 0；PointEncoder `scatter_mean` 空体素 → 0。
- 求解器输入 `normal_value` 取负号；normal 约束点为 dec_svh depth 0、1 的体素中心（先 0 后 1 拼接，权重公式见第 8 节）。
- `Normal-1`、`Normal-2` 权重存在但推理不评估。
- `populate_empty` 生成 `[0, dim]` 空张量：下游按"行数 0"处理，不得当作 null。
- neck 稠密网格 = depth 3 活跃坐标包围盒（闭区间），点云跨度大时体素数 = 包围盒体积，注意内存。
- 所有跨 grid 特征搬运（ZeroPadding、剪枝重排）都以目标 grid 的 `active_grid_coords()` 顺序为行序，经 `ijk_to_index` 查源行。

## 11. 不移植清单（本规格范围内）

| 项 | 位置 | 原因 |
| --- | --- | --- |
| `Conv3d(transposed=True)` / stride≠1 | `nn/modules.py` | ks 用 upsample='nearest'、pooling='max' |
| `Activation(LeakyReLU)`、`AdaptiveGroupNorm` | `nn/modules.py` | order='gcr' 不含 'l'；AdaGN 未被引用 |
| `Upsampling(mode='trilinear')` | `nn/modules.py` | ks 用 nearest |
| `MLPWithGrad` Fourier 分支（`n_fourier>0`、`tpB`） | `interpolator.py` | ks n_fourier=0 |
| `sdf_decoder`（geometry='neural'）| `__init__.py` | ks geometry='kernel'（udf_decoder 仍要移植） |
| `gt_decoder_svh` 结构监督分支 | `nn/unet.py` | 仅训练 |
| chunk 重建、sensor 特征、法线估计 preprocess | `__init__.py` | 任务范围外 |
| `balanced_kernel=True`、`approx_kernel_grad=True` | `fields/kernel_field.py` | 默认关闭 |
| snet / snet-wonormal 配置 | `configs.py` | 只移植 ks |

## 12. 未知项（需对照 checkpoint inventory 确认）

- `ks.pth` 中键集合与第 9 节清单逐一比对：确认无多余键（如 optimizer state、EMA 副本）、无缺失键、各形状一致。
- `Normal-1` / `Normal-2` 权重确实存在（结构上应存在，需实测确认）。
- checkpoint dtype 是否全部 float32（若为 float64/half 需转换）。
- `world_to_grid` / `grid_to_world` 的精确公式（本文按"体素中心为整数格点、origin=0.5·vs_d"推断）以任务 C 规格与 `_CpuIndexGrid` 实现为准。
- `sample_trilinear(return_grad=True)` 的梯度是否对世界坐标（含 1/vs 因子）——以任务 C 规格为准，第 7 节的链式法则在其之上不变。
