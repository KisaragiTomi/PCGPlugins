# -*- coding: utf-8 -*-
"""披挂岩壳（链 B）的资产收尾 + 演示关卡接线。

分成两半，都可重跑：

1. **把导进来的图案网格改成岩壳能用的样子**。`TinyGladeImportRockShell.py` 负责导入，
   但它在 UE 5.7 上跑到 `mesh.build()` 就抛了（`unreal.StaticMesh` 没有这个方法），
   于是 **build settings 之后的每一步都没执行、资产也没保存**。那几项是承重的：
   焊接会毁掉 `Tri*3+k` 的逐三角展开、半精度 UV 会毁掉 `cell_id`、不开 CPU 访问就读不到
   顶点缓冲。本脚本把这几项补上并保存 —— 没有改那个脚本（它是另一位 agent 的产物）。
   ⚠️ 顺带记一条实测：Interchange 的 glTF 管线把资产落在
   `.../rocky_terrain_shell/rocky_terrain_shell/StaticMeshes/`（名字出现**两次**），
   而那个脚本里的 `EXPECTED_ASSET` 只写了一层，所以它自己也 `load_asset` 失败过一次。

2. **演示关卡接线**：给地面填 `RockShellMaterial`。
   ⚠️ 这一条是今天那个教训的直接执行面 —— GPU 石阶的 `StairMesh`/`StairMaterial` 在两张
   演示关卡里一直是 NULL，石阶在画面里是一撮黑块，而单测 53/53、回归 55 条全绿。
   岩壳的 `IsRockShellDrawable()` 因此把"有没有材质"做成了显式判据，这里必须真的填上。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<本文件>" -unattended -nopause -nosplash -abslog=<日志>
"""

import unreal

PKG = "/PCGPlugins/HouseTest"
ASSET = "/Game/TinyGlade/Meshes/rocky_terrain_shell/rocky_terrain_shell/StaticMeshes/rocky_terrain_shell"
SHELL_MAT = "/Game/TinyGlade/MaterialInstances/MI_rocky_terrain"

# 原件实测（Docs/TinyGlade/CSRockShellPattern.md「首次导入后必须核对的四项」）
EXPECT_TRIANGLES = 49598
EXPECT_UV_CHANNELS = 3
EXPECT_MAX_CELL_ID = 608.0
EXPECT_THIN_AXIS_CM = 312.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
FAILS = []


def log(msg):
    unreal.log("ROCKSHELL %s" % msg)


def check(label, ok, detail=""):
    if ok:
        unreal.log("[PASS] %s %s" % (label, detail))
    else:
        FAILS.append(label)
        unreal.log_error("[FAIL] %s %s" % (label, detail))


def mesh_api():
    """UE 5.7 起 EditorStaticMeshLibrary 已弃用，改走 StaticMeshEditorSubsystem。"""
    try:
        sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        if sub:
            return sub
    except Exception:
        pass
    return unreal.EditorStaticMeshLibrary


API = mesh_api()


# =============================================================================
# 1) 资产收尾
# =============================================================================

def finalize_asset():
    mesh = unreal.EditorAssetLibrary.load_asset(ASSET)
    if not isinstance(mesh, unreal.StaticMesh):
        check("图案资产存在", False, "load_asset(%s) 拿不到 StaticMesh —— 先跑 TinyGladeImportRockShell.py" % ASSET)
        return None

    # 顺序是承重的：allow_cpu_access / Nanite 先设，再改 build settings ——
    # set_lod_build_settings 内部会 Modify + PostEditChange 触发一次完整重建，
    # 那一次重建必须已经看得到 allow_cpu_access，否则 CPU 侧副本还是不会被保留。
    mesh.set_editor_property("allow_cpu_access", True)
    try:
        nanite = mesh.get_editor_property("nanite_settings")
        nanite.set_editor_property("enabled", False)
        mesh.set_editor_property("nanite_settings", nanite)
    except Exception as exc:
        log("nanite settings skipped: %s" % exc)

    for lod in range(API.get_lod_count(mesh)):
        bs = API.get_lod_build_settings(mesh, lod)
        bs.set_editor_property("use_full_precision_u_vs", True)   # cell_id / dir 不能过 FP16
        bs.set_editor_property("generate_lightmap_u_vs", False)   # 会占掉一个 UV 通道并打乱编号
        bs.set_editor_property("remove_degenerates", False)       # 会静默改变三角数
        API.set_lod_build_settings(mesh, lod, bs)                 # 内部 PostEditChange ⇒ 重建
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)

    # ⚠️ 三角数不在这里查：`StaticMeshEditorSubsystem` 没有 `get_number_triangles`
    # （只有 `get_number_verts` / `get_num_uv_channels`），而**焊接后的顶点数本来就不等于
    # 三角 × 3**，拿它换算只会给出一个假的三角数。真正的判据在 verify_pattern() ——
    # C++ 侧按索引缓冲展开之后数出来的那个，正是 kernel 实际会跑的三角数。
    verts = API.get_number_verts(mesh, 0)
    uvs = API.get_num_uv_channels(mesh, 0)
    box = mesh.get_bounding_box()
    size = box.max - box.min
    thin = min(size.x, size.y, size.z)
    log("asset %s: %d 顶点（焊接后）/ %d UV 通道 / 包围盒 %.0f × %.0f × %.0f cm" %
        (ASSET, verts, uvs, size.x, size.y, size.z))
    check("图案 UV 通道 >= 3", uvs >= EXPECT_UV_CHANNELS, "uvs=%d" % uvs)
    check("图案最薄的一轴 = 312 cm（×65 没有被乘第二次）",
          abs(thin - EXPECT_THIN_AXIS_CM) < 20.0, "thin=%.1f cm" % thin)
    check("图案 CPU 访问已开", bool(mesh.get_editor_property("allow_cpu_access")))
    return mesh


# =============================================================================
# 2) 演示关卡接线 + 逐项核对（UV 值域与绕序只有 C++ 侧读得到逐顶点数据）
# =============================================================================

def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def wire_level(level, shell_mat):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    ground = find("Ground_Demo")
    if not ground:
        check("%s 有 Ground_Demo" % level, False)
        return None
    ground.set_editor_property("bRockShell", True)
    ground.set_editor_property("RockShellMaterial", shell_mat)
    ground.call_method("RebuildGroundMesh")   # 内部会重扫石阶并重披挂岩壳
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    log("%s: RockShellMaterial = %s" % (level, shell_mat.get_name() if shell_mat else None))
    return ground


def verify_pattern(ground):
    """C++ 侧抽取的实测结果 —— 逐顶点 UV 值域与绕序在 Python 里读不到。"""
    # UE Python 把「返回值 + 全部 out 参数」打成一个元组，但**元组长度不总是 1 + N**：
    # 实测这个 const UFUNCTION 只回了 5 项（返回值被并掉）。写死下标会在别的引擎版本上
    # 静默错位（把 uvs 当成 tris 之类），所以按长度分支。
    vals = list(ground.get_rock_shell_pattern_stats())
    log("pattern stats raw: %r" % (vals,))
    if len(vals) == 6:
        ok, tris, uvs, max_cell, flip, dir_agree = vals
    elif len(vals) == 5:
        tris, uvs, max_cell, flip, dir_agree = vals
        ok = tris > 0
    else:
        check("get_rock_shell_pattern_stats 的返回形状可识别", False, repr(vals))
        return
    log("pattern stats: ok=%s tris=%d uvs=%d maxCellId=%.1f flipWinding=%s dirAgreement=%.4f"
        % (ok, tris, uvs, max_cell, flip, dir_agree))
    check("C++ 侧抽得出图案", ok)
    check("展开后三角数 = 49,598", tris == EXPECT_TRIANGLES, "tris=%d" % tris)
    # UV1.x 必须是 0..608 而不是 0..1：有的导入路径会把 UV 归一化，一归一化 cell_id 就废了，
    # 而且完全静默 —— 全场只剩一个胞腔，壳看起来"没有块感"，没有任何报错。
    check("UV1.x 没有被归一化（CellId 上界 ≈ 608）",
          abs(max_cell - EXPECT_MAX_CELL_ID) < 1.0, "maxCellId=%.2f" % max_cell)
    check("现算的 DirToCentroid 与烘焙件一致（导入器没翻平面轴）",
          dir_agree > 0.9, "dirAgreement=%.4f" % dir_agree)
    log("绕序实测结果：%s（kernel 会按这个取符号，不需要人工确认）"
        % ("盖三角朝下 ⇒ 取负" if flip else "盖三角朝上 ⇒ 直接用"))

    # ⚠️ 坑 ⑩：一律调 `get_rock_shell_undrawable_reason()`，**不要** `is_rock_shell_drawable()` ——
    # 后者不可画时返回 `None`，`str(None) == "None"` 会伪造出一句像模像样的原因。
    # **空串 = 可画**（C++ 侧只在失败时才写原因）。⚠️ 别写成 `bool(返回值)` ——
    # 空串是 False，判据正好反过来，而"报 FAIL 但详情是空的"看起来像别的 bug（实测踩过一次）。
    why = str(ground.get_rock_shell_undrawable_reason())
    drawable = (why == "")
    check("岩壳会被画出来（组件/网格/流/材质逐环检查）", drawable, why)


def main():
    mesh = finalize_asset()
    shell_mat = unreal.EditorAssetLibrary.load_asset(SHELL_MAT)
    check("岩壳材质存在", shell_mat is not None, SHELL_MAT)

    ground = wire_level("L_TerrainOpsDemo", shell_mat)
    wire_level("L_HouseGroundDemo", shell_mat)

    if ground:
        # 回到地形演示关卡再核对（wire_level 之后关卡已经切走了）
        unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
        ground = find("Ground_Demo")
        if ground:
            verify_pattern(ground)

    unreal.log("========== ROCKSHELL SETUP SUMMARY ==========")
    for f in FAILS:
        unreal.log_error("  failed: %s" % f)
    unreal.log("ROCKSHELL SETUP FAILED" if FAILS else "ROCKSHELL SETUP OK")


try:
    main()
except Exception:
    # 准备阶段抛异常 ⇒ -ExecCmds 那条 Quit 再也执行不到，任务看着像挂死（踩过的坑）。
    import traceback
    unreal.log_error("ROCKSHELL SETUP EXCEPTION:\n%s" % traceback.format_exc())
    unreal.log("ROCKSHELL SETUP FAILED")
