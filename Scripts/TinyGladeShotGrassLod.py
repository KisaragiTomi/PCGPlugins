# -*- coding: utf-8 -*-
"""草的近 / 远两档（2026-09-22）出图 + 像素判据。

回答三个只有像素答得了的问题：

  A  **远处真的长了东西**：两档开着的那张，远景带（60 m 之外）的绿量要和"整张地面一档"
     那张相当 —— 差太多就是远处一片秃（组合草没画出来），或者反过来一堵墙。
  B  **交界不是一道硬圈**：`FadeDistance` 给足与给 0 各拍一张，沿**竖直方向**逐带算亮度，
     硬边那张会在 60 m 对应的那一行上出现明显跳变。
  C  **远处不是一片灰**：远景带的像素要偏绿（材质被静默换成默认材质时是中性灰）。

机位：离地 8 m、俯 12°，画面从 14 m 一直铺到地平线 —— 60 m 那条交界正好落在画面上半部。
另加一张 1.7 m 的平视图（`eye`），那才是玩家真正会看到的样子。

⚠️ 无头 / 脚本捕获里必须把窗口**钉到相机**（`SetGroundCoverViewOverride`）：近处窗口平时跟的是
"上一帧画过的透视视口"，而这张地面 512 m 见方、房子在角上，不钉的话窗口停在地面正中、
镜头里一根近处草都没有。

用法::

    set TG_SHOT_TAG=v1
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotGrassLod.py"

产物：`Saved/TinyGladeShots/grasslod_<tag>_*.png`，判据打在日志的 `GRASSLOD PIXELS` 行。
坑表同 `TinyGladeShotBushes.py`（离屏 SceneCapture、捕获自己的 PP 覆盖 Lumen、预热帧）。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v1")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

GRID_X, GRID_Y = 64, 36
CAM_HEIGHT = 800.0       # cm
CAM_PITCH = -12.0        # 度
EYE_HEIGHT = 170.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None,
         "samples": {}, "rt": None, "comp": None, "cap": None, "spawned": [], "step": 0, "phase": 0}

FIRST_TICK = 45
SETTLE = 6
WARMUP_CAPTURES = int(os.environ.get("TG_SHOT_WARMUP", "128"))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def set_flowers(on):
    """出图期间把花关掉：这一组图看的是**草**，而演示关卡的薰衣草 + 白花密到能盖住整片草地，
    近 / 远两档的差别全被它们淹掉（第二版就是这么读不出来的）。跑完恢复。
    ⚠️ Python 改 TArray 属性**不发变更通知**（单结构体属性才发），改完必须自己叫一次重散 ——
    这里由紧随其后的 `set_lod` 负责。"""
    g = STATE["ground"]
    g.set_editor_property("Flowers", STATE["flowers"] if on else [])


def set_lod(enabled, fade):
    g = STATE["ground"]
    grass = g.get_editor_property("Grass")
    lod = grass.get_editor_property("LOD")
    lod.set_editor_property("bEnabled", enabled)
    lod.set_editor_property("FadeDistance", fade)
    grass.set_editor_property("LOD", lod)
    g.set_editor_property("Grass", grass)
    g.call_method("RebuildGroundCover")
    far_i = g.call_method("GetGroundCoverFarIndex", (0,))
    near = g.call_method("DebugReadGroundCoverCountGpuSync", (0,))
    far = g.call_method("DebugReadGroundCoverCountGpuSync", (far_i,)) if far_i is not None and far_i >= 0 else None
    unreal.log("GRASSLOD 两档=%s 过渡带=%.0f cm ⇒ 近 %s 株 / 远 %s 簇" % (enabled, fade, near, far))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    house = find("House_Road")
    if not ground:
        unreal.log_error("GRASSLOD FAILED: no Ground_Demo")
        return None
    STATE["ground"] = ground

    # ⚠️ 机位要放在**空地**上：演示关卡的房子、树和灌木全挤在地面的那个角上（房子在 (891, 1586)），
    #    贴着房子放相机会把镜头埋进灌木里，拍出来满屏叶子、一根草都看不到（第一版就是这么废的）。
    #    往地面中心方向退到 150 m 开外，画面里只剩地被与天。
    base = house.get_actor_location() if house else ground.get_actor_location()
    z = base.z
    cam = unreal.Vector(base.x + 15000.0, base.y + 15000.0, z + CAM_HEIGHT)
    eye = unreal.Vector(base.x + 15000.0, base.y + 15000.0, z + EYE_HEIGHT)
    yaw = 45.0
    high_rot = unreal.Rotator(roll=0.0, pitch=CAM_PITCH, yaw=yaw)
    eye_rot = unreal.Rotator(roll=0.0, pitch=-2.0, yaw=yaw)

    # ⚠️ 窗口钉到相机（见文件头）。钉一次就够：后面每次 RebuildGroundCover 都用这个中心。
    ground.call_method("SetGroundCoverViewOverride", (cam,))

    STATE["flowers"] = ground.get_editor_property("Flowers")
    STATE["plan"] = [
        ("two", lambda: (set_flowers(False), set_lod(True, 1500.0)), (cam, high_rot)),      # 两档 + 过渡带
        ("nofade", lambda: set_lod(True, 0.0), (cam, high_rot)),      # 两档 + 硬边（对照）
        ("single", lambda: set_lod(False, 1500.0), (cam, high_rot)),  # 旧口径：整张地面一档
        ("eye", lambda: set_lod(True, 1500.0), (eye, eye_rot)),       # 平视：玩家视角
        ("flowers", lambda: (set_flowers(True), set_lod(True, 1500.0)), (cam, high_rot)),   # 花恢复后的全景
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
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam, high_rot)
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
    rows = []
    for gy in range(GRID_Y):
        y = int((gy + 0.5) * H / GRID_Y)
        row = []
        for gx in range(GRID_X):
            x = int((gx + 0.5) * W / GRID_X)
            col = unreal.RenderingLibrary.read_render_target_pixel(STATE["world"], STATE["rt"], x, y)
            row.append((col.r, col.g, col.b))
        rows.append(row)
    return rows


def row_distance(gy):
    """这一行的像素大致看的是多远的地面（cm）—— 用来把"第几行"翻译成"多少米"。"""
    half_v = math.degrees(math.atan(math.tan(math.radians(30.0)) * H / float(W)))
    frac = ((gy + 0.5) / GRID_Y) * 2.0 - 1.0      # −1 = 画面顶，+1 = 画面底
    down = -CAM_PITCH + frac * half_v             # 视线低于水平多少度
    if down <= 0.05:
        return None                               # 地平线以上：天
    return CAM_HEIGHT / math.tan(math.radians(down))


def luma(p):
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]


def band(rows, lo_cm, hi_cm):
    """取落在 [lo, hi] 距离带里的像素。"""
    out = []
    for gy, row in enumerate(rows):
        d = row_distance(gy)
        if d is not None and lo_cm <= d <= hi_cm:
            out.extend(row)
    return out


def mean_rgb(px):
    if not px:
        return (0.0, 0.0, 0.0)
    n = float(len(px))
    return (sum(p[0] for p in px) / n, sum(p[1] for p in px) / n, sum(p[2] for p in px) / n)


def report():
    s = STATE["samples"]
    ok = True
    # ⚠️ 距离带是按**高机位**（8 m / 俯 12°）反算的，`eye` 那张机位不同，只出图不参与判据。
    for name in ("two", "nofade", "single"):
        if name not in s:
            continue
        rows = s[name]
        near = mean_rgb(band(rows, 2000.0, 5000.0))     # 20–50 m：近处单根草
        far = mean_rgb(band(rows, 7000.0, 15000.0))     # 70–150 m：远处组合草
        unreal.log("GRASSLOD PIXELS %-7s 近带 rgb=(%.1f,%.1f,%.1f) 远带 rgb=(%.1f,%.1f,%.1f)"
                   % (name, near[0], near[1], near[2], far[0], far[1], far[2]))

    # A：远景带的绿量，两档 vs 一档
    if "two" in s and "single" in s:
        f2 = mean_rgb(band(s["two"], 7000.0, 15000.0))
        f1 = mean_rgb(band(s["single"], 7000.0, 15000.0))
        rel = (luma(f2) - luma(f1)) / max(luma(f1), 1.0)
        unreal.log("GRASSLOD PIXELS A 远景带亮度：两档 %.1f vs 一档 %.1f（差 %+.1f%%）"
                   % (luma(f2), luma(f1), rel * 100.0))
        if abs(rel) > 0.35:
            unreal.log_error("GRASSLOD !! 远景带与一档差了 %+.0f%% —— 远处要么秃了要么糊成一片" % (rel * 100.0))
            ok = False
        if f2[1] <= max(f2[0], f2[2]):
            unreal.log_error("GRASSLOD !! 远景带不偏绿 rgb=(%.1f,%.1f,%.1f) —— 八成被换成默认材质了" % f2)
            ok = False

    # B：交界处的跳变。逐行亮度的一阶差，看 60 m 那一行附近有没有尖峰
    for name in ("two", "nofade"):
        if name not in s:
            continue
        rows = s[name]
        prof = []
        for gy, row in enumerate(rows):
            d = row_distance(gy)
            if d is None or not (3000.0 <= d <= 20000.0):
                continue
            prof.append((d, luma(mean_rgb(row))))
        jumps = [(abs(prof[i][1] - prof[i - 1][1]), prof[i][0]) for i in range(1, len(prof))]
        worst = max(jumps) if jumps else (0.0, 0.0)
        unreal.log("GRASSLOD PIXELS B %-7s 逐行亮度最大跳变 %.2f @ %.0f m（%d 行）"
                   % (name, worst[0], worst[1] / 100.0, len(prof)))
        unreal.log("GRASSLOD PIXELS B %-7s 剖面 %s"
                   % (name, " ".join("%.0fm:%.0f" % (d / 100.0, L) for d, L in prof)))
    unreal.log("GRASSLOD PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


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
                    STATE["world"], STATE["rt"], OUT_DIR, "grasslod_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("GRASSLOD shot: grasslod_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    if STATE["ground"]:
        set_flowers(True)
        set_lod(True, 1500.0)
        STATE["ground"].call_method("ClearGroundCoverViewOverride")
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("GRASSLOD DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("GRASSLOD FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
