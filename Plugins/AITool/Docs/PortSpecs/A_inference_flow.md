# NKSR 推理路径总规格 (A: Python 控制流)

本文档是 NKSR (Neural Kernel Surface Reconstruction) 推理路径移植到纯 C++ (CPU) 的总控制流规格。移植工程师只依据本系列规格写代码，不读原始 Python 源码。本文覆盖从 `nksr.Reconstructor.reconstruct()` 到 `field.extract_dual_mesh(mise_iter=1)` 的全部 Python 侧逻辑；`_C.*` 原生算子与 `SparseIndexGrid` 内部数学在配套规格（见 [§10](#10-_c-算子清单对照索引)）中单独定稿，本文给出签名、shape 与一句话语义作为对照索引。

源码基准：`D:/MyProject/AITest/ConvertToSurface/ConvertToSurface/NKSR/package/nksr/`（版本 `1.0.3`）。
调用方入口：`D:/MyWork/UnrealProject/UETest574_2/Plugins/AITool/Scripts/NKSR/reconstruct.py`。

## 0. 移植范围

覆盖且仅覆盖以下调用序列（`config='ks'` 默认权重 `ks.pth`，全部 `float32`）：

```python
reconstructor = nksr.Reconstructor(device)                      # config='ks'
field = reconstructor.reconstruct(xyz, normal, detail_level=D)  # xyz/normal: [N,3] float32
mesh = field.extract_dual_mesh(mise_iter=1)                     # mesh.v [V,3] float32, mesh.f [T,3] int64
```

不移植（标注理由）：

| 分支 | 位置 | 不移植原因 |
| --- | --- | --- |
| 训练 / autograd backward | `solver.PCGSolver.backward`, `meshing.MarchingCubes.backward` | 推理时 `requires_grad=False`，永不触发 |
| sensor / 法线估计路径 | `feature=='sensor'` 分支、`utils.estimate_normals`、`filter_radius_inliers`、`get_estimate_normal_preprocess_fn`、`_C.pcproc.*` | 调用方总是提供 normal（`feature=='normal'`） |
| chunk 重建 | `reconstruct_by_chunk`、`utils.split_into_chunks`、`FusedField`、`SparseFeatureHierarchy.joined`、`get_f_bound` | `chunk_size=-1` 默认不切块，见 [§3](#3-大点云-chunk-处理) |
| 非 fused 求解 | `KernelField.solve_non_fused`、`QGMatrix`、`_C.kernel_eval.qg_building`、`csr_matrix_multiplication`、`_C.sparse_solve.ptr2ind` | `fused_mode=True` 默认 |
| 颜色/纹理场 | `texture_field`、`MeshingResult.c` | ks 路径恒为 `None` |
| `extract_primal_mesh` | `base_field.py` | 只用 dual mesh |
| `LayerField` / `points_in_active_voxel` | `layer_field.py` | ks 配置 `udf.enabled=True`，mask 用 `NeuralField` |
| Nystrom 采样 / `points_voxel_downsample` | `KernelField.solve` 内 `d >= nystrom_min_depth` 分支 | 默认 `nystrom_min_depth=100 > tree_depth`，永不触发 |
| `snet` / `snet-wonormal` 配置、`geometry=='neural'` 主场 | `configs.py`, `NeuralField` 作主场 | 只用 `ks`；但 `NeuralField` 作为 **mask field** 必须移植 |
| `build_iterative_coarsening` / `build_adaptive_normal_variation` | `svh.py` | 只用 `build_point_splatting` |
| `Upsampling(mode='trilinear')`、`Conv3d(transposed=True)`、`pooling='conv'`、`AdaptiveGroupNorm` | `nn/modules.py` | ks UNet 用 `nearest` / 非转置 / `max` |
| kernel map 缓存 (`svh.kernel_maps`) | `nn/modules.py` `cache_kmap` | 纯性能缓存，语义等价于每次重算；C++ 可选实现 |

## 1. 顶层调用图

### 1.1 `ks` 超参（写死为常量）

```yaml
feature: 'normal'          # 输入特征 = 法线
geometry: 'kernel'         # 主场 = KernelField
voxel_size: 0.1            # 最细层 voxel 尺寸（网络内部尺度）
kernel_dim: 4              # basis 特征 / theta 维度
tree_depth: 4              # 层级数 d = 0..3
adaptive_depth: 2          # 法线监督层数（d=0,1）；也是 LayerField 深度（未用）
unet.f_maps: 32            # UNet 基础通道数
udf.enabled: true          # mask field = UDF NeuralField
interpolator.n_hidden: 2   # kernel MLP 隐层重复数
interpolator.hidden_dim: 16
solver.pos_weight: 1.0e4
solver.normal_weight: 1.0e4
density_range: [1.0, 20.0] # detail_level 换算用
```

派生常量：`NKSRNetwork` 包含 `encoder`(PointEncoder, dim=6)、`interpolators`（4 个 `MLPFeatureInterpolator`，key `"0".."3"`）、`unet`(SparseStructureNet, in=32, blocks=4, basis=4, normal=3, f_maps=32, udf_branch=16)、`udf_decoder`(MultiscalePointDecoder, c_each_dim=16, depths=4, out_init=0.5, coords_depths=[2,3])。权重来自 `ks.pth['state_dict']`，key 前缀即上述子模块名。

### 1.2 `reconstruct(xyz, normal, detail_level=D)` 步骤总表

所有张量 `float32`（坐标/特征）、`int32`（ijk 坐标、grid 索引）、`int64`（拓扑索引），行主序 (row-major)。

| 步 | 调用 | 输入 → 输出 | 说明 |
| --- | --- | --- | --- |
| 1 | `preprocess_fn = default_preprocess` | `(xyz, normal, None)` 原样返回 | 恒等；`xyz` 为空 → 返回 `None` |
| 2 | 尺度换算（[§2](#2-detail_level--voxel_size-换算)） | `xyz [N,3]` → `global_scale: float` | `torch.unique(dim=0)` + `floor div` |
| 3 | `xyz = xyz / global_scale` | `[N,3]` | 仅当 `global_scale != 1.0`；normal 不变 |
| 4 | `feat = normal` | `[N,3]` | `feature=='normal'` |
| 5 | `svh = SparseFeatureHierarchy(0.1, 4)`; `svh.build_point_splatting(xyz)` | 4 层 `SparseIndexGrid` | 每层 `build_from_pointcloud_nearest_voxels`，见 [§4](#4-svh-与坐标约定) |
| 6 | `feat = network.encoder(xyz, feat, svh, 0)` | `[N,3]+[N,3]` → `c [Nv0, 32]` | PointNet 编码到 depth-0 voxel，见 [§5.1](#51-pointencoder) |
| 7 | `feat, svh, udf_svh = network.unet(feat, svh, adaptive_depth=2)` | → `FeaturesSet`, `decoder_svh`, `decoder_tmp_svh` | 稀疏 UNet，见 [§5.2](#52-sparsestructurenet-unet)。**返回的 `svh` 是新建的 `decoder_svh`（预测结构），不再是步 5 的 splatting svh** |
| 8 | `KernelField(svh, interpolators, feat.basis_features, approx_kernel_grad=False, solver_max_iter=2000, solver_tol=1e-5)` | → `output_field` | `grid_kernel[d] = basis_features[d]`（`balanced_kernel=False`） |
| 9 | 组装求解输入 | `normal_xyz [M,3]`, `normal_value [M,3]` | `normal_xyz = cat(get_voxel_centers(0), get_voxel_centers(1))`（d 升序）；`normal_value = cat(normal_features[0], normal_features[1])` |
| 10 | `output_field.solve(pos_xyz=xyz, normal_xyz, normal_value=-normal_value, pos_weight=1e4/N, normal_weight=1e4/M*0.01, reg_weight=1.0, nystrom_min_depth=100)` | → `output_field.solutions {d: [Nv_d]}` | 线性系统组装 + PCG，见 [§6](#6-kernelfield-求解-fused-路径) |
| 11 | `mask_field = NeuralField(udf_svh, network.udf_decoder, feat.udf_features)`; `mask_field.set_level_set(0.2)` | | `2 * voxel_size = 0.2`，见 [§7.3](#73-mask-field-neuralfield--udf-decoder) |
| 12 | `output_field.set_mask_field(mask_field)`; `clear_svh_kernel_maps()`; `set_scale(global_scale)` | → 返回 `output_field` | `set_scale` 同时传播给 mask_field（只设 `scale` 成员，`level_set` 不缩放） |

### 1.3 `extract_dual_mesh(mise_iter=1)` 步骤总表

默认参数：`grid_upsample=1`, `max_depth=100`, `trim=True`, `max_points=-1`。

| 步 | 调用 | 输入 → 输出 |
| --- | --- | --- |
| 1 | `_C.meshing.build_flattened_grid(grids[d], grids[d-1] or None, d != 3)` 对 d=0..3 | → 4 个 flattened grid |
| 2 | `_C.meshing.build_joint_dual_grid(flattened_grids)` | → `dual_grid` |
| 3 | `_C.meshing.dual_cube_graph(flattened_grids, dual_grid)` | → `dmc_graph [C,8] int64` |
| 4 | `dmc_vertices = cat_d(f_grid.grid_to_world(f_grid.active_grid_coords().float()))` | → `[P,3] float32`，**d 升序拼接**，跳过 `num_voxels()==0` 的层；`dmc_graph` 的索引指向该拼接顺序 |
| 5 | `dmc_vertices *= scale` | 回到输入坐标系 |
| 6 | `dmc_value = evaluate_f_bar(dmc_vertices)` | → `[P] float32`，见 [§7.1](#71-evaluate_f_bar-通用包装) |
| 7 | MISE 细分 ×1（[§8.2](#82-mise-迭代)） | 图/顶点/值全部更新 |
| 8 | `_C.meshing.marching_cubes(dmc_graph, dmc_vertices, dmc_value)` | → `v [V,3] float32`, `f [T,3] int64`（`vidx` 仅训练用） |
| 9 | trim：`vert_mask = mask_field.evaluate_f_bar(v) < 0.0`；`apply_vertex_mask` | 见 [§8.3](#83-trim-与-apply_vertex_mask) |
| 10 | 返回 `MeshingResult(v, f, c=None)` | 顶点已在输入坐标系（步 5 已乘 scale） |

## 2. detail_level → voxel_size 换算

`ks` 的 `hparams.voxel_size = 0.1` 固定不变；`detail_level` 通过缩放**输入点云**实现分辨率控制。调用方默认 `detail_level=1.0`（`reconstruct.py` 的 argparse 默认值；`reconstruct()` 自身签名默认 0.0）。

```text
输入: xyz [N,3], detail_level D (调用方默认 1.0), density_range = [1.0, 20.0]

if voxel_size 参数被显式指定:              # 本路径不走
    global_scale = voxel_size / 0.1
elif D is not None:                        # 默认路径
    vox_ijk     = unique_rows( floor(xyz / 0.1) )       # 逐分量 floor，long；unique 按行去重
    cur_density = N / vox_ijk.rows
    target      = 1.0 + (20.0 - 1.0) * (1.0 - D)        # D=1.0 → 1.0; D=0.0 → 20.0
    target      = max(target, 0.01)
    global_scale = sqrt(target / cur_density)
else:
    global_scale = 1.0

if global_scale != 1.0: xyz /= global_scale             # 网络工作坐标 = 输入 / global_scale
```

- 直觉：把点云缩放到"平均每个 0.1-voxel 含 target 个点"的密度，等效输入空间 voxel 尺寸 = `0.1 * global_scale`。
- 层级关系：depth `d` 的 voxel 尺寸 `vs_d = 0.1 * 2^d`（工作坐标系），d = 0..3 → 0.1 / 0.2 / 0.4 / 0.8。
- `floor` 必须是数学 floor（负坐标向下取整），等价 `torch.div(rounding_mode='floor')`。
- 输出网格顶点在 meshing 阶段乘回 `scale`，最终 mesh 在原输入坐标系。

## 3. 大点云 chunk 处理

- `reconstruct(chunk_size=-1)` 为默认且调用方不传 → **chunk 路径不是推理必经，不移植**（`split_into_chunks` / `reconstruct_by_chunk` / `FusedField` 全部跳过）。
- 场评估侧的 `evaluate_f_bar(xyz, max_points=-1)`：`max_points=-1`（`extract_dual_mesh` 默认透传）→ 单块评估。`max_points>0` 时按 `ceil(P/max_points)` 用 `torch.chunk` 均分逐块评估再拼接——是纯逐点 map，C++ 端可自由分块以控内存，结果不变。

## 4. SVH 与坐标约定

`SparseFeatureHierarchy`（下称 SVH）= 4 层 `SparseIndexGrid`，"voxel 角点对齐世界原点"：

```text
depth d:  voxel_size vs_d = 0.1 * 2^d,   origin_d = 0.5 * vs_d   (标量, 三轴同值)
grid_to_world(ijk) = ijk * vs_d + origin_d = (ijk + 0.5) * vs_d   # voxel (i,j,k) 的中心
world_to_grid(x)   = x / vs_d - 0.5
→ voxel (i,j,k) 覆盖世界区间 [i*vs_d, (i+1)*vs_d) × ...；round(world_to_grid(x)) = 包含 x 的 voxel 的 ijk
```

- `active_grid_coords()` 返回 `[Nv,3] int32` 活跃 voxel ijk；`ijk_to_index(ijk)` 返回 `[M] int` 活跃索引，不存在 = `-1`。**同一 grid 的 voxel 顺序（index 0..Nv-1）由 grid 内部决定，全流程一致引用；精确顺序约定见 IndexGrid 配套规格。**
- `build_point_splatting(pts)`：对每个 d 独立调用 `build_from_pointcloud_nearest_voxels(pts)` —— 激活每个点周围 2×2×2 = 8 个最近 voxel（以 voxel 中心为最近判据）；精确语义见配套规格。
- `get_voxel_centers(d) = grid_to_world(active_grid_coords().float())`；grid 为 `None` 时返回 `[0,3]`。
- `evaluate_voxel_status` / `get_test_grid` / `get_f_bound` / `permute_features` / `tensor_dict`：不在本路径（`evaluate_voxel_status` 仅训练 gt 分支）。

## 5. 网络前向

权重层级细节（每层 weight shape、GroupNorm 参数）由权重导出规格约束；本节固定**控制流、层顺序与 shape**。所有 Linear 为 `y = x @ W^T + b`。

### 5.1 PointEncoder

配置：`dim=6, c_dim=32, hidden_dim=32, n_blocks=3`。作用于 depth-0 grid（splatting svh）。

```text
输入: pts_xyz [N,3](工作坐标), pts_feature = normal [N,3], grid = svh.grids[0]

g   = world_to_grid(pts_xyz)                    # [N,3] float
vid = ijk_to_index(round(g).int())              # [N]，点所在 voxel 的活跃索引
loc = (g + 0.5) mod 1                           # [N,3] ∈ [0,1)，voxel 内相对下角点的局部坐标 (torch 余数语义, 结果非负)
mask = vid != -1                                # splatting 保证包含点的 voxel 必活跃，正常全 true
vid, loc, nrm = vid[mask], loc[mask], normal[mask]

h = fc_pos( cat[loc, nrm] )                     # Linear(6→64)
h = ResnetBlockFC_0(h)                          # 64→32
for blk in [blocks 1, 2]:                       # 共 2 次
    pooled = scatter_max(h, vid, dim_size=Nv0)  # 逐 voxel 逐通道取 max ([Nv0,32])
    h = blk( cat[h, pooled[vid]] )              # ResnetBlockFC 64→32
c = fc_c(h)                                     # Linear(32→32)
c = scatter_mean(c, vid, dim_size=Nv0)          # [Nv0, 32]; 无点的 voxel 输出全 0
```

`ResnetBlockFC(size_in, size_out)`（`size_h = min(size_in, size_out)`）：

```text
net = fc_0( ReLU(x) )            # Linear(size_in → size_h)
dx  = fc_1( ReLU(net) )          # Linear(size_h → size_out)；权重初始为 0（推理用加载值）
xs  = shortcut(x) if size_in != size_out else x   # Linear(size_in → size_out, 无 bias)
return xs + dx
```

- `scatter_max`：逐通道对相同 `vid` 的行取最大；空槽值随后不被读取（只 gather `pooled[vid]`），实现可任意填充。
- `scatter_mean`：sum / count，count=0 的 voxel 输出 0。

### 5.2 SparseStructureNet (UNet)

配置：`in=32, num_blocks=4, basis=4, normal=3, f_maps=32, order='gcr', num_groups=8, pooling='max', upsample='nearest', neck_type='dense', neck_expand=1, udf_branch=16`。通道表 `n_features = [32, 32, 64, 128, 256]`。

基础块：

- `SparseConvBlock(in, out, 'gcr', k)` = `GroupNorm(8, in)` → `Conv3d(in, out, kernel=k, stride=1, bias=False)` → `ReLU`。GroupNorm 为 torch 标准（affine, eps=1e-5），把 `[Nv, C]` 视作 `[1, C, Nv]`，每组统计跨（组内通道 × 全部 voxel）。
- `Conv3d` k=3：稀疏卷积，同层等 stride 时权重 `[27, Cin, Cout]`；kernel map 由 `_C.conv.convolution_kernel_map(in_grid, out_grid, 3)` 生成（27 个 offset 槽位的枚举顺序是移植关键，见 conv 配套规格），随后 `_C.conv.sparse_convolution(feat, kernel, nbmap, nbsizes, shape, transposed=False)`。Python 侧 kmap 后处理（一字不差移植）：

```python
kmap = convolution_kernel_map(in_grid, out_grid, ks).T   # [N_query, 27]，元素 = 源 voxel 索引或 -1
nbsizes = (kmap != -1).sum(dim=1)                        # [N_query] int32
nbmap   = nonzero(kmap != -1)                            # [nnz,2] = (行, offset槽) 行主序扫描
nbmap[:,0] = kmap.flatten()[ nbmap[:,0]*27 + nbmap[:,1] ] # 第0列替换为源 voxel 索引 → {in, offset}
shape = (in_grid.num_voxels, out_grid.num_voxels); nbmap/nbsizes 转 int32
```

- `Conv3d` k=1 且 stride=1：退化为 `out = feat @ kernel (+bias)`，权重 shape `[Cin, Cout]`。
- `MaxPooling(2)`：目标 grid 已存在（encoder 阶段 splatting svh 各层均已建）→ `in_grid.max_pool(feat, 2, coarse_grid)` 逐通道对 8 个子 voxel 取 max，随后 `feat[isinf(feat)] = 0`（粗 voxel 无活跃子 voxel 时 max_pool 产出 -inf，置 0）。
- `Upsampling(2, 'nearest')`：目标 grid 不存在 → `out_grid = in_grid.subdivided_grid(2, mask)`（只细分 `mask=true` 的 voxel，每个产生 8 个子 voxel）并注册到 out_svh；随后 `in_grid.subdivide(feat, 2, fine_grid)` 把父特征复制给子 voxel。
- `SparseZeroPadding`：把 in_grid 上的特征搬到 out_grid（同 depth）：`out_feat = 0; idx = in_grid.ijk_to_index(out_grid.active_grid_coords()); out_feat[idx != -1] = feat[idx[idx != -1]]`。
- `SparseDoubleConv`（encoder=true）：`[MaxPool(2) 仅 layer>0]` → `SparseConvBlock(conv1_in→conv1_out)` → `SparseConvBlock(→out)`，其中 `conv1_out = max(out//2, in)`。encoder=false（decoder）：两个 ConvBlock `in→out`、`out→out`。
- `SparseHead(in, out)`：`SparseConvBlock(in→in, k3)` → `Conv3d(in→out, k1, bias)`。enhanced（仅 basis head）：`SparseConvBlock(in→in, k3)` → `SparseConvBlock(in→mid, k3)` → `SparseConvBlock(mid→mid, k1)` → `Conv3d(mid→out, k1, bias)`，`mid = min(64, in)`。

前向控制流（`adaptive_depth=2`，`gt_decoder_svh=None`）：

```text
== 编码（splatting svh 上，grid 结构不变）==
feat_depth = 0
Enc0: DoubleConv 32→32 (无 pool)          → encoder_features[0] [Nv0,32]
Enc1: MaxPool→d1, DoubleConv 32→64        → encoder_features[1] [Nv1,64]
Enc2: MaxPool→d2, DoubleConv 64→128       → encoder_features[2] [Nv2,128]
Enc3: MaxPool→d3, DoubleConv 128→256      → encoder_features[3] [Nv3,256]

== 瓶颈 ==
decoder_svh、decoder_tmp_svh = 两个新的空 SVH (voxel_size=0.1, depth=4)
build_neck_grid: 取 encoder grids[3] 活跃坐标的 AABB [min, max]，
  生成稠密 ijk 盒 (arange(min_x, max_x+1) × ... 三轴 meshgrid, 'ij' 序, int32)
  → decoder_tmp_svh.build_from_grid_coords(3, coords)      # neck_expand=1 → 无 padding
feat = SparseZeroPadding(encoder_features[3], enc_grid3 → tmp_grid3)   # [NvT3, 256]

== 解码循环，迭代 4 次，feat_depth 依次 3,2,1,0 ==
heads 顺序（ModuleList 序 = 深→浅）: struct/udf/normal/basis heads[i] 对应 feat_depth = 3-i
upsample_mask = None
for it in 0..3:
    if it > 0:
        feat = Upsampling(feat, decoder_svh.grids[feat_depth+1] → decoder_tmp_svh 新建 grids[feat_depth],
                          mask=upsample_mask)                     # nearest ×2
        enc  = SparseZeroPadding(encoder_features[feat_depth], enc_grid → tmp_grid)
        feat = cat[enc, feat]  → Dec DoubleConv:                  # it=1: 384→128; it=2: 192→64; it=3: 96→32
    structure_features[fd] = StructHead(feat)      # [NvT_fd, 3] logits (tmp grid 上)
    udf_features[fd]       = UDFHead(feat)         # [NvT_fd, 16] (tmp grid 上)
    decision = argmax(structure_features[fd], dim=1).byte()   # 0=不存在,1=存在停止,2=存在继续
    exist = decision != 0
    if !any(exist): break
    decoder_svh.build_from_grid_coords(fd, tmp_grid.active_grid_coords()[exist])
    feat          = SparseZeroPadding(feat, tmp_grid → dec_grid)          # 限制到存在 voxel
    upsample_mask = SparseZeroPadding(decision == 2, tmp_grid → dec_grid) # bool，pad 填 false
    if fd < 2:  normal_features[fd] = NormalHead(feat)   # [Nv_fd, 3]，仅 d=1,0 (dec grid 上)
    basis_features[fd] = BasisHead(feat)                 # [Nv_fd, 4] enhanced (dec grid 上)
    if !any(upsample_mask): break

populate_empty: 缺失的 dict 项补 [0, C] 零张量 (structure C=3, normal C=3, basis C=4, udf C=16)
返回 (FeaturesSet, decoder_svh, decoder_tmp_svh)
```

关键约定：

- `basis_features[d]`、`normal_features[d]` 定义在 **decoder_svh**（预测结构）grid 上；`udf_features[d]` 定义在 **decoder_tmp_svh**（剪枝前超集）grid 上。行序 = 各自 grid 的 voxel index 序。
- `argmax` 平局取最小下标（torch 语义）。
- 早停（break）会使浅层 grid 为 `None`、对应特征为 `[0,C]`；下游全部按 `grid is None → 跳过` 处理。

## 6. KernelField 求解 (fused 路径)

### 6.1 构造

```text
KernelField(svh=decoder_svh, interpolator, features=basis_features,
            approx_kernel_grad=False, balanced_kernel=False,
            solver_max_iter=2000, solver_tol=1e-5)
grid_kernel[d] = features[d]          # [Nv_d, 4]，逐 voxel 的 theta（不经 MLP）
solutions = {}                        # 求解后 {d: alpha_d [Nv_d] float32}
level_set = 0.0
```

### 6.2 evaluate_kernel（查询点 theta）

```text
evaluate_kernel(xyz [Q,3], d, grad):
    grid = svh.grids[d];  None → 返回 None (grad 时 (None,None))
    if grad (且 approx_kernel_grad=False):
        s, ds = grid.sample_trilinear(xyz, features[d], return_grad=True)  # s [Q,4], ds [Q,4,3] (d s_c / d xyz)
        return MLP_d.forward_with_grad(s, ds)      # → theta [Q,4], dtheta [Q,4,3]
    else:
        s = grid.sample_trilinear(xyz, features[d])
        return MLP_d(s)                            # theta [Q,4]
```

`MLP_d` = `MLPWithGrad(n_inputs=4, n_outputs=4, n_layers=2, n_units=16, ReLU, 无 fourier)`，层序列（索引 0..6）：

```text
L0 Linear(4→16), L1 ReLU, L2 Linear(16→16), L3 ReLU, L4 Linear(16→16), L5 ReLU, L6 Linear(16→4)
带梯度前向（链式法则，与值同步逐层传播；J 初始 = ds [Q,4,3]）:
  偶数层 (Linear):  x = L(x);  J = W @ J            # W [out,in]，J [Q,out,3]
  奇数层 (ReLU):    x = relu(x); J = 1[x != 0] ⊙ J  # 用激活后的 x 判断 (x==0 → 0, 否则 1)
```

`sample_trilinear` 的插值/梯度公式（含边界零填充）见 IndexGrid 配套规格。

### 6.3 solve() 组装与 PCG

输入（[§1.2](#12-reconstructxyz-normal-detail_leveld-步骤总表) 步 9-10）：`pos_xyz = xyz [N,3]`；`normal_xyz [M,3]` = d=0 与 d=1 的 voxel 中心（升序拼接）；`normal_value [M,3] = -cat(normal_features[0], normal_features[1])`（**注意负号在调用处**）；`pos_weight = 1e4/N`；`normal_weight = 1e4/M * 0.1²`；`reg_weight = 1.0`。

```text
pos_kernel[d]    = evaluate_kernel(pos_xyz, d)          # [N,4] 或 None
normal_kernel[d] = evaluate_kernel(normal_xyz, d, grad) # ([M,4], [M,4,3]) 或 (None,None)

lhs_mat = SparseMatrix(4);  rhs = {}
for d = 3 down to 0:                    # 跳过 grids[d] is None
    rhs[d] = normal_weight * _C.kernel_eval.rhs_evaluation(
                 grids[d], normal_xyz, normal_kernel[d].theta,
                 grid_kernel[d], normal_kernel[d].dtheta, normal_value)[0]   # [Nv_d]
    # nystrom: d >= 100 恒 false → pos_xyz_d = pos_xyz, pos_weight_d = pos_weight
    for dd = 3 down to d:               # 跳过 None；块 (d, dd)，d <= dd（上三角）
        idxer = _C.kernel_eval.build_coo_indexer(grids[d], grids[dd])   # [Nv_d, K_nb]，值 = grid_dd 的 voxel 索引或 -1
        (rows, slots) = where(idxer != -1)      # 行主序扫描 → COO 条目顺序：按 grid_d 行升序、行内按槽位升序
        dd_inds = idxer[rows, slots]            # 每个条目的列（grid_dd voxel 索引），在覆写前提取
        idxer 转 int64 (CPU 恒 int64)；idxer[rows, slots] = arange(nnz)  # 覆写为条目序号
        d_inds = rows.int()
        gtg = _C.kernel_eval.matrix_building(grids[d], grids[dd], pos_xyz,
                  pos_kernel[d], pos_kernel[dd], grid_kernel[d], grid_kernel[dd],
                  empty[0,0,3], empty[0,0,3], idxer, grad=False, nnz)[0]     # [nnz]
        qtq = _C.kernel_eval.matrix_building(grids[d], grids[dd], normal_xyz,
                  normal_kernel[d].theta, normal_kernel[dd].theta,
                  grid_kernel[d], grid_kernel[dd],
                  normal_kernel[d].dtheta, normal_kernel[dd].dtheta, idxer, grad=True, nnz)[0]
        lhs = pos_weight * gtg + normal_weight * qtq
        if d == dd:
            lhs += reg_weight * _C.kernel_eval.k_building(grids[d], grid_kernel[d], idxer, nnz)[0]
        lhs_mat.add_block(d, dd, Nv_d, Nv_dd, a_i=d_inds, a_j=dd_inds, a_x=lhs)
solutions = lhs_mat.solve(rhs, {tol: 1e-5, max_iter: 2000})
```

`SparseMatrix`（块对称、只存上三角 `d <= dd`）布局约定：

```text
add_block(i, j, size_i, size_j, a_i, a_j, a_x):
    if i == j: inv_diag[i] = 1.0 / a_x[a_i == a_j]      # 对角元倒数，按 COO 条目顺序抽取（每行恰一个对角元）
    block_size[i] = size_i; block_size[j] = size_j
    blocks[(i,j)] = { a_p: ind2ptr(a_i, size_i),        # CSR 行指针 [size_i+1]（a_i 已按行升序）
                      a_j: a_j, a_x: a_x }              # 推理不存 a_i

solve(rhs, conf):
    rhs_vec  = cat(rhs[d] for d 升序 if d in rhs)                    # [ΣNv_d]
    block_ptr = 前缀和([block_size[0..3]])                           # 长度 5
    inv_diag_vec = cat(inv_diag[d] for d 升序 if not None)
    (x, iters) = _C.sparse_solve.solve_pcg(
        {(di,dj): a_p}, {(di,dj): a_j}, {(di,dj): a_x}, block_ptr,
        rhs_vec, inv_diag_vec, tol=1e-5, max_iter=2000, false)
    # Jacobi 预条件 CG；上三角块隐式对称展开；iters == max_iter → 仅告警不报错
    return { d: x[block_ptr[d] : block_ptr[d+1]] }                   # solutions[d] = alpha_d
```

## 7. 场评估

### 7.1 evaluate_f_bar（通用包装）

```text
evaluate_f_bar(xyz, max_points=-1):
    按 max_points 分块（默认单块）；每块:
        if scale != 1.0: q = xyz_chunk / scale       # 世界坐标 → 工作坐标
        f = evaluate_f(q, grad=False).value - level_set
    return cat(所有块)
```

### 7.2 主场 KernelField.evaluate_f

```text
evaluate_f(q [Q,3]):
    f = 0
    for d in 0..3:
        if grids[d] is None: continue
        theta_q = evaluate_kernel(q, d)              # [Q,4]（§6.2）
        f += _C.kernel_eval.kernel_evaluation(
                 grids[d], q, theta_q, grid_kernel[d], solutions[d],
                 empty[0,0,3], grad=False)[0]        # [Q]
    return f                                          # level_set = 0
```

语义：`f_d(q) = Σ_{v ∈ N(q)} alpha_d[v] · K_d(q, v)`，支撑域为 q 所在 voxel 的 3×3×3 邻域（27 voxel，与 Q/G 矩阵每行 27 列一致）；核函数 `K_d`（空间基函数 × theta 内积的组合）的精确公式见 kernel_eval 配套规格。meshing 路径恒 `grad=False`，`grad_kernel` 传空张量 `[0,0,3]`。

### 7.3 mask field (NeuralField + udf_decoder)

`NeuralField(svh=decoder_tmp_svh, decoder=udf_decoder, features=udf_features)`，`level_set=0.2`，`grad` 分支不在路径上（trim 只用值）。

`udf_decoder = MultiscalePointDecoder(c_each_dim=16, multiscale_depths=4, p_dim=3, out_dim=1, hidden_size=32, n_blocks=2, aggregation='cat', out_init=0.5, coords_depths=[2,3])`：

```text
forward(q [Q,3], svh=decoder_tmp_svh, feats=udf_features):
    # 局部坐标特征（世界/工作坐标直接取模，torch 余数语义非负）
    p = cat[ (q mod 0.4)/0.4 - 0.5,  (q mod 0.8)/0.8 - 0.5 ]     # [Q,6] ∈ [-0.5,0.5)
    # 多尺度特征
    for d in 0..3:
        c_d = grids[d].sample_trilinear(q, feats[d]) if grids[d] else zeros[Q,16]
    c = cat[c_0..c_3]                                             # [Q,64]
    net = fc_p(p)                                                 # Linear(6→32)
    for i in 0..1:
        net = net + fc_c[i](c)                                    # Linear(64→32)
        net = ResnetBlockFC_32(net)                               # 32→32 (无 shortcut Linear)
    return fc_out( ReLU(net) )                                    # Linear(32→1) → [Q,1]
```

`evaluate_f` 返回 `out[:,0]`。trim 判据（含 scale）：保留 `udf(v / scale) - 0.2 < 0` 的顶点。**注意 `level_set=0.2` 是工作坐标系里的常量，`set_scale` 不缩放它**。

## 8. extract_dual_mesh 详细控制流

### 8.1 网格展平与 dual 图（_C 交接）

- 对 d=0..3：`build_flattened_grid(grids[d], finer=grids[d-1]|None, conforming=(d != 3))` —— 生成"该层未被更细层覆盖的叶子 cell"网格；`conforming` 标志与相邻层过渡有关，精确语义见 meshing 配套规格。`grid_upsample=1` → 不做 `subdivided_grid`。
- `build_joint_dual_grid([f_0..f_3])` → 跨层 dual 网格；`dual_cube_graph(flattened_grids, dual_grid)` → `dmc_graph [C,8] int64`：每行 = 一个 dual cell 的 8 个角点，角点索引指向"各层 flattened grid 的 voxel 中心按 **d 升序**拼接"的全局顶点数组（空层跳过）。
- **8 角点的角标约定**（`subdivide_cube_indices` 依赖，必须与 `dual_cube_graph` 输出一致）：0-1-2-3 构成一个环（底面），4-5-6-7 为对面环，i 与 i+4 相连；三组平行棱分别为 {0-4,1-5,2-6,3-7}、{0-1,3-2,4-5,7-6}、{0-3,1-2,4-7,5-6}；体对角 0↔6, 1↔7, 2↔4, 3↔5。角标到 ijk 位的具体映射由 meshing 配套规格定稿。

### 8.2 MISE 迭代

`mise_iter=1` → 执行一轮：

```text
sign = dmc_value[dmc_graph] > 0                       # [C,8] bool（严格大于）
keep = !(all(sign, dim=1) || all(!sign, dim=1))       # 只留跨零等值面的 cube
dmc_graph = dmc_graph[keep]
(unq, dmc_graph) = unique(dmc_graph.flatten(), return_inverse)   # unq 升序；graph 重映射后 view(-1,8)
dmc_vertices = dmc_vertices[unq]
(dmc_graph, dmc_vertices) = subdivide_cube_indices(dmc_graph, dmc_vertices)
dmc_value = evaluate_f_bar(dmc_vertices)              # 全量重评估（含旧顶点）
```

`subdivide_cube_indices`（纯 Python，1:1 移植）——把每个 cube 分成 8 个子 cube。核心恒等式：**子 cube i 的角 j = 原 cube 角 i 与角 j 的中点**（i==j → 原角点；棱邻 → 棱中点；面对角 → 面心；体对角 → 体心）：

```text
输入: graph [C,8] int64, verts [P,3]
new_graph[i][j] 为 [C] 的索引列, i,j ∈ 0..7；new_verts 从 verts 起追加；base = P

1) 角点: new_graph[i][i] = graph[:, i]

2) 棱中点，按 3 组轴依次处理（顺序固定）:
   axis1 棱 [[0,4],[1,5],[3,7],[2,6]]; axis2 [[0,1],[3,2],[4,5],[7,6]]; axis3 [[0,3],[1,2],[4,7],[5,6]]
   对每组:
     pairs = cat_4条棱( stack[graph[:,e0], graph[:,e1]] )         # [4C,2]，棱内顶点序按上表 (e0,e1)
     (uniq, inv) = unique_rows(pairs)                             # 行字典序升序 + 逆映射
     mid = (verts[uniq[:,0]] + verts[uniq[:,1]]) / 2
     deg = uniq[:,0] != uniq[:,1]                                 # 退化棱（两端同点）不加新点
     追加 mid[deg] 到 new_verts
     new_idx = uniq[:,0].clone(); new_idx[deg] = base + arange(count(deg)); base += count(deg)
     per_entry = new_idx[inv]; 按 4 等分 chunk 回各条棱
     对第 g 条棱 (e0,e1): new_graph[e0][e1] = new_graph[e1][e0] = chunk[g]

3) 面心，按 3 组轴依次处理:
   set1 [[0,1,5,4],[3,2,6,7]]; set2 [[1,2,6,5],[0,3,7,4]]; set3 [[0,1,2,3],[4,5,6,7]]
   对每组:
     quads = cat_2面( stack[graph[:,f0..f3]] )                    # [2C,4]，角序按上表
     (uniq, inv) = unique_rows(quads); center = mean(verts[uniq[:,0..3]])
     deg = !(四个索引全相等)                                       # 注意：deg=true 表示"非退化"
     追加 center[deg]; new_idx = uniq[:,0].clone(); new_idx[deg] = base + arange(...); base += ...
     per_entry = new_idx[inv]; 2 等分 chunk 回两面
     对面 f: for i in 0..3: new_graph[f[i]][f[(i+2)%4]] = chunk[g]  # 面对角对赋值

4) 体心: center = mean(verts[graph[:,0..7]]); 全部追加; idx = base + arange(C)
   diag = [6,7,4,5,2,3,0,1]; for i in 0..7: new_graph[i][diag[i]] = idx

输出: new_graph = 竖向拼接 8 块，第 i 块 = stack(new_graph[i][0..7], dim=1)  # [8C, 8]，子 cube 按角 i 分块排列
      new_verts = cat[verts, 棱点(axis1,2,3 顺序), 面点(set1,2,3 顺序), 体心]
```

依赖的隐含一致性：相邻 cube 共享的棱/面，因角标约定一致，会以**相同的有序元组**出现在同一 axis/set 组里，`unique_rows`（不做元组规范化）即可去重。`unique_rows` 必须实现为字典序升序 + inverse 映射（与 `torch.unique(dim=0)` 一致），保证顶点顺序确定性。

### 8.3 trim 与 apply_vertex_mask

```text
(v, f, _) = _C.meshing.marching_cubes(dmc_graph, dmc_vertices, dmc_value)
             # 断言 max(graph) < P == len(value)；顶点在跨零棱上线性插值；三角形顶序/去重见 meshing 配套规格
mask = mask_field.evaluate_f_bar(v) < 0.0             # 保留 UDF 内侧顶点
apply_vertex_mask:
    map = full(V, -1); map[mask] = arange(count(mask))
    f = map[f]; v = v[mask]
    f = f[ all(f != -1, dim=1) ]                       # 任一顶点被删的三角形整体删除
返回 MeshingResult(v, f, None)
```

## 9. 边界情况与数值约定

- 空输入 / 预处理返回空 → `reconstruct` 返回 `None`（C++ 返回失败码）。
- UNet 早停 → 浅层 `grids[d] = None`、特征 `[0,C]`：solve/evaluate 循环全部按 None 跳过；`extract_dual_mesh` 未做 None 防护（`grids[d]._grid` 直接解引用）——原实现此处会崩，移植时应显式报错或跳过并记录。
- `normal_xyz.size(0) == 0`（d=0,1 全空）时 `normal_weight` 除零——原实现未防护，移植加断言。
- 全程 float32；PCG 也在 float32 上运行（`tol=1e-5` 相对/绝对语义见 sparse_solve 配套规格）；不收敛只警告，结果照用。
- `mod`/`%` 一律 torch 余数语义（结果与除数同号、非负）；`floor` 一律数学向下取整；`argmax` 平局取最小下标；`torch.unique` 输出升序。
- 随机性：无（推理路径没有任何随机源；MLPWithGrad 的 fourier 分支未启用）。
- torch_scatter 依赖仅 `scatter_max` / `scatter_mean`（PointEncoder），语义如 [§5.1](#51-pointencoder)。

## 10. _C 算子清单（对照索引）

推理路径实际触达的原生算子（CPU 变体 `_CpuIndexGrid` / `_C.*`）。签名按 Python 调用侧；精确数学在各配套规格中定稿。

### 10.1 _CpuIndexGrid 方法

| 方法 | 签名（Python 侧） | 语义 | 调用点 |
| --- | --- | --- | --- |
| `build_from_pointcloud_nearest_voxels` | `(points [N,3] f32)` | 激活每点周围 2×2×2 最近 voxel | `build_point_splatting` |
| `build_from_ijk_coords` | `(ijk [M,3] int, pad_min, pad_max)` | 由 ijk 列表建 grid（pad 恒 (0,0,0)） | UNet neck / decoder 建层 |
| `num_voxels` | `() → int` | 活跃 voxel 数 | 各处 |
| `active_grid_coords` | `() → [Nv,3] int32` | 活跃 voxel ijk（固定内部序） | 各处 |
| `ijk_to_index` | `([M,3] int) → [M] int` | ijk → 活跃索引，缺失 -1 | PointEncoder / ZeroPadding |
| `grid_to_world` / `world_to_grid` | `([M,3] f32) → [M,3] f32` | `(ijk+0.5)*vs` / `x/vs-0.5` | voxel 中心 / 局部坐标 |
| `sample_trilinear` | `(pts [Q,3], data [Nv,C], return_grad) → [Q,C] (+[Q,C,3])` | 邻域三线性插值（缺 voxel 按 0），可带对 xyz 的梯度 | interpolator / udf_decoder |
| `subdivided_grid` | `(factor=2, mask [Nv] bool) → grid` | 细分 mask 选中 voxel（×8） | UNet Upsampling |
| `subdivide` | `(fine_grid, factor=2, data [Nv,C]) → [Nv_fine,C]` | 父特征复制给子 voxel（nearest） | UNet Upsampling |
| `max_pool` | `(coarse_grid, factor=2, data [Nv,C]) → [Nv_c,C]` | 逐通道 8 子 voxel 取 max，空 → -inf | UNet MaxPooling |

不在路径：`build_from_pointcloud`、`coarsened_grid`、`dual_grid`、`points_in_active_voxel`、`splat_trilinear`、`sample_bezier`。

### 10.2 _C.conv

| 算子 | 签名 | 语义 |
| --- | --- | --- |
| `convolution_kernel_map` | `(src_grid, dst_grid, ksize=3) → [27, Nq] int` | 每个查询 voxel 的 27 个 offset 槽 → 源 voxel 索引 / -1；**offset 枚举顺序是移植关键** |
| `sparse_convolution` | `(feat [Nin,Cin], kernel [27,Cin,Cout], nbmap [nnz,2] i32, nbsizes [Nq] i32, shape (Nin,Nout), transposed=False) → [Nout,Cout]` | 稀疏 3D 卷积聚合（Python 侧 nbmap 构造见 [§5.2](#52-sparsestructurenet-unet)） |

### 10.3 _C.kernel_eval

| 算子 | 签名 | 语义 |
| --- | --- | --- |
| `rhs_evaluation` | `(grid_d, pts [M,3], theta [M,4], grid_theta [Nv,4], dtheta [M,4,3], values [M,3]) → ([Nv], …)` | RHS：`Σ_pts ∇K_d(p, v) · value_p` 聚合到 voxel（法线项） |
| `build_coo_indexer` | `(grid_d, grid_dd) → [Nv_d, K_nb] int` | 每个 d-层 voxel 与 dd-层邻域 voxel 的稀疏耦合模式，值 = dd 索引 / -1 |
| `matrix_building` | `(grid_d, grid_dd, pts, theta_d, theta_dd, gtheta_d, gtheta_dd, dtheta_d, dtheta_dd, indexer, use_grad, nnz) → ([nnz], …)` | 组装 `GᵀG`(use_grad=False) / `QᵀQ`(use_grad=True) 块的 COO 值 |
| `k_building` | `(grid_d, gtheta_d, indexer, nnz) → ([nnz], …)` | 正则块 `K`（同层核 Gram） |
| `kernel_evaluation` | `(grid_d, pts [Q,3], theta [Q,4], gtheta [Nv,4], alpha [Nv], grad_kernel [0,0,3], grad=False) → ([Q], [Q,3])` | `f_d(q) = Σ_v alpha_v K_d(q,v)`（27 邻域支撑） |

不在路径：`qg_building`、`csr_matrix_multiplication`（非 fused）。

### 10.4 _C.sparse_solve

| 算子 | 签名 | 语义 |
| --- | --- | --- |
| `ind2ptr` | `(row_inds [nnz], n_rows) → [n_rows+1]` | 已排序 COO 行索引 → CSR 行指针 |
| `solve_pcg` | `(csr_p:{(di,dj)}, csr_j:{...}, csr_x:{...}, block_ptr:list[5], rhs [n], inv_diag [n], tol=1e-5, max_iter=2000, false) → (x [n], iters)` | 块对称（只存上三角）Jacobi 预条件 CG |

不在路径：`ptr2ind`。

### 10.5 _C.meshing

| 算子 | 签名 | 语义 |
| --- | --- | --- |
| `build_flattened_grid` | `(grid_d, finer_grid_or_None, conforming: bool) → grid` | 该层叶子 cell 网格（剔除被细层覆盖区域） |
| `build_joint_dual_grid` | `([grids]) → dual_grid` | 跨层 dual 顶点网格 |
| `dual_cube_graph` | `([grids], dual_grid) → [C,8] int64` | dual cell 8 角点索引（指向 d 升序拼接的顶点数组），角标约定见 [§8.1](#81-网格展平与-dual-图_c-交接) |
| `marching_cubes` | `(graph [C,8] i64, pos [P,3] f32, value [P] f32) → (v [V,3] f32, f [T,3] i64, vidx [V,2] i64)` | 广义 MC（顶点去重、跨零棱线性插值）；`vidx` 推理不用 |

不在路径：`primal_cube_graph`。

## 11. 风险与移植注意（务必逐条核对）

1. **拼接顺序**：`normal_xyz`/`normal_value` 按 d=0,1 升序；`dmc_vertices` 按 flattened grid d 升序；`rhs_vec`/`inv_diag`/`block_ptr`/解切片按 d=0..3 升序——全部依赖 grid 内部 voxel index 序保持一致。
2. **负号**：solve 收到的是 `-normal_value`（UNet 预测法线取负）。
3. **权重公式**：`pos_weight = 1e4/N`、`normal_weight = 1e4/M × 0.01`、`reg_weight = 1.0`，其中 0.01 = `voxel_size²`。
4. **半格偏移**：`origin = 0.5·vs`，`grid_to_world(ijk) = (ijk+0.5)·vs`；PointEncoder 局部坐标 `(world_to_grid(x)+0.5) mod 1`，udf_decoder 局部坐标 `(x mod vs)/vs − 0.5`——两处公式不同但都基于同一角点对齐约定。
5. **COO 条目顺序**：`where(indexer != -1)` 的行主序决定 `a_x`/`inv_diag` 排列与 `matrix_building` 的 `indexer` 覆写值；CPU 上 indexer 恒 int64。
6. **两套 svh**：basis/normal 在 `decoder_svh`，udf 在 `decoder_tmp_svh`；主场与 mask field 分别持有。
7. **scale 语义**：场评估先 `xyz/scale` 再算 f；`level_set`（mask 0.2）不随 scale 缩放；mesh 顶点因 `dmc_vertices×scale` 已在输入坐标系。
8. **MISE 全量重评估**：细分后所有顶点（含旧顶点）重新过场评估，不做插值复用。
9. `torch.unique` 升序 + inverse 语义、`argmax` 平局规则、torch 非负取模——任何一个不一致都会静默改变输出拓扑。
10. `max_pool` 后 `-inf → 0`、`scatter_mean` 空 voxel → 0、ZeroPadding 缺失 → 0：三种"空补零"缺一不可。
