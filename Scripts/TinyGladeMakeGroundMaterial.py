# -*- coding: utf-8 -*-
"""
地面材质：按顶点色 R 通道把草地与土路混起来 —— 这是 P1 验收门里一直欠着的那份资产。

地面的 R 通道就是道路权重（ACSGroundActor 的通道语义），笔刷画的就是它。没有这份材质时
地面走引擎默认材质，画上去的路**在画面上完全看不见**，只有房子的门拱在响应，很容易误判成
"笔刷没生效"。

贴图用 Tiny Glade 提取出来的 dirtpath_1（路）与 dirtpath_grass（草）。
地面 UV0 是世界平铺（局部坐标 / UVWorldPeriod，默认 500 cm），所以贴图每 5 m 重复一次。

⚠️ 草地那张**曾经选错**，症状是"草地上一圈规则的亮白网格线，间距正好 5 m"（2026-08-30 查明）：
    原来用的 grass_patch_summer 不是地面贴图，是**草叶卡片**的贴图 —— RGB 是两段上下渐变、
    形状全在 alpha 里（TG 的 _generate_grass 把它当 billboard 用）。它 alpha=0 的空白边框
    **RGB 是纯白 255**：上边框 12 texel、左 6 + 右 4 texel。本材质只接 RGB、不看 alpha，
    于是每个平铺边界都糊上一条纯白线。
    实证：出图里线距实测 500.1 cm（= UVWorldPeriod）；线宽实测 13.4 / 11.5 texel，
    与贴图两轴白边的 12 / 10 texel 一一对上（各 +1.5 texel 是双线性扩散），**两轴不等宽这一点
    只可能来自那张贴图**；相邻瓦片内容按平移相等而非镜像相等 ⇒ 寻址是 Wrap，
    "被导成 Clamp"那个猜想不成立（真 Clamp 的话 5 m 外整片会被拉成纯白，不会有网格）。
    换成 dirtpath_grass：512² 不透明、无白边，左右/上下接缝的 |Δ亮度| 均值 2.5 / 2.4
    （内部相邻列本身就有 0.8），即**本来就是无缝平铺图**，而且它正是 dirtpath_1 在 TG 里的搭档。
⚠️ 别改回 terrain_summer 之类：那张（1024²）左右边 |Δ亮度| 均值 49，5 m 平铺会切出硬边。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

GRASS = "/Game/TinyGlade/Textures/dirtpath_grass"
PATH = "/Game/TinyGlade/Textures/dirtpath_1"

grass_tex = unreal.EditorAssetLibrary.load_asset(GRASS)
path_tex = unreal.EditorAssetLibrary.load_asset(PATH)
unreal.log("GROUNDMAT grass=%s path=%s" % (grass_tex, path_tex))


def diag_texture(tag, tex):
    """把寻址模式打进日志 —— 亮白网格线那次排查里，"是不是被导成 Clamp"只有这一行能证伪。"""
    if not tex:
        unreal.log("GROUNDMAT DIAG %s: <missing>" % tag)
        return
    unreal.log("GROUNDMAT DIAG %s addr=(%s,%s) size=%dx%d srgb=%s" % (
        tag,
        tex.get_editor_property("address_x"), tex.get_editor_property("address_y"),
        tex.blueprint_get_size_x(), tex.blueprint_get_size_y(),
        tex.get_editor_property("srgb")))


diag_texture("grass", grass_tex)
diag_texture("path", path_tex)

asset_path = "%s/M_TinyGladeGround" % PKG
if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
    unreal.EditorAssetLibrary.delete_asset(asset_path)
mat = TOOLS.create_asset("M_TinyGladeGround", PKG, unreal.Material, unreal.MaterialFactoryNew())

uv = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -900, 0)
uv.set_editor_property("coordinate_index", 0)


def sampler(tex, y, fallback):
    """有贴图就采样，没有就退回常数 —— 提取资产缺失时材质仍然可用，路照样看得见。"""
    if tex:
        node = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -600, y)
        node.set_editor_property("texture", tex)
        MEL.connect_material_expressions(uv, "", node, "UVs")
        return node, ""
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -600, y)
    node.set_editor_property("constant", fallback)
    return node, ""


grass, grass_out = sampler(grass_tex, -220, unreal.LinearColor(0.20, 0.34, 0.13, 1.0))
dirt, dirt_out = sampler(path_tex, 60, unreal.LinearColor(0.36, 0.27, 0.18, 1.0))

vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -600, 340)

# R = 道路权重。笔刷默认只动 R 通道（PaintChannelMask 默认 (1,0,0,0)）。
lerp = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -280, 0)
MEL.connect_material_expressions(grass, grass_out, lerp, "A")
MEL.connect_material_expressions(dirt, dirt_out, lerp, "B")
MEL.connect_material_expressions(vcol, "R", lerp, "Alpha")
MEL.connect_material_property(lerp, "", unreal.MaterialProperty.MP_BASE_COLOR)

rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -280, 200)
rough.set_editor_property("r", 0.95)
MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_loaded_asset(mat)
unreal.log("GROUNDMAT built %s" % mat.get_name())

# ---- 挂到两张演示关卡的地面 + 蓝图 CDO ----
bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeGround" % PKG)
if bp:
    unreal.get_default_object(bp.generated_class()).set_editor_property("GroundMaterial", mat)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    unreal.log("GROUNDMAT assigned to BP CDO")

A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for level in ("L_HouseGroundDemo", "L_TerrainOpsDemo"):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    n = 0
    for a in A.get_all_level_actors():
        if "Ground" not in a.get_class().get_name() or "Shaper" in a.get_class().get_name():
            continue
        # 实测陷阱：CDO 的默认值不传播到已存在的实例，实例上必须再写一份。
        a.set_editor_property("GroundMaterial", mat)
        n += 1
        # 地面 actor 的**数量与摆位**也一并记档：亮白网格线排查时的第二个候选成因是
        # "多块地面拼出来的缝"，只有这行能证伪（一块 = 32 m，缝不可能每 5 m 出现一条）。
        loc = a.get_actor_location()
        unreal.log("GROUNDMAT DIAG ground '%s' at (%.0f,%.0f,%.0f) cells=%dx%d cell=%.1f uvperiod=%.1f" % (
            a.get_actor_label(), loc.x, loc.y, loc.z,
            a.get_editor_property("NumCellsX"), a.get_editor_property("NumCellsY"),
            a.get_editor_property("CellSize"), a.get_editor_property("UVWorldPeriod")))
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log("GROUNDMAT %s -> %d ground actors" % (level, n))

unreal.log("GROUNDMAT DONE")
