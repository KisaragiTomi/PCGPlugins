# -*- coding: utf-8 -*-
"""
打开演示关卡并把视口摆到连拱正对面（人工查看用，脚本不退出编辑器）。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeFrameArches.py"

顺带确保路是画着的：门是道路推导出来的，没路就没拱可看。
"""
import unreal

# ⚠️ unreal.Rotator 的构造参数是 (roll, pitch, yaw)，**不是** (pitch, yaw, roll)。
#    写错的症状极具迷惑性：Rotator(-6, 90, 0) 会把 90 当成 pitch，相机直接朝正上方看天，
#    出图是一张纯渐变，看起来像"渲染没出来"。这里一律用关键字参数，别再靠位置传。

unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


ground = find("Ground_Demo")
house = find("House_Road")
if not (ground and house):
    unreal.log_error("FRAME FAILED: demo actors missing")
    raise SystemExit

loc = house.get_actor_location()
if house.get_open_door_count() == 0:
    ground.begin_paint_stroke()
    for i in range(17):
        y = loc.y - 800.0 + i * 100.0
        ground.apply_paint_stroke(unreal.Vector(loc.x, y, ground.sample_height(unreal.Vector2D(loc.x, y))))
    ground.end_paint_stroke()
    house.call_method("RebuildHouse")

unreal.log("FRAME doors=%d openings=%d" % (house.get_open_door_count(), house.get_opening_count()))

# 站在南墙外朝 +Y 看：南北两面长墙的连拱正对镜头。
unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(
    unreal.Vector(loc.x, loc.y - 900.0, 240.0), unreal.Rotator(roll=0.0, pitch=-5.0, yaw=90.0))
unreal.log("FRAME READY — 房子 House_Road 在 (%.0f, %.0f)，南北长墙各 3 个拱" % (loc.x, loc.y))
