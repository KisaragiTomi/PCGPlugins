# -*- coding: utf-8 -*-
"""
石阶 S2（逐实例抖动）的**同机位**对照图：抖动关 vs 抖动开。

为什么要单独一个脚本而不是把 `TinyGladeShotStairs.py` 跑两遍：机位是从**实测的石阶位置**
反算的，而 S2 的 Z 抖动会把石阶原点抖开 ±SizeJitter × StairRise ⇒ 两次跑出来的机位不同，
拍出来的两张图就不能逐像素比。这里把机位在**抖动关**的那一趟里算死，两趟共用。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairsJitter.py"

产物：Saved/TinyGladeShots/stairs_s1_*.png（无抖动） / stairs_s2_*.png（有抖动）

实测陷阱与 `TinyGladeShotStairs.py` 逐条相同（Rotator 参数序、必须真编辑器 + `-ExecCmds`、
离屏 SceneCapture2D 而不是 HighResShot、`RTF_RGBA8`）。

⚠️ **本组图受那条尚未修好的缺陷影响**：离屏 SceneCapture 只画得出固定世界位置的约 15 块
（回读说 216 块全在）。已被推翻的归因清单见 `TinyGladeShotStairs.py` 文件头 ⑤。
所幸那一小撮**恰好是一段沿等值线的连续石阶**，把它放大就能读出 S1 与 S2 的差别
（S1 是一排等长方块、块与块之间有可见缝；S2 长度跟弦、互相穿插、还带小偏航）。
"一根不少"仍然只能由回读断言负责。
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
    """笔刷是 3D 球：每一笔都要自己贴地，固定 Z 会让抬高的坡面整段落在球外。"""
    ground.begin_paint_stroke()
    for i in range(steps + 1):
        t = float(i) / steps
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        ground.apply_paint_stroke(unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y))))
    ground.end_paint_stroke()


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，一律用关键字参数。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def set_jitter(ground, on):
    """S2 的四个抖动量。关掉 = S1 的定长块 + 零偏航（`StairLengthBloat` 也退回 1）。"""
    ground.set_editor_property("StairLengthBloat", 1.06 if on else 1.0)
    ground.set_editor_property("StairLengthJitter", 0.10 if on else 0.0)
    ground.set_editor_property("StairSizeJitter", 0.12 if on else 0.0)
    ground.set_editor_property("StairYawJitter", 6.0 if on else 0.0)


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
step_mesh = unreal.load_asset(STAIR_MESH)
if not (ground and shaper and step_mesh):
    unreal.log_error("SHOT FAILED: demo actors or step mesh missing")
    raise SystemExit

c = shaper.get_actor_location()
junction_x = c.x + 400.0
spawned = []

second = ACTORS.spawn_actor_from_class(
    unreal.load_class(None, "%s/BP_GroundShaper.BP_GroundShaper_C" % PKG),
    unreal.Vector(c.x + 800.0, c.y, 0.0))
if second:
    # CDO 的默认值不传播到同会话 spawn 的实例 —— 实例上必须再写一份。
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
ground.set_editor_property("StairJitterSeed", 1)
ground.set_editor_property("BrushRadius", 260.0)

# 注记：这里原来还要关掉塑形物自持的旧路石阶（`StepMeshes` + `rebuild_steps`），
# 免得两条路的石阶叠在一起分不清是谁摆的。旧路已随 2026-08-30「裁决一」第二步整条删除，
# 场上只剩地面自己这一条，那几行（连同收尾的写回）一并去掉。

ground.reset_paint()
paint_line(ground, c.x - 900.0, c.y, c.x + 1700.0, c.y, 26)
paint_line(ground, junction_x, c.y - 900.0, junction_x, c.y + 900.0, 18)

# 场景代理在构造时就把 bGpuFrustumCulling 抄走，所以必须在下一次交接**之前**设，
# 再改一下固定容量逼一次 SetInstanceSourceGPU，新代理才会读到新值。
# （这两下对上面那条出图缺陷**没有影响** —— 开/关出图逐字节相同，容量不变时症状也一样，
#   见 `TinyGladeShotStairsProbe.py`。留着只是为了与 `TinyGladeShotStairs.py` 的机位条件一致。）
for comp in ground.get_components_by_class(unreal.CSGpuInstancedMeshComponent):
    comp.set_editor_property("bGpuFrustumCulling", False)
ground.set_editor_property("MaxStairInstances", 8192)

# ---- 机位在**抖动关**的那一趟算死，两趟共用（见文件头）----
set_jitter(ground, False)
ground.rebuild_stairs()
res = ground.debug_read_stairs_sync()
count, origins = (res[0], list(res[1])) if isinstance(res, tuple) else (res, [])
unreal.log("gpu stairs (jitter off): %d" % count)

if origins:
    mid_z = 0.5 * (min(p.z for p in origins) + max(p.z for p in origins))
    target = min(origins, key=lambda p: abs(p.x - junction_x) + 0.35 * abs(p.z - mid_z))
else:
    target = unreal.Vector(junction_x, c.y, 150.0)
unreal.log("aim target = (%.0f, %.0f, %.0f)" % (target.x, target.y, target.z))

# 近景看单块的踏面/立面与相邻两块的接缝；中景看整条阶梯读不读得成"一条"。
near = unreal.Vector(target.x - 330.0, target.y - 330.0, target.z + 190.0)
mid = unreal.Vector(target.x - 900.0, target.y - 900.0, target.z + 520.0)
CAMS = [("closeup", near, look_at(near, target)), ("junction", mid, look_at(mid, target))]

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

# ---- 这张演示关卡是脚本建的，一盏灯都没有：不补光只会出全黑 ----
sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000),
                                    unreal.Rotator(roll=0.0, pitch=-42.0, yaw=140.0))
sun.light_component.set_editor_property("intensity", 6.0)
spawned.append(sun)
spawned.append(ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)))
sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(c.x, c.y, 1500))
sky.light_component.set_editor_property("intensity", 1.0)
sky.light_component.set_editor_property("real_time_capture", True)
spawned.append(sky)

# 两趟 × 每趟每机位一张。SHOT[i] = (前缀, 抖动开关, rig 序号)
SHOTS = [("stairs_s1", False, 0), ("stairs_s1", False, 1),
         ("stairs_s2", True, 0), ("stairs_s2", True, 1)]

STATE = {"ticks": 0, "handle": None}
FIRST_TICK = 24
GAP = 8


def tick(delta):
    STATE["ticks"] += 1
    Index = STATE["ticks"] - FIRST_TICK
    if Index >= 0 and Index // GAP < len(SHOTS):
        Phase = Index % GAP
        prefix, jitter_on, rig = SHOTS[Index // GAP]
        name, cap, comp, rt = RIGS[rig]
        if Phase == 0:
            # 抖动开关每张都写一次：属性变更走 PostEditChangeProperty → RebuildStairs，
            # 整条链就是一次 dispatch，重复写的代价可以忽略，换来的是"这一张到底是哪一档"确定。
            set_jitter(ground, jitter_on)
            ground.rebuild_stairs()
            # 主视口驾驶捕获相机。⚠️ 这一下**不修**上面那条出图缺陷（当时的归因已被推翻），
            # 但它确实把"同一帧连拍多张"拆成了逐张隔开，所以照抄 TinyGladeShotStairs.py 的节奏。
            unreal.EditorLevelLibrary.pilot_level_actor(cap)
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif Phase == 4:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "%s_%s.png" % (prefix, name))
            unreal.log("shot: %s_%s.png" % (prefix, name))
    if STATE["ticks"] < FIRST_TICK + GAP * len(SHOTS) + 8:
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    ground.set_editor_property("StairMesh", None)   # 别把演示关卡留在改过的状态
    ground.reset_paint()
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.log("SHOT DONE")
    unreal.SystemLibrary.quit_editor()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
