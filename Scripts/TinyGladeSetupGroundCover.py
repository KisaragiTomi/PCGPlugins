# -*- coding: utf-8 -*-
"""
给地被（草 + 花）配资产：`ACSGroundActor` 的 `Grass` / `Flowers` 默认是空的，
不跑这一遍**一株都不会长**（网格为空 = 那个物种整条关掉，见 `CollectCoverSpecies`）。

选型依据（都是从 `D:\\MyProject\\Tiny Glade\\extracted\\meshes` 的原始 glb 量出来的）：

  · 草 = `SM_TG_GrassBlade`（12 顶点 / 10 三角，5.76 × 50 cm）。它是 TG `_grass.raster`
    那条 VS 公式在 h=1, w=1, LOD0 处的**数值烘焙件** —— 半宽 2.88 / 2.592 cm 与
    `clamp(2.5(1−t²),0,1) × 0.04 × 0.72` 逐位对上。密度取 TG 的满密度实测值 50 株/m²。

  · 花 = `lowpoly_flower`（**5 个三角**，36 × 38 × 15 cm）。TG 里唯一一件"单株"花；
    它的包围盒**离原点 30 cm 才开始**，2026-09-09 起地被**不再自动坐底**（`bSeatOnBase`
    已拆，理由见 `FCSGroundCoverSpecies::HeightOffset`：花高出地面可能正是想要的）。
    `HeightOffset = 0`（花冠浮在草尖附近，与关卡现状一致）。⚠️ `MI_TG_LowpolyFlower` 的风按
    "网格原点就在地面"调的（t = z/45），改这个偏移要同步改那个 MI 的 `HeightTBaseCm`。

  · 薰衣草 = `garden_flower_01_lavender`（88 三角，40 × 40 × 182 cm）。紫色花丛从网格 z≈0 长到
    82 cm，z<0 只有一根半径 2.5 cm 的绿茎、向下伸 1 m。原尺寸比房子还扎眼，缩到 0.35–0.55 ——
    但缩完整丛只有约 37 cm，比 40–55 cm 的草还矮，整个淹在草里读成"插进地面"
    （2026-09-14 用户指出）。所以 `HeightOffset = +25`：花穗冒出草尖，茎仍藏在草里；
    +35 起能看到一截茎（同机位对比过 0/15/25/35）。偏移不乘缩放，`MI_TG_Lavender` 的风根部
    因此按平均缩放 0.45 挪到真实地面：`HeightTBaseCm = −25/0.45`、`HeightTRangeCm = MeshHeightCm = 82 − base`。

  · ❌ **不要用** `meadow_lowpoly_flowers` / `clover_flowers` / `clover`：它们是 TG 的
    **整片预散布网格**（130 m 见方、几万个三角），一个实例就铺满全场，不是单株。

逐实例 custom data（材质里用 `Per Instance Custom Data` 节点读）：
  **[0] = 弯曲幅度**（TG `_945`/`_997`：均匀[0.5,2] × 辐射簇 0.5 倍 × (1−遮罩)）
  **[1] = 该株的世界高度 cm**（材质做风加权要用：TG 是 `smoothstep(-0.2, 0.5, h/50*0.2)`，矮草几乎不摆）
语义是**逐组件**的 —— 藤蔓那一家在同样的 [0]/[1] 上放 SpawnTime 与弧长，互不干扰。

材质：草走 `MI_TG_Grass`（`M_TG_Grass` 的实例，两面 foliage + 程序化风，无贴图 —— 与 TG 的草
"PS 一张贴图都不采、纯顶点色 + 解析法线"同路）。TG 的 clutter 把颜色全烘在**顶点流**里，花从前
走 `M_TG_VertexColor`；2026-09-14 起两种花都换成同一个母材质的实例（`UseVertexColor` 开着，颜色仍取
顶点色），为的是和草吃同一个风场：`MI_TG_LowpolyFlower`（另开 `VertexColorIsSRGB`，淡黄）、
`MI_TG_Lavender`（走旧的 sqrt 路径，紫色与 `M_TG_VertexColor` 一致）。

投影：花投、草不投（`bCastShadow`，2026-09-06 / 09-12 两次裁决）。脚本新建的物种结构体默认是 true，
所以草这一份必须显式写 false —— 不写的话重跑一次草就全投影了。

⚠️ 重跑会把**所有**字段写回下面这张表，关卡里手调过的也不例外：L_HouseGroundDemo 的白花密度
目前是手调的 0.5（表里是 1.2）。

⚠️ **材质必须勾 `bUsedWithInstancedStaticMeshes`**：没勾的在实例路径上会被引擎**静默换成
默认材质**，画面一片灰而所有 readback 断言照绿。本脚本会检查并补勾（这一步会改材质资产）。

⚠️ 值要烘进 **BP CDO 与关卡实例两处**：只改 C++ 默认值、或只改 CDO，关卡里已存在的实例
一个都不会变（状态文件坑表里的那条）。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
ASSET = "%s/TinyGladeAsset" % PKG

GRASS_MESH = "%s/Meshes/SM_TG_GrassBlade" % ASSET
GRASS_MAT_CANDIDATES = ["%s/Materials/MI_TG_Grass" % ASSET, "%s/Materials/M_TG_Grass" % ASSET]

# (网格, 材质, 密度株/m², 容量, 缩放下限, 缩放上限, 倾倒角, 高度偏移 cm, 盐)
# ⚠️ 「高度偏移」= `HeightOffset`：**网格原点就是落点**，没有自动坐底那一层。可正可负，
#    不乘逐株缩放（整片一起挪这么多厘米）。两种花的取值理由见文件头。
# ⚠️ 材质与高度偏移是**一对**：两个 MI 的风根部（`HeightTBaseCm`）是按这里的偏移算的，只改一边
#    花会绕着半空中的一点摆。
# 容量一律顶到 ClampMax：它是**天花板不是预算**，显存按实际格数分配（密度说了算），
# 所以调高只是把"密度自动退让"的触发点推远，密度用不到的时候一个字节都不多花。
CAP = 1048576
FLOWER_SPECS = [
    ("%s/Meshes/lowpoly_flower" % ASSET,            "%s/Materials/MI_TG_LowpolyFlower" % ASSET, 1.2,  CAP, 0.9,  1.6,  8.0,  0.0, 3),
    ("%s/Meshes/garden_flower_01_lavender" % ASSET, "%s/Materials/MI_TG_Lavender" % ASSET,      0.25, CAP, 0.35, 0.55, 5.0, 25.0, 5),
]


def load(path):
    return unreal.EditorAssetLibrary.load_asset(path)


def ensure_instanced_flag(mat, tag):
    """补勾 `bUsedWithInstancedStaticMeshes` —— 没勾的材质在实例路径上会被静默替换成默认材质。

    要爬到**根母材质**再勾：`UMaterialInstance` 上没有这个开关，勾在实例上不会有任何效果，
    也不会报错。
    """
    if not mat:
        return None
    root = mat
    while root is not None and root.get_class().get_name() != "Material":
        root = root.get_editor_property("Parent")
    if root is None:
        unreal.log_warning("COVERSET %s: 找不到根母材质，无法检查 bUsedWithInstancedStaticMeshes" % tag)
        return mat
    if root.get_editor_property("bUsedWithInstancedStaticMeshes"):
        unreal.log("COVERSET %-10s %s 已勾 bUsedWithInstancedStaticMeshes" % (tag, root.get_name()))
        return mat
    root.set_editor_property("bUsedWithInstancedStaticMeshes", True)
    unreal.EditorAssetLibrary.save_loaded_asset(root)
    unreal.log("COVERSET %-10s %s **补勾** bUsedWithInstancedStaticMeshes 并保存" % (tag, root.get_name()))
    return mat


def make_species(mesh, mat, density, cap, lo, hi, lean, height_offset, salt, cast_shadow,
                 height_jitter=0.25, align=0.0, sink=2.0,
                 clump_size=250.0, clump_radial=0.30, clump_align=0.5):
    # ⚠️ 一律用 **C++ 属性名**（同 `TinyGladeSetupStairs.py` 的既有约定）：python 侧的 snake_case
    #    对 `b` 前缀布尔另有一套改名规则（`bCastShadow` → `cast_shadow`），猜错会抛异常。
    s = unreal.CSGroundCoverSpecies()
    s.set_editor_property("Mesh", mesh)
    s.set_editor_property("Material", mat)
    s.set_editor_property("DensityPerSqM", density)
    s.set_editor_property("MaxInstances", cap)
    s.set_editor_property("ScaleRange", unreal.Vector2D(lo, hi))
    s.set_editor_property("HeightJitter", height_jitter)
    s.set_editor_property("LeanDegrees", lean)
    s.set_editor_property("AlignToNormal", align)
    s.set_editor_property("Sink", sink)
    # 垂直方向唯一的旋钮：网格原点即落点，不存在按包围盒自动坐底那一层。
    s.set_editor_property("HeightOffset", height_offset)
    # 簇朝向：默认就是 TG 实测的那一组（2.5 m 簇、30% 辐射、权重上限 0.5）。
    s.set_editor_property("ClumpSize", clump_size)
    s.set_editor_property("ClumpRadialChance", clump_radial)
    s.set_editor_property("ClumpAlignment", clump_align)
    # 高度基准整簇共享（TG 的 `hash01(簇id*13)*1.5+0.5`），逐叶只叠 HeightJitter。
    s.set_editor_property("ScaleClumpShare", 1.0)
    # 弯曲幅度 → 逐实例 custom data[0]，供材质做 WPO。TG：均匀[0.5,2] × (辐射簇 ? 0.5 : 1)。
    s.set_editor_property("BendRange", unreal.Vector2D(0.5, 2.0))
    s.set_editor_property("RadialBendScale", 0.5)
    s.set_editor_property("Salt", salt)
    s.set_editor_property("bCastShadow", cast_shadow)
    return s


grass_mesh = load(GRASS_MESH)
grass_mat = next((m for m in (load(p) for p in GRASS_MAT_CANDIDATES) if m), None)
if not grass_mesh:
    unreal.log_error("COVERSET FAILED: %s 不存在" % GRASS_MESH)
    raise SystemExit
if not grass_mat:
    unreal.log_error("COVERSET FAILED: 草材质一个都不存在 %s" % GRASS_MAT_CANDIDATES)
    raise SystemExit

ensure_instanced_flag(grass_mat, "grass")

# 草的 `SM_TG_GrassBlade` Min.Z 正好是 0，所以高度偏移给 0 就是贴地（从前开不开坐底都一样）。
grass = make_species(grass_mesh, grass_mat, 50.0, CAP, 0.85, 1.25, 27.0, 0.0, 1, False)   # 27° = TG 的 0.3 × 90°

flowers = []
for path, mat_path, density, cap, lo, hi, lean, height_offset, salt in FLOWER_SPECS:
    mesh = load(path)
    if not mesh:
        # 缺一种花不算失败：TG 提取件的成色不一，缺了就少一种，草与其余的照长。
        unreal.log_warning("COVERSET 跳过缺失的花：%s" % path)
        continue
    flower_mat = load(mat_path)
    if not flower_mat:
        # 缺材质**是**失败：空材质槽在实例路径上会被静默换成默认材质，一片灰而断言照绿。
        unreal.log_error("COVERSET FAILED: %s 不存在" % mat_path)
        raise SystemExit
    ensure_instanced_flag(flower_mat, mesh.get_name())
    box = mesh.get_bounding_box()
    # minZ 与 HeightOffset 一起打出来：两者相加就是"花底离地多少 cm"，
    # 而这一条现在**只由配置决定**，不再被包围盒偷偷改写。
    unreal.log("COVERSET flower %-26s tris=%d 尺寸=%.0f×%.0f×%.0f cm minZ=%.1f 高度偏移=%.1f 底离地=%.1f"
               % (mesh.get_name(), mesh.get_num_triangles(0),
                  box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z,
                  box.min.z, height_offset, box.min.z + height_offset))
    flowers.append(make_species(mesh, flower_mat, density, cap, lo, hi, lean, height_offset, salt, True))


def apply(obj, where):
    obj.set_editor_property("bGroundCoverEnabled", True)
    obj.set_editor_property("Grass", grass)
    obj.set_editor_property("Flowers", flowers)
    unreal.log("COVERSET %-34s 草 50 株/m² + %d 种花" % (where, len(flowers)))


# ---- BP CDO：新拖进关卡的实例从这里取默认值 ----
bp = load("%s/BP_TinyGladeGround" % PKG)
if bp:
    apply(unreal.get_default_object(bp.generated_class()), "CDO")
    unreal.EditorAssetLibrary.save_loaded_asset(bp)

# ---- 关卡实例：CDO 的默认值**不传播到已存在的实例**，必须逐个再写一份 ----
A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for level in ("L_TerrainOpsDemo", "L_HouseGroundDemo"):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    n = 0
    for a in A.get_all_level_actors():
        name = a.get_class().get_name()
        if "Ground" not in name or "Shaper" in name:
            continue
        apply(a, "%s/%s" % (level, a.get_actor_label()))
        # 散一趟并把 GPU 计数打出来 —— "脚本跑成功了"与"真的长出草了"是两回事，
        # 而后者只有 counter 说了算（CPU 全程不知道长了几株）。
        a.call_method("RebuildGroundCover")
        reason = a.call_method("GetGroundCoverUndrawableReason")
        if reason:
            unreal.log_warning("COVERSET %s 画不出来：%s" % (a.get_actor_label(), reason))
        for i in range(1 + len(flowers)):
            unreal.log("COVERSET   物种 %d 实例数 = %s"
                       % (i, a.call_method("DebugReadGroundCoverCountGpuSync", (i,))))
        n += 1
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log("COVERSET %s -> %d ground actors" % (level, n))

unreal.log("COVERSET DONE")
