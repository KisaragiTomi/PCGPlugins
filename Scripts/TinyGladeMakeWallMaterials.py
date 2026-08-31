# -*- coding: utf-8 -*-
"""
建 TinyGlade 墙体的三个材质，并挂到演示关卡上。

M_TinyGladeWall（Masked）  —— 洞由这条 OpacityMask 判据逐像素 discard 切出。
M_TinyGladeReveal（Opaque）—— **原本是洞口内壁**。用户 2026-08-29 裁决"不要内壁，中间用砖块
    填满"（门框砖，见 TinyGladeSetupFrame.py）之后，`ACSHouseActor::RevealMaterial` 这个属性
    随内壁一起作废、已从 C++ 删掉。这个材质资产**没有跟着删**：它现在是承重柱的材质
    （PillarMaterial），关卡里还引用着，改名/删掉会留下一堆空引用。
M_TinyGladeRoof（Opaque） —— 屋面。

⚠️ OpacityMask 里那段 HLSL 是 CSHouseProfile.h 里 CSHouse_ClipKeeps() 的逐字翻译。
   两处不一致就会出现"CPU 谓词说能放、画面上切穿帮"，而且不会报任何错。改一处必须改两处。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

# CSHouse_ClipKeeps() 的逐字翻译：返回 1 = 保留，0 = discard。
CLIP_HLSL = """
float2 p = Q;
int shape = (int)(Shape * 255.0f + 0.5f);
bool inside;
if (shape == 1)       inside = max(abs(p.x), abs(p.y)) < 1.0f;                        // Rect
else if (shape == 2)  inside = dot(p, p) < 1.0f;                                      // Circle
else if (shape == 0)  inside = (p.y <= 0.0f) ? (abs(p.x) < 1.0f) : (dot(p, p) < 1.0f); // Arch
else                  inside = false;                                                  // 255 = 这块面板没有洞
return inside ? 0.0f : 1.0f;
"""


def make_or_load(name):
    path = "%s/%s" % (PKG, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    return TOOLS.create_asset(name, PKG, unreal.Material, unreal.MaterialFactoryNew())


def add_const3(mat, r, g, b, x, y):
    e = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, x, y)
    e.set_editor_property("constant", unreal.LinearColor(r, g, b, 1.0))
    return e


def build_wall():
    mat = make_or_load("M_TinyGladeWall")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    # 双面会露出墙板背面成黑洞。断口由门框砖填满（内壁那条路已作废），所以这里必须保持单面。
    mat.set_editor_property("two_sided", False)

    uv1 = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -700, 0)
    uv1.set_editor_property("coordinate_index", 1)   # UV1 = 解析裁剪场 q

    vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -700, 200)

    clip = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -350, 100)
    clip.set_editor_property("code", CLIP_HLSL)
    clip.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    clip.set_editor_property("description", "TinyGladeArchClip")
    def custom_input(name):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        return ci
    clip.set_editor_property("inputs", [custom_input("Q"), custom_input("Shape")])
    MEL.connect_material_expressions(uv1, "", clip, "Q")
    MEL.connect_material_expressions(vcol, "B", clip, "Shape")
    MEL.connect_material_property(clip, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    base = add_const3(mat, 0.62, 0.58, 0.52, -350, -180)
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -350, -60)
    rough.set_editor_property("r", 0.85)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


def build_plain(name, r, g, b):
    mat = make_or_load(name)
    base = add_const3(mat, r, g, b, -350, 0)
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -350, 120)
    rough.set_editor_property("r", 0.9)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


wall = build_wall()
# 名字是内壁时代留下的（见文件头）；今天它只当承重柱材质用，比墙略暗。
reveal = build_plain("M_TinyGladeReveal", 0.34, 0.30, 0.26)
roof = build_plain("M_TinyGladeRoof", 0.30, 0.16, 0.13)

unreal.log("built: %s / %s / %s" % (wall.get_name(), reveal.get_name(), roof.get_name()))

# ---- 挂到演示关卡的两栋房子 + 蓝图 CDO ----
# CDO 那份供手动拖放时带默认值；实例那份是这次要看的（实测陷阱：CDO 不传播到已存在的实例）。
bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeHouse" % PKG)
if bp:
    cdo = unreal.get_default_object(bp.generated_class())
    for prop, mat in (("WallMaterial", wall), ("RoofMaterial", roof), ("PillarMaterial", reveal)):
        cdo.set_editor_property(prop, mat)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    unreal.log("assigned to BP_TinyGladeHouse CDO")

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
count = 0
for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if "House" not in a.get_class().get_name():
        continue
    for prop, mat in (("WallMaterial", wall), ("RoofMaterial", roof), ("PillarMaterial", reveal)):
        a.set_editor_property(prop, mat)
    a.call_method("RebuildHouse")
    count += 1
unreal.log("assigned to %d house actors" % count)
unreal.EditorLoadingAndSavingUtils.save_current_level()
unreal.log("MATERIALS DONE")
