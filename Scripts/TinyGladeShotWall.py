# -*- coding: utf-8 -*-
"""样条墙（`ACSWallActor`，墙化计划第 1 步）的出图 + **像素判据**。

参考图：`Docs/TinyGlade/img/tiny-glade-ref-freehand-wall-20260922.jpg`（TG 的 L 形 freehand 墙：灰泥墙身、
压顶砖 + 隔块抬起的垛口、拐角砖柱、两面爬藤）。本脚本在 `L_HouseGroundDemo` 的草地上现生成一面默认 L 形墙
（**不存盘**），从同一机位拍"没有墙 / 有墙"两张逐像素比，再拍一张近 TG 截图的俯视、一张贴地看垛口剪影。

判据（只挡明显失效，阈值未标定，第一轮只记实测值）：
  A  墙真的画在屏幕上：同机位有墙 / 无墙的差异像素率 > 1%。
  B  不是一团黑：变化像素平均亮度 > 15。

坑表与 `TinyGladeShotDecor.py` 同一份（真编辑器 + tick 回调、离屏 SceneCapture、捕获自己的 PP 覆盖 Lumen、
ViewState + 一帧一次预热、主视口驾驶捕获相机），这里不重复抄。另：藤默认生长动画（出现后按 60 cm/s 长），
这里把 `bGrowOnLoad` 关掉，拍到的是长成的藤。

用法::

    set TG_SHOT_TAG=v1
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotWall.py"

产物：`Saved/TinyGladeShots/wall_<tag>_*.png`，判据打在日志的 `WALL PIXELS` 行。挑出来给 README 展示的图手动放进
`Docs/Result/`（那个目录只放 README 里展示的图，脚本不往里自动写）。
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
# 纯黑像素的上限：Lumen 预热没起作用时是成片的黑（≥ 5%）。地面中部的薰衣草地被卡片**背面**是纯黑的，
# 贴地那张（low）前景里全是它们，实测 0.46%–0.62% —— 0.5% 的通用阈值会被它们误报，这里放到 2%。
ZERO_FAIL = 0.02
WALL_DIFF_FAIL = 0.01
WALL_LUMA_FAIL = 15.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None, "wall": None, "origin": None,
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


def ground_z(x, y):
    g = STATE["ground"]
    return g.sample_height(unreal.Vector2D(x, y)) if g else 0.0


def local(x, y, z):
    """墙局部坐标 → 世界（墙原点落在地面上，局部 Z 相对那一点的地面高度）。"""
    o = STATE["origin"]
    return unreal.Vector(o.x + x, o.y + y, o.z + z)


def spawn_wall():
    o = STATE["origin"]
    wall = ACTORS.spawn_actor_from_class(unreal.CSWallActor, o, unreal.Rotator(0.0, 0.0, 0.0))
    wall.set_actor_label("Wall_ShotProbe")
    vine = wall.get_editor_property("vine")
    vine.set_editor_property("grow_on_load", False)   # bGrowOnLoad：UE Python 去掉 bool 的 b 前缀
    wall.set_editor_property("vine", vine)
    wall.rebuild_wall()
    STATE["wall"] = wall
    STATE["spawned"].append(wall)
    unreal.log("WALL spawned at %s: length=%.0f points=%d tris=%d coping=%d merlons=%d quoins=%d bricks=%d strands=%d leaves=%d flowers=%d onGround=%s"
               % (o, wall.get_wall_length(), wall.get_path_point_count(), wall.get_body_triangle_count(),
                  wall.get_coping_brick_count(), wall.get_merlon_count(), wall.get_quoin_column_count(),
                  wall.get_brick_count(), wall.get_vine_strand_count(), wall.get_vine_leaf_count(),
                  wall.get_vine_flower_count(), wall.is_on_ground()))
    unreal.log("WALL gpu: bricks=%d leaves=%d mismatch='%s'"
               % (wall.debug_read_brick_count_gpu_sync(), wall.debug_read_vine_leaf_count_gpu_sync(),
                  wall.debug_get_gpu_asset_mismatch_sync()))


def spawn_curve_wall():
    """一条 S 形弯墙（四个控制点全是 Curve）：夹角都大 ⇒ 整面圆滑，不出拐角角石、墙顶砖不断开。"""
    o = STATE["origin"]
    base = unreal.Vector(o.x, o.y + 1300.0, ground_z(o.x, o.y + 1300.0))
    wall = ACTORS.spawn_actor_from_class(unreal.CSWallActor, base, unreal.Rotator(0.0, 0.0, 0.0))
    wall.set_actor_label("Wall_ShotCurve")
    spline = wall.get_spline()
    spline.clear_spline_points(False)
    for x, y in ((0.0, 0.0), (350.0, -250.0), (750.0, -150.0), (1000.0, 250.0)):
        spline.add_spline_point(unreal.Vector(x, y, 0.0), unreal.SplineCoordinateSpace.LOCAL, False)
    for i in range(4):
        spline.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
    spline.update_spline()
    vine = wall.get_editor_property("vine")
    vine.set_editor_property("grow_on_load", False)
    wall.set_editor_property("vine", vine)
    wall.rebuild_wall()
    STATE["spawned"].append(wall)
    unreal.log("WALL curve: length=%.0f points=%d coping=%d merlons=%d quoins=%d strands=%d"
               % (wall.get_wall_length(), wall.get_path_point_count(), wall.get_coping_brick_count(),
                  wall.get_merlon_count(), wall.get_quoin_column_count(), wall.get_vine_strand_count()))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()
    ground = find("Ground_Demo")
    house = find("House_Road")
    if not (ground and house):
        unreal.log_error("WALL FAILED: no Ground_Demo / House_Road")
        return None
    STATE["ground"] = ground

    # 墙放在地面中部的空草地上。⚠️ 演示内容（房子、路）挤在 512 m 地面的**一角**（09-21 从 128 m 扩出去时
    # 原点没动），第一版放在房子西南 12 m，一半落在地面外（悬空无藤、墙脚落回样条高度）。
    hl = house.get_actor_location()
    ox, oy = float(os.environ.get("TG_WALL_X", "20000")), float(os.environ.get("TG_WALL_Y", "20000"))
    STATE["origin"] = unreal.Vector(ox, oy, ground_z(ox, oy))
    unreal.log("WALL origin %s (house at %s, road weight %.2f)" % (STATE["origin"], hl,
               ground.sample_road_weight(unreal.Vector2D(ox + 300.0, oy))))

    # 机位（墙局部：第一段沿 +X 0→600，第二段沿 +Y 在 x=600 处 0→450）：
    #   tg   —— 近 TG 截图：从墙外西南侧高处往东北俯看拐角，近墙横在画面下方、另一段往里走；
    #   wide —— 退远看整面 L 墙与房子的比例；
    #   low  —— 贴地斜看墙顶，看垛口的剪影。
    cam_tg = local(260.0, -420.0, 520.0)
    aim_tg = local(430.0, 140.0, 40.0)
    cam_wide = local(-500.0, -900.0, 900.0)
    aim_wide = local(350.0, 250.0, 0.0)
    cam_low = local(-250.0, -260.0, 160.0)
    aim_low = local(600.0, 120.0, 150.0)
    cam_curve = local(500.0, 250.0, 1100.0)
    aim_curve = local(500.0, 1300.0, 0.0)
    CURVE = (cam_curve, look_at(cam_curve, aim_curve))
    TG = (cam_tg, look_at(cam_tg, aim_tg))
    WIDE = (cam_wide, look_at(cam_wide, aim_wide))
    LOW = (cam_low, look_at(cam_low, aim_low))

    # (名字, 拍之前要做的事, 机位)。A 的两张必须**同机位、只差墙在不在**。
    STATE["plan"] = [
        ("tg_off", lambda: None, TG),
        ("tg_on", spawn_wall, TG),
        ("wide", lambda: None, WIDE),
        ("low", lambda: None, LOW),
        ("curve", spawn_curve_wall, CURVE),
    ]

    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
                                                        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_tg, TG[1])
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
        unreal.log("WALL PIXELS %-8s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("WALL !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用" % (name, z * 100.0))
            ok = False
    off, on = s.get("tg_off"), s.get("tg_on")
    if off and on:
        mask = [max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) > PIXEL_DELTA for a, b in zip(off, on)]
        changed = sum(1 for m in mask if m)
        ratio = changed / float(GRID_POINTS)
        unreal.log("WALL PIXELS on-vs-off: %d/%d changed (%.1f%%)" % (changed, GRID_POINTS, ratio * 100.0))
        if ratio < WALL_DIFF_FAIL:
            unreal.log_error("WALL !! 生成墙几乎没有改变画面 —— 墙没有被画出来")
            ok = False
        lit = [on[i] for i, m in enumerate(mask) if m]
        if lit:
            luma = sum(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in lit) / float(len(lit))
            r = sum(p[0] for p in lit) / float(len(lit))
            g = sum(p[1] for p in lit) / float(len(lit))
            b = sum(p[2] for p in lit) / float(len(lit))
            unreal.log("WALL PIXELS wall pixels: luma=%.1f rgb=(%.1f, %.1f, %.1f)" % (luma, r, g, b))
            if luma < WALL_LUMA_FAIL:
                unreal.log_error("WALL !! 墙覆盖处平均亮度只有 %.1f —— 八成是一团黑" % luma)
                ok = False
    unreal.log("WALL PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, action, (loc, rot) = STATE["plan"][step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            try:
                action()
            except Exception:
                import traceback
                unreal.log_error("WALL FAILED in action %s:\n%s" % (name, traceback.format_exc()))
            STATE["cap"].set_actor_location_and_rotation(loc, rot, False, False)
            unreal.EditorLevelLibrary.pilot_level_actor(STATE["cap"])
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(STATE["world"], STATE["rt"], OUT_DIR, "wall_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("WALL shot: wall_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("WALL DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("WALL FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
