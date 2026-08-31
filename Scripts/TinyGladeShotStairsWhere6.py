# -*- coding: utf-8 -*-
"""
第六轮定位：**把地面网格藏起来**，看石阶是不是「只是被地形挡住」。

第五轮的场景深度回读（`SCS_SceneDepth`，RTF_RGBA32F，normalize=False）给出的硬事实：
在**石阶明明画出了颜色**的那些像素上，深度是 100000000（远平面，与天空同值），
而且开关石阶前后**逐点相同** ⇒ 这条路的基通道**不写深度**（有 early-Z 预通道时这是正常的），
但预通道里**也没有它** ⇒ 石阶的可见性完全由「基通道 DepthNearOrEqual 对着别人写的深度」决定。

于是最后一个二选一：
  甲：块画在对的位置，只是被地形抢先写了更近的深度 ⇒ 藏掉地面网格后 216 块应当全部出现在对的位置；
  乙：块画在别处 ⇒ 藏掉地面后它们会出现在**与 Cube 参照不同**的地方。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsWhere6.py"

产物：Saved/TinyGladeShots/w6_<phase>_<cam>.png
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
    unreal.log_error("W6 FAILED: demo actors or meshes missing")
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
    unreal.log("W6 origins=%d" % count)

    # 地面网格组件（石阶组件是它的兄弟，不受影响）
    ground_meshes = [cp for cp in ground.get_components_by_class(unreal.PrimitiveComponent)
                     if not isinstance(cp, unreal.CSGpuInstancedMeshComponent)]
    unreal.log("W6 ground primitive components to hide: %s"
               % ", ".join("%s(%s)" % (cp.get_name(), cp.get_class().get_name()) for cp in ground_meshes))

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

    # 深度探针：与 wide 同机位，SCS_SceneDepth。石阶像素上读到 1e8 ⇒ 这条路不写深度。
    STAIR_PX = [(590, 483), (606, 536), (880, 367), (949, 400), (1051, 509), (1134, 452), (1186, 565)]
    rt_depth = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA32F, unreal.LinearColor(0, 0, 0, 1), False)
    depth_cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, wide, look_at(wide, aim))
    depth_comp = depth_cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    depth_comp.set_editor_property("texture_target", rt_depth)
    depth_comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_SCENE_DEPTH)
    depth_comp.set_editor_property("fov_angle", 60.0)
    depth_comp.set_editor_property("capture_every_frame", False)
    depth_comp.set_editor_property("capture_on_movement", False)
    spawned.append(depth_cap)

    def sample_depth(tag):
        depth_comp.capture_scene()
        out = []
        for (x, y) in STAIR_PX:
            c = unreal.RenderingLibrary.read_render_target_raw_pixel(world, rt_depth, x, y, normalize=False)
            out.append("(%d,%d)=%.1f" % (x, y, c.r))
        unreal.log("W6 depth[%s] %s" % (tag, "  ".join(out)))

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

    def hide_ground():
        for cp in ground_meshes:
            cp.set_visibility(False, False)

    def phase_gh_off():
        hide_ground()
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()

    def phase_gh_on():
        hide_ground()
        ground.set_editor_property("StairMesh", step_mesh)
        ground.rebuild_stairs()
        hide_ground()   # 重建可能重新注册组件

    def phase_gh_cube():
        hide_ground()
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()
        hide_ground()
        for p in origins:
            m = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, p, unreal.Rotator(0, 0, 0))
            m.set_mobility(unreal.ComponentMobility.MOVABLE)
            m.static_mesh_component.set_static_mesh(cube_mesh)
            m.set_actor_scale3d(unreal.Vector(0.60, 1.00, 0.45))
            spawned.append(m)

    STEPS = []
    for pname, fn in (("gh_off", phase_gh_off), ("gh_on", phase_gh_on), ("gh_cube", phase_gh_cube)):
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
                    unreal.log("W6 phase: %s" % label)
            else:
                cap, comp, rt = payload
                if Phase == 0:
                    unreal.EditorLevelLibrary.pilot_level_actor(cap)
                    unreal.EditorLevelLibrary.editor_invalidate_viewports()
                elif Phase == 4:
                    comp.capture_scene()
                    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "w6_%s.png" % label)
                    unreal.log("W6 shot: w6_%s.png" % label)
                    if label.endswith("_wide"):
                        sample_depth(label)
        if STATE["ticks"] < FIRST_TICK + GAP * len(STEPS) + 8:
            return
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        for cp in ground_meshes:
            cp.set_visibility(True, False)
        ground.set_editor_property("StairMesh", None)
        ground.reset_paint()
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("W6 DONE")
        unreal.SystemLibrary.quit_editor()

    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)

except Exception as e:
    unreal.log_error("W6 FAILED during setup: %s" % e)
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.SystemLibrary.quit_editor()
