# -*- coding: utf-8 -*-
"""疑案 A 的诊断探针：拖动中的画面 ≠ 松手后的画面，到底是**什么**在差。

`TinyGladeShotResize.py` 只报出了「差 6.3–9.0% 像素、背光墙精确 (0,0,0)」这个现象，
真因一直没定位。本脚本不出交付图，只做**四组一次性对照**，每组回答一个二选一的问题：

  Q1 **是不是收敛速度问题**：同一个拖动态，预热 8 / 32 / 128 / 384 次各导一张。
     若 384 次那张收敛到松手图 ⇒ 「预热被作废、只是喂不够」；若 384 次仍钉在同一个数
     ⇒ 是**持久**的渲染状态差，不是历史没喂饱。
  Q2 **是不是 Lumen**：把捕获组件的 GI/反射覆盖去掉（引擎本来就写死关 Lumen，见坑 ⑤），
     拖动 vs 松手再比一次。差异消失 ⇒ 只在 Lumen 这一路；仍在 ⇒ 与直接光/阴影有关。
  Q3 **几何到底一不一样**：`SCS_BASE_COLOR` 只出反照率、零光照。两张必须逐像素相同 ——
     不同的话前面所有「几何没错」的结论一起作废。
  Q4 **最小的什么动作能治好它**：拖动态之后分别补
       · 组件可见性 off→on（**只重建场景代理**，不碰任何几何/缓冲）
       · 空转 256 帧（而不是 24）
       · 再来一次 `reevaluate_site()`（哈希短路，应当是 no-op）
       · 用**一次** 400 cm 的推拉走到同一个尺寸（而不是 8 次 50 cm）
     哪个变干净，真因就在哪一侧。

产物：`Saved/TinyGladeShots/probe_*.png` + 日志里的 `PROBE ` 行。
判读用 `Scripts/TinyGladeShotResizeProbeStats.py`。

用法（必须真编辑器 + `-ExecCmds`，见 `TinyGladeShotResize.py` 坑 ②）::

    UnrealEditor.exe <project> -ExecCmds="py <此文件>" -abslog=<独立日志>

⚠️ **`-ExecCmds` 里不要跟 `; Quit`**：`py` 注册完 tick 回调就返回，Quit 会当场把编辑器关掉、
一张图都拍不到。收尾由脚本自己的 `quit_editor()` 负责（异常路径也在 try 里兜住，坑 ⑥）。

坑 ①/④/⑤/⑥/⑧/⑨/⑩/⑪ 全部沿用 `TinyGladeShotResize.py` 文件头那一份，逐条适用。
"""
import math
import os

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

FIRST_TICK = 60
SETTLE_TICKS = 24
START = unreal.Vector2D(600.0, 400.0)
STEP_CM = 50.0
PUSHES = 8                       # 与 `TinyGladeShotResize.py` 的 f8 完全同一个态

STATE = {"ticks": 0, "handle": None, "jobs": [], "job": 0, "shots": 0,
         "spawned": [], "world": None, "house": None, "ground": None, "comp": None, "rt": None,
         "home": None, "settle": 0, "stage": "reset", "pushed": 0, "extra": 0}


def look_at(frm, to):
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def lumen_pp(on):
    """坑 ⑤：离屏 capture 被引擎写死 `DynamicGlobalIlluminationMethod = None`。

    `on=False` 就是**不覆盖** —— 也就是引擎默认的那条「没有 Lumen」的路。
    """
    pp = unreal.PostProcessSettings()
    if on:
        pp.set_editor_property("override_dynamic_global_illumination_method", True)
        pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
        pp.set_editor_property("override_reflection_method", True)
        pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
    return pp


def note(name):
    """CPU 侧的量 + **组件包围盒**。后者是本轮新加的：拖动期包围盒是量化只涨的（`CSShaperSteps::QuantizeUp`），
    松手才收紧 —— 如果画面差异跟着它走，真因就在包围盒这一侧。

    ⚠️ 坑 ⑩：一律调 `Get*UndrawableReason()`，不要 `is_*_drawable()`。
    """
    h = STATE["house"]
    size = h.get_editor_property("FootprintSize")
    origin, extent = h.get_actor_bounds(False)
    reasons = []
    for label, getter in (("vine", h.get_vine_undrawable_reason),
                          ("decor", h.get_decor_undrawable_reason),
                          ("window", h.get_window_undrawable_reason)):
        why = str(getter())
        if why:
            reasons.append("%s:%s" % (label, why))
    loc = h.get_actor_location()
    unreal.log("PROBE %-12s size=(%.1f,%.1f) axis=%s loc=(%.2f,%.2f,%.2f) "
               "bounds_o=(%.1f,%.1f,%.1f) bounds_e=(%.1f,%.1f,%.1f) "
               "doors=%d openings=%d bricks=%d vines=%d leaves=%d decor=%d pillars=%d%s"
               % (name, size.x, size.y, h.get_editor_property("RidgeAxis"),
                  loc.x, loc.y, loc.z,
                  origin.x, origin.y, origin.z, extent.x, extent.y, extent.z,
                  h.get_open_door_count(), h.get_opening_count(), h.get_frame_brick_count(),
                  h.get_vine_segment_count(), h.get_vine_leaf_count(),
                  h.get_decor_instance_count(), h.get_pillar_count(),
                  "" if not reasons else ("  undrawable: " + " ".join(reasons))))


def reset_house():
    h, home = STATE["house"], STATE["home"]
    h.set_editor_property("bFrameEnabled", True)
    h.set_editor_property("bVineEnabled", True)
    h.set_editor_property("bDecorEnabled", True)
    h.set_editor_property("FootprintBandFraction", 0.20)
    h.set_actor_location(unreal.Vector(home.x, home.y, h.get_actor_location().z), False, False)
    h.set_editor_property("FootprintSize", START)
    h.set_editor_property("RidgeAxis", unreal.CSRidgeAxis.X)
    h.call_method("RebuildHouse")


def recentre():
    h, home = STATE["house"], STATE["home"]
    h.set_actor_location(unreal.Vector(home.x, home.y, h.get_actor_location().z), False, False)
    h.reevaluate_site()


def set_house_visible(flag):
    """只重建场景代理：`SetVisibility` 走 `MarkRenderStateDirty`，一个顶点都不重算。"""
    for comp in STATE["house"].get_components_by_class(unreal.PrimitiveComponent):
        comp.set_visibility(flag, False)


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground, house = find("Ground_Demo"), find("House_Road")
    if not (ground and house):
        unreal.log_error("PROBE FAILED: no Ground_Demo / House_Road")
        return None
    STATE["ground"], STATE["house"] = ground, house
    STATE["home"] = house.get_actor_location()

    loc = house.get_actor_location()
    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(17):
        y = loc.y - 800.0 + i * 100.0
        ground.apply_paint_stroke(unreal.Vector(loc.x, y, ground.sample_height(unreal.Vector2D(loc.x, y))))
    ground.end_paint_stroke()

    # 机位与 `TinyGladeShotResize.py` 逐字节相同 —— 两组图必须可以直接互比。
    aim = unreal.Vector(loc.x, loc.y, loc.z + 150.0)
    cam = unreal.Vector(loc.x + 1450.0, loc.y - 1450.0, loc.z + 900.0)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    actor = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam, look_at(cam, aim))
    comp = actor.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", lumen_pp(True))
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)      # 坑 ⑧
    STATE["comp"], STATE["rt"] = comp, rt
    STATE["spawned"].append(actor)

    # 一条任务 = (名字, 松手否, 预热导出表, 选项)。
    L = [8, 32, 128, 384]
    jobs = [
        # Q1：收敛阶梯。这两条是整个探针的主判据。
        ("dragL", False, L, {}),
        ("relL", True, L, {}),
        # Q2：把 Lumen 拿掉（= 引擎默认那条路）。
        ("dragN", False, [8, 32], {"lumen": False}),
        ("relN", True, [8, 32], {"lumen": False}),
        # Q3：纯反照率，零光照。两张不同 ⇒「几何一样」的结论作废。
        ("dragB", False, [4], {"source": unreal.SceneCaptureSource.SCS_BASE_COLOR}),
        ("relB", True, [4], {"source": unreal.SceneCaptureSource.SCS_BASE_COLOR}),
        # Q4：最小治疗动作。
        ("dragV", False, [32], {"vis": True}),        # 只重建场景代理
        ("dragS", False, [32], {"settle": 256}),      # 空转久一点
        ("dragE", False, [32], {"reeval": True}),     # 再来一次 reevaluate_site（应当是 no-op）
        ("drag1", False, [32], {"single": True}),     # 一次推到位，而不是 8 次
        # Q5（光照那一半的隔离）：`recentre()` 走的是 `ApplyBodyPlacement` 的**增量变换**路
        # （只搬顶点，不重生成），而松手走的是全量重上传。所以先问一句：**不拉尺寸、
        # 只平移**，够不够把那面墙压黑？
        ("moveL", False, [32], {"pushes": 0, "move": 400.0}),      # 只平移：搬走再搬回
        ("moveA", False, [32], {"pushes": 0, "move": 400.0, "stay": True}),   # 搬走不搬回
        ("dragNR", False, [32], {"norecentre": True}),             # 拉尺寸但**不归位**
    ]
    STATE["jobs"] = jobs
    unreal.log("PROBE cam at (%.0f,%.0f,%.0f) aim (%.0f,%.0f,%.0f) jobs=%d"
               % (cam.x, cam.y, cam.z, aim.x, aim.y, aim.z, len(jobs)))
    return world


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return

    if STATE["job"] >= len(STATE["jobs"]):
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        if STATE["ground"]:
            STATE["ground"].reset_paint()
        for a in STATE["spawned"]:
            ACTORS.destroy_actor(a)
        unreal.log("PROBE DONE jobs=%d" % len(STATE["jobs"]))
        unreal.SystemLibrary.quit_editor()
        return

    name, released, ladder, opt = STATE["jobs"][STATE["job"]]
    h = STATE["house"]

    if STATE["stage"] == "reset":
        reset_house()
        STATE["comp"].set_editor_property("post_process_settings", lumen_pp(opt.get("lumen", True)))
        STATE["comp"].set_editor_property(
            "capture_source", opt.get("source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR))
        STATE["pushed"], STATE["stage"], STATE["extra"] = 0, "push", 0
        return

    if STATE["stage"] == "push":
        target = opt.get("pushes", 1 if opt.get("single", False) else PUSHES)
        if STATE["pushed"] < target:
            step = STEP_CM * PUSHES * 0.5 if opt.get("single", False) else STEP_CM * 0.5
            h.push_edge(0, step)
            h.push_edge(2, step)
            STATE["pushed"] += 1
            return
        # 只平移：把 actor 搬走（`stay` 时就停在那儿），走的正是 `recentre()` 那条
        # `ApplyBodyPlacement` 增量变换路，但一个尺寸都没改。
        move = opt.get("move", 0.0)
        if move != 0.0:
            home, loc = STATE["home"], h.get_actor_location()
            h.set_actor_location(unreal.Vector(home.x, home.y + move, loc.z), False, False)
            h.reevaluate_site()
        if not opt.get("norecentre", False) and not opt.get("stay", False):
            recentre()
        if released:
            h.push_edge(2, 0.0, True)
        if opt.get("reeval", False):
            h.reevaluate_site()
        if opt.get("vis", False):
            set_house_visible(False)
        note(name)
        STATE["settle"], STATE["shots"], STATE["stage"] = 0, 0, "settle"
        return

    if STATE["stage"] == "settle":
        STATE["settle"] += 1
        if opt.get("vis", False) and STATE["settle"] == 2:
            set_house_visible(True)
        if STATE["settle"] >= opt.get("settle", SETTLE_TICKS):
            STATE["stage"] = "shoot"
        return

    STATE["comp"].capture_scene()
    STATE["shots"] += 1
    if STATE["shots"] in ladder:
        unreal.RenderingLibrary.export_render_target(
            STATE["world"], STATE["rt"], OUT_DIR, "probe_%s_w%03d.png" % (name, STATE["shots"]))
        unreal.log("PROBE shot: probe_%s_w%03d.png" % (name, STATE["shots"]))
    if STATE["shots"] >= max(ladder):
        STATE["job"] += 1
        STATE["stage"] = "reset"


try:                                   # 坑 ⑥
    STATE["world"] = build()
except Exception as exc:
    unreal.log_error("PROBE FAILED in build(): %s" % exc)
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
