# -*- coding: utf-8 -*-
"""Nanite 截面 HLOD 的测试内容：一批 Nanite 测试网格、配色材质、子蓝图 BP_NaniteCutHLOD、测试关卡 L_NaniteCutHLODTest。

全部放在插件 Content 下的新文件夹 /PCGPlugins/NaniteCutHLOD/：

    Meshes/SM_NCH_*        Geometry Script 生成、开 Nanite 的测试网格（带噪声起伏，几千到上万三角，层级有得走）
                           另有两个给可见性剔除用的：SM_NCH_SerpentCave（实心岩块里挖一条 S 形隧道，两头开口）、
                           SM_NCH_House（有地板和屋顶的房子，前屋开门窗、后屋完全封闭）
    Materials/MI_NCH_*     BasicShapeMaterial 的配色实例
    BP_NaniteCutHLOD       ACSNaniteCutHLODActor 的子蓝图
    L_NaniteCutHLODTest    Template_Default 起步：一群物体 + 几个带排除标签的红色物体 + 一个 BP 实例，
                           外加山洞（洞口、拐弯、深处各放物体）、房子（前屋、后屋各放物体）、一堆互相穿插的石头

外面绝对看不见的：隧道中段的物体、后屋里的物体与后屋内壁、石头堆互相穿插的内部面 —— HLOD 应当把它们删光；
洞口与前屋里的物体从外面看得见（透过洞口 / 门窗），必须留着。

另有三种验证烘焙（BakeHLOD）的材质，按格子轮流分给物体：
    M_NCH_WorldChecker   WorldPosition 驱动的棋盘格 + 高度渐变，粗糙度随格子变（房子也用它）
    M_NCH_ObjectTint     ObjectPosition 驱动的逐物体颜色 —— 合在一起烘就全错，验证逐源喂图元数据（石头堆用它）
    M_NCH_GridNormal     引擎 Grid_M 遮罩混两色 + Grid_N 法线贴图，UV 平铺两次（山洞用它）

可重跑：已有资产复用并覆盖参数；关卡存在就 load，并按 label 前缀 NCH_ 清掉上一轮脚本放的 actor。
要重新生成某个网格：环境变量 NCH_REBUILD=SM_NCH_SerpentCave,SM_NCH_House（逗号分隔）。
用法（编辑器会在脚本跑完后退出）::

    UnrealEditor-Cmd.exe <project> -ExecutePythonScript="<本文件>" -unattended -nosplash -stdout -AbsLog=<log>

日志里 `NCH SETUP` 开头的行是进度，最后一行 `NCH SETUP DONE` / `NCH SETUP FAILED`。
"""
import math
import os
import random
import traceback

import unreal

ROOT = "/PCGPlugins/NaniteCutHLOD"
MESH_DIR = ROOT + "/Meshes"
MAT_DIR = ROOT + "/Materials"
LEVEL = ROOT + "/L_NaniteCutHLODTest"
BP_PATH = ROOT + "/BP_NaniteCutHLOD"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
BASIC_MATERIAL = "/Engine/BasicShapes/BasicShapeMaterial"
EXCLUDE_TAG = "NaniteCutHLOD_Exclude"   # 与 UCSNaniteCutOps::DefaultExcludeTag() 一致
LABEL_PREFIX = "NCH_"

CLUSTER_CENTER = unreal.Vector(0.0, 0.0, 0.0)
GRID = 6                 # 6×6 个格子
SPACING = 650.0          # 格距（cm）→ 整群约 39 m 见方
SWITCH_DISTANCE = 8000.0 # BP 实例的切换距离（cm）：1 像素 ≈ 8.3 cm

# 可见性剔除的测试件，摆在格子群外面一圈（相对 CLUSTER_CENTER）。
CAVE_OFFSET = (0.0, 3700.0)          # 岩块 30 × 18 × 9 m，隧道沿 x 走
CAVE_SIZE = (3000.0, 1800.0, 900.0)
TUNNEL_RADIUS = 200.0
TUNNEL_Z = 300.0                     # 隧道中心离岩块底面的高度
TUNNEL_AMPLITUDE = 500.0             # y = A·sin(W·πx / 1500)：两头在岩块端面正中
TUNNEL_WAVES = 2.0                   # 两个整波 = 四个弯：中段 ±600 以内与每个洞口之间至少隔两道弯，直线视线过不去
HOUSE_OFFSET = (0.0, -3600.0)        # 12 × 8 × 4.5 m，墙 / 地板 / 屋顶厚 25 cm，x = 125..175 是隔墙
PILE_OFFSET = (-3500.0, 0.0)         # 七块互相穿插的石头
REBUILD = {n.strip() for n in os.environ.get("NCH_REBUILD", "").split(",") if n.strip()}

ASSETS = unreal.AssetToolsHelpers.get_asset_tools()
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
SM_EDITOR = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
GS = unreal  # Geometry Script 的函数库都挂在 unreal 模块下

COLORS = {
    "Sand": unreal.LinearColor(0.72, 0.60, 0.42, 1.0),
    "Stone": unreal.LinearColor(0.42, 0.43, 0.45, 1.0),
    "Moss": unreal.LinearColor(0.30, 0.45, 0.22, 1.0),
    "Terracotta": unreal.LinearColor(0.70, 0.36, 0.22, 1.0),
    "Slate": unreal.LinearColor(0.28, 0.36, 0.48, 1.0),
    "ExcludedRed": unreal.LinearColor(0.85, 0.08, 0.08, 1.0),
    "Cave": unreal.LinearColor(0.36, 0.31, 0.27, 1.0),
    "Plaster": unreal.LinearColor(0.82, 0.76, 0.64, 1.0),
}


def log(msg):
    unreal.log("NCH SETUP " + msg)


# ---------------------------------------------------------------------------- 材质

def make_material_instances():
    parent = unreal.load_asset(BASIC_MATERIAL)
    if parent is None:
        raise RuntimeError("找不到 " + BASIC_MATERIAL)
    names = [str(n) for n in unreal.MaterialEditingLibrary.get_vector_parameter_names(parent)]
    color_param = "Color" if "Color" in names else (names[0] if names else None)
    log("BasicShapeMaterial vector params = %s, using %s" % (names, color_param))

    out = {}
    for name, color in COLORS.items():
        path = "%s/MI_NCH_%s" % (MAT_DIR, name)
        mic = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
        if mic is None:
            mic = ASSETS.create_asset("MI_NCH_" + name, MAT_DIR, unreal.MaterialInstanceConstant,
                                      unreal.MaterialInstanceConstantFactoryNew())
        unreal.MaterialEditingLibrary.set_material_instance_parent(mic, parent)
        if color_param:
            unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(mic, color_param, color)
        unreal.MaterialEditingLibrary.update_material_instance(mic)
        out[name] = mic
    return out


# ---------------------------------------------------------------------------- 验证烘焙的测试材质

MEL = unreal.MaterialEditingLibrary


def expr(mat, cls, x, y, **props):
    node = MEL.create_material_expression(mat, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def link(src, dst, dst_input="", src_output=""):
    if not MEL.connect_material_expressions(src, src_output, dst, dst_input):
        raise RuntimeError("连线失败 %s -> %s.%s" % (src.get_name(), dst.get_name(), dst_input))


def sampler_type_for(texture):
    comp = texture.get_editor_property("compression_settings")
    if comp == unreal.TextureCompressionSettings.TC_NORMALMAP:
        return unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL
    if comp == unreal.TextureCompressionSettings.TC_MASKS:
        return unreal.MaterialSamplerType.SAMPLERTYPE_MASKS
    if comp == unreal.TextureCompressionSettings.TC_GRAYSCALE:
        return (unreal.MaterialSamplerType.SAMPLERTYPE_GRAYSCALE if texture.get_editor_property("srgb")
                else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    return (unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if texture.get_editor_property("srgb")
            else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)


def fresh_material(name):
    path = "%s/%s" % (MAT_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.load_asset(path)
        MEL.delete_all_material_expressions(mat)
    else:
        mat = ASSETS.create_asset(name, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    return mat


def build_world_checker():
    mat = fresh_material("M_NCH_WorldChecker")
    wp = expr(mat, unreal.MaterialExpressionWorldPosition, -1400, 0)
    cell = expr(mat, unreal.MaterialExpressionDivide, -1200, 0, const_b=150.0)
    link(wp, cell, "A")
    fl = expr(mat, unreal.MaterialExpressionFloor, -1050, 0)
    link(cell, fl)
    mr = expr(mat, unreal.MaterialExpressionComponentMask, -900, -60, r=True, g=False, b=False, a=False)
    mg = expr(mat, unreal.MaterialExpressionComponentMask, -900, 60, r=False, g=True, b=False, a=False)
    link(fl, mr)
    link(fl, mg)
    add = expr(mat, unreal.MaterialExpressionAdd, -750, 0)
    link(mr, add, "A")
    link(mg, add, "B")
    half = expr(mat, unreal.MaterialExpressionMultiply, -600, 0, const_b=0.5)
    link(add, half, "A")
    fr = expr(mat, unreal.MaterialExpressionFrac, -450, 0)
    link(half, fr)
    checker = expr(mat, unreal.MaterialExpressionMultiply, -300, 0, const_b=2.0)
    link(fr, checker, "A")
    ca = expr(mat, unreal.MaterialExpressionConstant3Vector, -300, -220, constant=unreal.LinearColor(0.78, 0.62, 0.36, 1.0))
    cb = expr(mat, unreal.MaterialExpressionConstant3Vector, -300, -120, constant=unreal.LinearColor(0.20, 0.34, 0.52, 1.0))
    color = expr(mat, unreal.MaterialExpressionLinearInterpolate, -100, -150)
    link(ca, color, "A")
    link(cb, color, "B")
    link(checker, color, "Alpha")
    # 高度渐变：离地越高越亮
    mz = expr(mat, unreal.MaterialExpressionComponentMask, -900, 260, r=False, g=False, b=True, a=False)
    link(wp, mz)
    zdiv = expr(mat, unreal.MaterialExpressionDivide, -750, 260, const_b=900.0)
    link(mz, zdiv, "A")
    zsat = expr(mat, unreal.MaterialExpressionSaturate, -600, 260)
    link(zdiv, zsat)
    zl = expr(mat, unreal.MaterialExpressionLinearInterpolate, -450, 260, const_a=0.45, const_b=1.0)
    link(zsat, zl, "Alpha")
    final = expr(mat, unreal.MaterialExpressionMultiply, 100, -100)
    link(color, final, "A")
    link(zl, final, "B")
    MEL.connect_material_property(final, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = expr(mat, unreal.MaterialExpressionLinearInterpolate, 100, 150, const_a=0.25, const_b=0.85)
    link(checker, rough, "Alpha")
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    return mat


def build_object_tint():
    mat = fresh_material("M_NCH_ObjectTint")
    op = expr(mat, unreal.MaterialExpressionObjectPositionWS, -900, 0)
    scale = expr(mat, unreal.MaterialExpressionMultiply, -700, 0, const_b=0.00173)
    link(op, scale, "A")
    fr = expr(mat, unreal.MaterialExpressionFrac, -550, 0)
    link(scale, fr)
    span = expr(mat, unreal.MaterialExpressionMultiply, -400, 0, const_b=0.7)
    link(fr, span, "A")
    bias = expr(mat, unreal.MaterialExpressionAdd, -250, 0, const_b=0.2)
    link(span, bias, "A")
    MEL.connect_material_property(bias, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = expr(mat, unreal.MaterialExpressionConstant, -250, 200, r=0.55)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    return mat


def build_grid_normal():
    mat = fresh_material("M_NCH_GridNormal")
    grid_m = unreal.load_asset("/Engine/EngineMaterials/T_Default_Material_Grid_M")
    grid_n = unreal.load_asset("/Engine/EngineMaterials/T_Default_Material_Grid_N")
    if grid_m is None or grid_n is None:
        raise RuntimeError("找不到引擎的 Grid_M / Grid_N")
    uv = expr(mat, unreal.MaterialExpressionTextureCoordinate, -900, 0, coordinate_index=0, u_tiling=2.0, v_tiling=2.0)
    mask = expr(mat, unreal.MaterialExpressionTextureSample, -650, -150, texture=grid_m, sampler_type=sampler_type_for(grid_m))
    link(uv, mask, "UVs")
    normal = expr(mat, unreal.MaterialExpressionTextureSample, -650, 200, texture=grid_n, sampler_type=sampler_type_for(grid_n))
    link(uv, normal, "UVs")
    ca = expr(mat, unreal.MaterialExpressionConstant3Vector, -400, -300, constant=unreal.LinearColor(0.55, 0.50, 0.44, 1.0))
    cb = expr(mat, unreal.MaterialExpressionConstant3Vector, -400, -200, constant=unreal.LinearColor(0.16, 0.30, 0.22, 1.0))
    color = expr(mat, unreal.MaterialExpressionLinearInterpolate, -150, -200)
    link(ca, color, "A")
    link(cb, color, "B")
    link(mask, color, "Alpha", "R")
    MEL.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = expr(mat, unreal.MaterialExpressionLinearInterpolate, -150, 0, const_a=0.85, const_b=0.3)
    link(mask, rough, "Alpha", "R")
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
    return mat


def make_test_materials():
    out = {}
    for name, builder in (("WorldChecker", build_world_checker), ("ObjectTint", build_object_tint), ("GridNormal", build_grid_normal)):
        mat = builder()
        MEL.recompile_material(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)
        out[name] = mat
    log("test materials: %s" % ", ".join(sorted(out)))
    return out


# ---------------------------------------------------------------------------- 网格

def new_dynamic_mesh():
    return unreal.new_object(unreal.DynamicMesh)


def perlin(mesh, magnitude, frequency, seed):
    opts = unreal.GeometryScriptPerlinNoiseOptions()
    layer = opts.get_editor_property("base_layer")
    layer.set_editor_property("magnitude", magnitude)
    layer.set_editor_property("frequency", frequency)
    layer.set_editor_property("random_seed", seed)
    opts.set_editor_property("base_layer", layer)
    opts.set_editor_property("apply_along_normal", True)
    unreal.GeometryScript_MeshDeformers.apply_perlin_noise_to_mesh2(mesh, unreal.GeometryScriptMeshSelection(), opts)


def recompute_normals(mesh):
    unreal.GeometryScript_Normals.recompute_normals(mesh, unreal.GeometryScriptCalculateNormalsOptions())


def build_rock(mesh):
    unreal.GeometryScript_Primitives.append_sphere_lat_long(
        mesh, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(), 150.0, 48, 96)
    perlin(mesh, 38.0, 0.012, 11)


def build_boulder(mesh):
    unreal.GeometryScript_Primitives.append_box(
        mesh, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(), 320.0, 240.0, 190.0, 28, 22, 18)
    perlin(mesh, 26.0, 0.015, 23)


def build_pillar(mesh):
    unreal.GeometryScript_Primitives.append_cylinder(
        mesh, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(), 60.0, 600.0, 48, 60, True)
    perlin(mesh, 9.0, 0.03, 37)


def build_torus(mesh):
    revolve = unreal.GeometryScriptRevolveOptions()
    unreal.GeometryScript_Primitives.append_torus(
        mesh, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(), revolve, 160.0, 45.0, 96, 48)
    perlin(mesh, 7.0, 0.035, 41)


def build_spire(mesh):
    unreal.GeometryScript_Primitives.append_cone(
        mesh, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(), 130.0, 10.0, 460.0, 64, 40, True)
    perlin(mesh, 12.0, 0.02, 53)


MESHES = [
    ("SM_NCH_Rock", build_rock, "Sand"),
    ("SM_NCH_Boulder", build_boulder, "Stone"),
    ("SM_NCH_Pillar", build_pillar, "Terracotta"),
    ("SM_NCH_Torus", build_torus, "Slate"),
    ("SM_NCH_Spire", build_spire, "Moss"),
]


def split_normals(mesh):
    """房子这种硬边要分裂法线；拿不到分裂接口就退回平滑法线。"""
    try:
        split = unreal.GeometryScriptSplitNormalsOptions()
        split.set_editor_property("split_by_opening_angle", True)
        split.set_editor_property("opening_angle_deg", 40.0)
        unreal.GeometryScript_Normals.compute_split_normals(mesh, split, unreal.GeometryScriptCalculateNormalsOptions())
    except Exception as exc:
        log("split normals unavailable (%s), smooth normals instead" % exc)
        recompute_normals(mesh)


def mesh_volume(mesh):
    result = unreal.GeometryScript_MeshQueries.get_mesh_volume_area(mesh)
    return float(result[-1]) if isinstance(result, (tuple, list)) else 0.0


def subtract(mesh, tool):
    opts = unreal.GeometryScriptMeshBooleanOptions()
    unreal.GeometryScript_MeshBooleans.apply_mesh_boolean(
        mesh, unreal.Transform(), tool, unreal.Transform(), unreal.GeometryScriptBooleanOperation.SUBTRACT, opts)


def tunnel_path_y(x):
    return TUNNEL_AMPLITUDE * math.sin(TUNNEL_WAVES * math.pi * x / (CAVE_SIZE[0] * 0.5))


def tunnel_tool(reverse=False):
    """圆截面沿 S 形路径扫掠出的实心管，两头伸出岩块端面。"""
    circle = [unreal.Vector2D(TUNNEL_RADIUS * math.cos(2.0 * math.pi * k / 24), TUNNEL_RADIUS * math.sin(2.0 * math.pi * k / 24))
              for k in range(24)]
    if reverse:
        circle.reverse()
    half = CAVE_SIZE[0] * 0.5
    path = []
    samples = 192
    for i in range(samples + 1):
        x = -(half + 350.0) + (2.0 * half + 700.0) * i / samples
        dydx = TUNNEL_AMPLITUDE * TUNNEL_WAVES * math.pi / half * math.cos(TUNNEL_WAVES * math.pi * x / half)
        path.append(unreal.Transform(location=unreal.Vector(x, tunnel_path_y(x), TUNNEL_Z),
                                     rotation=unreal.Rotator(roll=0.0, pitch=0.0, yaw=math.degrees(math.atan2(dydx, 1.0))),
                                     scale=unreal.Vector(1.0, 1.0, 1.0)))
    tool = new_dynamic_mesh()
    unreal.GeometryScript_Primitives.append_sweep_polygon(
        tool, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(), circle, path, False, True, 1.0, 1.0, 0.0, 1.0)
    return tool


def build_cave(mesh):
    sx, sy, sz = CAVE_SIZE
    unreal.GeometryScript_Primitives.append_box(mesh, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(), sx, sy, sz, 60, 36, 18)
    perlin(mesh, 45.0, 0.003, 61)
    tool = tunnel_tool()
    volume = mesh_volume(tool)
    if volume < 0.0:
        log("tunnel tool came out inside-out (volume %.0f), reversing the profile" % volume)
        tool = tunnel_tool(reverse=True)
    before = unreal.GeometryScript_MeshQueries.get_num_triangle_i_ds(mesh)
    subtract(mesh, tool)
    log("cave: block %d triangles -> %d after carving the tunnel (tool volume %.0f cm3)" % (
        before, unreal.GeometryScript_MeshQueries.get_num_triangle_i_ds(mesh), mesh_volume(tool)))


def box_tool(cx, cy, z0, sx, sy, sz):
    tool = new_dynamic_mesh()
    unreal.GeometryScript_Primitives.append_box(tool, unreal.GeometryScriptPrimitiveOptions(),
                                                unreal.Transform(location=unreal.Vector(cx, cy, z0)), sx, sy, sz, 1, 1, 1)
    return tool


def build_house(mesh):
    # 外盒 12 × 8 × 4.5 m（原点在底面中心），挖两个房间：前屋 x = -575..125，后屋 x = 175..575，中间 50 cm 隔墙。
    unreal.GeometryScript_Primitives.append_box(mesh, unreal.GeometryScriptPrimitiveOptions(), unreal.Transform(),
                                                1200.0, 800.0, 450.0, 24, 16, 9)
    subtract(mesh, box_tool(-225.0, 0.0, 25.0, 700.0, 750.0, 400.0))      # 前屋
    subtract(mesh, box_tool(375.0, 0.0, 25.0, 400.0, 750.0, 400.0))       # 后屋：没有任何开口
    subtract(mesh, box_tool(-600.0, 0.0, 25.0, 100.0, 120.0, 230.0))      # 门（-X 墙）
    subtract(mesh, box_tool(-350.0, -400.0, 150.0, 150.0, 100.0, 120.0))  # 窗（-Y 墙）
    subtract(mesh, box_tool(-150.0, 400.0, 150.0, 150.0, 100.0, 120.0))   # 窗（+Y 墙）
    log("house: %d triangles" % unreal.GeometryScript_MeshQueries.get_num_triangle_i_ds(mesh))


SPECIAL_MESHES = [
    ("SM_NCH_SerpentCave", build_cave, "Cave"),
    ("SM_NCH_House", build_house, "Plaster"),
]


def make_meshes(materials):
    """已有的网格原样复用（要重新生成就先删掉那个资产）；没有的用 Geometry Script 生成、开 Nanite 建成资产。"""
    out = {}
    for name, builder, color in MESHES + SPECIAL_MESHES:
        path = "%s/%s" % (MESH_DIR, name)
        if name in REBUILD and unreal.EditorAssetLibrary.does_asset_exist(path):
            unreal.EditorAssetLibrary.delete_asset(path)
            log("%s: deleted for rebuild" % name)
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            sm = unreal.load_asset(path)
            log("%s: reuse existing asset" % name)
        else:
            mesh = new_dynamic_mesh()
            builder(mesh)
            if builder is build_house:
                split_normals(mesh)
            else:
                recompute_normals(mesh)
            tris = unreal.GeometryScript_MeshQueries.get_num_triangle_i_ds(mesh)
            opts = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
            opts.set_editor_property("enable_recompute_normals", False)
            opts.set_editor_property("enable_recompute_tangents", True)
            opts.set_editor_property("enable_nanite", True)
            opts.set_editor_property("enable_collision", False)
            sm, outcome = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(mesh, path, opts)
            log("%s: generated %d triangles (outcome %s)" % (name, tris, outcome))
        if sm is None:
            raise RuntimeError("建不出 " + path)

        # Nanite 数据在构建里生成：没开的显式打开并重建。
        settings = SM_EDITOR.get_nanite_settings(sm)
        if not settings.get_editor_property("enabled"):
            settings.set_editor_property("enabled", True)
            SM_EDITOR.set_nanite_settings(sm, settings, apply_changes=True)
        sm.set_material(0, materials[color])
        unreal.EditorAssetLibrary.save_loaded_asset(sm)
        log("%s: nanite=%s" % (name, SM_EDITOR.get_nanite_settings(sm).get_editor_property("enabled")))
        out[name] = sm
    return out


# ---------------------------------------------------------------------------- 蓝图

def make_blueprint():
    if unreal.EditorAssetLibrary.does_asset_exist(BP_PATH):
        bp = unreal.load_asset(BP_PATH)
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.CSNaniteCutHLODActor)
        bp = ASSETS.create_asset(BP_PATH.rsplit("/", 1)[1], ROOT, unreal.Blueprint, factory)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    cdo = unreal.get_default_object(bp.generated_class())
    if not isinstance(cdo, unreal.CSNaniteCutHLODActor):
        raise RuntimeError("%s 的父类不是 CSNaniteCutHLODActor" % BP_PATH)
    cdo.set_editor_property("switch_distance", SWITCH_DISTANCE)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    log("blueprint %s ok (parent CSNaniteCutHLODActor)" % BP_PATH)
    return bp


# ---------------------------------------------------------------------------- 关卡

def open_level():
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        LEVELS.load_level(LEVEL)
        stale = [a for a in ACTORS.get_all_level_actors() if a.get_actor_label().startswith(LABEL_PREFIX)]
        for a in stale:
            ACTORS.destroy_actor(a)
        log("loaded existing level, removed %d old actors" % len(stale))
    else:
        if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
            raise RuntimeError("new_level_from_template 失败")
        log("created level from " + TEMPLATE)


def prepare_floor():
    # 模板地板：放大到整群之下，打上排除标签（它本来也不是 Nanite，打标签是为了演示排除规则也管它）。
    floors = [a for a in ACTORS.get_all_level_actors()
              if isinstance(a, unreal.StaticMeshActor) and "floor" in a.get_actor_label().lower()]
    for floor in floors:
        comp = floor.static_mesh_component
        box = comp.static_mesh.get_bounding_box()
        size_x = max(box.max.x - box.min.x, 1.0)
        size_y = max(box.max.y - box.min.y, 1.0)
        scale = floor.get_actor_scale3d()
        floor.set_actor_scale3d(unreal.Vector(16000.0 / size_x, 16000.0 / size_y, scale.z))
        floor.set_actor_location(unreal.Vector(CLUSTER_CENTER.x, CLUSTER_CENTER.y, floor.get_actor_location().z), False, False)
        tags = list(floor.get_editor_property("tags"))
        if EXCLUDE_TAG not in [str(t) for t in tags]:
            tags.append(unreal.Name(EXCLUDE_TAG))
            floor.set_editor_property("tags", tags)
        log("floor %s scaled to 160 m, tagged %s" % (floor.get_actor_label(), EXCLUDE_TAG))
    return floors


def floor_top_z(floors):
    if not floors:
        return 0.0
    origin, extent = floors[0].get_actor_bounds(False)
    return origin.z + extent.z


def place_objects(meshes, materials, ground_z):
    rng = random.Random(20260922)
    names = [m[0] for m in MESHES]
    placed = []
    excluded_cells = {(1, 4), (3, 1), (4, 3), (5, 5)}
    mirrored_cells = {(0, 2), (2, 5), (5, 0)}
    half = (GRID - 1) * 0.5
    for gx in range(GRID):
        for gy in range(GRID):
            name = names[rng.randrange(len(names))]
            sm = meshes[name]
            s = rng.uniform(0.75, 1.6)
            scale = unreal.Vector(s * rng.uniform(0.85, 1.15), s * rng.uniform(0.85, 1.15), s * rng.uniform(0.85, 1.25))
            if (gx, gy) in mirrored_cells:
                scale.x = -scale.x
            yaw = rng.uniform(0.0, 360.0)
            x = CLUSTER_CENTER.x + (gx - half) * SPACING + rng.uniform(-120.0, 120.0)
            y = CLUSTER_CENTER.y + (gy - half) * SPACING + rng.uniform(-120.0, 120.0)
            box = sm.get_bounding_box()
            z = ground_z - box.min.z * abs(scale.z) - 5.0   # 底面略压进地面
            actor = ACTORS.spawn_actor_from_object(sm, unreal.Vector(x, y, z), unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw))
            actor.set_actor_scale3d(scale)
            comp = actor.static_mesh_component
            excluded = (gx, gy) in excluded_cells
            if excluded:
                comp.set_material(0, materials["ExcludedRed"])
                actor.set_editor_property("tags", [unreal.Name(EXCLUDE_TAG)])
            else:
                # 四种轮流：网格自带的纯色 / 世界棋盘格 / 按物体上色 / 贴图 + 法线
                variant = (gx * GRID + gy) % 4
                if variant and TEST_MATERIALS:
                    comp.set_material(0, TEST_MATERIALS[("WorldChecker", "ObjectTint", "GridNormal")[variant - 1]])
            label = "%sObj_%d_%d%s" % (LABEL_PREFIX, gx, gy, "_Excluded" if excluded else "")
            actor.set_actor_label(label)
            actor.set_folder_path("NaniteCutHLOD/Sources")
            placed.append(actor)
    log("placed %d objects (%d excluded, %d mirrored)" % (len(placed), len(excluded_cells), len(mirrored_cells)))
    return placed


def spawn(sm, x, y, z, yaw=0.0, scale=1.0, label="", folder="NaniteCutHLOD/Sources"):
    actor = ACTORS.spawn_actor_from_object(sm, unreal.Vector(x, y, z), unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw))
    actor.set_actor_scale3d(scale if isinstance(scale, unreal.Vector) else unreal.Vector(scale, scale, scale))
    actor.set_actor_label(LABEL_PREFIX + label)
    actor.set_folder_path(folder)
    return actor


def seat_z(sm, ground, scale):
    """让网格底面落在 ground 上的 actor z。"""
    return ground - sm.get_bounding_box().min.z * scale


def place_special(meshes, ground_z):
    placed = []
    cx, cy = CLUSTER_CENTER.x, CLUSTER_CENTER.y

    # 山洞：岩块略压进地面；隧道底在岩块底面上 TUNNEL_Z - TUNNEL_RADIUS。
    ox, oy = cx + CAVE_OFFSET[0], cy + CAVE_OFFSET[1]
    placed.append(spawn(meshes["SM_NCH_SerpentCave"], ox, oy, ground_z - 20.0, label="Obj_Cave", folder="NaniteCutHLOD/Cave"))
    if TEST_MATERIALS:
        placed[-1].static_mesh_component.set_material(0, TEST_MATERIALS["GridNormal"])
    tunnel_floor = ground_z - 20.0 + TUNNEL_Z - TUNNEL_RADIUS
    cave_items = [   # (名字, 沿隧道的 x, 网格, 缩放) —— 洞口两个看得见，第一道弯里两个看不准，中段三个外面绝对看不见
        ("CaveMouthW", -1380.0, "SM_NCH_Spire", 0.45),
        ("CaveBendW", -1125.0, "SM_NCH_Rock", 0.55),
        ("CaveDeep1", -375.0, "SM_NCH_Torus", 0.45),
        ("CaveDeep2", 0.0, "SM_NCH_Spire", 0.45),
        ("CaveDeep3", 375.0, "SM_NCH_Rock", 0.55),
        ("CaveBendE", 1125.0, "SM_NCH_Pillar", 0.3),
        ("CaveMouthE", 1380.0, "SM_NCH_Rock", 0.55),
    ]
    for name, x, mesh_name, scale in cave_items:
        sm = meshes[mesh_name]
        placed.append(spawn(sm, ox + x, oy + tunnel_path_y(x), seat_z(sm, tunnel_floor + 25.0, scale), yaw=37.0 * len(placed),
                            scale=scale, label="Obj_" + name, folder="NaniteCutHLOD/Cave"))

    # 房子：前屋有门窗，后屋封死。
    hx, hy = cx + HOUSE_OFFSET[0], cy + HOUSE_OFFSET[1]
    placed.append(spawn(meshes["SM_NCH_House"], hx, hy, ground_z - 2.0, label="Obj_House", folder="NaniteCutHLOD/House"))
    if TEST_MATERIALS:
        placed[-1].static_mesh_component.set_material(0, TEST_MATERIALS["WorldChecker"])
    room_floor = ground_z - 2.0 + 25.0
    house_items = [  # (名字, 相对房子的 x, y, 网格, 缩放)
        ("HouseFrontPillar", -470.0, 230.0, "SM_NCH_Pillar", 0.5),
        ("HouseFrontRock", -120.0, -200.0, "SM_NCH_Rock", 0.6),
        ("HouseBackRock", 300.0, 180.0, "SM_NCH_Rock", 0.6),
        ("HouseBackTorus", 470.0, -160.0, "SM_NCH_Torus", 0.55),
        ("HouseBackSpire", 330.0, -230.0, "SM_NCH_Spire", 0.55),
    ]
    for name, x, y, mesh_name, scale in house_items:
        sm = meshes[mesh_name]
        placed.append(spawn(sm, hx + x, hy + y, seat_z(sm, room_floor, scale), yaw=23.0 * len(placed),
                            scale=scale, label="Obj_" + name, folder="NaniteCutHLOD/House"))

    # 石头堆：七块挨得很近、大小不一，彼此深深穿插。
    px, py = cx + PILE_OFFSET[0], cy + PILE_OFFSET[1]
    rng = random.Random(7)
    for i in range(7):
        name = "SM_NCH_Rock" if i % 2 == 0 else "SM_NCH_Boulder"
        sm = meshes[name]
        scale = rng.uniform(1.1, 1.6)
        x, y = px + rng.uniform(-220.0, 220.0), py + rng.uniform(-220.0, 220.0)
        z = seat_z(sm, ground_z, scale) - rng.uniform(0.0, 60.0) + (80.0 if i >= 5 else 0.0)
        placed.append(spawn(sm, x, y, z, yaw=rng.uniform(0.0, 360.0), scale=scale, label="Obj_Pile_%d" % i, folder="NaniteCutHLOD/Pile"))
        if TEST_MATERIALS:
            placed[-1].static_mesh_component.set_material(0, TEST_MATERIALS["ObjectTint"])
    log("placed %d special objects (cave + %d items, house + %d items, pile 7)" % (len(placed), len(cave_items), len(house_items)))
    return placed


def place_hlod_actor(bp, ground_z):
    # 盒子要把格子群、山洞、房子、石头堆都框进去（按组件包围盒中心判断）。
    half_x = max(GRID * SPACING * 0.5, abs(PILE_OFFSET[0]) + 600.0, CAVE_SIZE[0] * 0.5) + 400.0
    half_y = max(GRID * SPACING * 0.5, abs(CAVE_OFFSET[1]) + CAVE_SIZE[1] * 0.5, abs(HOUSE_OFFSET[1]) + 400.0) + 400.0
    extent = unreal.Vector(half_x, half_y, 1500.0)
    loc = unreal.Vector(CLUSTER_CENTER.x, CLUSTER_CENTER.y, ground_z + 600.0)
    actor = ACTORS.spawn_actor_from_class(bp.generated_class(), loc)
    actor.set_actor_label(LABEL_PREFIX + "HLOD")
    actor.set_folder_path("NaniteCutHLOD")
    # 新建、同一会话里刚编译的 BP，CDO 上写的默认值不会传播到这次 spawn 的实例 —— 实例上再写一份。
    actor.set_editor_property("switch_distance", SWITCH_DISTANCE)
    actor.get_editor_property("gather_box").set_box_extent(extent)
    log("placed %s, gather extent %s, switch distance %.0f" % (actor.get_actor_label(), extent, SWITCH_DISTANCE))
    return actor


TEST_MATERIALS = {}


def main():
    for d in (ROOT, MESH_DIR, MAT_DIR):
        unreal.EditorAssetLibrary.make_directory(d)
    materials = make_material_instances()
    TEST_MATERIALS.update(make_test_materials())
    meshes = make_meshes(materials)
    bp = make_blueprint()
    open_level()
    floors = prepare_floor()
    ground_z = floor_top_z(floors)
    place_objects(meshes, materials, ground_z)
    place_special(meshes, ground_z)
    place_hlod_actor(bp, ground_z)
    LEVELS.save_current_level()
    unreal.EditorAssetLibrary.save_directory(ROOT, only_if_is_dirty=False, recursive=True)
    log("DONE level=%s" % LEVEL)


try:
    main()
except Exception:
    unreal.log_error("NCH SETUP FAILED\n" + traceback.format_exc())
