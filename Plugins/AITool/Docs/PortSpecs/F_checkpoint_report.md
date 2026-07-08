# F. ks.pth 权重清单报告

NKSR 默认权重 `ks.pth`（`C:/Users/KLW/.cache/torch/hub/checkpoints/ks.pth`，57,601,367 字节）的全量解析结果。由 `Plugins/AITool/Tools/inspect_checkpoint.py` 在无 torch 环境（仅 numpy）下生成，供 C++ 移植时核对每个权重的键名、dtype、shape 与存储布局。

## 产物

| 文件 | 内容 |
| --- | --- |
| `Plugins/AITool/Docs/PortSpecs/F_checkpoint_inventory.json` | `{key: {dtype, shape, numel}}` 全量清单 + 顶层结构 + 前缀分组统计 |
| `Plugins/AITool/Intermediate/PortWork/ks_weights.npz` | 全部 213 个权重数组，key 原样，C-contiguous float32 |
| `Plugins/AITool/Docs/PortSpecs/F_hparams.json` | **未生成** —— checkpoint 内没有 `hyper_parameters`（见下） |
| `Plugins/AITool/Tools/inspect_checkpoint.py` | 解析脚本，可重跑复现 |

复现命令：

```bash
"C:/Users/KLW/AppData/Local/Programs/Python/Python311/python.exe" \
  "D:/MyWork/UnrealProject/UETest574_2/Plugins/AITool/Tools/inspect_checkpoint.py"
```

## 顶层结构

torch zip 归档（prefix `archive/`，215 个 zip 条目）。`archive/data.pkl` 反序列化后顶层是一个只有一个键的 dict：

```text
{ "state_dict": OrderedDict(len=213) }   # 除 state_dict 外没有任何其它顶层键
```

- 没有 `hyper_parameters`、`epoch`、`global_step`、`optimizer_states` 等 Lightning 字段 —— 这是一个已剥离训练状态的纯权重文件。
- 因此 F_hparams.json 无法从 checkpoint 生成；**全部网络超参（voxel_size、UNet 深度、GroupNorm 组数、kernel 维数等）必须由 nksr 源码的默认构造参数提供**，并在其它规格任务（网络结构规格）中固定成文。本报告下方由 shape 反推出的结构参数可用于交叉校验。

## 参数总量与 dtype 分布

| 统计项 | 值 |
| --- | --- |
| 张量个数 | 213 |
| dtype 分布 | float32 × 213（无 fp16 / bf16 / int） |
| 浮点参数总量 | 14,383,065（float32 ≈ 54.86 MB，与文件体积一致） |
| npz 验证 | 213 个数组 shape/dtype 与清单一致，均无 NaN/Inf |

## 键名前缀分组统计

| 前缀组 | 张量数 | numel | 示例键 (shape) |
| --- | --- | --- | --- |
| `encoder.fc_pos` | 2 | 448 | `encoder.fc_pos.weight [64, 6]` |
| `encoder.blocks` | 15 | 15,552 | `encoder.blocks.0.fc_0.weight [32, 64]`, `encoder.blocks.0.shortcut.weight [32, 64]` |
| `encoder.fc_c` | 2 | 1,056 | `encoder.fc_c.weight [32, 32]` |
| `unet.encoders` | 24 | 1,798,144 | `unet.encoders.Enc1.SingleConv2.Conv.kernel [27, 32, 64]` |
| `unet.decoders` | 18 | 2,324,224 | `unet.decoders.Dec-2.SingleConv1.Conv.kernel [27, 384, 128]` |
| `unet.basis_heads` | 44 | 3,168,464 | `unet.basis_heads.Basis-1.OutConv.kernel [64, 4]` |
| `unet.udf_heads` | 20 | 2,358,784 | `unet.udf_heads.UDF-1.OutConv.kernel [256, 16]` |
| `unet.normal_heads` | 20 | 2,352,492 | `unet.normal_heads.Normal-1.OutConv.kernel [256, 3]` |
| `unet.struct_heads` | 20 | 2,352,492 | `unet.struct_heads.Struct-1.OutConv.kernel [256, 3]` |
| `interpolators.0` ~ `interpolators.3` | 8 × 4 | 692 × 4 | `interpolators.0.mlp.layers.0.weight [16, 4]` |
| `udf_decoder.*` | 16 | 8,641 | `udf_decoder.fc_out.weight [1, 32]` |

全部 213 个键的完整 shape 清单见 `F_checkpoint_inventory.json` 的 `state_dict` 字段，此处不重复。

## 各模块结构（由 shape 反推，需与源码规格互相校验）

### encoder — 点云局部特征编码器（ConvONet LocalPoolPointnet 形制）

```text
fc_pos:  Linear 6 -> 64          # 输入 6 维 = xyz(3) + normal(3)；64 = 2*hidden
blocks:  3 x ResnetBlockFC       # fc_0: 64->32, fc_1: 32->32, shortcut: 64->32(无 bias)
                                 # 输入 64 = concat(当前特征 32, 体素内池化特征 32)
fc_c:    Linear 32 -> 32         # 输出 32 维体素特征
```

- `shortcut` 只有 `weight` 没有 `bias`（`Linear(bias=False)`）。
- 推断：hidden=32、n_blocks=3、输入维 6，与 ConvONet 局部点编码器逐 shape 吻合。

### unet — 稀疏 3D UNet（4 层深度，3x3x3 卷积）

通道宽度按深度：level 0=32, 1=64, 2=128, 3=256。

| 子模块 | 命名 | 结构（每个卷积块） |
| --- | --- | --- |
| encoders | `Enc0` `Enc1` `Enc2` `Enc3` | `SingleConv1`(27, C, C) + `SingleConv2`(27, C, C') ，Enc1/2/3 的 SingleConv2 翻倍通道 |
| decoders | `Dec-2` `Dec-3` `Dec-4` | `SingleConv1`(27, C_deep+C_skip, C_out) + `SingleConv2`(27, C_out, C_out)；如 Dec-2: 384=256+128 -> 128 |
| basis_heads | `Basis-1` ~ `Basis-4` | `SingleConv`(27,C,C) + `SingleConv2`(27,C,64) + `OneConv0`(1x1x1, 64->64) + `OutConv`(64->4, 有 bias) |
| udf_heads | `UDF-1` ~ `UDF-4` | `SingleConv`(27,C,C) + `OutConv`(C->16, 有 bias) |
| normal_heads | `Normal-1` ~ `Normal-4` | `SingleConv`(27,C,C) + `OutConv`(C->3, 有 bias) |
| struct_heads | `Struct-1` ~ `Struct-4` | `SingleConv`(27,C,C) + `OutConv`(C->3, 有 bias) |

- 头部后缀 `-1`..`-4` 对应 4 个输出深度（每个 UNet 分辨率层各一组头），`Basis-4`/`UDF-4` 等在最细层通道为 32。
- 每个 `SingleConv*` = `GroupNorm` + `Conv`；**GroupNorm 的 weight/bias 长度等于该卷积的输入通道数**（如 `Dec-2.SingleConv1.GroupNorm [384]` 对 kernel `[27, 384, 128]`），说明执行顺序为 GroupNorm →（激活）→ Conv（norm-before-conv）。
- `Conv.kernel` 均无 bias；`OutConv` 有 bias。
- 推断：basis 头输出 4 = kernel basis 维数；udf 头输出 16 = 每层 UDF 特征维（4 层 × 16 = 64，恰为 `udf_decoder.fc_c` 的输入 64）；struct 头输出 3 = 结构分类 3 类。

### interpolators — 每深度一个小 MLP（4 个，结构相同）

```text
mlp.layers: Linear 4->16, act, Linear 16->16, act, Linear 16->16, act, Linear 16->4
            # nn.Sequential 索引 0/2/4/6 为 Linear，1/3/5 为激活（无参数）
```

### udf_decoder — UDF 细化解码器（ConvONet decoder 形制）

```text
fc_p:    Linear 6 -> 32                  # 查询点输入 6 维
blocks:  2 x ResnetBlockFC(32->32)       # fc_0/fc_1: 32->32，无 shortcut 键（恒等捷径）
fc_c:    ModuleList[2] Linear 64 -> 32   # 每个 block 前注入一次条件特征（输入 64 维）
fc_out:  Linear 32 -> 1                  # 标量 UDF 输出
```

## 存储布局约定（C++ 加载 npz 时必须遵守）

- 所有数组为 little-endian、C-order（row-major）、contiguous 的 `.npy`（npz = zip 容器）。
- `nn.Linear` 的 `weight` shape 为 **[out_features, in_features]**，前向为 `y = x · Wᵀ + b`。
- 稀疏卷积 `Conv.kernel` shape 为 **[27, C_in, C_out]**（kernel 体积在最前，输入通道在中间）—— 与 Linear 的 [out, in] 顺序**相反**，混用会得到静默转置错误。`OneConv0.Conv.kernel`/`OutConv.kernel` 为 1x1x1 卷积，shape [C_in, C_out]。
- 27 = 3x3x3 邻域体积。**27 个 offset 的枚举顺序无法从权重推断**，必须与网络结构规格中由 nksr 源码确定的 offset 顺序一字不差对齐。
- npz 内 key 与 `state_dict` 键名完全一致（含 `.`、`-` 字符，如 `unet.decoders.Dec-2.SingleConv1.Conv.kernel`）。

## 解析方法摘要

`inspect_checkpoint.py` 不依赖 torch：

- zip 中定位 `*/data.pkl`，用 `pickle.Unpickler` 子类反序列化；
- `persistent_load` 把 `('storage', <StorageType>, key, location, numel)` 解成 `(dtype, key, numel)`，数据从 `archive/data/<key>` 按 little-endian 原始字节读出；
- `find_class` 拦截：`torch._utils._rebuild_tensor_v2` → numpy 重建（支持 `storage_offset`/`stride`，输出 ascontiguousarray）；`torch.*Storage` → dtype 占位 stub；其余 torch/omegaconf 类 → 宽容 stub（本文件实际只遇到 `torch.FloatStorage` 一个 stub 类）；
- bfloat16 有转 float32 的兜底路径（本文件未用到）。

## Open Questions

- GroupNorm 的 `num_groups` 不体现在权重 shape 中，需由 nksr 源码确定并写入网络结构规格。
- 各激活函数类型（encoder/decoder 的 ReLU/SiLU 等）同样不体现在权重中，需源码确认。
- 27 邻域 offset 枚举顺序、UNet 上采样/下采样的具体机制（stride-2 conv 还是 pooling/subdivision）需源码确认；本 checkpoint 中没有独立的 down/up 采样权重，提示采样操作无参数。
