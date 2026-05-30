# ComputeShaderMeshGenerator Triangle Cache Pitfalls

本文记录 `AComputeShaderMeshGenerator` 当前三角形缓存的存储方式，以及 triangle 跨越多个 voxel 时需要注意的问题。

## 当前存储

Triangle cache 不是全局唯一 triangle 表，而是按 active voxel 分 page 存储的候选 triangle list。

| 层级 | 结构 | 含义 |
| --- | --- | --- |
| CPU | `CacheState.ActiveCells` | 当前激活的 voxel key 集合 |
| CPU | `CacheState.CellToPage` | `FCSMeshGeneratorVoxelKey -> PageIndex` |
| CPU | `CacheState.FreePages` | 可复用 page 列表 |
| GPU | `VoxelMetaRT` | 每个 page 一条元数据 |
| GPU | `TriangleVertexRT` | 每个 triangle 3 个 `float4` 顶点 |
| GPU | `TriangleNormalRT` | 每个 triangle 1 个 `float4` 法线 |

每个 active voxel 会分配一个 `PageIndex`：

```text
TriStart = PageIndex * MaxTrianglesPerVoxel
TriCapacity = MaxTrianglesPerVoxel
```

`VoxelMetaRT` 每个 page 一个像素：

| Channel | 含义 |
| --- | --- |
| `x` | active flag |
| `y` | cache generation |
| `z` | `TriStart` |
| `w` | triangle count；负数表示 overflow |

`TriangleVertexRT` 的布局：

```text
TriSlot * 3 + 0 = float4(P0.xyz, 1)
TriSlot * 3 + 1 = float4(P1.xyz, 1)
TriSlot * 3 + 2 = float4(P2.xyz, 1)
```

`TriangleNormalRT` 的布局：

```text
TriSlot = float4(Normal.xyz, 0)
```

## 跨 Voxel Triangle

当前 `ScatterTrianglesToVoxelCacheCS` 使用 `TriangleAABBOverlapsVoxel()` 判断 triangle 是否写入某个 voxel。这个函数只做 triangle AABB 与 voxel AABB 的重叠测试，不是精确 triangle-box intersection。

因此，一个跨越多个 voxel 的 triangle 会被写入多个 voxel page。这个行为适合做宽松候选缓存，但不能当成精确几何覆盖集合。

## 主要坑点

| 问题 | 表现 | 结果 |
| --- | --- | --- |
| 重复写入 | 同一 triangle 命中多个 voxel | 存储和写入放大 |
| 边界重复 | voxel 边界上的 triangle 同时命中邻居 cell | 后续统计可能重复计数 |
| 误收录 | AABB 重叠但 triangle 实际没穿过 voxel | 后续查询需要再精筛 |
| Page 溢出 | 单 voxel triangle 数超过 `MaxTrianglesPerVoxel` | `VoxelMetaRT.w` 为负，超出 triangle 丢失 |
| 参考点漏检 | 预过滤只保留参考点附近 triangle | 长瘦 triangle 可能跨入 active 区域却没进 cache |
| 性能增长 | 每个 dirty voxel 扫一遍候选 triangle | 成本接近 `DirtyVoxelCount * TriangleCount` |
| 旧 RT 数据 | inactive page 的旧像素不一定马上清空 | 读取时必须以 CPU page 映射和 meta 为准 |

## 使用原则

1. `CacheState.CellToPage` 和 `VoxelMetaRT` 是读取 page 的有效索引，不要裸扫整张 RT 当作有效数据。
2. Triangle cache 存的是宽松候选三角形列表，后续距离、投影、碰撞或采样逻辑应再做精确过滤。
3. 如果需要严格覆盖，优先把 `TriangleAABBOverlapsVoxel()` 升级为精确 triangle-box intersection。
4. 如果单 voxel 经常 overflow，先调大 `MaxTrianglesPerVoxel`，再考虑降低 `VoxelSize`、拆大 triangle 或做多级 cache。
5. 如果长瘦 triangle 容易漏检，需要放宽 `ReferenceFilterDistance`，或让提取阶段同时考虑 triangle bounds，而不是只依赖参考点附近筛选。
