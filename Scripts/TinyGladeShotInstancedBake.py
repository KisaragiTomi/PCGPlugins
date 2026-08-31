# -*- coding: utf-8 -*-
"""实例路烘焙出口（裁决六 ①）的**像素判据**：GPU 实例版 vs 烘焙版，同机位。

要回答的是一件只有像素能答的事：**烘出来的东西与画面上那个是不是同一个东西。**
`GetMeshDescription(0)` 里数出来的三角 / 角点 / UV / 顶点色（单测那条 `FrameBricksSurviveBake`）
只证明"数据带过去了"，证明不了"带过去的数据摆在对的地方、被材质读成了对的颜色"。

被测对象取**藤枝**，不取门框砖：`M_TinyGladeBrick` 是一张纯常数材质，读不到逐实例随机 ⇒
拿它对照的话"通道丢没丢"在像素上根本不显影，那是一道假门。藤枝材质有 0.80~1.15 的
逐实例明暗，通道一断整墙藤就塌成同一个色。

三张图，同机位：
  gpu     —— GPU 实例版（现状）
  baked   —— 实例组件藏起来，换成烘出来的 StaticMesh 摆在同一变换上
  gone    —— 两者都不画（对照组：确认这个机位**真的拍到了藤**）

判据：
  A  gpu vs baked 的差异率必须**低于**噪声门（几何 / 材质 / 通道都对上了）
  B  gpu vs gone 的差异率必须**高于**信号门（这个机位确实拍到了藤，A 才有内容）

⚠️ A 是一道**上界**门，与本项目其余出图脚本的下界门性质相反：它在"什么都没拍到"时
   会自动全绿。所以 B 不是附赠品，是 A 的前提。

用法::

    set TG_SHOT_TAG=v1
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotInstancedBake.py"

**故意破坏实验**（验证门是活的，破坏的一律是**世界侧**，不动阈值）::

    set TG_BAKE_BREAK=offset   # 把烘焙件挪开 40 cm      ⇒ A 应当报红
    set TG_BAKE_BREAK=nomat    # 烘焙件换成引擎默认材质  ⇒ A 应当报红
    set TG_BAKE_BREAK=hide     # 烘焙件根本不摆出来      ⇒ A 应当报红

产物：`Saved/TinyGladeShots/instbake_<tag>_*.png`，判据打在日志的 `BAKE PIXELS` 行。

--- 沿用的坑（口径与 `TinyGladeShotVine.py` 逐条相同）--------------------------
 ① `unreal.Rotator` 是 (roll, pitch, yaw)，一律关键字参数。
 ② 必须真编辑器 + `-ExecCmds="py <脚本>"`；`-ExecutePythonScript` 跑完即退、tick 不触发。
 ③ 离屏 `SceneCapture2D`，不要 `HighResShot`（本工程是 4 分屏）。
 ④ `create_render_target2d` 显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ 捕获组件自己那份 `post_process_settings` 把 Lumen 覆盖回来（引擎写死关掉了它）。
 ⑥ 准备段整个包在 try 里，否则异常 ⇒ 编辑器永不退出。
 ⑦ `always_persist_rendering_state = True`，否则 PPV 里的曝光一条都不生效。
 ⑧ 开了 ⑦ 就必须预热：**一帧一次**、共 128 次 `capture_scene()`（32 只是收敛下限）。
 ⑨ 实例的可见集按主视口视角压 ⇒ 先让主视口驾驶捕获相机。
 ⑩ 一律调 `Get*UndrawableReason()`，不要 `is_*_drawable()`。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v1")
BREAK = os.environ.get("TG_BAKE_BREAK", "").strip().lower()
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
BAKE_ASSET = "/Game/Automation/GpuInstanced/SM_ShotVineBranches"
W, H = 1600, 900

# 采样格与容差：与 `TinyGladeShotVine.py` / `TinyGladeShotRockShell.py` 同一套。
GRID_X, GRID_Y = 48, 27
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
ZERO_FAIL = 0.005

# 坑 ⑧ 的执行面。128 是 2026-08-31 重标定的收敛平台，32 只是"间接光不恒零"那一档的下限。
WARMUP_CAPTURES = int(os.environ.get("TG_SHOT_WARMUP", "128"))
FIRST_TICK = 45
SETTLE = 6

# 两道门的标定记录在文件尾「本轮实测」。信号门沿用本项目口径（实测值的一半 = "被测对象
# 在屏幕上的覆盖面至少还剩一半"）；噪声门是上界，取在"完好"与最弱的一次破坏之间。
SIGNAL_FAIL = float(os.environ.get("TG_BAKE_SIGNAL", "0.098"))
NOISE_FAIL = float(os.environ.get("TG_BAKE_NOISE", "0.02"))

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "house": None,
         "samples": {}, "rt": None, "comp": None, "cap": None, "spawned": [],
         "step": 0, "phase": 0, "baked": None, "bake_actor": None}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def show_bake(visible):
    a = STATE["bake_actor"]
    if not a:
        return
    a.set_actor_hidden_in_game(not visible)
    a.set_is_temporarily_hidden_in_editor(not visible)


def state_gpu():
    """现状：实例组件画，烘焙件不在。"""
    STATE["house"].call_method("DebugSetVineBranchInstancesHidden", args=(False,))
    show_bake(False)
    unreal.log("BAKE state gpu    branches=%d" % STATE["house"].get_vine_segment_count())


def state_baked():
    """换路：实例组件藏起来，烘焙件摆出来。**只换这一件事**。"""
    STATE["house"].call_method("DebugSetVineBranchInstancesHidden", args=(True,))
    show_bake(BREAK != "hide")
    unreal.log("BAKE state baked  actor=%s" % (STATE["bake_actor"] is not None))


def state_gone():
    """对照组：两条路都不画 —— 用来证明这个机位真的拍到了藤。"""
    STATE["house"].call_method("DebugSetVineBranchInstancesHidden", args=(True,))
    show_bake(False)
    unreal.log("BAKE state gone")


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    house = find("House_Road")
    if not (ground and house):
        unreal.log_error("BAKE FAILED: no Ground_Demo / House_Road")
        return None
    STATE["house"] = house

    ground.reset_paint()
    # ⚠️ **不要**为了让藤枝露出来而把叶撤掉：`GetVineUndrawableReason()` 把"没有 VineLeafMesh"
    # 直接判成不可画，撤叶等于自己把那条自诊断弄红，然后拿一个已经报红的世界去出图。
    # 叶与花在三张图里逐位相同、对差异率贡献恰好 0，留着只影响藤枝的**覆盖面**，
    # 而覆盖面正是下面 B 那条信号门量的东西 —— 它自己会说话。
    house.call_method("RebuildHouse")

    # ⚠️ 坑 ⑩：调原因版，不调 is_vine_drawable()。
    why = str(house.get_vine_undrawable_reason())
    unreal.log("BAKE drawable=%s%s" % (why == "", "" if why == "" else (" -- " + why)))
    if why != "":
        unreal.log_error("BAKE !! 藤画不出来：%s -- 后面的判据全是空的" % why)

    # ---- 烘一遍，走的是组件自己那条出口 ----
    baked = house.call_method("DebugBakeVineBranchesSync", args=(BAKE_ASSET,))
    if not baked:
        unreal.log_error("BAKE FAILED: DebugBakeVineBranchesSync 没产出资产")
        return None
    STATE["baked"] = baked
    unreal.log("BAKE asset %s tris=%d verts=%d branches=%d"
               % (baked.get_name(), baked.get_num_triangles(0), baked.get_num_vertices(0),
                  house.get_vine_segment_count()))

    # 烘回的是 actor 局部空间 ⇒ 摆在同一个变换上就该逐像素重合。
    loc = house.get_actor_location()
    if BREAK == "offset":
        # 世界侧的破坏：几何 / 材质 / 通道全对，只是位置错了 ——
        # 这是"烘焙空间搞错"那一类缺陷的画面症状。
        loc = unreal.Vector(loc.x + 40.0, loc.y, loc.z)
        unreal.log_warning("BAKE ⚠️ 故意破坏：烘焙件挪开 40 cm，A 应当报红")
    bake_actor = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, loc, house.get_actor_rotation())
    bake_actor.set_actor_label("BakedVineBranches")
    smc = bake_actor.static_mesh_component
    smc.set_static_mesh(baked)
    if BREAK == "nomat":
        # 世界侧的破坏：换成引擎默认材质 —— 与"母材质没勾实例标志被静默换掉"逐像素同症状。
        smc.set_material(0, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))
        unreal.log_warning("BAKE ⚠️ 故意破坏：烘焙件换成默认材质，A 应当报红")
    if BREAK == "hide":
        unreal.log_warning("BAKE ⚠️ 故意破坏：烘焙件根本不摆出来，A 应当报红")
    STATE["bake_actor"] = bake_actor
    STATE["spawned"].append(bake_actor)
    mat0 = smc.get_material(0)
    unreal.log("BAKE baked material = %s" % (mat0.get_name() if mat0 else "None"))

    # 机位从房子参数反算（南墙正面，与 `TinyGladeShotVine.py` 的 CAM_WALL 同一条）。
    foot = house.get_editor_property("FootprintSize")
    wall_h = house.get_editor_property("WallHeight")
    hloc = house.get_actor_location()
    cam = unreal.Vector(hloc.x - foot.x * 0.20, hloc.y - foot.y * 0.5 - 420.0, hloc.z + wall_h * 0.45)
    aim = unreal.Vector(hloc.x + foot.x * 0.10, hloc.y - foot.y * 0.5, hloc.z + wall_h * 0.45)
    CAM = (cam, look_at(cam, aim))

    STATE["plan"] = [
        ("gpu", state_gpu, CAM),
        ("baked", state_baked, CAM),
        ("gone", state_gone, CAM),
    ]

    # 坑 ⑤：把捕获自己那份 PP 覆盖建起来，Lumen 才会回到出图里。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam, CAM[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)   # 坑 ⑦
    STATE["rt"], STATE["comp"], STATE["cap"] = rt, comp, cap
    STATE["spawned"].append(cap)
    return world


def sample_pixels():
    """采样格上的 RGB。逐像素读 144 万次太慢，48 × 27 = 1296 个点足以给出稳定的差异率。"""
    out = []
    for gy in range(GRID_Y):
        y = int((gy + 0.5) * H / GRID_Y)
        for gx in range(GRID_X):
            x = int((gx + 0.5) * W / GRID_X)
            col = unreal.RenderingLibrary.read_render_target_pixel(STATE["world"], STATE["rt"], x, y)
            out.append((col.r, col.g, col.b))
    return out


def changed_mask(a, b):
    return [max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > PIXEL_DELTA
            for pa, pb in zip(a, b)]


def zero_ratio(samples):
    """精确 (0,0,0) 占比 —— 坑 ⑧ 有没有被预热掉的**指纹**。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


def ratio(a_key, b_key):
    a, b = STATE["samples"].get(a_key), STATE["samples"].get(b_key)
    if not (a and b):
        return None
    return sum(1 for m in changed_mask(a, b) if m) / float(GRID_POINTS)


def report():
    ok = True
    for name in sorted(STATE["samples"]):
        z = zero_ratio(STATE["samples"][name])
        unreal.log("BAKE PIXELS %-8s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("BAKE !! %s 有 %.3f%% 的像素精确 (0,0,0) -- Lumen 预热没起作用（坑 ⑧）"
                             % (name, z * 100.0))
            ok = False

    signal = ratio("gpu", "gone")
    noise = ratio("gpu", "baked")
    unreal.log("BAKE PIXELS B 信号  gpu vs gone : %.2f%%  门 >= %.2f%%"
               % ((signal or 0.0) * 100.0, SIGNAL_FAIL * 100.0))
    unreal.log("BAKE PIXELS A 噪声  gpu vs baked: %.2f%%  门 <= %.2f%%"
               % ((noise or 0.0) * 100.0, NOISE_FAIL * 100.0))

    # B 先判：A 是一道**上界**门，它在"什么都没拍到"时会自动全绿 —— 没有 B 撑着 A 就是假门。
    if signal is None or signal < SIGNAL_FAIL:
        unreal.log_error("BAKE !! 这个机位几乎没拍到藤（%.2f%%）-- A 那条上界门是空的"
                         % ((signal or 0.0) * 100.0))
        ok = False
    if noise is None or noise > NOISE_FAIL:
        unreal.log_error("BAKE !! 烘焙版与实例版差得太多（%.2f%%）-- 几何 / 材质 / 逐实例随机通道有一条没对上"
                         % ((noise or 0.0) * 100.0))
        ok = False

    unreal.log("BAKE PIXEL VERDICT %s%s"
               % ("OK" if ok else "FAILED", ("（故意破坏 %s，红是对的）" % BREAK) if BREAK else ""))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, action, (loc, rot) = STATE["plan"][step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            action()
            STATE["cap"].set_actor_location_and_rotation(loc, rot, False, False)
            unreal.EditorLevelLibrary.pilot_level_actor(STATE["cap"])   # 坑 ⑨
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            # 坑 ⑧：一帧只打一次 capture_scene()，Lumen 的帧间历史才会真的往前走。
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "instbake_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("BAKE shot: instbake_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    house = STATE["house"]
    if house:
        # 关卡里的房子恢复出厂设定：本脚本唯一改过的世界状态就是"藤枝组件藏没藏"。
        house.call_method("DebugSetVineBranchInstancesHidden", args=(False,))
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.EditorAssetLibrary.delete_asset(BAKE_ASSET)
    unreal.log("BAKE DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("BAKE FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)

# --- 本轮实测（w128、L_HouseGroundDemo、House_Road、362 段藤枝，2026-08-31）------
#
#   实验                                   B 信号     A 噪声    说明
#   完好                                   19.60%     0.00%    1296 个采样点**一个都没变**
#   TG_BAKE_BREAK=offset（挪开 40 cm）      19.75%    35.26%
#   TG_BAKE_BREAK=nomat（默认材质）         19.83%    14.81%
#   TG_BAKE_BREAK=hide（干脆不摆）          19.83%    19.83%    塌成 gone，是 A 的上界
#   通道清零（代码侧：烘焙时 alpha 写 0）     19.91%     6.87%    ← 裁决六 ③ 那条缺陷的画面读数
#
# ⇒ 信号门 19.60% / 2 = 0.098；噪声门取 0.02 —— 完好那次是 0.00%，采样底噪一格 = 0.077%，
#   而最弱的一次破坏（通道清零）是 6.87%。0.02 离底噪 26 倍、离最弱破坏 1/3.4，两头都不贴边。
#
# ⚠️ **"通道清零"那一行是这条脚本存在的理由**：它在单测里只让一条断言变红
#   （`逐实例随机不止一种取值`），三角数 / 角点数 / UV 组数 / 实例数四条**全部照绿**；
#   在画面上则是整墙藤塌成同一个色。改这份脚本前先想清楚会不会把这一行弄哑。
