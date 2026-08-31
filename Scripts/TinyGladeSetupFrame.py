# -*- coding: utf-8 -*-
"""
给演示关卡的房子接上门框砖 —— 用 Tiny Glade 提取出来的那块 brick。

TG 整个游戏只有一块砖网格（/Game/TinyGlade/Meshes/brick），是个 **100³ 的居中单位立方体**，
尺寸全靠逐实例非均匀缩放（逆向报告 §1.4「Affine3Packed transform，非均匀缩放 = 砖尺寸」）。
所以这里给的是"想要多大"，不是"选哪块"。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
BRICK = "/Game/TinyGlade/Meshes/brick/StaticMeshes/brick"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

brick = unreal.EditorAssetLibrary.load_asset(BRICK)
if not brick:
    unreal.log_error("FRAME SETUP FAILED: no brick at %s" % BRICK)
    raise SystemExit
size = brick.get_bounding_box().max - brick.get_bounding_box().min
unreal.log("FRAME brick mesh %s size=(%.1f, %.1f, %.1f)" % (brick.get_name(), size.x, size.y, size.z))

# ---- 砖材质。GPU 实例化那条 VF 必须勾 "Used with Instanced Static Meshes"，
#      否则材质在实例路径上不编译，砖会整批画成默认棋盘格。----
path = "%s/M_TinyGladeBrick" % PKG
if unreal.EditorAssetLibrary.does_asset_exist(path):
    unreal.EditorAssetLibrary.delete_asset(path)
mat = TOOLS.create_asset("M_TinyGladeBrick", PKG, unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property("used_with_instanced_static_meshes", True)
base = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -350, 0)
base.set_editor_property("constant", unreal.LinearColor(0.46, 0.42, 0.37, 1.0))
MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -350, 120)
rough.set_editor_property("r", 0.92)
MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_loaded_asset(mat)

FRAME_PROPS = (
    ("FrameBrickMesh", brick),
    ("FrameMaterial", mat),
    ("bFrameEnabled", True),
    ("FrameBrickLength", 26.0),
    ("FrameBrickDepth", 20.0),
    ("FrameBrickThickness", 0.0),   # 0 = 用墙厚，砖正好填满 clip 断口
    # 砖缝是**负的**：排布不留缝，全靠沿曲线胀大 10% 让相邻砖互相穿插（TG 的做法，
    # 理由与轴的对位见 CSHouseActor.h 的 FrameBrickBloat 注释）。这两行必须一起改 ——
    # 之前 CDO 写死的 FrameBrickGap = 1.5 是正缝，块数一跳整条拱缘就露一条缝。
    # ⚠️ 光改 C++ 默认值不够：这个脚本把值烘进了 BP CDO 与关卡实例，改完必须重跑一次。
    ("FrameBrickGap", 0.0),
    ("FrameBrickBloat", 1.1),
)

bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeHouse" % PKG)
if bp:
    cdo = unreal.get_default_object(bp.generated_class())
    for name, value in FRAME_PROPS:
        cdo.set_editor_property(name, value)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    unreal.log("FRAME assigned to BP CDO")

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
count = 0
for a in A.get_all_level_actors():
    if "House" not in a.get_class().get_name():
        continue
    # 实测陷阱：CDO 的默认值不传播到已存在的实例，实例上必须再写一份。
    for name, value in FRAME_PROPS:
        a.set_editor_property(name, value)
    a.call_method("RebuildHouse")
    unreal.log("FRAME %s bricks=%d openings=%d" % (a.get_actor_label(), a.get_frame_brick_count(), a.get_opening_count()))
    count += 1
unreal.EditorLoadingAndSavingUtils.save_current_level()
unreal.log("FRAME SETUP DONE (%d houses)" % count)
