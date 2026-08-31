# -*- coding: utf-8 -*-
"""
一次性诊断：**离屏 SceneCapture 上那十几块石阶到底是什么** —— 位置对不对、是不是这一趟扫出来的。

上一轮留下的最硬事实是「indirect args 的 InstanceCount = 216，出图逐像素不动」，
于是先不猜机制，只问三个可证伪的问题，全部用同机位出图回答：

  ① 那批块是不是 `UCSGpuInstancedMeshComponent` 画的？
     —— `StairMesh=None`（组件连显存一起销毁）后再拍同一张，块消失即是。
  ② 它们在不在**回读说的**位置上？
     —— 在 `DebugReadStairsSync` 回读的每个原点上摆一个白球（普通 StaticMeshActor，
        走的是完全正常的渲染路径），球与块是否重合一眼可见。
  ③ 画出来的是不是**这一趟 compaction 的产物**？
     —— 把 `StairZOffset` 抬 600 cm 重扫，块跟着抬 = 活的；不动 = 陈旧缓冲。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsWhere.py"

产物：Saved/TinyGladeShots/where_<phase>_<cam>.png + where_origins.txt
"""
import math

import unreal

PKG = "/PCGPlugins/HouseTest"
STAIR_MESH = "/Game/TinyGlade/Meshes/stairs_step/StaticMeshes/stairs_step.stairs_step"
MARKER_MESH = "/Engine/BasicShapes/Sphere.Sphere"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()

spawned = []
STATE = {"ticks": 0, "handle": None, "step": 0}


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


def read_origins(tag):
    res = ground.debug_read_stairs_sync()
    count, origins = (res[0], list(res[1])) if isinstance(res, tuple) else (res, [])
    if origins:
        unreal.log("WHERE[%s] count=%d bbox X %.0f..%.0f Y %.0f..%.0f Z %.0f..%.0f" % (
            tag, count,
            min(p.x for p in origins), max(p.x for p in origins),
            min(p.y for p in origins), max(p.y for p in origins),
            min(p.z for p in origins), max(p.z for p in origins)))
    else:
        unreal.log("WHERE[%s] count=%d (no origins)" % (tag, count))
    return origins


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
step_mesh = unreal.load_asset(STAIR_MESH)
marker_mesh = unreal.load_asset(MARKER_MESH)
if not (ground and shaper and step_mesh and marker_mesh):
    unreal.log_error("WHERE FAILED: demo actors or meshes missing")
    raise SystemExit

try:
    c = ground.get_actor_location()
    unreal.log("WHERE ground actor at (%.0f, %.0f, %.0f)" % (c.x, c.y, c.z))
    c = shaper.get_actor_location()
    unreal.log("WHERE shaper at (%.0f, %.0f, %.0f)" % (c.x, c.y, c.z))
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

    origins = read_origins("truth")
    with open(OUT_DIR + "/where_origins.txt", "w", encoding="utf-8") as f:
        for p in origins:
            f.write("%.2f %.2f %.2f\n" % (p.x, p.y, p.z))

    # ---- 白球标记：回读说的每一个原点摆一个，走完全正常的渲染路径 ----
    for p in origins:
        m = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, p, unreal.Rotator(0, 0, 0))
        m.set_mobility(unreal.ComponentMobility.MOVABLE)
        m.static_mesh_component.set_static_mesh(marker_mesh)
        m.set_actor_scale3d(unreal.Vector(0.30, 0.30, 0.30))
        spawned.append(m)
    unreal.log("WHERE markers spawned: %d" % len(origins))

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
    top = unreal.Vector(junction_x, c.y, 2600.0)
    side = unreal.Vector(junction_x + 2200.0, c.y + 1900.0, 1300.0)

    CAMS = [
        ("wide", wide, look_at(wide, aim)),
        ("top", top, unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0)),
        ("side", side, look_at(side, aim)),
    ]

    RIGS = []
    for name, cam_loc, cam_rot in CAMS:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_loc, cam_rot)
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", 60.0)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        RIGS.append((name, cap, comp, rt))
        spawned.append(cap)

    def phase_truth():
        unreal.log("WHERE phase: truth (stairs on, ZOffset 0)")

    def phase_zoff():
        ground.set_editor_property("StairZOffset", 600.0)
        ground.rebuild_stairs()
        read_origins("zoff")

    def phase_off():
        ground.set_editor_property("StairZOffset", 0.0)
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()
        unreal.log("WHERE phase: off (StairMesh cleared)")

    STEPS = []
    for pname, fn in (("truth", phase_truth), ("zoff", phase_zoff), ("off", phase_off)):
        STEPS.append(("setup", fn, pname))
        for cname, cap, comp, rt in RIGS:
            STEPS.append(("shot", (cap, comp, rt), "%s_%s" % (pname, cname)))

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
                cap, comp, rt = payload
                if Phase == 0:
                    unreal.EditorLevelLibrary.pilot_level_actor(cap)
                    unreal.EditorLevelLibrary.editor_invalidate_viewports()
                elif Phase == 4:
                    unreal.log("WHERE trace-begin %s" % label)
                    unreal.SystemLibrary.execute_console_command(world, "cs.GpuInstanced.Verbose 1")
                    comp.capture_scene()
                    unreal.SystemLibrary.execute_console_command(world, "cs.GpuInstanced.Verbose 0")
                    unreal.log("WHERE trace-end %s" % label)
                    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "where_%s.png" % label)
                    unreal.log("WHERE shot: where_%s.png" % label)
        if STATE["ticks"] < FIRST_TICK + GAP * len(STEPS) + 8:
            return
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        ground.set_editor_property("StairMesh", None)
        ground.reset_paint()
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("WHERE DONE")
        unreal.SystemLibrary.quit_editor()

    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)

except Exception as e:
    unreal.log_error("WHERE FAILED during setup: %s" % e)
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.SystemLibrary.quit_editor()
