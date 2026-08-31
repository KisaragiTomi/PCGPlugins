# -*- coding: utf-8 -*-
"""
第四轮定位：石阶**只在天空背景上看得见**，凡是地形挡在后面的一律不出现 —— 它输的是**深度测试**。

前三轮定死的事实：
  · 回读原点正确；在同一批原点上摆 216 个 60x100x45 的 Cube，wide 89948 px / side 97321 px；
  · 同一批实例走 GPU 实例路，z0 时 wide 3021 px / side 9 px / 正俯视 0 px；
  · 每一次离屏捕获都**确实**跑了一次 cull（视角正确、bFrustumCull=0）并**确实**发了 draw
    （`[CSGpuInstancedTrace]` 逐视图日志，visible=1）—— 不是没画，是画了看不见；
  · `StairZOffset=+600` 把它们抬到天上（背景是天空）之后 **216 块全部出现**，位置与
    「原点 + 600」完全吻合 ⇒ 变换与原点都是对的；
  · 把 `StairBlockSize.Z` 加到 300，也只在地形轮廓线**外侧**露出柱子。

于是只剩一个可证伪的猜想：**这条路的深度与颜色对不上**（early-Z 预通道画在别处 / 基通道
的 depth 比较模式对不上），凡是背后有不透明像素的地方就被判成"被遮挡"。

判据（同机位、只改一个控制台开关）：
  · `r.EarlyZPass 0`  关掉深度预通道。石阶像素数若跳到 Cube 的量级 ⇒ 猜想成立。
  · `r.Fog 0`         排除"只是被大气/雾染成天空色"这种误读。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsWhere4.py"

产物：Saved/TinyGladeShots/w4_<phase>_<cam>.png
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
STATE = {"ticks": 0, "handle": None}


def cmd(c):
    unreal.SystemLibrary.execute_console_command(world, c)
    unreal.log("W4 cmd: %s" % c)


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


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
step_mesh = unreal.load_asset(STAIR_MESH)
cube_mesh = unreal.load_asset(CUBE_MESH)
if not (ground and shaper and step_mesh and cube_mesh):
    unreal.log_error("W4 FAILED: demo actors or meshes missing")
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
    unreal.log("W4 origins=%d" % count)

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

    def phase_off():
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()

    def phase_base():
        ground.set_editor_property("StairMesh", step_mesh)
        ground.rebuild_stairs()

    def phase_noearlyz():
        cmd("r.EarlyZPass 0")

    def phase_earlyz2():
        cmd("r.EarlyZPass 2")

    def phase_nofog():
        cmd("r.EarlyZPass 1")
        cmd("r.Fog 0")

    def phase_cube():
        cmd("r.Fog 1")
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()
        for p in origins:
            m = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, p, unreal.Rotator(0, 0, 0))
            m.set_mobility(unreal.ComponentMobility.MOVABLE)
            m.static_mesh_component.set_static_mesh(cube_mesh)
            m.set_actor_scale3d(unreal.Vector(0.60, 1.00, 0.45))
            spawned.append(m)

    STEPS = []
    for pname, fn in (("off", phase_off), ("base", phase_base), ("noearlyz", phase_noearlyz),
                      ("earlyz2", phase_earlyz2), ("nofog", phase_nofog), ("cube", phase_cube)):
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
                    unreal.log("W4 phase: %s" % label)
            else:
                cap, comp, rt = payload
                if Phase == 0:
                    unreal.EditorLevelLibrary.pilot_level_actor(cap)
                    unreal.EditorLevelLibrary.editor_invalidate_viewports()
                elif Phase == 4:
                    comp.capture_scene()
                    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "w4_%s.png" % label)
                    unreal.log("W4 shot: w4_%s.png" % label)
        if STATE["ticks"] < FIRST_TICK + GAP * len(STEPS) + 8:
            return
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        cmd("r.EarlyZPass 3")
        cmd("r.Fog 1")
        ground.set_editor_property("StairMesh", None)
        ground.reset_paint()
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("W4 DONE")
        unreal.SystemLibrary.quit_editor()

    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)

except Exception as e:
    unreal.log_error("W4 FAILED during setup: %s" % e)
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.SystemLibrary.quit_editor()
