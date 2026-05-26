# Instance Brush Paint Design

本文描述一个 Actor-owned instance 笔刷工具的实现方案。目标是拖拽绘制时只显示 `DrawDebugPoint` 预览点，松开鼠标后再一次性写入目标 Actor 自己的 `UInstancedStaticMeshComponent` 或 `UHierarchicalInstancedStaticMeshComponent`。

## 目标

1. 笔刷由目标 Actor 发起，绘制结果归属于该 Actor。
2. 鼠标按下和拖拽期间不创建 instance，只维护一组 pending preview points。
3. 拖拽期间用 `DrawDebugPoint` 显示即将写入的位置。
4. 鼠标松开后，将 pending transforms 批量写入目标 Actor 下的 instance component。
5. 笔刷显示和地表碰撞遵循 UE Foliage 笔刷的行为边界。
6. `Esc` 退出临时编辑模式；本版不需要 Undo/Redo。

## 非目标

1. 不把结果写入 `AInstancedFoliageActor`。
2. 不在拖拽期间持续 `AddInstance`。
3. 不依赖 `FoliageEdit` 模块的 private header。
4. 不在 Runtime 模块里处理编辑器视口输入。

## 模块职责

| 模块 | 类型 | 职责 |
| --- | --- | --- |
| `ComputeShaderGenerator` | Runtime | 持有 Actor、instance component、写入/删除 API |
| `PCGPluginsEditor` 或新 Editor module | Editor | 处理 EdMode/InteractiveTool、鼠标输入、笔刷显示、preview debug point |
| `Foliage` | Runtime/Editor 可用依赖 | 使用 public `AInstancedFoliageActor::FoliageTrace` 复用 foliage trace 行为 |

Runtime 层只暴露稳定 API：

```cpp
UHierarchicalInstancedStaticMeshComponent* GetOrCreatePaintComponent(UStaticMesh* Mesh);
void CommitPaintInstances(const TArray<FTransform>& WorldTransforms, UStaticMesh* Mesh);
void RemovePaintInstancesInSphere(const FSphere& WorldSphere, UStaticMesh* Mesh);
```

Editor Tool 负责：

```text
MouseDown -> BeginStroke
MouseMove -> UpdateBrushTrace + SamplePreviewPoints + DrawDebugPoint
MouseUp   -> CommitStrokeToTargetActor
Esc       -> CancelStrokeAndExitTemporaryMode
```

## 交互流程

| 阶段 | 行为 | 是否写入 instance |
| --- | --- | --- |
| Hover | trace 鼠标下地表，显示 foliage brush sphere | 否 |
| MouseDown | 清空 pending 点，开始 stroke | 否 |
| Drag | 按笔刷半径、密度、间距采样；通过 foliage trace 投射到可绘制表面；用 `DrawDebugPoint` 显示 | 否 |
| MouseUp | 批量写入目标 Actor 的 HISM/ISM，然后保持或退出工具由工具设置决定 | 是 |
| `Esc` | 清空 pending 点和 preview 状态，退出临时编辑模式并回到普通选择模式 | 否 |

## 笔刷显示

笔刷半径显示使用 foliage 工具相同的资源和临时组件思路：

```cpp
BrushMaterial = LoadObject<UMaterial>(
    nullptr,
    TEXT("/Engine/EditorLandscapeResources/FoliageBrushSphereMaterial.FoliageBrushSphereMaterial"));

BrushMesh = LoadObject<UStaticMesh>(
    nullptr,
    TEXT("/Engine/EngineMeshes/Sphere.Sphere"));
```

临时 `UStaticMeshComponent` 规则：

```cpp
SphereBrushComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), TEXT("CSInstanceBrushSphere"));
SphereBrushComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
SphereBrushComponent->SetCollisionObjectType(ECC_WorldDynamic);
SphereBrushComponent->SetStaticMesh(BrushMesh);
SphereBrushComponent->SetMaterial(0, BrushMID);
SphereBrushComponent->SetAbsolute(true, true, true);
SphereBrushComponent->CastShadow = false;
```

位置和缩放：

```cpp
FTransform BrushTransform(
    FQuat::Identity,
    BrushLocation,
    FVector(BrushRadius * 0.00625f));

SphereBrushComponent->SetRelativeTransform(BrushTransform);
```

`DrawDebugPoint` 只用于显示 pending preview point，不替代 brush sphere：

```cpp
DrawDebugPoint(
    World,
    PreviewPoint.Location,
    PreviewPointSize,
    PreviewPoint.Color,
    false,
    PreviewLifetime,
    SDPG_World);
```

建议 `PreviewLifetime` 使用很短时间，例如 `0.05f` 到 `0.15f`，每次拖拽更新时重画。不要使用 persistent debug point，否则取消 stroke 或 viewport 刷新时容易残留。

## 碰撞和 Trace 逻辑

绘制 trace 使用 foliage 的 public API，而不是复制 `FoliageEdit` private 实现：

```cpp
AInstancedFoliageActor::FoliageTrace(
    World,
    Hit,
    FDesiredFoliageInstance(StartTrace, EndTrace, FoliageTypeOrNull),
    TEXT("CSInstanceBrush"),
    bReturnFaceIndex,
    TraceFilterFunc,
    bAverageNormal);
```

关键行为：

| 行为 | 规则 |
| --- | --- |
| Object type | 使用 `ECC_WorldStatic` |
| Trace shape | 使用 `FDesiredFoliageInstance::TraceRadius` 的 sphere sweep |
| Hidden actor | 跳过 `IsTemporarilyHiddenInEditor()` 的 Actor |
| Collision | 跳过没有 query collision 或不 block `ECC_WorldStatic` 的组件 |
| Brush/Volume | 跳过不适合作为绘制表面的 brush volume |
| Face index | 需要材质/顶点/层过滤时开启 `bReturnFaceIndex` |
| Normal | 需要贴合法线朝向时开启 `bAverageNormal` |

因为 `FFoliagePaintingGeometryFilter` 在 `FoliageEdit` private header 中，插件侧实现自己的过滤器：

```cpp
struct FCSInstanceBrushGeometryFilter
{
    bool bAllowLandscape = true;
    bool bAllowStaticMesh = true;
    bool bAllowBSP = false;
    bool bAllowFoliage = false;
    bool bAllowTranslucent = false;
    TWeakObjectPtr<AActor> TargetActor;

    bool operator()(const UPrimitiveComponent* Component) const;
};
```

过滤规则对齐 foliage allow/deny list：

| 选项 | 允许对象 |
| --- | --- |
| `bAllowLandscape` | `ULandscapeHeightfieldCollisionComponent` |
| `bAllowStaticMesh` | 普通 `UStaticMeshComponent`，不包含 foliage-owned component |
| `bAllowBSP` | `UBrushComponent` 或 `UModelComponent` |
| `bAllowFoliage` | `UFoliageInstancedStaticMeshComponent` 或 foliage-owned actor |
| `bAllowTranslucent` | 为 false 时跳过 translucent material |

额外规则：

1. 默认跳过 `TargetActor` 自己的 painted instance component，避免边画边把预览/结果当成地表。
2. 如果需要在已有 painted instance 上继续绘制，增加 `bAllowTargetPaintedInstancesAsSurface`。
3. 如果目标 Actor 有 `GeneratorBounds`，可选地要求 hit point 在 bounds 内。

## Preview Point 采样

每次 drag 更新时，先根据鼠标 trace 得到 `BrushLocation` 和 `BrushNormal`。然后在 brush disc 内生成候选点，并沿 foliage 的球形笔刷方向投射：

```cpp
BrushNormal.FindBestAxisVectors(U, V);

FVector DiscPoint = Ru * U + Rv * V;
FVector SphereOffset = FMath::Sqrt(
    FMath::Max(1.0f - (Ru * Ru + Rv * Rv), 0.001f)) * BrushNormal;

FVector Start = BrushLocation + BrushRadius * (DiscPoint + SphereOffset);
FVector End = BrushLocation + BrushRadius * (DiscPoint - SphereOffset);
```

命中后生成 preview record：

```cpp
struct FCSInstancePaintPreviewPoint
{
    FVector Location = FVector::ZeroVector;
    FVector Normal = FVector::UpVector;
    FTransform WorldTransform = FTransform::Identity;
    FHitResult Hit;
};
```

采样时需要做 spacing 去重：

| 检查 | 目的 |
| --- | --- |
| 与 pending points 距离 | 防止一次 stroke 内过密 |
| 与目标 HISM 现有 instance 距离 | 防止重复刷在已有 instance 上 |
| 与 surface normal/slope | 控制陡坡或反面 |
| 与 `GeneratorBounds` | 限制绘制区域 |

## Commit 逻辑

鼠标松开时才写入 instance：

```cpp
void FCSInstanceBrushTool::EndStroke()
{
    if (!TargetActor.IsValid() || PendingTransforms.IsEmpty())
    {
        ClearPendingPreview();
        return;
    }

    UHierarchicalInstancedStaticMeshComponent* HISM =
        TargetActor->GetOrCreatePaintComponent(PaintMesh);

    for (const FTransform& WorldTransform : PendingTransforms)
    {
        HISM->AddInstance(WorldTransform, true);
    }

    HISM->MarkRenderStateDirty();
    TargetActor->MarkPackageDirty();
    ClearPendingPreview();
}
```

如果 UE 当前版本可用批量接口，优先批量提交：

```cpp
HISM->AddInstances(PendingTransforms, false, true);
```

## 数据归属

目标 Actor 推荐维护按 mesh 分组的组件：

```cpp
UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Instance Paint")
TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> PaintedInstanceComponents;
```

如果 `TMap` 不适合作为直接编辑属性，可改成数组结构：

```cpp
USTRUCT(BlueprintType)
struct FCSInstancePaintComponentSlot
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UStaticMesh> Mesh = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component = nullptr;
};
```

组件创建规则：

1. Outer 使用目标 Actor。
2. Attach 到 `SceneRoot`。
3. 本版不要求 `RF_Transactional`；后续需要 Undo/Redo 时再启用。
4. 设置 `StaticMesh` 和默认 material。
5. 注册组件并加入 Actor instance component 列表。

## 临时编辑模式退出

`Esc` 是退出临时编辑模式的统一入口。它不触发 commit，也不把 pending preview points 写入 instance。

| 操作 | 行为 |
| --- | --- |
| Drag preview | 只更新 pending points 和 `DrawDebugPoint` |
| MouseUp commit | 写入 pending transforms 并清空 preview |
| `Esc` while dragging | 清空 pending points，隐藏 brush sphere，退出临时编辑模式 |
| `Esc` while hovering | 隐藏 brush sphere，退出临时编辑模式 |
| Right click cancel | 可选支持；行为应等同于 `Esc` |

退出时需要清理：

1. `PendingPreviewPoints` 和 `PendingTransforms`。
2. 短生命周期 debug point 状态。
3. 临时 `SphereBrushComponent` 的显示或注册状态。
4. 当前 tool/EdMode 中保存的 `TargetActor` 弱引用。
5. 编辑器模式回到普通选择模式。

## 实现顺序

1. 在 Runtime Actor 增加 `GetOrCreatePaintComponent` 和 `CommitPaintInstances`。
2. 在 Editor module 创建 instance brush EdMode 或 InteractiveTool。
3. 复用 foliage brush sphere material/mesh 显示半径。
4. 使用 `AInstancedFoliageActor::FoliageTrace` 加自定义 `FCSInstanceBrushGeometryFilter`。
5. Drag 时只写 `PendingPreviewPoints` 并 `DrawDebugPoint`。
6. MouseUp 时一次性写入 HISM/ISM。
7. 绑定 `Esc`：清空 pending preview，隐藏 brush sphere，退出临时编辑模式。
8. 补 spacing、bounds 限制。

## 开放问题

1. 是否允许画在已有 painted instance 上。
2. 是否需要 erase brush；如果需要，erase 也应先 preview，MouseUp 后再删除。
3. 是否每种 mesh 一个 HISM，还是每种 mesh/material/placement profile 一个 HISM。
4. 是否使用 `AComputeShaderMeshGenerator::GeneratorBounds` 作为强制绘制范围。
5. 是否需要把 pending preview points 同步给 GPU cache 或只在 commit 后更新 cache。
