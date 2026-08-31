# -*- coding: utf-8 -*-
"""
拍 GPU 石阶（marching squares，计划「石阶改造」的 S1）的验证图。

单测与回归能断言"有几级、层连不连续"，但"接合处那圈石阶到底连不连成一条"只有像素能回答 ——
这正是 S1 的核心验收项（旧路的星形等高线在两座相接土台的接合处必然断段）。

场景：`Shaper_Mound` 旁边再放一座相距 800 的土台，两盘不相交但裙边融成一体（接合线上
253 cm）。两条路：
  · 沿两座中心的连线 —— 一路爬上 A、跨过接合处的鞍、再爬上 B；
  · 沿接合线（与上一条垂直）—— 穿过合成体的"腰"，那圈等值线不属于任何一座。

四条实测陷阱（都踩过，别重蹈）：
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。写错的症状很迷惑：
    Rotator(-6, 90, 0) 会把 90 当 pitch，相机直接朝天，出图是一张纯渐变。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发，出图全黑。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏，抓的是当前激活
    的那一格（可能是张空的正交面板）。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`：默认是浮点格式，导出的 .png 是 HDR 内容。
 ⑤ **已修（2026-08-30）**：曾有一条「离屏 SceneCapture 无论相机指哪里都只画出
    固定世界位置的约 15 块石阶」的缺陷。真因**不在 cull、不在 SceneCapture、也不在摆位**：
    `FCSGpuInstancedMeshVertexFactory` 被**静默地踢出了深度预通道**（完整机制写在该文件
    两条 `SupportsPosition*OnlyStream` override 的注释里）。后果是「画了颜色但不占深度」：
    本工程是全深度预通道（Lumen/VSM 强制 `DDM_AllOpaque`），基通道因此是 `DepthRead`，
    谁都不写深度 ⇒ 同一趟里**后画的地面**直接把整片石阶盖掉，
    只剩地形轮廓线外侧、背后是天空的那十几块。
    实测硬证（同机位、只改那一行）：石阶像素数 **3021 → 84542**（俯视机位 **0 → 与 Cube 同数量级**）。
    下面这些归因都已被实测/源码推翻，**別再走**：
      · 「`bCulledThisFamily` 跨族 sticky」—— 每族重置（`CSGpuInstancedMeshSceneProxy.cpp:174-177`）。
      · 「没 override `IsActiveThisFrame`」—— 基类默认就是 `return true`。
      · 「两个 family 共用一张 RDG 图」—— 5.7.4 是每 renderer 一张图。
      · 「中段改容量 ⇒ 画侧拿陈旧裸指针」—— `TinyGladeShotStairsProbe.py` 实测推翻。
      · 视锥剔除、LOD、容量、抓拍节奏、驾驶视口、`r.EarlyZPass`（改了没用）。
    回归判据就写在本文件末尾的 `STAIRS PIXEL` 一行：把每一级石阶的世界原点
    投影到俯视图上，比对「开石阶 / 关石阶」两张图在这些像素上的差异。
    **缺陷复现时它是 0**，修好时它接近 100%。

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotStairs.py"

产物：Saved/TinyGladeShots/stairs_*.png
"""
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


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
step_mesh = unreal.load_asset(STAIR_MESH)
if not (ground and shaper and step_mesh):
    unreal.log_error("SHOT FAILED: demo actors or step mesh missing")
    raise SystemExit

c = shaper.get_actor_location()
junction_x = c.x + 400.0

spawned = []

# ---- 第二座土台：与第一座相距 800，裙边融成一体 ----
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

# ---- 石阶归**地面**（扫描域是全局的），参数照 S1 的口径 ----
ground.set_editor_property("StairMesh", step_mesh)
ground.set_editor_property("StairMaterial", unreal.load_asset("%s/M_TinyGladeBrick" % PKG))
ground.set_editor_property("StairStepHeight", 30.0)
ground.set_editor_property("StairCellSize", 100.0)     # ⚠️ 必须 ≈ 石阶长度，否则重叠/断续
ground.set_editor_property("StairBlockSize", unreal.Vector(60.0, 100.0, 45.0))
ground.set_editor_property("StairEmbed", 10.0)
# 演示关卡的笔刷默认很窄（一条路只盖两格），拍图时铺宽一点才看得出"一条阶梯"而不是两块砖。
ground.set_editor_property("BrushRadius", 260.0)
# 注记：这里原来还要关掉塑形物自持的旧路石阶（`StepMeshes` + `rebuild_steps`），
# 免得两条路的石阶叠在一起分不清是谁摆的。旧路已随 2026-08-30「裁决一」第二步整条删除，
# 场上只剩地面自己这一条，那几行（连同收尾的写回）一并去掉。

ground.reset_paint()

# 路 ①：沿两座中心的连线 —— 爬 A → 跨过接合处的鞍 → 爬 B。
paint_line(ground, c.x - 900.0, c.y, c.x + 1700.0, c.y, 26)
# 路 ②：**沿接合线**，与 ① 垂直。这条路上的每一条等值线都是合成体的"腰"，不属于任何一座 ——
# 旧路的星形闭式解在这里必然断段，这就是 S1 要证明的那一半。
paint_line(ground, junction_x, c.y - 900.0, junction_x, c.y + 900.0, 18)

# 容量按这关的最坏情况放宽（两座土台 + 两条路）。**剔除开关一律保持默认** ——
# 出图必须走用户真正会走的那条路，绕行只会把缺陷藏回去。
ground.set_editor_property("MaxStairInstances", 8192)
ground.rebuild_stairs()

res = ground.debug_read_stairs_sync()
count, origins = (res[0], list(res[1])) if isinstance(res, tuple) else (res, [])
unreal.log("gpu stairs scattered: %d" % count)
if origins:
    unreal.log("stairs bbox X %.0f..%.0f  Y %.0f..%.0f  Z %.0f..%.0f" % (
        min(p.x for p in origins), max(p.x for p in origins),
        min(p.y for p in origins), max(p.y for p in origins),
        min(p.z for p in origins), max(p.z for p in origins)))

# ---- 这张演示关卡是脚本建的，一盏灯都没有：不补光的话打光通路只会出全黑 ----
sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000),
                                    unreal.Rotator(roll=0.0, pitch=-42.0, yaw=140.0))
sun.light_component.set_editor_property("intensity", 6.0)
spawned.append(sun)
spawned.append(ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)))
sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(c.x, c.y, 1500))
sky.light_component.set_editor_property("intensity", 1.0)
sky.light_component.set_editor_property("real_time_capture", True)
spawned.append(sky)

def look_at(frm, to):
    """机位一律从**实测的石阶位置**反算，不靠手调 —— 手调机位在地形改一点之后就全废了。
    ⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    import math
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


# 瞄准点：最靠近接合线、且在半山腰那一层的那一级石阶。
if origins:
    mid_z = 0.5 * (min(p.z for p in origins) + max(p.z for p in origins))
    target = min(origins, key=lambda p: abs(p.x - junction_x) + 0.35 * abs(p.z - mid_z))
else:
    target = unreal.Vector(junction_x, c.y, 150.0)
unreal.log("aim target = (%.0f, %.0f, %.0f)" % (target.x, target.y, target.z))

near = unreal.Vector(target.x - 330.0, target.y - 330.0, target.z + 190.0)
wide = unreal.Vector(junction_x - 1500.0, c.y - 1700.0, 1150.0)
# 俯视机位从**实测的石阶包围盒**反算，不写死高度。
#
# ⚠️ **写死的 1500 把下面那条 `STAIRS PIXEL` 判据悄悄变成了空跑**（2026-08-31 实测）：
# 地形参数漂一点（本轮实测散出 **389** 级、铺开 2385 cm）石阶就顶出画框，
# 40 个投影样本全落在界外 ⇒ `STAIRS PIXEL FAILED: no samples`。
# 那是判据自己坏了，不是被测物坏了 —— 而一条不能报红的判据比没有判据更坏。
# 针孔投影是闭式的（pitch=-90 / yaw=0），所以高度可以直接解出来。
if origins:
    import math as _math
    _min_x = min(p.x for p in origins); _max_x = max(p.x for p in origins)
    _min_y = min(p.y for p in origins); _max_y = max(p.y for p in origins)
    _max_z = max(p.z for p in origins)
    _tan_h = _math.tan(_math.radians(60.0 * 0.5))
    _tan_v = _tan_h * float(H) / float(W)
    # 屏幕横 = 世界 +Y、屏幕纵 = 世界 +X（见 project_overhead）。两轴各取一个下界，再留1.2 余量。
    _need = max((_max_y - _min_y) * 0.5 / _tan_h, (_max_x - _min_x) * 0.5 / _tan_v) * 1.2
    top = unreal.Vector(0.5 * (_min_x + _max_x), 0.5 * (_min_y + _max_y), _max_z + max(_need, 600.0))
else:
    top = unreal.Vector(junction_x, c.y, 1500.0)
unreal.log("STAIRS overhead cam at (%.0f, %.0f, %.0f)" % (top.x, top.y, top.z))

SHOTS = [
    # 贴近一级石阶：踏面 / 立面 / 扎进坡里的后缘，看这张。
    ("stairs_closeup", near, look_at(near, target), unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR),
    # 稍退一步：接合处那一串是不是首尾相接、有没有断口。
    ("stairs_junction", unreal.Vector(target.x - 900.0, target.y - 900.0, target.z + 520.0),
     look_at(unreal.Vector(target.x - 900.0, target.y - 900.0, target.z + 520.0), target),
     unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR),
    # 3/4 全景：两座土台 + 两条路 + 整条阶梯。
    ("stairs_overview", wide, look_at(wide, unreal.Vector(junction_x, c.y, 150.0)),
     unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR),
    # 俯视：等值线的整体结构 —— 接合处那圈"腰"到底连不连成一条。
    ("stairs_overhead", top, unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0),
     unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR),
]

RIGS = []
for name, cam_loc, cam_rot, source in SHOTS:
    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_loc, cam_rot)
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", source)
    comp.set_editor_property("fov_angle", 60.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    RIGS.append((name, cap, comp, rt))
    spawned.append(cap)

STATE = {"ticks": 0, "handle": None}

# ---------------------------------------------------------------------------------------------
# 回归判据（`STAIRS PIXEL`）—— 这条缺陷再回来时会**报红**的那一行
#
# 判的是"石阶到底有没有出现在画面上"，而不是"实例存不存在"（后者由 DebugReadStairsSync 的
# 回读断言管）。做法：把每一级石阶的世界原点按俯视机位的针孔模型投到屏幕上，同机位拍
# 「开石阶 / 关石阶」两张，比对这些像素。缺陷复现时俯视图上**一个像素都不变**（实测 0/1 px），
# 修好后这些像素几乎全变。
# ⚠️ 只对俯视机位成立：pitch=-90 / yaw=0 时相机右 = 世界 +Y、相机上 = 世界 +X，投影是闭式的。
# ---------------------------------------------------------------------------------------------
CHECK_SAMPLES = 40
CHECK_MIN_RATIO = 0.50


def project_overhead(p):
    """俯视机位（top，pitch=-90、yaw=0、水平 FOV 60）的针孔投影。返回 None 表示落在画面外。"""
    import math
    d = top.z - p.z
    if d <= 1.0:
        return None
    tan_h = math.tan(math.radians(60.0 * 0.5))
    tan_v = tan_h * float(H) / float(W)
    sx = 0.5 * W * (1.0 + (p.y - top.y) / (d * tan_h))
    sy = 0.5 * H * (1.0 - (p.x - top.x) / (d * tan_v))
    x, y = int(round(sx)), int(round(sy))
    if x < 4 or x >= W - 4 or y < 4 or y >= H - 4:
        return None
    return (x, y)


def sample_overhead(rt, pixels):
    out = []
    for (x, y) in pixels:
        col = unreal.RenderingLibrary.read_render_target_pixel(world, rt, x, y)
        out.append((col.r, col.g, col.b))
    return out


CHECK = {"pixels": [], "on": None}
if origins:
    step = max(1, len(origins) // CHECK_SAMPLES)
    for p in origins[::step]:
        px = project_overhead(p)
        if px is not None and px not in CHECK["pixels"]:
            CHECK["pixels"].append(px)
unreal.log("STAIRS PIXEL: %d sample pixels projected from %d stair origins"
           % (len(CHECK["pixels"]), len(origins)))

FIRST_TICK = 24
GAP = 8
OVERHEAD_RIG = len(RIGS) - 1        # stairs_overhead
CHECK_STEP = len(RIGS)              # 拍完四张之后，再用俯视那台拍一张"关掉石阶"的对照


def run_check():
    """关掉石阶、同机位再拍一张，比对投影像素。"""
    name, cap, comp, rt = RIGS[OVERHEAD_RIG]
    if not CHECK["pixels"] or CHECK["on"] is None:
        unreal.log_error("STAIRS PIXEL FAILED: no samples")
        return
    ground.set_editor_property("StairMesh", None)
    ground.rebuild_stairs()
    comp.capture_scene()
    off = sample_overhead(rt, CHECK["pixels"])
    changed = sum(1 for a, b in zip(CHECK["on"], off)
                  if max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) > 12)
    ratio = float(changed) / float(len(CHECK["pixels"]))
    ok = ratio >= CHECK_MIN_RATIO
    unreal.log("STAIRS PIXEL %s: %d/%d (%.0f%%) of the projected stair pixels change when the "
               "stairs are switched off (need >= %.0f%%)"
               % ("OK" if ok else "FAILED", changed, len(CHECK["pixels"]), 100.0 * ratio,
                  100.0 * CHECK_MIN_RATIO))
    if not ok:
        unreal.log_error("STAIRS PIXEL FAILED: the GPU instanced stairs are not reaching the "
                         "offscreen capture. See CSGpuInstancedMeshVertexFactory's "
                         "SupportsPosition*OnlyStream overrides (depth prepass admission).")


def tick(delta):
    STATE["ticks"] += 1
    Index = STATE["ticks"] - FIRST_TICK
    if Index >= 0 and Index // GAP < len(RIGS):
        Phase = Index % GAP
        name, cap, comp, rt = RIGS[Index // GAP]
        if Phase == 4:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "%s.png" % name)
            unreal.log("shot: %s.png" % name)
            if Index // GAP == OVERHEAD_RIG and CHECK["pixels"]:
                CHECK["on"] = sample_overhead(rt, CHECK["pixels"])
    elif Index // GAP == CHECK_STEP and Index % GAP == 4:
        run_check()
    if STATE["ticks"] < FIRST_TICK + GAP * (len(RIGS) + 1) + 8:
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    ground.set_editor_property("StairMesh", None)   # 关掉整条路径，别把演示关卡留在改过的状态
    ground.reset_paint()
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.log("SHOT DONE")
    unreal.SystemLibrary.quit_editor()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
