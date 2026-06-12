# ComputeShaderMeshGenerator 合并重构方案

## 目标

消除 `ComputeShaderMeshGenerator.h` 中重复的方法重载、重复的 RenderTarget 持有、重复的有效性标记等，减少维护成本。

---

## 1. `BuildActiveCellsFromReferencePoints` 两重重载

### 现状

```cpp
// 重载A：用 this->ReferencePoints（virtual）
virtual void BuildActiveCellsFromReferencePoints(float ActivationRadius, TSet<FCSMeshGeneratorVoxelKey>& OutCells) const;

// 重载B：传入显式 ReferencePoints（virtual）
virtual void BuildActiveCellsFromReferencePoints(const TArray<FVector>& InReferencePoints, float ActivationRadius, TSet<FCSMeshGeneratorVoxelKey>& OutCells) const;
```

### 方案

- 重载B 保留为 **virtual**，作为核心实现
- 重载A 降级为 **非 virtual inline** convenience wrapper

```cpp
/** 用显式参考点构建活跃 cell。子类可覆写。 */
virtual void BuildActiveCellsFromReferencePoints(const TArray<FVector>& InReferencePoints, float ActivationRadius, TSet<FCSMeshGeneratorVoxelKey>& OutCells) const;

/** Convenience: 用 this->ReferencePoints 调上述方法。 */
void BuildActiveCellsFromReferencePoints(float ActivationRadius, TSet<FCSMeshGeneratorVoxelKey>& OutCells) const
{
    BuildActiveCellsFromReferencePoints(ReferencePoints, ActivationRadius, OutCells);
}
```

### 影响

- 调用侧无变化，签名兼容
- 子类只需 override 重载B

---

## 2. `EnsureTriangleCacheByBox` 两重重载

### 现状

```cpp
// 重载A：用 GeneratorBounds（virtual）
virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(FName RequestId, bool bForceFullRebuild = false);

// 重载B：显式 BoxCenter/BoxExtent（virtual）
virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(FName RequestId, const FVector& BoxCenter, const FVector& BoxExtent, bool bForceFullRebuild = false);
```

### 方案

- 重载B 保留为 **virtual**，作为核心实现
- 重载A 降级为 **非 virtual inline** convenience wrapper，从 GeneratorBounds 提取 BoxCenter/BoxExtent 后调重载B

```cpp
/** 以显式包围盒确保缓存。子类可覆写。 */
virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(FName RequestId, const FVector& BoxCenter, const FVector& BoxExtent, bool bForceFullRebuild = false);

/** Convenience: 用当前 GeneratorBounds 调上述方法。 */
FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(FName RequestId, bool bForceFullRebuild = false);
```

### 注意

重载A 不再 virtual 后，`UpdateMeshGeneratorCacheByBox()` 内部调重载A 的路径需检查，需改为直接调重载B 或保持不变（重载A 现在是 inline，会自动展开为调重载B）。

### 影响

- 调用侧无变化
- 子类只需 override 重载B

---

## 3. RenderTarget 双重持有（Actor + CacheHandle）

### 现状

**Actor 成员：**
```cpp
UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT;
TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT;
TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT;
```

**CacheHandle 成员：**
```cpp
UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Triangle Cache")
TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT = nullptr;
TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT = nullptr;
TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT = nullptr;
```

### 问题

- 同一个 RT 被两处 `TObjectPtr` 引用，生命周期管理分散
- `GetTriangleCacheHandle()` 每次调用都需手动同步这 3 个指针
- 存在 Actor 和 Handle 不同步的风险

### 方案

**Actor 中的 3 个 RT UPROPERTY 是为蓝图暴露和 GC 跟踪用的，不能删除。方案是从 CacheHandle 中去掉 RT 指针**，改为仅保留统计信息。

重构 `FCSMeshGeneratorTriangleCacheHandle`：

```cpp
USTRUCT(BlueprintType)
struct FCSMeshGeneratorTriangleCacheHandle
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly)
    bool bValid = false;

    UPROPERTY(BlueprintReadOnly)
    int32 CacheGeneration = 0;

    UPROPERTY(BlueprintReadOnly)
    FBox CachedWorldBounds = FBox(ForceInit);

    UPROPERTY(BlueprintReadOnly)
    FIntVector GridSize = FIntVector::ZeroValue;

    UPROPERTY(BlueprintReadOnly)
    float VoxelSize = 0.0f;

    UPROPERTY(BlueprintReadOnly)
    int32 ActiveVoxelCount = 0;

    UPROPERTY(BlueprintReadOnly)
    int32 DirtyVoxelCount = 0;

    // 移除以下三个 RT 字段
    // TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT;
    // TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT;
    // TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT;
};
```

`GetTriangleCacheHandle()` 不再拷贝 RT 指针，只填充统计信息。

### 影响

- `GetTriangleCacheHandle()` 的调用者如果之前直接读 Handle 的 RT 字段，需改为从 Actor 读取
- 需全局搜索 `TriangleCacheHandle.VoxelMetaRT` / `TriangleVertexRT` / `TriangleNormalRT` 的引用点

---

## 4. `bValid` 在三处 Handle 中重复

### 现状

- `FCSMeshGeneratorTriangleTextureDataHandle::bValid`
- `FCSMeshGeneratorSurfaceVoxelTextureDataHandle::bValid`
- `FCSMeshGeneratorTriangleCacheHandle::bValid`

### 方案

**不强制抽取基类**（代价大于收益），但统一语义约定：

- 三个 `bValid` 语义均为"此 Handle 的关联数据是否有效"
- 在 `.cpp` 实现中确保 `ResetCacheRuntime()` / `ClearGeneratedDataTextureCache()` 等清理路径同时置 `bValid = false`
- 将来若扩展到更多 Handle 类型，再抽取 `FCSMeshGeneratorDataHandleBase { bool bValid; }` 基类

### 影响

- 无代码变更，仅约定

---

## 5. `SourceWorldBounds` / `CachedWorldBounds` 语义重叠

### 现状

| 字段 | 所在结构体 | 语义 |
|---|---|---|
| `SourceWorldBounds` | `FCSMeshGeneratorTriangleTextureDataHandle` | 生成三⻆形数据时的源世界范围 |
| `SourceWorldBounds` | `FCSMeshGeneratorSurfaceVoxelTextureDataHandle` | 生成体素数据时的源世界范围 |
| `CachedWorldBounds` | `FCSMeshGeneratorTriangleCacheHandle` | 当前缓存覆盖的世界范围 |

### 方案

- 统一命名为 `CachedWorldBounds`（三者本质都是"数据覆盖的世界范围"）
- 修改 `FCSMeshGeneratorTriangleTextureDataHandle::SourceWorldBounds` → `CachedWorldBounds`
- 修改 `FCSMeshGeneratorSurfaceVoxelTextureDataHandle::SourceWorldBounds` → `CachedWorldBounds`

### 影响

- 需全局重命名，搜索 `SourceWorldBounds` 的引用点
- 对蓝图接口无影响（结构体导出基于类型，字段重命名后 UE 反射自动处理）

---

## 实施顺序

| 步骤 | 内容 | 风险 |
|---|---|---|
| 1 | 合并方法重载 #1 #2（改 virtual → inline wrapper） | 低，签名兼容 |
| 2 | 移除 CacheHandle 中的 RT 字段（#3） | 中，需全局搜索引用 |
| 3 | 统一 Bounds 命名（#5） | 低，纯重命名 |
| 4 | 确认 `bValid` 清理路径完整性（#4） | 低，仅验证 |

---

## 验证清单

- [ ] 全量编译通过（`ComputeShaderGenerator` 模块 + 依赖模块）
- [ ] `GetTriangleCacheHandle()` 的蓝图调用者未受影响
- [ ] 缓存 dirty cell 重写路径正常（`DispatchDirtyVoxelTriangleCacheUpdate`）
- [ ] `EnsureTriangleCache` / `EnsureTriangleCacheByBox` 返回 Handle 的统计信息正确
- [ ] `ClearMeshGeneratorCache` 后 Handle 的 `bValid == false`