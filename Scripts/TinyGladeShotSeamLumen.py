# -*- coding: utf-8 -*-
"""板缝纯黑第三轮：黑的是**地面**，而且只在 **Lumen** 那条路上黑。

前两轮已经判死的事实（别再重查）：

  · 缝里的 `SCS_BASE_COLOR` 与"整条岩壳关掉"的对照图 **100% 逐像素相同** ⇒ 缝里画的是**地面**；
  · 把岩壳整条关掉，同机位精确 (0,0,0) 从 17.60% **涨到** 41.56% ⇒ 岩壳是**遮住**黑的那一方，
    不是造成黑的那一方；
  · 岩壳材质 `MI_rocky_terrain` 的母材质是 `MSM_Unlit` —— 壳根本不参与光照，
    所以"壳亮、地黑"从来就不是同一条光路的两个结果；
  · **同机位、只把捕获里那条"把 GI 翻回 Lumen"的覆盖去掉，精确零 17.25% → 0.00%。**

本轮回答最后一个岔口：Lumen 给出的零是**收敛不足**（单帧抓拍）还是**结构性的零**
（gpumesh 进不了 Lumen 场景）。四个对照各只动一件事：

  A `l_oneshot`    Lumen + 单帧 `capture_scene()`（= 现有出图脚本的做法，基线）
  B `l_converged`  Lumen + `capture_every_frame=True` 连拍到收敛 ⇒ 若黑消失，是收敛问题
  C `l_leak`       Lumen + `LumenSkylightLeaking=1.0` / 距离拉满 ⇒ 若黑消失，一个 PPV 旋钮就够
  D `no_lumen`     GI = None（引擎在离屏 capture 里的默认）⇒ 对照组，已知 0.00%

用法（真编辑器 + tick）::

    UnrealEditor.exe <uproject> -ExecCmds="py <本文件>" -nosplash -abslog=<独立日志>
"""
import math

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "jobs": [], "spawned": [], "ground": None, "world": None,
         "loc": None, "rot": None, "converged": None}


def log(msg):
    unreal.log("SEAMLUMEN %s" % msg)


def look_at(frm, to):
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def make_capture(world, name, gi, every_frame=False, leak=None):
    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, STATE["loc"], STATE["rot"])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", every_frame)
    comp.set_editor_property("capture_on_movement", False)
    if gi:
        # 坑 ⑤：离屏 capture 被引擎在 blend 完世界 PPV **之后**写死 GI = None，只能这样翻回来。
        pp = unreal.PostProcessSettings()
        pp.set_editor_property("override_dynamic_global_illumination_method", True)
        pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
        pp.set_editor_property("override_reflection_method", True)
        pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
        if leak is not None:
            pp.set_editor_property("override_lumen_skylight_leaking", True)
            pp.set_editor_property("lumen_skylight_leaking", leak)
            try:
                pp.set_editor_property("override_lumen_skylight_leaking_distance", True)
                pp.set_editor_property("lumen_skylight_leaking_distance", 100000.0)
            except Exception as exc:
                log("leaking_distance 写不进去: %s" % exc)
        comp.set_editor_property("post_process_settings", pp)
        comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)   # 坑 ⑧：没它曝光不生效
    STATE["spawned"].append(cap)
    return comp, rt


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = next((a for a in ACTORS.get_all_level_actors()
                   if "Ground" in a.get_class().get_name() and "Shaper" not in a.get_class().get_name()), None)
    shapers = [a for a in ACTORS.get_all_level_actors() if "Shaper" in a.get_class().get_name()]
    if not ground or not shapers:
        unreal.log_error("SEAMLUMEN FAILED: ground=%s shapers=%d" % (bool(ground), len(shapers)))
        return None
    STATE["ground"] = ground
    hub = shapers[0].get_actor_location()

    sun_yaw = 0.0
    for a in ACTORS.get_all_level_actors():
        if a.get_class().get_name() == "DirectionalLight":
            sun_yaw = a.get_actor_rotation().yaw
            break
    sx, sy = math.cos(math.radians(sun_yaw)), math.sin(math.radians(sun_yaw))

    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(41):
        t = -1600.0 + i * 80.0
        x, y = hub.x + sx * t, hub.y + sy * t
        ground.apply_paint_stroke(unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y))))
    ground.end_paint_stroke()

    def at(along, up):
        x, y = hub.x + sx * along, hub.y + sy * along
        return unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y)) + up)

    STATE["loc"] = at(2600.0, 260.0)
    STATE["rot"] = look_at(STATE["loc"], at(0.0, -120.0))

    # 连拍那一路从第一帧就开着，让 Lumen 的屏幕探针 / 辐照缓存有历史可用。
    STATE["converged"] = make_capture(world, "seamlumen_l_converged", gi=True, every_frame=True)
    STATE["jobs"].append(("seamlumen_l_oneshot", make_capture(world, "seamlumen_l_oneshot", gi=True)))
    STATE["jobs"].append(("seamlumen_l_leak", make_capture(world, "seamlumen_l_leak", gi=True, leak=1.0)))
    STATE["jobs"].append(("seamlumen_no_lumen", make_capture(world, "seamlumen_no_lumen", gi=False)))
    return world


FIRST_TICK = 150      # 连拍那一路要足够多帧收敛，其余的在这之后才拍
GAP = 12


def tick(delta):
    STATE["ticks"] += 1
    index = STATE["ticks"] - FIRST_TICK
    if index >= 0 and index // GAP < len(STATE["jobs"]):
        name, (comp, rt) = STATE["jobs"][index // GAP]
        if index % GAP == 8:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR, "%s.png" % name)
            log("shot: %s.png (tick %d)" % (name, STATE["ticks"]))
    if STATE["ticks"] < FIRST_TICK + GAP * len(STATE["jobs"]) + 12:
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    try:
        comp, rt = STATE["converged"]
        unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR, "seamlumen_l_converged.png")
        log("shot: seamlumen_l_converged.png (连拍 %d 帧)" % STATE["ticks"])
        STATE["ground"].reset_paint()
        for a in STATE["spawned"]:
            ACTORS.destroy_actor(a)
    except Exception as exc:
        unreal.log_error("SEAMLUMEN teardown: %s" % exc)
    log("DONE")
    unreal.SystemLibrary.quit_editor()


try:
    STATE["world"] = build()
except Exception as exc:
    import traceback
    unreal.log_error("SEAMLUMEN FAILED in build(): %s\n%s" % (exc, traceback.format_exc()))
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
