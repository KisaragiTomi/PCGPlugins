# -*- coding: utf-8 -*-
"""
给 `L_HouseGroundDemo` 里刚摆的 6 棵树出图：全景俯瞰 + 地面视角 + 近景各一张。

**首要判据是贴地**：`TinyGladePlaceTrees.py` 的射线一次都没命中（地面 `BP_TinyGladeGround_C`
多半没有碰撞体），6 棵全部退回 z=0。树是悬空还是埋进土里，只能看图。

⚠️ 本脚本**不存盘关卡**：只 spawn 一个离屏 SceneCapture，出图后销毁。关卡光照 / 后处理
用关卡自己的，不另加。坑清单同 `TinyGladeShotCanopy.py`（PP 覆盖开 Lumen、
always_persist_rendering_state、一帧一次 capture_scene 预热、Rotator 是 roll/pitch/yaw）。
"""
import unreal

LEVEL = "/PCGPlugins/HouseTest/L_HouseGroundDemo"
OUT_DIR = r"C:\Users\KLW\AppData\Local\Temp\claude\D--MyProject-UnrealProject-UETest574-2\663fbe13-5a7c-4be3-a860-830f2ae635f7\scratchpad\demo_tree_shots"

W, H = 1600, 900
FIRST_TICK = 12
SETTLE = 4
WARMUP = 48

# 名字 -> (相机位置, 朝向, FOV)
SHOTS = [
    ("aerial", unreal.Vector(3200, -2800, 2400), unreal.Rotator(roll=0, pitch=-26, yaw=133), 60.0),
    ("ground", unreal.Vector(1100, 700, 250), unreal.Rotator(roll=0, pitch=4, yaw=191), 70.0),
    ("closeup", unreal.Vector(-250, -350, 420), unreal.Rotator(roll=0, pitch=2, yaw=225), 55.0),
]

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "step": 0, "phase": 0, "spawned": []}


def log(m):
    unreal.log("DEMOTREE %s" % m)


def setup():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(LEVEL):
        unreal.log_error("DEMOTREE !! 关卡加载失败")
        return None
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    pp = unreal.PostProcessSettings()
    pp.set_editor_property("override_dynamic_global_illumination_method", True)
    pp.set_editor_property("dynamic_global_illumination_method",
                           unreal.DynamicGlobalIlluminationMethod.LUMEN)
    pp.set_editor_property("override_reflection_method", True)
    pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, SHOTS[0][1], SHOTS[0][2])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)
    STATE["rt"], STATE["comp"], STATE["cap"], STATE["world"] = rt, comp, cap, world
    STATE["spawned"].append(cap)
    return world


def tick(_delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(SHOTS):
        name, loc, rot, fov = SHOTS[step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            STATE["cap"].set_actor_location_and_rotation(loc, rot, False, False)
            STATE["comp"].set_editor_property("fov_angle", fov)
        elif phase >= SETTLE:
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "demotree_%s.png" % name)
                log("出图 demotree_%s.png" % name)
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)        # 只销毁自己 spawn 的，关卡不存盘
    log("DONE")
    unreal.SystemLibrary.quit_editor()


try:
    if setup() is None:
        unreal.SystemLibrary.quit_editor()
    else:
        STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
except Exception as exc:
    unreal.log_error("DEMOTREE !! 准备段抛异常：%s" % exc)
    unreal.SystemLibrary.quit_editor()
