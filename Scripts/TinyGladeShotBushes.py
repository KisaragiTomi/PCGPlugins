# -*- coding: utf-8 -*-
"""建筑周边灌木（第七条派生链，照 TG `_garden_spawn_bushes_buildings.cs`）的出图 + **像素判据**。

readback 已经在 C++ 用例里验过几何（环带、路、花是本体的子集、重散确定性），这里只回答像素才能答的：

  A  **灌木真的画在屏幕上**：同一机位、只切 `bBuildingBushesEnabled` 拍两张，逐像素比。
     灌木走 Nanite（`bush_body` / `bush_flowers` 都开着 Nanite）—— 实例写进 GPU-Scene 之后
     剔除 / 光栅全归引擎，所以"组件建出来了"证明不了"画出来了"。
  B  **不是一片灰、也不是一团黑**：变化像素的平均亮度要够、且偏绿（叶子）——
     材质被静默换成默认材质时是中性灰（R ≈ G ≈ B）。

用法（tag 决定文件名前缀）::

    set TG_SHOT_TAG=v1
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotBushes.py"

产物：`Saved/TinyGladeShots/bushes_<tag>_*.png`，判据打在日志的 `BUSH PIXELS` 行。

坑表与 `TinyGladeShotDecor.py` 同一份（真编辑器 + tick 回调、离屏 SceneCapture、捕获自己的 PP
覆盖 Lumen、ViewState + 一帧一次预热、主视口驾驶捕获相机），这里不重复抄。
⚠️ 阈值**还没标定**：第一轮只记录实测值；标定口径同别的出图脚本（先量真值、再取一半）。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v1")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

GRID_X, GRID_Y = 48, 27
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
ZERO_FAIL = 0.005

# 未标定的下限：只挡"完全没画出来 / 全黑"这两种明显失效。标定后按实测值的一半替换。
BUSH_DIFF_FAIL = 0.01
BUSH_LUMA_FAIL = 15.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None, "house": None,
         "samples": {}, "rt": None, "comp": None, "cap": None, "spawned": [], "step": 0, "phase": 0}

FIRST_TICK = 45
SETTLE = 6
WARMUP_CAPTURES = int(os.environ.get("TG_SHOT_WARMUP", "128"))


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def set_bushes(on):
    g = STATE["ground"]
    g.set_editor_property("building_bushes_enabled", on)
    g.rebuild_building_bushes()
    unreal.log("BUSH enabled=%s" % on)


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    house = find("House_Road")
    if not (ground and house):
        unreal.log_error("BUSH FAILED: no Ground_Demo / House_Road")
        return None
    STATE["ground"], STATE["house"] = ground, house
    house.call_method("RebuildHouse")      # 顺带把外皮推给地面
    ground.rebuild_building_bushes()

    loc = house.get_actor_location()
    foot = house.get_editor_property("FootprintSize")
    wall_h = house.get_editor_property("WallHeight")

    # 机位从房子参数反算。灌木贴着墙脚一圈，所以一张压低看墙脚、一张斜俯看整圈。
    cam_low = unreal.Vector(loc.x - foot.x * 0.9, loc.y - foot.y * 0.5 - 900.0, loc.z + wall_h * 0.45)
    aim_low = unreal.Vector(loc.x, loc.y - foot.y * 0.5, loc.z + wall_h * 0.1)
    cam_wide = unreal.Vector(loc.x - foot.x * 1.4, loc.y - foot.y * 1.8, loc.z + wall_h * 2.2)
    aim_wide = unreal.Vector(loc.x, loc.y, loc.z)
    CAM_LOW = (cam_low, look_at(cam_low, aim_low))
    CAM_WIDE = (cam_wide, look_at(cam_wide, aim_wide))

    # (名字, 拍之前要做的事, 机位)。A 的两张必须**同机位、只切开关**。
    STATE["plan"] = [
        ("low_off", lambda: set_bushes(False), CAM_LOW),
        ("low_on", lambda: set_bushes(True), CAM_LOW),
        ("wide_on", lambda: None, CAM_WIDE),
    ]

    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_low, CAM_LOW[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 60.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)
    STATE["rt"], STATE["comp"], STATE["cap"] = rt, comp, cap
    STATE["spawned"].append(cap)
    return world


def sample_pixels():
    out = []
    for gy in range(GRID_Y):
        y = int((gy + 0.5) * H / GRID_Y)
        for gx in range(GRID_X):
            x = int((gx + 0.5) * W / GRID_X)
            col = unreal.RenderingLibrary.read_render_target_pixel(STATE["world"], STATE["rt"], x, y)
            out.append((col.r, col.g, col.b))
    return out


def report():
    s = STATE["samples"]
    ok = True
    for name in sorted(s):
        z = sum(1 for p in s[name] if p == (0, 0, 0)) / float(len(s[name]))
        unreal.log("BUSH PIXELS %-10s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("BUSH !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用" % (name, z * 100.0))
            ok = False

    off, on = s.get("low_off"), s.get("low_on")
    if off and on:
        mask = [max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) > PIXEL_DELTA for a, b in zip(off, on)]
        changed = sum(1 for m in mask if m)
        ratio = changed / float(GRID_POINTS)
        unreal.log("BUSH PIXELS on-vs-off: %d/%d changed (%.1f%%)" % (changed, GRID_POINTS, ratio * 100.0))
        if ratio < BUSH_DIFF_FAIL:
            unreal.log_error("BUSH !! 切换 bBuildingBushesEnabled 几乎没有改变画面 —— 灌木没有被画出来")
            ok = False
        lit = [on[i] for i, m in enumerate(mask) if m]
        if lit:
            luma = sum(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in lit) / float(len(lit))
            r = sum(p[0] for p in lit) / float(len(lit))
            g = sum(p[1] for p in lit) / float(len(lit))
            b = sum(p[2] for p in lit) / float(len(lit))
            unreal.log("BUSH PIXELS bush pixels: luma=%.1f rgb=(%.1f, %.1f, %.1f)" % (luma, r, g, b))
            if luma < BUSH_LUMA_FAIL:
                unreal.log_error("BUSH !! 灌木覆盖处平均亮度只有 %.1f —— 八成是一团黑" % luma)
                ok = False
    unreal.log("BUSH PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, action, (loc, rot) = STATE["plan"][step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            action()
            STATE["cap"].set_actor_location_and_rotation(loc, rot, False, False)
            unreal.EditorLevelLibrary.pilot_level_actor(STATE["cap"])
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "bushes_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("BUSH shot: bushes_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    if STATE["ground"]:
        set_bushes(True)
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("BUSH DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("BUSH FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
