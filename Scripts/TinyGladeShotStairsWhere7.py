# -*- coding: utf-8 -*-
"""
第七轮定位：**是材质还是这条渲染路**让石阶不写深度。

第六轮的硬事实（藏掉地面网格后同机位测的）：
  · 石阶 216 块**全部画出来了**，位置与在回读原点上摆的 Cube 一致（bbox 逐像素对齐）；
  · 但石阶像素上的 `SCS_SceneDepth` 是 **100000000（远平面）**，
    而同一批位置上的 Cube 读到 **2529..3199 cm** 的真实距离。
⇒ 这条路**只写颜色不写深度**，于是后画的不透明物（地面）直接把它盖掉，
   只有背后是天空的那十几块能留下来。

本轮把「材质」与「渲染路」拆开：
  · `brick`   石阶用 M_TinyGladeBrick（现状）
  · `default` 石阶换成引擎默认材质（Opaque，且是 special engine material，permutation 一定编）
  · `ground`  石阶换成地面材质（地面**是**写深度的那条路）
  · `cubebrick` 普通 StaticMeshActor 摆在同样的原点上，但贴 M_TinyGladeBrick
                —— 若 Cube 仍然写深度，材质就被排除，问题在实例路本身。
全部**藏掉地面网格**拍，保证石阶不被遮挡，只看深度。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsWhere7.py"
"""
import math

import unreal

PKG = "/PCGPlugins/HouseTest"
STAIR_MESH = "/Game/TinyGlade/Meshes/stairs_step/StaticMeshes/stairs_step.stairs_step"
CUBE_MESH = "/Engine/BasicShapes/Cube.Cube"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900
STAIR_PX = [(590, 483), (606, 536), (880, 367), (949, 400), (1051, 509), (1134, 452), (1186, 565)]

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


def describe(mat, tag):
    if not mat:
        unreal.log("W7 material[%s] = None" % tag)
        return
    base = mat.get_base_material() if hasattr(mat, "get_base_material") else mat
    bits = []
    for prop in ("blend_mode", "shading_model", "two_sided", "dither_opacity_mask",
                 "allow_negative_emissive_color"):
        try:
            bits.append("%s=%s" % (prop, base.get_editor_property(prop)))
        except Exception:
            pass
    for prop in ("used_with_instanced_static_meshes", "used_with_static_lighting"):
        try:
            bits.append("%s=%s" % (prop, base.get_editor_property(prop)))
        except Exception:
            pass
    unreal.log("W7 material[%s] %s : %s" % (tag, base.get_name(), " ".join(bits)))


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
step_mesh = unreal.load_asset(STAIR_MESH)
cube_mesh = unreal.load_asset(CUBE_MESH)
brick_mat = unreal.load_asset("%s/M_TinyGladeBrick" % PKG)
default_mat = unreal.load_asset("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")
if not (ground and shaper and step_mesh and cube_mesh and brick_mat):
    unreal.log_error("W7 FAILED: demo actors or assets missing")
    raise SystemExit

try:
    ground_mat = ground.get_editor_property("GroundMaterial")
    describe(brick_mat, "brick")
    describe(default_mat, "default")
    describe(ground_mat, "ground")

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
    ground.set_editor_property("StairMaterial", brick_mat)
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
    unreal.log("W7 origins=%d" % count)

    ground_meshes = [cp for cp in ground.get_components_by_class(unreal.PrimitiveComponent)
                     if not isinstance(cp, unreal.CSGpuInstancedMeshComponent)]

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

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, wide, look_at(wide, aim))
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 60.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    spawned.append(cap)

    def hide_ground():
        for cp in ground_meshes:
            cp.set_visibility(False, False)

    def stairs_with(mat):
        def fn():
            hide_ground()
            ground.set_editor_property("StairMesh", step_mesh)
            ground.set_editor_property("StairMaterial", mat)
            ground.rebuild_stairs()
            hide_ground()
        return fn

    cubes = []

    def phase_cubebrick():
        hide_ground()
        ground.set_editor_property("StairMesh", None)
        ground.rebuild_stairs()
        hide_ground()
        for p in origins:
            m = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, p, unreal.Rotator(0, 0, 0))
            m.set_mobility(unreal.ComponentMobility.MOVABLE)
            m.static_mesh_component.set_static_mesh(cube_mesh)
            m.static_mesh_component.set_material(0, brick_mat)
            m.set_actor_scale3d(unreal.Vector(0.60, 1.00, 0.45))
            cubes.append(m)
            spawned.append(m)

    PHASES = [("brick", stairs_with(brick_mat)),
              ("default", stairs_with(default_mat)),
              ("gmat", stairs_with(ground_mat)),
              ("cubebrick", phase_cubebrick)]

    STEPS = []
    for pname, fn in PHASES:
        STEPS.append(("setup", fn, pname))
        STEPS.append(("shot", None, pname))

    FIRST_TICK = 24
    GAP = 10

    def tick(delta):
        STATE["ticks"] += 1
        Index = STATE["ticks"] - FIRST_TICK
        if Index >= 0 and Index // GAP < len(STEPS):
            kind, payload, label = STEPS[Index // GAP]
            Phase = Index % GAP
            if kind == "setup":
                if Phase == 0:
                    payload()
                    unreal.log("W7 phase: %s" % label)
            else:
                if Phase == 0:
                    unreal.EditorLevelLibrary.pilot_level_actor(cap)
                    unreal.EditorLevelLibrary.editor_invalidate_viewports()
                elif Phase == 4:
                    comp.capture_scene()
                    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "w7_%s.png" % label)
                elif Phase == 7:
                    depth_comp.capture_scene()
                    out = []
                    for (x, y) in STAIR_PX:
                        col = unreal.RenderingLibrary.read_render_target_raw_pixel(world, rt_depth, x, y, normalize=False)
                        out.append("(%d,%d)=%.1f" % (x, y, col.r))
                    unreal.log("W7 depth[%s] %s" % (label, "  ".join(out)))
        if STATE["ticks"] < FIRST_TICK + GAP * len(STEPS) + 8:
            return
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        for cp in ground_meshes:
            cp.set_visibility(True, False)
        ground.set_editor_property("StairMesh", None)
        ground.set_editor_property("StairMaterial", brick_mat)
        ground.reset_paint()
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("W7 DONE")
        unreal.SystemLibrary.quit_editor()

    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)

except Exception as e:
    unreal.log_error("W7 FAILED during setup: %s" % e)
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.SystemLibrary.quit_editor()
