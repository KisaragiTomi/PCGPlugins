# -*- coding: utf-8 -*-
"""拱间墩的改前/改后同机位对照图（计划 D6 的实拍裁决，`Docs/TinyGlade/img/TG_continuous_arches.png`）。

要回答的是两件只有像素能答、readback 与单测都答不了的事：

  A  **整面墙的连拱之间还剩不剩"墙"这个表面**（`wall` 机位）：一条路把一面长墙的三个拱
     全点亮，两处残料跨度连成一片。改后起拱线以下应当看不见灰泥，只剩门樘砖砌成的墩。
  B  **砖是不是从拱圈不间断地砌到地面**（`pier` 机位，贴脸）：接缝处不许看得出换料。
     这一条单测**永远测不到** —— 砖是 GPU 实例、灰泥是房体网格，两套东西的接缝只在画面里存在。

判据是**像素差**，不是"看着不一样"：`before` 与 `after` 在同机位下的差异像素占比，
阈值取实测值的一半（见 `TinyGladeShotPierStats.py`）。

**门必须是活的**：所以还有第三次运行 `sabotage` —— 墩样式开着，但把 `PierStyleMaxWidth`
按到 0（跨度再窄也不判墩）。它与 `before` 应当**逐像素几乎相同**，差异远低于阈值。
不做这一步的话，"before 和 after 不一样"这条判据可能只是在量光照噪声。

2026-08-30 这里曾经还有 `legacyframe` / `legacyoff` 两个 tag，靠 `csh.FrameLegacy` 把门框砖
切回旧路，用来拍"墩上那条竖缝没了"的对照。**旧路与那个 CVar 已随裁决一第二步删除**，
两个 tag 因此一并去掉；那一组对照的图仍留在 `Saved/TinyGladeShots/`（见
`TinyGladeShotPierStats.py`），而"新旧两条路把砖摆在同一个地方"这条判据现在由单测
`House.FrameAnalyticMatchesLegacy` 守着（它带自己的 CPU 镜像，不依赖产线旧路）。

用法（tag 决定文件名前缀与这一趟的世界配置）::

    set TG_SHOT_TAG=before      (墩样式关)
    set TG_SHOT_TAG=after       (墩样式开)
    set TG_SHOT_TAG=sabotage    (墩样式开，但阈值按到 0 —— 故意让墩不发生)
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotPier.py"

产物：`Saved/TinyGladeShots/pier_<tag>_*.png`。

--- 踩过的坑（逐条来自状态文件的坑表）-------------------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
 ② 必须用**真编辑器** + `-ExecCmds="py <脚本>"`：`-ExecutePythonScript` 跑完即退，
    tick 回调根本没机会触发，而下面的预热连拍全靠 tick。
 ③ 离屏 `SceneCapture2D`，不要 `HighResShot`（本工程起来是 4 分屏）。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ 离屏 capture 被引擎写死关掉 Lumen，得用捕获组件自己的 `post_process_settings` 覆盖回来。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**。准备段整个包在 try 里。
 ⑦ `always_persist_rendering_state = True`：不开它 PPV 里的曝光一条都不生效。
 ⑧ 与 ⑦ 配套的另一半：导出前必须 `capture_scene()` 预热 32 次。Lumen 的最终聚集靠帧间历史，
    一帧抓出来的间接光恒为零，症状看着像"几何有洞"。**⑦⑧ 缺一不可。**
 ⑨ 这张演示关卡是脚本建的，**一盏灯都没有**：不补光只会出全黑。补之前先查，别补重了。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "before")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

FIRST_TICK = 60          # 世界侧收敛（天光实时捕获、Lumen 场景）要几十帧
# 坑 ⑨ 的执行面。**128 不是保守取整，32 是真的不够**（2026-08-31 重标定）。
# 两档收敛得分开看（整表与重跑方式在 `TinyGladeShotSeamWarmup.py` 的文件头）：
#   ① 「间接光不恒零」：zero% 在 **16 次**就到 0.000% —— 当初取 32 的全部依据，
#      而它只能证明"不是黑图"；
#   ② 「画面不再变」：相对 w384 的差异点占比 16 次 45.293% / **32 次 0.540%** /
#      64 次起 0.000%（192 次那 0.077% = 1/1296，是采样噪声底）。
#      另一个更难收的场景（上一轮的拖动态）：w32 差 2.04%、w128 差 0.015%。
# ⇒ 32 会给每一条像素判据搭上 0.5%~2% 的**系统误差**，与最薄那条门
#   （zero > 0.5%）同量级；取 128 = 收敛平台往里一整个倍频。
#   代价实测：一次 capture_scene() ≈ 35 ms（1081 次 / 38 s）⇒ 每张图多约 3.4 s。
# ⚠️ 必须**一帧一次**：同一帧连打 128 次不推进帧间历史，等于没预热。
# ⚠️ 环境变量只给**纯记录图**降档（set TG_SHOT_WARMUP=32），带门的图一律 128。
WARMUP_CAPTURES = int(os.environ.get("TG_SHOT_WARMUP", "128"))

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "rigs": [], "spawned": [], "ground": None, "world": None}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def ensure_lighting(centre):
    """坑 ⑨：演示关卡可能一盏灯都没有。先查再补，补重了两张图就不是同一个光照。"""
    have_sun = have_sky = False
    for a in ACTORS.get_all_level_actors():
        cls = a.get_class().get_name()
        have_sun = have_sun or cls == "DirectionalLight"
        have_sky = have_sky or cls == "SkyLight"
    if not have_sun:
        sun = ACTORS.spawn_actor_from_class(
            unreal.DirectionalLight, unreal.Vector(0, 0, 1000),
            unreal.Rotator(roll=0.0, pitch=-38.0, yaw=150.0))
        sun.light_component.set_editor_property("intensity", 6.0)
        STATE["spawned"].append(sun)
        STATE["spawned"].append(ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)))
    if not have_sky:
        sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(centre.x, centre.y, 1500))
        sky.light_component.set_editor_property("intensity", 1.0)
        # 静态立方图从没捕获过 = 一张黑图，而 Details 面板里它看着一切正常（坑表里那条）。
        sky.light_component.set_editor_property("real_time_capture", True)
        STATE["spawned"].append(sky)
    unreal.log("PIER lighting: sun=%s sky=%s (spawned %d)" % (have_sun, have_sky, len(STATE["spawned"])))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()
    ground, house = find("Ground_Demo"), find("House_Road")
    if not (ground and house):
        unreal.log_error("PIER FAILED: demo actors missing")
        return None
    STATE["ground"] = ground

    # ---- 这一趟的世界配置 ----
    # 墩宽钉在砖脚宽度上：墩上那一列砖骑在墩心、横向占 FrameBrickDepth ⇒ 墩宽超过它，
    # 墩两侧就会露出能看穿的缝。关卡实例上的值可能是旧默认，显式写一遍。
    brick_depth = house.get_editor_property("FrameBrickDepth")
    house.set_editor_property("PierWidth", float(brick_depth))
    house.set_editor_property("bPierStyleEnabled", TAG != "before")
    # sabotage：样式开着，但阈值按到 0 ⇒ 任何跨度都判不成墩。它应当与 before 逐像素几乎相同。
    house.set_editor_property("PierStyleMaxWidth", 0.0 if TAG == "sabotage" else 60.0)
    house.set_editor_property("PierStyleRestoreWidth", 0.0 if TAG == "sabotage" else 75.0)

    # ---- 一条路穿过整面墙：一面长墙的三个拱全点亮，两处跨度连成一片 ----
    loc = house.get_actor_location()
    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(17):
        y = loc.y - 800.0 + i * 100.0
        ground.apply_paint_stroke(unreal.Vector(loc.x, y, ground.sample_height(unreal.Vector2D(loc.x, y))))
    ground.end_paint_stroke()
    house.call_method("RebuildHouse")

    foot = house.get_editor_property("FootprintSize")
    pier_w = house.get_editor_property("PierWidth")
    unreal.log("PIER tag=%s doors=%d piers=%d bricks=%d PierWidth=%.1f FrameBrickDepth=%.1f "
               "PierStyleMaxWidth=%.1f enabled=%s"
               % (TAG, house.get_open_door_count(), house.get_pier_span_count(),
                  house.get_frame_brick_count(), pier_w, brick_depth,
                  house.get_editor_property("PierStyleMaxWidth"),
                  house.get_editor_property("bPierStyleEnabled")))

    ensure_lighting(loc)

    # ---- 机位 ----
    # 南墙（边 0）：Start = (−HX, −HY)、U = +X ⇒ 墙空间 S 对应局部 X = S − HX，外表面在 −HY。
    # 子段：可用 = 长 − 2×护角，N = round(可用/段距)，段距 = 可用/N，拱心 = 护角 + (k+½)·段距。
    yaw = math.radians(house.get_actor_rotation().yaw)
    ax = (math.cos(yaw), math.sin(yaw))          # 局部 +X 的世界方向
    ay = (-math.sin(yaw), math.cos(yaw))         # 局部 +Y

    def world_at(lx, ly, lz):
        return unreal.Vector(loc.x + ax[0] * lx + ay[0] * ly,
                             loc.y + ax[1] * lx + ay[1] * ly,
                             loc.z + lz)

    half_x, half_y = foot.x * 0.5, foot.y * 0.5
    margin = house.get_editor_property("CornerMargin")
    usable = foot.x - 2.0 * margin
    slots = max(1, int(round(usable / house.get_editor_property("DoorPitchTarget"))))
    pitch = usable / slots
    # 两拱之间的墩心 = 相邻两拱心的中点（= 第 k 个子段的分界）。取第一处。
    pier_s = margin + pitch
    pier_x = pier_s - half_x
    unreal.log("PIER wall: slots=%d pitch=%.1f first pier at S=%.1f (local x=%.1f)"
               % (slots, pitch, pier_s, pier_x))

    cams = [
        # A 整面墙：站在南墙外正对，三个拱与两处墩同框。
        ("wall", world_at(0.0, -half_y - 900.0, 250.0), world_at(0.0, -half_y, 170.0), 60.0),
        # B 贴脸看第一处墩：证明砖从拱圈连续砌到地面，接缝处看不出换料。
        ("pier", world_at(pier_x, -half_y - 400.0, 85.0), world_at(pier_x, -half_y, 85.0), 40.0),
    ]

    # 坑 ⑤：只翻回 GI/反射方法两项，其余交给世界 PPV（否则量的是脚本不是关卡）。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    for name, cam_loc, aim, fov in cams:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_loc, look_at(cam_loc, aim))
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", fov)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        comp.set_editor_property("post_process_settings", cap_pp)
        comp.set_editor_property("post_process_blend_weight", 1.0)
        comp.set_editor_property("always_persist_rendering_state", True)   # 坑 ⑦，与 ⑧ 缺一不可
        STATE["rigs"].append([name, comp, rt, 0, False])
        STATE["spawned"].append(cap)
        unreal.log("PIER cam %-5s at (%.0f, %.0f, %.0f) fov=%.0f" % (name, cam_loc.x, cam_loc.y, cam_loc.z, fov))
    return world


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    # 一帧只推进**一个**机位：几路同时连拍会互相抢帧，谁都攒不满自己的 Lumen 历史。
    for rig in STATE["rigs"]:
        name, comp, rt, shots, exported = rig
        if exported:
            continue
        comp.capture_scene()
        rig[3] = shots + 1
        if rig[3] >= WARMUP_CAPTURES:
            unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR,
                                                         "pier_%s_%s.png" % (TAG, name))
            rig[4] = True
            unreal.log("PIER shot: pier_%s_%s.png（预热 %d 帧）" % (TAG, name, rig[3]))
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("PIER DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception as exc:
    unreal.log_error("PIER FAILED in build(): %s" % exc)
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
