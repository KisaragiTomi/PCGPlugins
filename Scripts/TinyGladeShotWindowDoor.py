# -*- coding: utf-8 -*-
"""窗贴墙脚变门 + 玩家绘制楼梯：**出图复核**（2026-09-16）。

单测（`House.DoorFormRule` / `House.WindowBecomesDoor` / `Stairs.*`）钉的是判据与数字；这里只回答数字答不了的：
门网格是不是朝外、底边是不是贴着墙脚、门铃 / 花环挂没挂歪、窗那几件有没有藏干净、楼梯砖的朝向与台基观感。

--- 做什么 ---------------------------------------------------------------------
在演示关卡里**洞最少**的那栋房上，用笔刷的同一个执行面 `CSHouseLibrary.place_marker_along_ray`
沿四面墙每 20 cm 扫候选点：先找一个点在墙脚附近、被接受且落成门的；再找一个点在窗高、被接受且仍是窗的。
再在房子一角外面放一座默认样条的 `CSStairsActor`。四张图：门那面墙、门特写、窗、楼梯。

判据打在日志的 `SHOTWD` 行（出图之外的最低限度数字核对，免得"图是旧的"那种假绿）。

用法::

    UnrealEditor.exe <project> -ExecCmds="py D:/MyProject/UnrealProject/UETest574/Plugins/PCGPlugins/Scripts/TinyGladeShotWindowDoor.py"

产物：`Saved/TinyGladeShots/windowdoor_{wall,door,window,stairs}.png`。**不存关卡**。

--- 坑（沿用 `TinyGladeShotDoorWidth.py` 那张表）----------------------------
 ① `unreal.Rotator` 一律关键字参数。② 必须 `-ExecCmds="py <脚本>"`。③ 离屏 `SceneCapture2D`。
 ④ `RTF_RGBA8`。⑤ 捕获组件 PP 翻回 Lumen。⑥ 准备段包 try，出错自己 quit。⑧ `always_persist_rendering_state`。
 ⑨ 一帧一次 `capture_scene()` 预热。别的房子先藏掉（隔壁房子会挡视线）。
"""
import math
import os
import traceback

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H_PIX = 1600, 900
FIRST_TICK = 45
SETTLE = 20
WARMUP = int(os.environ.get("TG_SHOT_WARMUP", "96"))
WINDOW_BP = "%s/BP_Window_Cottage_1x1.BP_Window_Cottage_1x1_C" % PKG


def make_pp():
    pp = unreal.PostProcessSettings()
    pp.set_editor_property("override_dynamic_global_illumination_method", True)
    pp.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
    pp.set_editor_property("override_reflection_method", True)
    pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
    return pp


def look_at(frm, to):
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def setup():
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    spawned = []

    # 挑**洞最少**的那栋房：演示关卡的 `House_Road` 一面长墙上有道路拱门 + 一排间距约 280 cm 的窗，
    # 一扇门的面板格要约 155 cm，剩下能放门的区间只有几厘米宽（2026-09-16 第二版扫遍整面墙一个都没中）。
    houses = [a for a in actors.get_all_level_actors() if a.get_class().get_name().startswith("BP_TinyGladeHouse")
              or a.get_class().get_name() == "CSHouseActor"]
    if not houses:
        raise RuntimeError("no house in the demo level")
    house = min(houses, key=lambda h: int(h.call_method("GetOpeningCount")))
    loc = house.get_actor_location()
    fp = house.get_editor_property("footprint_size")
    hx, hy = float(fp.x) * 0.5, float(fp.y) * 0.5
    wall_h = float(house.get_editor_property("wall_height"))
    yaw = math.radians(house.get_actor_rotation().yaw)
    unreal.log("SHOTWD house %s loc=(%.0f, %.0f, %.0f) fp=(%.0f, %.0f) wall=%.0f yaw=%.0f openings=%d"
               % (house.get_actor_label(), loc.x, loc.y, loc.z, fp.x, fp.y, wall_h, math.degrees(yaw),
                  int(house.call_method("GetOpeningCount"))))

    hidden = []
    for other in houses:
        if other == house:
            continue
        other.set_actor_hidden_in_game(True)
        other.set_is_temporarily_hidden_in_editor(True)
        hidden.append(other)

    window_class = unreal.load_class(None, WINDOW_BP)
    if not window_class:
        raise RuntimeError("cannot load %s" % WINDOW_BP)

    # 房子局部轴转到世界：ax = 局部 +X，ay = 局部 +Y。
    ax = unreal.Vector(math.cos(yaw), math.sin(yaw), 0.0)
    ay = unreal.Vector(-math.sin(yaw), math.cos(yaw), 0.0)

    def to_world(local_x, local_y, z):
        return unreal.Vector(loc.x + ax.x * local_x + ay.x * local_y, loc.y + ax.y * local_x + ay.y * local_y, loc.z + z)

    # 四面墙：(外法线的局部方向, 沿墙方向, 半长, 半厚)。
    walls = [((0.0, -1.0), (1.0, 0.0), hx, hy), ((0.0, 1.0), (1.0, 0.0), hx, hy),
             ((-1.0, 0.0), (0.0, 1.0), hy, hx), ((1.0, 0.0), (0.0, 1.0), hy, hx)]
    rejects = {}

    def place_first_accepted(z, want_door, avoid=None):
        """沿四面墙每 20 cm 扫一个候选点，挑第一个被接受、形态也对的。被拒的原因汇总进日志。"""
        for normal, along, half_len, half_depth in walls:
            s = -half_len + 80.0
            while s <= half_len - 80.0:
                lx = normal[0] * (half_depth + 300.0) + along[0] * s
                ly = normal[1] * (half_depth + 300.0) + along[1] * s
                origin = to_world(lx, ly, z)
                if avoid is not None and math.hypot(origin.x - avoid.x, origin.y - avoid.y) < 500.0:
                    s += 20.0
                    continue
                ray = unreal.Vector(-(ax.x * normal[0] + ay.x * normal[1]), -(ax.y * normal[0] + ay.y * normal[1]), 0.0)
                m = unreal.CSHouseLibrary.place_marker_along_ray(house, window_class, origin, ray, 1000.0)
                if m and m.causes_cut() and bool(m.is_door_form()) == want_door:
                    unreal.log("SHOTWD placed %s on wall n=%s at s=%.0f" % ("door" if want_door else "window", normal, s))
                    return m
                if m:
                    key = "%s/door=%s" % (m.get_last_reject(), m.is_door_form())
                    rejects[key] = rejects.get(key, 0) + 1
                    actors.destroy_actor(m)
                s += 20.0
        unreal.log("SHOTWD rejects %s" % rejects)
        raise RuntimeError("no free spot for a %s on any wall" % ("door" if want_door else "window"))

    door = place_first_accepted(50.0, True)
    spawned.append(door)
    # 窗放在离门远一点的地方（射线起点离门水平 5 m 以外；起点在墙外 3 m，折到墙上约 4 m），
    # 免得房子的洞表还没刷新时两扇互相压着 —— 谓词读的是上一次重求值的洞表，这一轮新登记的门它还看不见。
    window = place_first_accepted(170.0, False, avoid=door.get_actor_location())
    spawned.append(window)
    unreal.log("SHOTWD rejects on the way %s" % rejects)

    # 楼梯：房子一角外面，沿世界 +X 上行。默认样条（三点、水平约 520、升 250）。
    stairs_at = to_world(hx + 250.0, -hy - 250.0, 0.0)
    stairs = actors.spawn_actor_from_class(unreal.CSStairsActor, stairs_at, unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
    spawned.append(stairs)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H_PIX, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)

    def make_capture(cam, target, fov):
        cap = actors.spawn_actor_from_class(unreal.SceneCapture2D, cam, look_at(cam, target))
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", fov)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        comp.set_editor_property("post_process_settings", make_pp())
        comp.set_editor_property("post_process_blend_weight", 1.0)
        comp.set_editor_property("always_persist_rendering_state", True)
        spawned.append(cap)
        return cap, comp

    # 机位跟着**实际落下的**两扇走（候选点是扫出来的，事先不知道落在哪、也未必在同一面墙上）。
    # 标记的 +X 朝墙内 ⇒ 相机站在 −X 那一侧，稍微偏一点角度才看得出进深（门框凸没凸出墙面）。
    def front_of(marker, back, side, height, target_z):
        at = marker.get_actor_location()
        inward = marker.get_actor_forward_vector()
        along = unreal.Vector(-inward.y, inward.x, 0.0)
        cam = unreal.Vector(at.x - inward.x * back + along.x * side, at.y - inward.y * back + along.y * side, loc.z + height)
        return cam, unreal.Vector(at.x, at.y, loc.z + target_z)

    shots = [
        ("wall",) + front_of(door, 1000.0, 0.0, 320.0, 150.0) + (50.0,),
        ("door",) + front_of(door, 380.0, 140.0, 170.0, 125.0) + (42.0,),
        ("window",) + front_of(window, 520.0, -120.0, 230.0, 190.0) + (40.0,),
        ("stairs", unreal.Vector(stairs_at.x + 260.0, stairs_at.y - 950.0, loc.z + 420.0), unreal.Vector(stairs_at.x + 260.0, stairs_at.y + 60.0, loc.z + 110.0), 45.0),
    ]
    captures = [(label,) + make_capture(cam, target, fov) for label, cam, target, fov in shots]

    state = {"ticks": 0, "step": 0, "phase": 0, "handle": None, "logged": False}

    def report():
        ok = True

        def check(cond, text):
            nonlocal ok
            unreal.log(("SHOTWD ok   " if cond else "SHOTWD FAIL ") + text)
            if not cond:
                ok = False

        check(not window.is_door_form(), "the high click stays a window")
        check(window.causes_cut(), "the window cuts a hole (reject=%s)" % window.get_last_reject())
        check(door.is_door_form(), "the click near the wall foot lands as a door")
        check(door.causes_cut(), "the door cuts a hole (reject=%s)" % door.get_last_reject())
        anchor = door.get_anchor()
        check(abs(float(anchor.get_editor_property("sill_z"))) < 0.01, "the door's floor is the wall foot")
        dz = float(door.get_actor_location().z - loc.z)
        _, door_h = door.get_form_size(True)
        check(abs(dz - door_h * 0.5) < 1.0, "the door marker sits at its centre height (%.1f vs %.1f)" % (dz, door_h * 0.5))
        unreal.log("SHOTWD door clutter choice=%d step_bricks=%d rails=%s"
                   % (door.get_door_clutter_choice(), door.get_door_step_brick_count(), door.has_door_rails()))
        # ⚠️ bool 属性在 Python 里去掉 `b` 前缀：`bDoorForm` → `door_form`。
        cuts = [o for o in house.call_method("GetCurrentOpenings") if o.get_editor_property("door_form")]
        check(len(cuts) == 1, "exactly one door-form hole in the house (%d)" % len(cuts))
        if cuts:
            c = cuts[0]
            unreal.log("SHOTWD door hole edge=%d s=%.1f w=%.1f z0=%.1f z1=%.1f"
                       % (c.get_editor_property("edge_index"), c.get_editor_property("center_s"), c.get_editor_property("width"),
                          c.get_editor_property("z0"), c.get_editor_property("z1")))
        check(stairs.get_step_count() > 0, "the default stairs build steps (%d steps, %d bricks, %d ladder runs)"
              % (stairs.get_step_count(), stairs.get_brick_count(), stairs.get_ladder_run_count()))
        unreal.log("SHOTWD %s" % ("OK" if ok else "FAILED"))

    def finish():
        unreal.unregister_slate_post_tick_callback(state["handle"])
        for a in hidden:
            if a:
                a.set_is_temporarily_hidden_in_editor(False)
        for a in spawned:
            if a:
                actors.destroy_actor(a)
        unreal.SystemLibrary.quit_editor()

    def tick(delta):
        state["ticks"] += 1
        if state["ticks"] < FIRST_TICK:
            return
        if not state["logged"]:
            if state["ticks"] < FIRST_TICK + SETTLE:
                return
            state["logged"] = True
            house.call_method("ReevaluateSite")
            report()
        if state["step"] >= len(captures):
            finish()
            return
        label, cap, comp = captures[state["step"]]
        if state["phase"] == 0:
            unreal.EditorLevelLibrary.pilot_level_actor(cap)
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        state["phase"] += 1
        comp.capture_scene()
        if state["phase"] >= WARMUP:
            unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "windowdoor_%s.png" % label)
            state["step"] += 1
            state["phase"] = 0

    state["handle"] = unreal.register_slate_post_tick_callback(tick)


try:
    setup()
except Exception:
    unreal.log_error("SHOTWD FAILED\n" + traceback.format_exc())
    unreal.SystemLibrary.quit_editor()
