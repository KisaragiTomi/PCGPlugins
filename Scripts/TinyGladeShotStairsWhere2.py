# -*- coding: utf-8 -*-
"""
第二轮定位：`TinyGladeShotStairsWhere.py` 已证明**石阶在离屏捕获里画得好好的** ——
`StairZOffset=+600` 时 216 块全部出现在天上，图案与回读原点上的白球一一对应；
`StairZOffset=0` 时同机位与"关掉石阶"**逐像素相同**（side 机位 0 px 差异）。
也就是说：不是画不出来，是**画出来被埋住了**。

本脚本只回答"埋多深、被谁埋的"，全部用同机位的「石阶像素数」曲线：

  · `off`          基准（StairMesh=None）
  · `z0..z600`     ZOffset 扫描 —— 石阶像素数从 0 变成几万的那个台阶就是埋深
  · `emb-200`      StairEmbed 取负（沿坡向外挑出）—— 判定是不是"扎进坡里"造成的
  · `tall`         StairBlockSize.Z=300 —— 块够高就该露头，判定是不是纯粹的高度不足
  · `cube`         关掉石阶，在回读原点上摆 60x100x45 的 Cube —— 正确摆位长什么样

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsWhere2.py"

产物：Saved/TinyGladeShots/w2_<phase>_<cam>.png
"""
import math

import unreal

PKG = "/PCGPlugins/HouseTest"
STAIR_MESH = "/Game/TinyGlade/Meshes/stairs_step/StaticMeshes/stairs_step.stairs_step"
CUBE_MESH = "/Engine/BasicShapes/Cube.Cube"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()

spawned = []
cubes = []
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


def read_origins(tag):
    res = ground.debug_read_stairs_sync()
    count, origins = (res[0], list(res[1])) if isinstance(res, tuple) else (res, [])
    if origins:
        unreal.log("W2[%s] count=%d bbox X %.0f..%.0f Y %.0f..%.0f Z %.0f..%.0f" % (
            tag, count,
            min(p.x for p in origins), max(p.x for p in origins),
            min(p.y for p in origins), max(p.y for p in origins),
            min(p.z for p in origins), max(p.z for p in origins)))
    else:
        unreal.log("W2[%s] count=%d (no origins)" % (tag, count))
    return origins


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
step_mesh = unreal.load_asset(STAIR_MESH)
cube_mesh = unreal.load_asset(CUBE_MESH)
if not (ground and shaper and step_mesh and cube_mesh):
    unreal.log_error("W2 FAILED: demo actors or meshes missing")
    raise SystemExit

try:
    box = step_mesh.get_bounding_box()
    unreal.log("W2 stair mesh local box min (%.2f,%.2f,%.2f) max (%.2f,%.2f,%.2f)" % (
        box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z))

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
    base_origins = read_origins("base")
    for p in base_origins[:8]:
        unreal.log("W2   origin (%.1f, %.1f, %.1f)  ground height there %.1f" % (
            p.x, p.y, p.z, ground.sample_height(unreal.Vector2D(p.x, p.y))))

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
    side = unreal.Vector(junction_x + 2200.0, c.y + 1900.0, 1300.0)

    RIGS = []
    for name, cam_loc, cam_rot in (("wide", wide, look_at(wide, aim)), ("side", side, look_at(side, aim))):
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

    def make_setup(tag, **props):
        def fn():
            for k, v in props.items():
                ground.set_editor_property(k, v)
            ground.rebuild_stairs()
            read_origins(tag)
        return fn

    def phase_off():
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()
        unreal.log("W2[off] stairs cleared")

    def phase_cube():
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()
        for p in base_origins:
            m = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, p, unreal.Rotator(0, 0, 0))
            m.set_mobility(unreal.ComponentMobility.MOVABLE)
            m.static_mesh_component.set_static_mesh(cube_mesh)
            m.set_actor_scale3d(unreal.Vector(0.60, 1.00, 0.45))
            cubes.append(m)
            spawned.append(m)
        unreal.log("W2[cube] %d cubes at readback origins" % len(cubes))

    PHASES = [
        ("off", phase_off),
        ("z0", make_setup("z0", StairMesh=step_mesh, StairZOffset=0.0, StairEmbed=10.0,
                          StairBlockSize=unreal.Vector(60.0, 100.0, 45.0))),
        ("z15", make_setup("z15", StairZOffset=15.0)),
        ("z30", make_setup("z30", StairZOffset=30.0)),
        ("z60", make_setup("z60", StairZOffset=60.0)),
        ("z120", make_setup("z120", StairZOffset=120.0)),
        ("z240", make_setup("z240", StairZOffset=240.0)),
        ("emb", make_setup("emb", StairZOffset=0.0, StairEmbed=-200.0)),
        ("tall", make_setup("tall", StairEmbed=10.0, StairBlockSize=unreal.Vector(60.0, 100.0, 300.0))),
        ("cube", phase_cube),
    ]

    STEPS = []
    for pname, fn in PHASES:
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
                    comp.capture_scene()
                    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "w2_%s.png" % label)
                    unreal.log("W2 shot: w2_%s.png" % label)
        if STATE["ticks"] < FIRST_TICK + GAP * len(STEPS) + 8:
            return
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        ground.set_editor_property("StairMesh", None)
        ground.set_editor_property("StairZOffset", 0.0)
        ground.set_editor_property("StairEmbed", 10.0)
        ground.set_editor_property("StairBlockSize", unreal.Vector(60.0, 100.0, 45.0))
        ground.reset_paint()
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("W2 DONE")
        unreal.SystemLibrary.quit_editor()

    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)

except Exception as e:
    unreal.log_error("W2 FAILED during setup: %s" % e)
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.SystemLibrary.quit_editor()
