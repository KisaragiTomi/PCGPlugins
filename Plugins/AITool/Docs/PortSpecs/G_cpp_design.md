# G — AITool 插件 NKSR C++ 移植总设计

本文件是移植实现的**总契约**。实现模块时以本文件 + 对应分规格（A~F）为准。
分规格：A_inference_flow.md（控制流）、B_network_arch.md（网络）、C_conv_kernel_eval.md（卷积/核求值）、
D_vdbops_grid.md（网格）、E_meshing_solver.md（提网格/求解器）、F_checkpoint_report.md（权重清单）。

## 0. 目标

- 纯 C++（CPU、float32）实现 NKSR `ks` 配置的完整推理：点云(+法向) → 网格。
- 无 Python、无外部路径依赖；权重 `Resources/nksr_ks.nkw`（~55MB）随插件分发。
- 模块类型 Runtime，可进打包游戏（RuntimeDependencies 带权重）。
- 单线程结果确定；仅在"每个输出元素由单一线程写"的循环用 ParallelFor。

## 1. 全局约定（违反任意一条 = 静默错误）

编号 GC-x，实现中的关键位置需注释引用。

- **GC-1 取整**：`RoundHalfUp(x) = floor(x + 0.5)`（半格上取，非 std::round/rint）。
  `FloorDiv(a,b)`：负数向下取整（非 C++ 截断除）。`PosMod(x,m) ∈ [0,m)`（torch `%` 语义）。
- **GC-2 坐标变换**：grid = world*scale + translate，`scale = 1/voxelSize`、`translate = -origin/voxelSize`
  以 **double 计算后截断为 float 保存**，之后逐点变换全程 float、保持该代数形式（勿改写为 `(x-origin)/vs`）。
  层级 d：`voxelSize_d = vs*2^d`，`origin_d = 0.5*voxelSize_d`。dual grid：voxelSize 同、origin 减半格。
- **GC-3 体素序**：活跃体素按 **(i,j,k) 字典序**（i 最外）排序存储；
  `Coords[r] = ijk of index r` 与 `IjkToIndex(Coords[r]) == r` 互逆。特征矩阵行序 == 体素序。
  一切 per-voxel 张量（特征/解向量/状态）都跟随此序。与 Python 对拍时按 ijk 对齐（不逐行比较）。
- **GC-4 kernel offset 枚举**：3×3×3 核，offset 枚举 x 最外、z 最内；`kernelStart = floor(-k/2 + 1) = -1`；
  offset 加在 **source** 体素坐标上；中心 kIdx=13。权重布局 `[27][Cin][Cout]`，不转置。
- **GC-5 补零**：max_pool 后 `-inf → 0`；scatter_mean 空桶 → 0；SparseZeroPadding 缺失体素 → 0；
  sample_trilinear/bezier 对 inactive 体素按 0 贡献（权重不重归一化，bezier 权重和恒为 8）。
- **GC-6 torch 语义**：`unique` = 升序排序 + inverse 映射；`argmax` 平局取最小下标；
  GroupNorm(8 组, eps=1e-5, 有偏方差) 统计覆盖**整张**稀疏张量（禁止分块）。
- **GC-7 符号**：`normal_value = -unet_normal_features`（取负在 Reconstructor 拼装处）；
  MC 的 cubeType 位 i 置位 iff `sdf[i] < 0`；MISE 筛选用 `value > 0`；trim 保留 `mask_f_bar < 0` 的顶点。
- **GC-8 缩放**：`global_scale` 作用于输入 xyz（除）与输出网格顶点（乘）；
  mask 场 level_set = 2*voxel_size = 0.2 在**工作坐标系**，不随 scale 缩放。
- **GC-9 精度**：数值全程 float32；只有 GC-2 的变换构造用 double。求解器 float32、
  收敛判据 `||r||₂ ≤ tol*||b||₂`，tol=1e-5，max_iter=2000，不收敛仅告警。

## 2. `ks` 固定超参（configs.py，权重内无 hparams）

| 项 | 值 |
|---|---|
| feature / geometry | normal / kernel |
| voxel_size / tree_depth / adaptive_depth | 0.1 / 4 / 2 |
| kernel_dim / unet f_maps | 4 / 32（通道 [32,32,64,128,256]） |
| udf.enabled | true（mask = NeuralField, level_set 0.2） |
| interpolator | MLP 4→16→16→16→4，全 ReLU，带手写雅可比链 |
| solver pos_weight / normal_weight | 1e4/N / 1e4/M·vs² |
| reg_weight / density_range | 1.0 / [1,20] |
| 默认参数 | detail_level 由调用方传入（BP 默认 1.0）、mise_iter=1、fused_mode、nystrom 不触发 |

## 3. 文件布局（Source/AIToolModule/）

```
Public/
  NKSRReconstruct.h        # Blueprint API（函数库 + 设置/结果结构 + 异步节点声明引用）
  NKSRMeshData.h           # FNKSRMeshData / FNKSRPointCloud（BP 可见的数据结构）
Private/NKSR/
  NKSRCommon.h             # GC-1 工具、FNKSRMatrix、日志类别 LogNKSR
  NKSRGrid.h/.cpp          # FNKSRIndexGrid（规格 D 全部算子）
  NKSRTensorOps.h/.cpp     # GEMM(Eigen)、Linear、ReLU、GroupNorm、scatter_max/mean、ResnetBlockFC
  NKSRWeights.h/.cpp       # .nkw 加载器 + FNKSRWeightStore 单例（逐 key 校验 shape）
  NKSRConv.h/.cpp          # BuildKernelMap + SparseConvolution（规格 C §conv）
  NKSRKernelEval.h/.cpp    # BezierBasis、KernelEvaluation、BuildCooIndexer、MatrixBuilding、KBuilding、RhsEvaluation（规格 C）
  NKSRSolver.h/.cpp        # Ind2Ptr、FNKSRBlockMatrix、SolvePCG（规格 E §solver）
  NKSRMCTables.h           # edgeTable/triangleTable/numVertsTable/cubeRelTable/e2iTable…（从 mc_data.h 逐字复制）
  NKSRMeshing.h/.cpp       # BuildFlattenedGrid、BuildJointDualGrid、DualCubeGraph、MarchingCubes、MISE 细分、ApplyVertexMask（规格 E）
  NKSRNetwork.h/.cpp       # PointEncoder、UNet(SparseStructureNet)、各 Head、MultiscalePointDecoder、MLPInterpolator（规格 B）
  NKSRFields.h/.cpp        # FNKSRKernelField(solve/evaluate_f)、FNKSRNeuralField、FNKSRLayerField、ExtractDualMesh（规格 A/E）
  NKSRNormals.h/.cpp       # kd-tree + PCA 法向 + MST 一致朝向
  NKSRPointCloudIO.h/.cpp  # PLY(ascii/binary-LE)/OBJ/XYZ/CSV/NPY 读、OBJ 写、NPY 读写（golden 用）
  NKSRReconstructor.h/.cpp # 管线编排（= Reconstructor.reconstruct + extract_dual_mesh），阶段 dump 钩子
  NKSRReconstructAsync.h/.cpp  # UNKSRReconstructAsync（UBlueprintAsyncActionBase）
  NKSRCommandlet.h/.cpp    # UNKSRCommandlet：-Input= -Output= [-DumpDir=] [-DetailLevel=]，golden 生成/回归入口
```

## 4. 核心类型（NKSRCommon.h）

```cpp
struct FNKSRMatrix        // 行优先 float32；Rows×Cols；Cols=1 兼作向量
{
    TArray<float> Data;
    int32 Rows = 0, Cols = 0;
    // View() 返回 Eigen::Map<Eigen::Matrix<float,Dynamic,Dynamic,RowMajor>>
};
struct FNKSRGradTensor { TArray<float> Data; int32 Rows = 0, Channels = 0; };
// 布局 [Rows][Channels][3]，即 grad_kernel 张量 [N,C,3]

using FNKSRIjk = FIntVector;   // int32 ijk
```

空 GradTensor（Rows=0）语义 = Python 的 `zeros((0,C,3))`，各算子按"无梯度核"处理（规格 C）。

## 5. 跨模块签名（与 _C 一一对应）

```cpp
// NKSRGrid.h —— 规格 D
class FNKSRIndexGrid {
    double VoxelSizeD, OriginD;           // 构造参数
    float  Scale, Translate;              // GC-2 截断副本（各向同性，三轴同参）
    TArray<FNKSRIjk> Coords;              // GC-3 字典序
    TMap<FNKSRIjk, int32> Lookup;
public:
    void BuildFromPointsNearestVoxels(TConstArrayView<FVector3f> Pts);
    void BuildFromIjkCoords(TConstArrayView<FNKSRIjk> Ijk, FIntVector PadMin, FIntVector PadMax);
    FNKSRIndexGrid Coarsened(int32 Factor) const;
    FNKSRIndexGrid Subdivided(int32 Factor, TConstArrayView<bool> Mask) const;   // Mask 可空
    FNKSRIndexGrid Dual() const;
    int32 NumVoxels() const;  double VoxelSize() const;  double Origin() const;
    TConstArrayView<FNKSRIjk> ActiveGridCoords() const;
    void IjkToIndex(TConstArrayView<FNKSRIjk> Ijk, TArray<int32>& Out) const;   // 缺失 = -1
    void WorldToGrid(TConstArrayView<FVector3f> P, TArray<FVector3f>& Out) const;
    void GridToWorld(TConstArrayView<FVector3f> G, TArray<FVector3f>& Out) const;
    void PointsInActiveVoxel(TConstArrayView<FVector3f> P, TArray<bool>& Out) const;
    void SampleTrilinear(TConstArrayView<FVector3f> P, const FNKSRMatrix& GridData,
                         FNKSRMatrix& Out, FNKSRGradTensor* OutGrad = nullptr) const;
    void SampleBezier   (TConstArrayView<FVector3f> P, const FNKSRMatrix& GridData,
                         FNKSRMatrix& Out, FNKSRGradTensor* OutGrad = nullptr) const;
    void SplatTrilinear (TConstArrayView<FVector3f> P, const FNKSRMatrix& PointData, FNKSRMatrix& Out) const;
    void Subdivide(const FNKSRMatrix& CoarseData, int32 Factor, const FNKSRIndexGrid& FineGrid, FNKSRMatrix& Out) const; // nearest 上采样
    void MaxPool  (const FNKSRMatrix& FineData,  int32 Factor, const FNKSRIndexGrid& CoarseGrid, FNKSRMatrix& Out) const; // 含 -inf→0（GC-5）
};

// NKSRConv.h —— 规格 C
struct FNKSRKernelMap { TArray<FIntPoint> NbMap; /* {source,target} 对 */ TArray<int32> NbSizes; /* 27 组 */ };
void BuildKernelMap(const FNKSRIndexGrid& InGrid, const FNKSRIndexGrid& OutGrid, int32 KernelSize, FNKSRKernelMap& Out);
void SparseConvolution(const FNKSRMatrix& InFeat, const FNKSRMatrix& Kernel /*[27*Cin,Cout] 平铺*/,
                       const FNKSRKernelMap& KMap, int32 InRows, int32 OutRows, bool bTransposed, FNKSRMatrix& Out);

// NKSRKernelEval.h —— 规格 C（签名镜像 _C.kernel_eval，张量参数按上表类型）
void KernelEvaluation(const FNKSRIndexGrid&, TConstArrayView<FVector3f> Xyz,
                      const FNKSRMatrix& XyzKernel, const FNKSRMatrix& GridKernel,
                      const TArray<float>& Solution, const FNKSRGradTensor& GradKernelXyz,
                      bool bGrad, TArray<float>& OutF, TArray<FVector3f>* OutGradF);
void BuildCooIndexer(const FNKSRIndexGrid& GridD, const FNKSRIndexGrid& GridDD, TArray<int64>& OutIndexer /*[Nd,125]*/);
void MatrixBuilding(/* 规格 C matrix_building 全参 */);
void KBuilding(/* 规格 C k_building 全参 */);
void RhsEvaluation(/* 规格 C rhs_evaluation 全参 */);

// NKSRSolver.h —— 规格 E
void Ind2Ptr(TConstArrayView<int32> RowInds, int32 NumRows, TArray<int32>& OutPtr);
struct FNKSRBlockMatrix { /* (di,dj)→{RowPtr,ColInds,Values} 上三角，BlockSize[depth]，InvDiag */ };
int32 SolvePCG(const FNKSRBlockMatrix&, TConstArrayView<float> Rhs, float Tol, int32 MaxIter, TArray<float>& OutX);

// NKSRMeshing.h —— 规格 E
FNKSRIndexGrid BuildFlattenedGrid(const FNKSRIndexGrid& Grid, const FNKSRIndexGrid* FinerGrid, bool bConforming);
FNKSRIndexGrid BuildJointDualGrid(TConstArrayView<const FNKSRIndexGrid*> Flattened);
void DualCubeGraph(TConstArrayView<const FNKSRIndexGrid*> Flattened, const FNKSRIndexGrid& Dual, TArray<int64>& OutGraph /*[Nc][8] 展平*/);
void MarchingCubes(TConstArrayView<int64> CubeGraph, TConstArrayView<FVector3f> CornerPos,
                   TConstArrayView<float> CornerValue, TArray<FVector3f>& OutV, TArray<FIntVector>& OutF);
void SubdivideCubeIndices(TArray<int64>& CubeGraph /*inout*/, TArray<FVector3f>& Vertices /*inout*/);   // 规格 A §MISE
void ApplyVertexMask(TArray<FVector3f>& V, TArray<FIntVector>& F, TConstArrayView<bool> Keep);
```

## 6. 权重

- 格式 `.nkw`：header `{u32 magic='NKSW', u32 version=1, u32 count, u32 reserved}`；
  每张量 `{u16 nameLen, utf8 name, u8 dtype(0=f32), u8 ndim, u32 dims[ndim], f32[] data}`，全 LE。
- `Tools/convert_checkpoint.py`（纯 numpy，从 ks.pth 或 ks_weights.npz 生成）→ `Resources/nksr_ks.nkw`。
- 加载：`FNKSRWeightStore::Get()`（一次加载 + FCriticalSection），键→FNKSRMatrix；
  加载时逐 key 校验存在性与 shape（对照规格 F 清单；缺失/形状不符 → 失败返回，不 assert 崩溃）。
  推理不用的键（如 Normal-3/-4）允许存在，不校验不裁剪。
- 键名含 `-`（`Dec-2` 等），仅作 TMap 字符串键，不派生标识符。

## 7. Blueprint API（Public/NKSRReconstruct.h）

```cpp
USTRUCT(BlueprintType) struct FNKSRSettings
{
    float DetailLevel = 1.0f;          // 0~1；VoxelSize>0 时失效
    float VoxelSize   = 0.0f;          // 直接指定工作体素尺寸（输入单位）
    int32 MiseIter    = 1;
    bool  bEstimateNormalsIfMissing = true;
    int32 NormalKnn   = 30;
    int32 SolverMaxIter = 2000;
    float SolverTol   = 1e-5f;
    bool  bVerboseLog = false;
};
USTRUCT(BlueprintType) struct FNKSRResult { bSuccess, OutputFilePath, VertexCount, FaceCount, ErrorMessage, ElapsedSeconds };

UCLASS() class UNKSRReconstructLibrary : public UBlueprintFunctionLibrary
{
    // 同步（阻塞调用线程；保持旧签名兼容）：文件 → 文件(OBJ)
    static FNKSRResult RunNKSRReconstruction(const FString& InputFilePath, const FString& OutputFilePath, const FNKSRSettings& Settings);
    // 数组 → 网格数据
    static bool ReconstructPointCloud(const TArray<FVector>& Points, const TArray<FVector>& Normals,
                                      const FNKSRSettings& Settings, FNKSRMeshData& OutMesh, FString& OutError);
    static bool MeshDataToDynamicMesh(const FNKSRMeshData& Mesh, UDynamicMesh* Target);   // GeometryFramework
    static FString GetAIToolPluginDir();
};
// 异步：UNKSRReconstructAsync::ReconstructAsync(...) → OnCompleted(FNKSRResult) / 后台线程执行，
// FThreadSafeBool 取消位在阶段边界检查（Cancel() 提前返回）。
```

删除：PythonExePath / NKSRPackagePath / Device 字段，GetBundledPythonPath / GetDefaultPythonPath /
GetDefaultNKSRPackagePath / GetNKSRScriptPath，Scripts/、setup_python.ps1，一切硬编码机器路径。

## 8. 法向估计（NKSRNormals）

输入无法向时：简单 kd-tree（中位切分，栈上遍历）→ kNN(k=NormalKnn) PCA 最小特征向量 →
一致朝向：kNN 图边权 `1-|nᵢ·nⱼ|` 的 MST（Prim）+ BFS 传播翻转（对齐 open3d
`orient_normals_consistent_tangent_plane` 行为）。确定性：邻接与堆比较用 (weight, idxA, idxB) 全序。

## 9. IO（NKSRPointCloudIO）

- PLY：ascii + binary_little_endian，读 x/y/z[/nx/ny/nz]，兼容 float/double 属性；element 顺序无关。
- OBJ：`v`/`vn` 行（vn 数与 v 数一致才用）；忽略面/其它。CSV/XYZ/TXT：空白或逗号分隔 3 或 ≥6 列。
- NPY：v1.0/2.0 头，float32/float64，C 序，(N,3)/(N,≥6)；拒绝 Fortran 序并报可读错误。
- 写 OBJ：v/f（1-based）。NPY 读写供 golden 对拍（写 v1.0、float32/int64）。
- 全部返回 `bool + OutError`，不崩溃。

## 10. 验证协议（golden）

- `Tools/gen_input.py`：固定 seed 造两组输入存 .npy：
  a) 球面 8k 点 + 法向 + 1% 噪声；b) 无法向版本（测 C++ 法向估计只做端到端）。
- `Tools/gen_golden.py`（参考环境，直接调 nksr 内部逐阶段）dump 到 `Intermediate/PortWork/golden/`：
  每层 svh coords（按 ijk 排序）、encoder 输出、unet 各层 struct/basis/normal/udf（带 coords 键）、
  solve 解（按 ijk 键）、dmc_vertices/values、最终 mesh v/f。
- C++ Commandlet `-DumpDir=` 输出同名 .npy；`Tools/compare_golden.py` 按 ijk 对齐比较：
  结构（coords 集合）须完全一致；特征/解 rtol 1e-3 + atol 1e-4 分级报告；mesh 顶点用最近邻距离度量。
- 合成端到端（无 Python）：commandlet `-SelfTest`：内置球点云 → 网格顶点半径 |r-R| 统计断言。

## 11. 实现分工（工作流 agent，按依赖层）

| 波次 | 模块 | 依赖 | 规格 |
|---|---|---|---|
| W1 | Grid / TensorOps / Weights+convert_checkpoint.py / MCTables / PointCloudIO / Normals | Common.h | D / B / F / E / — / — |
| W2 | Conv / KernelEval / Solver / Meshing | W1 头文件 | C / C / E / E |
| W3 | Network / Fields | W1+W2 | B / A+E |
| W4 | Reconstructor / BP API / Async / Commandlet / Build.cs / uplugin / 清理 | 全部 | A |

头文件（契约）由集成者先行提交；agent 只写 .cpp（+私有内部函数），不改公共签名；
发现契约缺陷 → 在返回中报告而非擅自改头。

## 12. 许可

NKSR 为 NVIDIA Source Code License-NC（非商用）。插件加 `ThirdPartyNotices.md`：
算法与权重来源、许可条款指引；mc 表格源自 mc_data.h（同许可范围）。
