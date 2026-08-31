# -*- coding: utf-8 -*-
"""
一次性诊断：**离屏 SceneCapture 只画出十几块石阶** 的真因是不是「生命周期中段改容量」。

假设（来自调查，附代码证据）：`RunCulling` 有 `AllocationGeneration` 守卫
（`CSGpuInstancedMeshSceneProxy.cpp:406-410`），`GetDynamicMeshElements` **没有**（`:642-643`）。
容量在中段变化（`MaxStairInstances` 4096 → 8192 ⇒ `ResizeStreamsSync` 把六条 aux 流整套重分配）
之后，画侧继续用 `DrawDesc`/顶点工厂缓存下来的**裸 RHI 指针**，画的是一套没人再写的旧缓冲 ——
症状恰好是"固定世界位置的一小撮块、与相机无关、与所有剔除开关无关"。

判据只有一条，可证伪：**全程不改容量**（只让它分配一次），其余与 `TinyGladeShotStairsJitter.py`
逐字相同。石阶要是画出来了，假设成立；仍然只有十几块，假设被推翻。

## ❌ 实测结论（2026-08-30）：**假设被推翻，别再走这条**

日志只有一次 `stair instance source handed over (capacity=4096)`（确认没有中段重分配），
回读 216 级，出图 `stairs_probe_nogrow.png` **仍然是同一小撮十几块、位置与之前逐帧相同**。
所以「中段改容量 ⇒ 画侧拿着陈旧裸指针」**不是**真因。

这个脚本留着当反证的存档 —— 下一轮如果又想到"是不是容量变化搞的"，这里已经有答案了。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsProbe.py"

产物：Saved/TinyGladeShots/stairs_probe_nogrow.png
"""
import math

import unreal

PKG = "/PCGPlugins/HouseTest"
STAIR_MESH = "/Game/TinyGlade/Meshes/stairs_step/StaticMeshes/stairs_step.stairs_step"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()


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
if not (ground and shaper and step_mesh):
    unreal.log_error("PROBE FAILED: demo actors or step mesh missing")
    raise SystemExit

c = shaper.get_actor_location()
junction_x = c.x + 400.0
spawned = []

second = ACTORS.spawn_actor_from_class(
    unreal.load_class(None, "%s/BP_GroundShaper.BP_GroundShaper_C" % PKG),
    unreal.Vector(c.x + 800.0, c.y, 0.0))
if second:
    for name, value in (("Ground", ground), ("Radius", 300.0), ("FalloffDistance", 400.0),
                        ("LiftHeight", 300.0)):
        second.set_editor_property(name, value)
    second.rebuild_terrain()
    spawned.append(second)

# ⚠️ 本探针的**全部内容**就是这一行：容量在建组件**之前**定死，此后一个字都不改。
# 对照脚本是在石阶跑起来之后才写 8192 —— 那一下会把六条 aux 流整套重分配。
ground.set_editor_property("MaxStairInstances", 4096)

ground.set_editor_property("StairMesh", step_mesh)
ground.set_editor_property("StairMaterial", unreal.load_asset("%s/M_TinyGladeBrick" % PKG))
ground.set_editor_property("StairStepHeight", 30.0)
ground.set_editor_property("StairCellSize", 100.0)
ground.set_editor_property("StairBlockSize", unreal.Vector(60.0, 100.0, 45.0))
ground.set_editor_property("StairEmbed", 10.0)
ground.set_editor_property("BrushRadius", 260.0)

# 注记：这里原来还要关掉塑形物自持的旧路石阶（`StepMeshes` + `rebuild_steps`），
# 免得两条路的石阶叠在一起分不清是谁摆的。旧路已随 2026-08-30「裁决一」第二步整条删除，
# 场上只剩地面自己这一条，那几行（连同收尾的写回）一并去掉。

ground.reset_paint()
paint_line(ground, c.x - 900.0, c.y, c.x + 1700.0, c.y, 26)
paint_line(ground, junction_x, c.y - 900.0, junction_x, c.y + 900.0, 18)
ground.rebuild_stairs()

res = ground.debug_read_stairs_sync()
count, origins = (res[0], list(res[1])) if isinstance(res, tuple) else (res, [])
unreal.log("PROBE gpu stairs scattered: %d" % count)

wide = unreal.Vector(junction_x - 1500.0, c.y - 1700.0, 1150.0)
rot = look_at(wide, unreal.Vector(junction_x, c.y, 150.0))

sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000),
                                    unreal.Rotator(roll=0.0, pitch=-42.0, yaw=140.0))
sun.light_component.set_editor_property("intensity", 6.0)
spawned.append(sun)
spawned.append(ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)))
sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(c.x, c.y, 1500))
sky.light_component.set_editor_property("intensity", 1.0)
sky.light_component.set_editor_property("real_time_capture", True)
spawned.append(sky)

rt = unreal.RenderingLibrary.create_render_target2d(
    world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, wide, rot)
comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property("texture_target", rt)
comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property("fov_angle", 60.0)
comp.set_editor_property("capture_every_frame", False)
comp.set_editor_property("capture_on_movement", False)
spawned.append(cap)

STATE = {"ticks": 0, "handle": None}


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] == 24:
        unreal.EditorLevelLibrary.pilot_level_actor(cap)
        unreal.EditorLevelLibrary.editor_invalidate_viewports()
    elif STATE["ticks"] == 30:
        comp.capture_scene()
        unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "stairs_probe_nogrow.png")
        unreal.log("shot: stairs_probe_nogrow.png")
    elif STATE["ticks"] > 38:
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        ground.set_editor_property("StairMesh", None)
        ground.reset_paint()
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("PROBE DONE")
        unreal.SystemLibrary.quit_editor()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
