# AITool — NKSR 点云表面重建（纯 C++）

NKSR（NVIDIA Neural Kernel Surface Reconstruction, `ks` 配置）推理路径的插件内 C++ (CPU/float32) 实现。
无 Python、无外部依赖；网络权重随插件分发（`Resources/nksr_ks.nkw`，55 MB）。许可见 [ThirdPartyNotices.md](ThirdPartyNotices.md)（NVIDIA NSCL，非商用）。

## 使用

Blueprint（`UNKSRReconstructLibrary` / `UNKSRReconstructAsync`）：

| 入口 | 说明 |
|---|---|
| `RunNKSRReconstruction(InputFile, OutputFile, Settings)` | 文件→OBJ，阻塞调用线程 |
| `ReconstructPointCloud(Points, Normals, Settings, OutMesh)` | 数组→`FNKSRMeshData` |
| `ReconstructFileAsync / ReconstructPointsAsync` | 后台线程异步节点，`OnCompleted/OnFailed`，可 `Cancel()` |
| `MeshDataToDynamicMesh(MeshData, DynamicMesh)` | 结果写入 `UDynamicMesh` |

输入格式：PLY（ascii/binary-LE）、OBJ（v/vn）、XYZ/TXT/CSV、NPY（(N,3)/(N,≥6) float32/64）。
无法向时默认自动估计（PCA + MST 一致朝向，`FNKSRSettings::NormalKnn`）。

命令行（无编辑器 UI）：

```
UnrealEditor-Cmd.exe <项目.uproject> -run=NKSR -Input=<文件> [-Output=<obj>]
    [-DetailLevel=1.0] [-VoxelSize=0] [-MiseIter=1] [-DumpDir=<目录>] [-SelfTest]
```

`-SelfTest`：内置合成球端到端断言（顶点半径误差）。`-DumpDir`：逐阶段 .npy dump（对拍用）。

## 权重再生成

`Resources/nksr_ks.nkw` 由官方 `ks.pth` 转换（纯 numpy，不需要 torch）：

```
python Tools/inspect_checkpoint.py    # ks.pth -> Intermediate/PortWork/ks_weights.npz + 清单
python Tools/convert_checkpoint.py    # npz -> Resources/nksr_ks.nkw（含独立回读校验）
```

## 与原版 Python 实现对拍（golden）

```
python Tools/gen_input.py                                   # 确定性输入点云
python Tools/gen_golden.py --input ... --out golden/        # 原版 NKSR (CPU) 逐阶段 dump（需参考环境）
UnrealEditor-Cmd.exe ... -run=NKSR -Input=... -DumpDir=cpp_dump/
python Tools/compare_golden.py --golden golden/ --cpp cpp_dump/   # 按 ijk 对齐比较
```

注意：外部工作区自带的 `_C.pyd` 是 torch 2.8-nightly ABI，与 2.7.0 正式版不兼容；
运行 gen_golden 前需先用 `Intermediate/PortWork/rebuild_pyd.bat` 对当前 torch 重编 `_C`。

## 移植文档

设计契约与逐模块移植规格：[Docs/PortSpecs/](Docs/PortSpecs/)（G = 总设计与全局约定 GC-1..GC-9；A–F = 分规格）。
