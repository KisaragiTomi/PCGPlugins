# Third-Party Notices — AITool Plugin

## NKSR (Neural Kernel Surface Reconstruction)

The C++ reconstruction implementation in `Source/AIToolModule/Private/NKSR/` is a port of the
inference path of **NKSR** by NVIDIA (https://github.com/nv-tlabs/NKSR), including the marching
cubes tables in `NKSRMCTables.h` (from `csrc/meshing/mc_data.h`).

The bundled network weights `Resources/nksr_ks.nkw` are converted from the official `ks.pth`
checkpoint (https://huggingface.co/heiwang1997/nksr-checkpoints).

Code and weights are subject to the **NVIDIA Source Code License-NC (non-commercial)**:
https://github.com/nv-tlabs/NKSR/blob/public/LICENSE.txt

> Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
> Any use, reproduction, disclosure or distribution of this software and related documentation
> without an express license agreement from NVIDIA CORPORATION & AFFILIATES is strictly prohibited.

Commercial use of this plugin's NKSR functionality requires a separate license from NVIDIA.

## Eigen

Dense linear algebra via Unreal Engine's bundled Eigen 3.4 (MPL2, `EIGEN_MPL2_ONLY`).
