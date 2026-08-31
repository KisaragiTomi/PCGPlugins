# -*- coding: utf-8 -*-
"""
第五轮定位：石阶像素的**场景深度到底是多少**。

第四轮已排除 early-Z（`r.EarlyZPass 0/2` 与默认逐像素相同，石阶像素数 3021 / 3022 / 3022）。
剩下的模型只有一个：**屏幕位置与大小都对，深度却远得离谱** —— 于是凡是背后有不透明像素的
地方它都输掉深度测试，只在天空背景上露出来。这个模型能同时解释：
  · 只在地形轮廓线外侧看得见；
  · 正俯视（画面全是地形）一块都看不见；
  · 抬到天上（背景是天空）216 块全出现，且位置与「原点 + ZOffset」吻合。

判据：同机位拍一张 `SCS_SceneDepth`（R32f），在**已知的石阶像素**上读深度，
与「相机到该石阶原点的真实距离」比。相等 ⇒ 模型被推翻；大出一大截 ⇒ 模型成立，比值就是倍数。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsWhere5.py"
"""
import math

import unreal

PKG = "/PCGPlugins/HouseTest"
STAIR_MESH = "/Game/TinyGlade/Meshes/stairs_step/StaticMeshes/stairs_step.stairs_step"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

# w4_base_wide.png 与 w4_off_wide.png 的逐像素差（>12）里取的样本 —— 这些像素上有且只有石阶。
STAIR_PX = [(480, 370), (496, 363), (505, 347), (515, 342), (526, 337), (541, 314),
            (556, 317), (562, 304), (585, 309), (601, 296), (622, 301), (643, 287)]
REF_PX = [(100, 60), (800, 700), (1200, 500)]   # 天空 / 地形 / 地形，做标定

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()

spawned = []
STATE = {"ticks": 0, "handle": None}


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def paint_line(ground, x0, y0, x1, y1, steps):
    ground.begin_paint_stroke()
    for i in range(steps + 1):
        t = float(i) / steps
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        ground.apply_paint_stroke(unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y))))
    ground.end_paint_stroke()


def look_at(frm, to):
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def sample(rt, tag):
    out = []
    for (x, y) in STAIR_PX + REF_PX:
        try:
            c = unreal.RenderingLibrary.read_render_target_raw_pixel(world, rt, x, y, normalize=False)
            out.append("(%d,%d)=%.1f" % (x, y, c.r))
        except Exception as e:
            out.append("(%d,%d)=ERR %s" % (x, y, e))
    unreal.log("W5 depth[%s] %s" % (tag, "  ".join(out)))


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
step_mesh = unreal.load_asset(STAIR_MESH)
if not (ground and shaper and step_mesh):
    unreal.log_error("W5 FAILED: demo actors or mesh missing")
    raise SystemExit

try:
    c = shaper.get_actor_location()
    junction_x = c.x + 400.0

    second = ACTORS.spawn_actor_from_class(
        unreal.load_class(None, "%s/BP_GroundShaper.BP_GroundShaper_C" % PKG),
        unreal.Vector(c.x + 800.0, c.y, 0.0))
    if second:
        for name, value in (("Ground", ground), ("Radius", 300.0), ("FalloffDistance", 400.0),
                            ("LiftHeight", 300.0)):
            second.set_editor_property(name, value)
        second.rebuild_terrain()
        spawned.append(second)

    ground.set_editor_property("StairMesh", step_mesh)
    ground.set_editor_property("StairMaterial", unreal.load_asset("%s/M_TinyGladeBrick" % PKG))
    ground.set_editor_property("StairStepHeight", 30.0)
    ground.set_editor_property("StairCellSize", 100.0)
    ground.set_editor_property("StairBlockSize", unreal.Vector(60.0, 100.0, 45.0))
    ground.set_editor_property("StairEmbed", 10.0)
    ground.set_editor_property("StairZOffset", 0.0)
    ground.set_editor_property("BrushRadius", 260.0)

    # 注记：这里原来还要关掉塑形物自持的旧路石阶（`StepMeshes` + `rebuild_steps`），
    # 免得两条路的石阶叠在一起分不清是谁摆的。旧路已随 2026-08-30「裁决一」第二步整条删除，
    # 场上只剩地面自己这一条，那几行（连同收尾的写回）一并去掉。

    ground.reset_paint()
    paint_line(ground, c.x - 900.0, c.y, c.x + 1700.0, c.y, 26)
    paint_line(ground, junction_x, c.y - 900.0, junction_x, c.y + 900.0, 18)

    for comp in ground.get_components_by_class(unreal.CSGpuInstancedMeshComponent):
        comp.set_editor_property("bGpuFrustumCulling", False)
    ground.set_editor_property("MaxStairInstances", 8192)
    ground.rebuild_stairs()

    res = ground.debug_read_stairs_sync()
    count, origins = (res[0], list(res[1])) if isinstance(res, tuple) else (res, [])

    sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000),
                                        unreal.Rotator(roll=0.0, pitch=-42.0, yaw=140.0))
    sun.light_component.set_editor_property("intensity", 6.0)
    spawned.append(sun)
    spawned.append(ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)))
    sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(c.x, c.y, 1500))
    sky.light_component.set_editor_property("intensity", 1.0)
    sky.light_component.set_editor_property("real_time_capture", True)
    spawned.append(sky)

    aim = unreal.Vector(junction_x, c.y, 150.0)
    wide = unreal.Vector(junction_x - 1900.0, c.y - 2100.0, 1500.0)
    # 相机到最近 / 最远石阶原点的真实距离，作为深度的对照。
    if origins:
        d = sorted(math.sqrt((p.x - wide.x) ** 2 + (p.y - wide.y) ** 2 + (p.z - wide.z) ** 2) for p in origins)
        unreal.log("W5 camera (%.0f,%.0f,%.0f)  true distance to stair origins: min %.0f  median %.0f  max %.0f"
                   % (wide.x, wide.y, wide.z, d[0], d[len(d) // 2], d[-1]))

    rt_depth = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA32F, unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, wide, look_at(wide, aim))
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt_depth)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_SCENE_DEPTH)
    comp.set_editor_property("fov_angle", 60.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    spawned.append(cap)

    def phase_on():
        ground.set_editor_property("StairMesh", step_mesh)
        ground.rebuild_stairs()

    def phase_off():
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()

    STEPS = [("setup", phase_on, "on"), ("shot", None, "on"),
             ("setup", phase_off, "off"), ("shot", None, "off")]

    FIRST_TICK = 24
    GAP = 8

    def tick(delta):
        STATE["ticks"] += 1
        Index = STATE["ticks"] - FIRST_TICK
        if Index >= 0 and Index // GAP < len(STEPS):
            kind, payload, label = STEPS[Index // GAP]
            Phase = Index % GAP
            if kind == "setup":
                if Phase == 0:
                    payload()
            else:
                if Phase == 0:
                    unreal.EditorLevelLibrary.pilot_level_actor(cap)
                    unreal.EditorLevelLibrary.editor_invalidate_viewports()
                elif Phase == 4:
                    comp.capture_scene()
                elif Phase == 6:
                    sample(rt_depth, label)
        if STATE["ticks"] < FIRST_TICK + GAP * len(STEPS) + 8:
            return
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        ground.set_editor_property("StairMesh", None)
        ground.reset_paint()
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("W5 DONE")
        unreal.SystemLibrary.quit_editor()

    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)

except Exception as e:
    unreal.log_error("W5 FAILED during setup: %s" % e)
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.SystemLibrary.quit_editor()
