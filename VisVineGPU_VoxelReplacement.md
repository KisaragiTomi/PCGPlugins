# VisVine GPU 阶段：体素替换 BVH 投射方案

## 现有 BVH 的真实用途分析

通过阅读代码，BVH/PrefixMesh 实际承担 **4 个独立角色**：

| 角色 | 发生位置 | 代码行 | 数据 | 产物 |
|------|----------|--------|------|------|
| **A: 位置投射** | CPU `PrepareVineVisualizationLinesCPU` | L628 | BVH 最近点查询 → `NearestPoint.Position` | 路径顶点投影到 mesh 表面（10 次迭代 noise 循环中每帧重做） |
| **B: 位置投射（Plane merge）** | CPU 同上 | L750 | BVH 最近点查询 → `NearestPoint.Position` | Plane 藤蔓合并后重新投射到表面 |
| **C: 法线偏移** | CPU 同上 | L762-776 | BVH 最近点查询 → `EditMesh.GetTriNormal(TriangleID)` | `VertexLocation += Normal * VinesOffset` 沿表面法线外推 |
| **D: 法线获取（GPU）** | GPU `BuildVineVisualizationMeshCS` | L306-309 | 网格 → 三角面遍历 → 法线 | 构建局部坐标系（Tangent/Binormal/FrameNormal）用于剖面顶点生成 |

**关键发现**：
- GPU 端 `FindClosestMeshPoint` 返回的 `Projected` 位置**实际上未被使用**（L321 `Center = Query` 始终用原始路径点），GPU 只需要法线来构建局部坐标系
- CPU 端位置投射（角色 A/B）会被反复调用，每个 line 的 10 次迭代每帧都查询
- CPU 端法线偏移（角色 C）使用单三角面法线 `GetTriNormal`，没有面积加权

## 替换方式总览

```
旧：Mesh顶点投影        →  新：体素位置投射（邻域最近点查询）
旧：单三角面法线         →  新：面积加权 + Blur 平滑体素法线
旧：MKBH 遍历 O(log N)  →  新：Cell 哈希 O(1) + 邻域搜索
旧：GPU 网格加速遍历    →  新：GPU 体素哈希查找
```

## 实施计划

### Step 1: 存储体素数据

| 文件 | 改动 |
|------|------|
| `GeometryEditorActor.h` | `AVineContainer` 新增 `FCSSurfaceVoxelData CachedSurfaceVoxels` |
| `GeometryEditorActor.cpp` L2072 | `(void)` → 赋值到 `CachedSurfaceVoxels` |

```cpp
// 当前
(void)GetBoxSceneSurfaceVoxelsFromGPU(SC.VoxelSize);
// 改为
CachedSurfaceVoxels = GetBoxSceneSurfaceVoxelsFromGPU(SC.VoxelSize);
```

`FCSSurfaceVoxelData` 是 `TArray` 成员，`GenerateVines` 返回时自动析构。

**Positions vs TargetPositions 语义说明**：

| 字段 | 来源 | 语义 |
|------|------|------|
| `Positions` | GLSL L412-413: `vec3 center = VoxelOrigin + (vec3(cell) + 0.5) * cellSize` | grid cell center（仅用于层列/查找，初始化后不再修改） |
| `TargetPositions` | GLSL L470-484: `center + (Σ offset×weight) / Σ weight` | 面积加权质心（所有三角面交点求平均 → 真实表面位置） |
| `Normals` | GLSL L460-467: `normalize(Σ normal×area)` → blur | 面积加权 + 3D 均值滤波后的平滑法线 |

**8 邻域采样时**：Tri-Linear 空间权重基于 cell 坐标（`Positions` 间接决定），距离权重基于 `TargetPositions`（面积加权质心）。最终顶点出到加权目标位置而非原始路径点。

### Step 2: CPU 端替换 — 角色 A/B/C

#### 2.1 新增体素查询函数

```cpp
// 对藤蔓路径的每个顶点：查找所在 cell 的 8 个角点体素
// 按顶点到各体素 TargetPosition 的反距离平方加权，返回加权法线 + 目标位置
static bool ProjectVinePathToVoxels(
    const FCSSurfaceVoxelData& VoxelData,
    TArray<FVector>& PathVertices,           // 原地修改为加权目标位置
    TArray<FVector>* OutNormals = nullptr);  // 输出加权法线
```

内部实现（CPU 端使用 unordered_map<int3_hash, int32> 做 cell 索引）：

```cpp
// 1. 构建 cell→index 哈希表（一次性）
TMap<FIntVector, int32> CellToIndex;
for (int32 i = 0; i < VoxelData.Cells.Num(); ++i)
    CellToIndex.Add(VoxelData.Cells[i], i);

// 2. 对每个顶点:
int3 BaseCell = floor((Vertex - VoxelOrigin) / VoxelSize);
float3 CellLocal = fract((Vertex - VoxelOrigin) / VoxelSize);
float TriLinearW[8] = { ... };

float3 SumWNormal = Zero, SumWTarget = Zero;
float TotalW = 0;

for (int Corner = 0; Corner < 8; ++Corner)
{
    int3 CornerCell = BaseCell + (Corner & 1, (Corner>>1)&1, (Corner>>2)&1);
    int32* VoxelIdx = CellToIndex.Find(CornerCell);
    if (!VoxelIdx) continue;

    // 阶段 1: 空间邻近权重（基于 TargetPosition 实际质心位置）
    FVector Target = VoxelData.TargetPositions[*VoxelIdx];
    float DistSq = FVector::DistSquared(Vertex, Target);
    float SpatialW = 1.0f / FMath::Max(DistSq, KINDA_SMALL_NUMBER);

    // 阶段 2: 组合 = Tri-Linear 网格权重 × 空间邻近权重
    float FinalW = TriLinearW[Corner] * SpatialW;

    SumWNormal += VoxelData.Normals[*VoxelIdx] * FinalW;
    SumWTarget += Target * FinalW;
    TotalW += FinalW;
}

if (TotalW > KINDA_SMALL_NUMBER)
{
    OutVertex = SumWTarget / TotalW;               // 加权目标位置
    OutNormal  = (SumWNormal / TotalW).GetSafeNormal();
}
```

#### 2.2 修改 `PrepareVineVisualizationLinesCPU`

| 角色 | 旧代码 | 新代码 |
|------|--------|--------|
| **A: 位置投射** (L628) | `TryFindNearestPointOnVinePrefixMesh(..., "NoiseProject")` → `VertexLocation = NearestPoint.Position` | `ProjectVinePathToVoxels(CachedSurfaceVoxels, VertexLocation)` |
| **B: Plane merge** (L750) | `TryFindNearestPointOnVinePrefixMesh(..., "MergeProject")` → `VertexLocation = NearestPoint.Position` | 同上 |
| **C: 法线偏移** (L762-776) | `TryFindNearestPointOnVinePrefixMesh` + `EditMesh.GetTriNormal` → `VertexLocation += Normal * VinesOffset` | `ProjectVinePathToVoxels` 返回法线 → `VertexLocation += VoxelNormal * VinesOffset` |

#### 2.3 两种实现路径对比

**路径 1: 新增重载函数**

```cpp
// 旧签名
static bool PrepareVineVisualizationLinesCPU(
    ..., UDynamicMesh* PrefixMesh, const FGeometryScriptDynamicMeshBVH& BVH, ...);

// 新签名（体素版）
static bool PrepareVineVisualizationLinesCPU_Voxel(
    ..., const FCSSurfaceVoxelData& VoxelData, ...);
```

- `VisVineGPUInternal` 调用新重载
- 保留旧函数给 `VisVineCPU` 回退路径

**路径 2: 函数内部分支**

在现有 `PrepareVineVisualizationLinesCPU` 内部，用 `FCSSurfaceVoxelData` 可空性决定走 BVH 还是体素路径。

**推荐路径 1**：改动范围可控，旧路径完整保留。

### Step 3: GPU 端替换 — 角色 D

#### 3.1 采样策略：8 邻域加权

每个路径顶点落在某个体素 cell 内，采样该 cell 的 8 个角点体素。**关键**：体素位置不是 cell center，而是面积加权质心（`TargetPosition`）。权重计算必须基于实际质心位置，而非纯网格插值。

```
顶点 P 落在 cell (cx, cy, cz) 内
采样 8 个角点 cell 对应的体素（如果存在）：

     (cx,cy+1,cz+1) ──── (cx+1,cy+1,cz+1)
        /│                  /│
       / │                 / │
(cx,cy+1,cz) ──── (cx+1,cy+1,cz)
      │  │                │  │
      │ (cx,cy,cz+1) ────│──(cx+1,cy,cz+1)
      │ /                 │ /
      │/                  │/
   (cx,cy,cz) ──── (cx+1,cy,cz)

每个体素 i 的实际位置: Ti = VoxelTargetPositions[i]  （不是 cell center！）
```

**两阶段加权**（均基于 `TargetPosition` 的实际位置）：

*阶段 1 — 空间邻近权重*：顶点 P 到各体素质心 Ti 的反距离平方

```hlsl
float distSq = dot(P - Ti, P - Ti);
float spatialW = 1.0f / max(distSq, 1.0e-6f);  // 质心越近权重越大
```

*阶段 2 — Tri-Linear 网格权重*：归一化后作为各体素贡献系数，乘以空间权重

```
TriLinearW[i] = { (1-ax)(1-ay)(1-az), ax·(1-ay)(1-az), ... }
// 这 8 个权重之和 = 1，表示 P 在 cell 内的相对位置
```

**组合权重**：

```hlsl
FinalW[i] = TriLinearW[i] * spatialW[i];  // 网格位置 × 表面质心距离
```

> **为什么保留 Tri-Linear 权重**：即使 TargetPositions 偏离 cell center，cell 坐标仍反映"这个体素属于表面哪一块"。Tri-Linear 确保了：P 更靠近某个角点时，该角的体素获得更大权重，避免对角线方向的体素过度参与。距离权重在此基础上进一步区分同 cell 内各体素的表面贴合度。

最终法线 = Σ(FinalWi × Ni) / ΣFinalWi，最终目标位置 = Σ(FinalWi × Ti) / ΣFinalWi。

#### 3.2 共享内存加速方案

线程组内相邻线程的顶点彼此靠近 → 它们查询的 8 邻域体素有大量重叠。使用 shared memory 缓存当前线程组覆盖的 voxel 窗口：

```
算法（每个 threadgroup）:
  1. 计算线程组内所有顶点的 AABB
  2. 将 AABB 对应的 voxel cell 范围（可能 4×4×4 ~ 8×8×8）
  3. 所有线程协作：并行加载这些 voxel 到 groupshared 数组
  4. Barrier 同步
  5. 每个线程从 groupshared 中查找自己的 8 个角点体素
  6. 计算反距离平方加权法线 + 目标位置
```

**shared memory 布局**:

```hlsl
#define VOXEL_WINDOW_SIZE 8  // 每维度最大缓冲 cell 数

groupshared float4 gs_VoxelPositions[VOXEL_WINDOW_SIZE * VOXEL_WINDOW_SIZE * VOXEL_WINDOW_SIZE];
groupshared float4 gs_VoxelNormals[VOXEL_WINDOW_SIZE * VOXEL_WINDOW_SIZE * VOXEL_WINDOW_SIZE];
groupshared uint   gs_VoxelValid[VOXEL_WINDOW_SIZE * VOXEL_WINDOW_SIZE * VOXEL_WINDOW_SIZE]; // 0=invalid
```

每线程组先通过哈希表加载窗口内所有体素，然后所有线程并行完成 8 邻域采样。

优化点：实际窗口大小 = 线程组内顶点覆盖的 cell 范围 + 1（因为每个顶点采样 8 个角）。64 个线程通常覆盖 4×4×4 范围 → 窗口约 5×5×5。

#### 3.3 新计算着色器 `VineVisualizationVoxel.usf`

```hlsl
// ===== 输入（替代 MeshTriangleVertices + Grid） =====
StructuredBuffer<float4> VoxelPositions;
StructuredBuffer<float4> VoxelNormals;
StructuredBuffer<float4> VoxelTargetPositions;     // ★ 面积加权质心
StructuredBuffer<int4>   VoxelCells;               // grid coord: (cx, cy, cz, index)
uint VoxelCount;
float VoxelSize;
float3 VoxelOrigin;                                // grid origin (world space)
uint VoxelHashSlotCount;                           // hash table capacity
uint VoxelHashSlotCountShift;                      // log2(slotCount) for fast modulo

// 哈希表项 → 查体素索引
StructuredBuffer<uint> VoxelHashSlots;             // 线性探测哈希槽
StructuredBuffer<uint> VoxelHashIndices;           // 槽 → voxel 索引

// ===== 保留 =====
StructuredBuffer<float4> PathPoints;
StructuredBuffer<int4> PathPointMeta;
StructuredBuffer<float> PathPointCurveU;
StructuredBuffer<int4> SegmentMeta;
RWStructuredBuffer<float4> RW_OutVertices;
RWStructuredBuffer<float2> RW_OutUVs;
RWStructuredBuffer<uint> RW_OutIndices;
// ... uniform 参数 ...

// ===== Shared Memory =====
#define VOXEL_WINDOW_MAX (MAX_VOXEL_WINDOW * MAX_VOXEL_WINDOW * MAX_VOXEL_WINDOW)
groupshared float4 gs_TargetPos[VOXEL_WINDOW_MAX];
groupshared float4 gs_Normal[VOXEL_WINDOW_MAX];
groupshared uint   gs_Valid[VOXEL_WINDOW_MAX];

// 哈希函数
uint VoxelHash(int3 Cell)
{
    return (uint(Cell.x) * 73856093u ^ uint(Cell.y) * 19349663u ^ uint(Cell.z) * 83492791u);
}

// 通过哈希表查找体素 → 返回索引，未找到返回 VoxelCount
uint LookupVoxel(int3 Cell)
{
    uint Hash = VoxelHash(Cell) >> VoxelHashSlotCountShift;
    for (uint Probe = 0u; Probe < 8u; ++Probe)
    {
        uint SlotIdx = (Hash + Probe) % VoxelHashSlotCount;
        uint VoxelIdx = VoxelHashIndices[SlotIdx];
        if (VoxelIdx >= VoxelCount) return VoxelCount; // 空槽
        int4 FoundCell = VoxelCells[VoxelIdx];
        if (FoundCell.x == Cell.x && FoundCell.y == Cell.y && FoundCell.z == Cell.z)
            return VoxelIdx;
    }
    return VoxelCount;
}

// 8 邻域 Tri-Linear 索引
uint3 GetCornerCell(int3 BaseCell, uint CornerIdx)
{
    return int3(
        BaseCell.x + int((CornerIdx >> 0) & 1u),
        BaseCell.y + int((CornerIdx >> 1) & 1u),
        BaseCell.z + int((CornerIdx >> 2) & 1u));
}

[numthreads(THREADGROUPSIZE_X, THREADGROUPSIZE_Y, 1)]
void VineVisualizationVoxelCS(uint3 DT : SV_DispatchThreadID, uint GI : SV_GroupIndex)
{
    // === Phase 1: 计算 group 内顶点范围，确定 voxel 窗口 ===
    uint VertexIdx = DT.x;
    float3 Query = float3(0, 0, 0);
    bool bHasVertex = VertexIdx < PathPointCount;
    if (bHasVertex)
        Query = PathPoints[VertexIdx].xyz;

    int3 BaseCell = floor((Query - VoxelOrigin) / VoxelSize);
    
    // 线程 0 收集 group 内 cell 范围
    // ... Barrier ...

    // === Phase 2: 协作加载窗口内所有体素到 groupshared ===
    // ... 多线程并行装载 ...

    // === Phase 3: 每线程采样自己的 8 个角点 ===
    float3 SumWeightedNormal = float3(0, 0, 0);
    float3 SumWeightedTarget = float3(0, 0, 0);
    float TotalWeight = 0.0f;
    
    // Tri-Linear 网格权重（P 在 cell 内的归一化坐标）
    float3 CellLocal = (Query - VoxelOrigin) / VoxelSize - floor((Query - VoxelOrigin) / VoxelSize);
    float TriLinearWeights[8] = {
        (1-CellLocal.x)*(1-CellLocal.y)*(1-CellLocal.z),
        (CellLocal.x)  *(1-CellLocal.y)*(1-CellLocal.z),
        (1-CellLocal.x)*(CellLocal.y)  *(1-CellLocal.z),
        (CellLocal.x)  *(CellLocal.y)  *(1-CellLocal.z),
        (1-CellLocal.x)*(1-CellLocal.y)*(CellLocal.z),
        (CellLocal.x)  *(1-CellLocal.y)*(CellLocal.z),
        (1-CellLocal.x)*(CellLocal.y)  *(CellLocal.z),
        (CellLocal.x)  *(CellLocal.y)  *(CellLocal.z),
    };

    for (uint Corner = 0u; Corner < 8u; ++Corner)
    {
        int3 CornerCell = GetCornerCell(BaseCell, Corner);
        uint VoxelIdx = LookupVoxel(CornerCell);
        if (VoxelIdx >= VoxelCount) continue;

        // 阶段 1: 空间邻近权重 — 顶点到体素质心(TargetPos)的反距离平方
        float3 Target = VoxelTargetPositions[VoxelIdx].xyz;
        float DistSq = dot(Query - Target, Query - Target);
        float SpatialW = 1.0f / max(DistSq, 1.0e-6f);

        // 阶段 2: 组合权重 = Tri-Linear 网格权重 × 空间邻近权重
        float FinalWeight = TriLinearWeights[Corner] * SpatialW;
        SumWeightedNormal += VoxelNormals[VoxelIdx].xyz * FinalWeight;
        SumWeightedTarget += Target * FinalWeight;
        TotalWeight += FinalWeight;
    }

    float3 FinalNormal = TotalWeight > 1.0e-8f 
        ? SafeNormalize3(SumWeightedNormal / TotalWeight, float3(0, 0, 1))
        : float3(0, 0, 1);
    float3 FinalTarget  = TotalWeight > 1.0e-8f 
        ? SumWeightedTarget / TotalWeight 
        : Query;

    // Phase 4: 生成剖面顶点（与原逻辑一致）
    // 使用 FinalNormal 构建局部坐标系
    // Binormal = cross(FinalNormal, Tangent)
    // OutPosition = FinalTarget + Binormal * Profile.x + FrameNormal * Profile.y  ★ 用加权目标位置
    // ...
}
```

**关键区别**：
- **旧**: 每个线程独立遍历全部三角面 → `TestTriangle` → 找最近点 + 单三角面法线
- **新**: 线程组协作装载 voxel 窗口到 shared memory → 每个线程从共享缓存采样 8 个角点 → 反距离平方加权 → 平滑法线 + 精确目标位置

#### 3.4 C++ 端改动

| 改动 | 说明 |
|------|------|
| 新增 `FVineVisualizationVoxelCS` 全局着色器类 | `IMPLEMENT_GLOBAL_SHADER`，`THREADGROUPSIZE_X=64` ，`THREADGROUPSIZE_Y=1` |
| 新增 `DispatchVineVisualizationGPU_Voxel()` | 上传 VoxelPositions + Normals + TargetPositions + Cells + HashTable，替代 MeshTriangles + Grid |
| HashTable 复用体素化阶段已有的 `VoxelHashSlots` + `VoxelHashIndices` 结构 | 无需重新构建（体素化时已生成） |
| `VisVineGPUInternal` 调用新 dispatch | 移除 `ExtractDynamicMeshTrianglesForGPU`、`BuildUniformGridForTriangles` |

#### 3.5 Shared Memory 窗口大小调优

| ThreadGroup 规模 | 典型 cell 范围 | 窗口大小 | Shared Mem 占用 |
|------------------|---------------|---------|----------------|
| 64 线程, 1D 分布 | 64 个路径点可能跨 4×4×4 cells | 5³=125 | ~4KB |
| 64 线程, 8×8 分布 | 如果改用 2D threadgroup | 更紧凑 | 更小 |

**内存计算**:
```cpp
// 假设 MAX_VOXEL_WINDOW = 6 → 216 cells
// gs_TargetPos: 216 × 16B = 3.5KB
// gs_Normal:    216 × 16B = 3.5KB
// gs_Valid:     216 × 4B  = 0.9KB
// Total: ~8KB → 远低于 32KB SM 限制
```

### Step 4: 清理

| 文件 | 移除内容 |
|------|----------|
| `GeometryEditorActor.cpp` | `VisVineGPUInternal` 中的 `RebuildVineBVHForPrefixMesh` 调用 |
| `GeometryEditorActor.cpp` | `ExtractDynamicMeshTrianglesForGPU` 调用 |
| `GeometryEditorActor.cpp` | `BuildUniformGridForTriangles` 调用 |
| `GeometryEditorActor.h` | （可选）新增 `bUseVoxelProjection` 开关 |

### Step 5: 编译验证

## 对比总结

| 维度 | 旧方案（BVH+Mesh） | 新方案（Voxel 8 邻域加权） |
|------|-------------------|---------------------------|
| CPU 位置投射 | `FindNearestPointOnMesh` O(log N) × 每顶点 | Cell 哈希 O(1) → 8 角点 Tri-Linear × 反距离²加权 |
| CPU 法线获取 | `GetTriNormal` 单三角面法线 | 8 体素面积加权 blur 法线按距离²加权 |
| GPU 输入数据 | Mesh 三角面 + Grid 加速结构 (~500KB) | Voxel Pos/Normal/TargetPos/Cells + HashTable (~80KB) |
| GPU 查询逻辑 | `WorldToGridCell` → 遍历三角面 → `ClosestPointOnTriangle` / 单线 | **Shared memory** 协作装载 voxel 窗口 → 每线程本地 8 点采样 → 加权 |
| GPU 法线质量 | `cross(B-A, C-A)` 单三角面法线 | 面积加权 + blur（GPU 预计算）+ 距离² 平滑插值 |
| GPU 顶点定位 | `Center = Query`（原始路径点，未用投射结果） | `Center = 加权目标位置`（更贴合表面） |
| 回退路径 | — | `VisVineCPU` + 旧 `PrepareVineVisualizationLinesCPU` 完整保留 |

## 风险与回退

| 风险 | 缓解措施 |
|------|----------|
| 体素分辨率不足导致藤蔓穿透表面 | `SC.VoxelSize` 可调（默认 ~10cm，藤蔓 ~50cm 半径足够） |
| 边界查询遗漏 | 3×3×3 邻域搜索覆盖相邻 cell |
| 性能退化 | 保留旧 BVH 路径 — 通过开关 `bUseVoxelProjection` 切换 |
