# -*- coding: utf-8 -*-
"""装饰摆件（D12 锚点那一半）的出图 + **像素判据**。

要回答的是三件只有像素能答、readback 与 `IsDecorDrawable()` 都答不了的事：

  A  **摆件真的出现在房子周围**：`IsDecorDrawable()` 只证明"组件/快照/实例源/材质都在且材质
     支持实例化"，它证明不了那十几个实例有没有变成屏幕上的颜色。所以这里拍**同一机位、
     只切 `bDecorEnabled`** 的两张，逐像素比 —— 差异像素率就是"它画出来了"的硬证据。
  B  **摆件不是一片灰、也不是一团黑**：母材质没勾 `bUsedWithInstancedStaticMeshes` 时引擎会
     **静默换成默认材质**（一片灰，且症状与"没绑材质"逐像素相同）；顶点色搬错时会是一团黑。
     两种失效都能让 A 的差异率很高，所以另外量一条：变化像素的**平均亮度**要够高，
     且**暖调**（R > B）—— 引擎默认材质是中性灰，`R ≈ B`。
  C  **门那一家真的锚在门上**：画一笔路开出拱之后，门两侧与门前引道多出摆件，画面必须变。
     数值侧配一条：**门那一家**的锚点数必须从 0 涨上去。⚠️ 不能看总数 —— 开拱同时会把
     门口净空与路面上的墙脚锚点排掉，两边可以刚好抵消（实测碰到过：20 → 20）。

用法（tag 决定文件名前缀）::

    set TG_SHOT_TAG=v1
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotDecor.py"

产物：`Saved/TinyGladeShots/decor_<tag>_*.png`，判据打在日志的 `DECOR PIXELS` 行。

--- 踩过的坑（全部来自状态文件的坑表）-----------------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：唯一的翻回办法是给捕获组件自己的
    `post_process_settings` 再覆盖一次（它在引擎那两行之后才 apply）。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**。准备段整个包在 try 里。
 ⑦ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState** ⇒ **PPV 里的曝光一条都不生效**。
    与坑 ⑤ 症状字面相同、根因完全不同。
 ⑧ ⚠️ **修了 ⑦ 才会触发 ⑧，两条必须一起加。** 有 ViewState 之后 Lumen 才真的跑起来，
    而它的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒ 间接光恒为零 ⇒ 没被直射的
    lit 表面全部精确 (0,0,0)。对本脚本是**致命**的：判据 B 量的就是"摆件不是黑的"。
 ⑨ 摆件走 GPU 实例路（`UCSGpuInstancedMeshComponent`）。状态文件那条"离屏 capture 画不出
    GPU 实例"只在 `L_TerrainOpsDemo` 的石阶上复现过，`L_HouseGroundDemo` 的门框砖与藤蔓
    都画对了 —— 本脚本用的正是后者那张关卡，并沿用"先让主视口驾驶捕获相机"的缓解手段。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v1")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

# 判据的采样格与容差：与 `TinyGladeShotVine.py` / `TinyGladeShotRockShell.py` 同一套。
GRID_X, GRID_Y = 48, 27
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
ZERO_FAIL = 0.005

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None, "house": None,
         "samples": {}, "counts": {}, "rt": None, "comp": None, "cap": None, "spawned": [],
         "step": 0, "phase": 0}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def note(name):
    h = STATE["house"]
    STATE["counts"][name] = (h.get_decor_anchor_count(), h.get_decor_instance_count(),
                             h.get_open_door_count(), h.get_decor_gate_anchor_count())
    unreal.log("DECOR %-18s anchors=%d instances=%d doors=%d gate=%d"
               % ((name,) + STATE["counts"][name]))


def set_decor(on, name):
    STATE["house"].set_editor_property("bDecorEnabled", on)
    STATE["house"].call_method("RebuildHouse")
    note(name)


def paint_road():
    g, h = STATE["ground"], STATE["house"]
    loc = h.get_actor_location()
    g.begin_paint_stroke()
    for i in range(17):
        y = loc.y - 800.0 + i * 100.0
        g.apply_paint_stroke(unreal.Vector(loc.x, y, g.sample_height(unreal.Vector2D(loc.x, y))))
    g.end_paint_stroke()
    h.call_method("RebuildHouse")
    note("road")


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    house = find("House_Road")
    if not (ground and house):
        unreal.log_error("DECOR FAILED: no Ground_Demo / House_Road")
        return None
    STATE["ground"], STATE["house"] = ground, house

    ground.reset_paint()
    house.call_method("RebuildHouse")

    # 渲染侧的自诊断先打一行 —— 判据红了先看这行，它能一句话区分"没画出来"与"画了但看不见"。
    # ⚠️ 调原因版，**别调 is_decor_drawable()**：UE Python 把"bool 返回值 + 一个 out 参数"
    # 收成单一返回值 —— 可画时拿到空串、**不可画时拿到 `None`**，原因串整个丢掉，
    # 恰好在唯一需要它的时候失效（这一行存在的全部价值就是那句原因）。
    why = str(house.get_decor_undrawable_reason())
    drawable = (why == "")
    unreal.log("DECOR drawable=%s%s" % (drawable, "" if drawable else (" —— " + why)))
    if not drawable:
        unreal.log_error("DECOR !! 摆件画不出来：%s" % why)

    loc = house.get_actor_location()
    foot = house.get_editor_property("FootprintSize")
    wall_h = house.get_editor_property("WallHeight")

    # 机位一律从房子参数反算，不手调 —— 手调的机位在房子一改尺寸之后就全废了。
    # 南墙（-Y 那面）正对着路，也是拱开在的地方；摆件贴着墙脚，所以机位要压低。
    cam_wall = unreal.Vector(loc.x - foot.x * 0.55, loc.y - foot.y * 0.5 - 520.0, loc.z + wall_h * 0.55)
    aim_wall = unreal.Vector(loc.x + foot.x * 0.15, loc.y - foot.y * 0.5, loc.z + wall_h * 0.15)
    cam_3q = unreal.Vector(loc.x - foot.x * 1.0, loc.y - foot.y * 1.5, loc.z + wall_h * 1.3)
    aim_3q = unreal.Vector(loc.x, loc.y, loc.z + wall_h * 0.45)

    CAM_WALL = (cam_wall, look_at(cam_wall, aim_wall))
    CAM_3Q = (cam_3q, look_at(cam_3q, aim_3q))

    # (名字, 拍之前要做的事, 机位)。A 的两张必须**同机位、只切开关**，否则差异率没有意义。
    STATE["plan"] = [
        ("wall_decor_off", lambda: set_decor(False, "decor off"), CAM_WALL),
        ("wall_decor_on", lambda: set_decor(True, "decor on"), CAM_WALL),
        ("house3q", lambda: note("3q"), CAM_3Q),
        ("gate_no_road", lambda: note("no road"), CAM_WALL),
        ("gate_road", paint_road, CAM_WALL),
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
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_wall, CAM_WALL[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    # 坑 ⑦ + ⑧ 必须成对：开 ViewState，然后靠 WARMUP_CAPTURES 一帧一次地推进 Lumen 的帧间历史。
    comp.set_editor_property("always_persist_rendering_state", True)
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


FIRST_TICK = 45      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 6           # 切开关 / 画路 → 重建房子 + 组件重注册，留几帧
# 坑 ⑨ 的执行面。**128 不是保守取整，32 是真的不够**（2026-08-31 重标定）。
# 两档收敛得分开看（整表与重跑方式在 `TinyGladeShotSeamWarmup.py` 的文件头）：
#   ① 「间接光不恒零」：zero% 在 **16 次**就到 0.000% —— 当初取 32 的全部依据，
#      而它只能证明"不是黑图"；
#   ② 「画面不再变」：相对 w384 的差异点占比 16 次 45.293% / **32 次 0.540%** /
#      64 次起 0.000%（192 次那 0.077% = 1/1296，是采样噪声底）。
#      另一个更难收的场景（上一轮的拖动态）：w32 差 2.04%、w128 差 0.015%。
# ⇒ 32 会给每一条像素判据搭上 0.5%~2% 的**系统误差**，与最薄那条门
#   （zero > 0.5%）同量级；取 128 = 收敛平台往里一整个倍频。
#   代价实测：一次 capture_scene() ≈ 35 ms（1081 次 / 38 s）⇒ 每张图多约 3.4 s。
# ⚠️ 必须**一帧一次**：同一帧连打 128 次不推进帧间历史，等于没预热。
# ⚠️ 环境变量只给**纯记录图**降档（set TG_SHOT_WARMUP=32），带门的图一律 128。
WARMUP_CAPTURES = int(os.environ.get("TG_SHOT_WARMUP", "128"))

# 判据的阈值。口径与本项目其余出图脚本一致：**先量真值、再取实测值的一半**，
# 不是"取一个能绿的小数"。标定实测（材质与资产定版后连跑两次，差异 ≤ 1 个采样点）：
#   A  切 `bDecorEnabled` 的墙面差异率   **8.7%**（113/1296）
#   B  摆件覆盖处平均亮度                **45.7**，rgb=(44.0, 49.1, 33.1)
#   C  开拱前后差异率                    **73.2%**（949/1296）
#
# A 与 C 取实测值的一半；物理含义是"摆件 / 门那一家在屏幕上的覆盖面至少还剩一半"。
# B 的失效模式不是"变少"而是"变黑"：第一轮标定时真的撞到过 —— base color 被
# `MaterialExpressionClamp` 接断之后摆件全黑，当时 luma 只有 **19.2**、且精确 (0,0,0) 占比
# 从 0.154% 涨到 0.694%。取一半（22.0）恰好把那一档留在红带里。
#
# ⚙️ 色调那一条（`red > blue * 1.15`）**不是取一半的阈值而是结构判据**：引擎默认
# 表面材质是中性灰（R ≈ B），而 clutter 的顶点色是木头（实测 R/B ≈ 1.7，上屏后 1.33）。
# 它抵的是"母材质没勾 `bUsedWithInstancedStaticMeshes` ⇒ 静默换成默认材质"那一枪。
DECOR_DIFF_FAIL = 0.043   # A = 8.7% / 2
DECOR_LUMA_FAIL = 22.0    # B = 45.7 / 2
GATE_DIFF_FAIL = 0.366    # C = 73.2% / 2


def report():
    s = STATE["samples"]
    ok = True
    for name in sorted(s):
        z = zero_ratio(s[name])
        unreal.log("DECOR PIXELS %-16s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("DECOR !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑧）"
                             % (name, z * 100.0))
            ok = False

    off, on = s.get("wall_decor_off"), s.get("wall_decor_on")
    if off and on:
        mask = changed_mask(off, on)
        changed = sum(1 for m in mask if m)
        ratio = changed / float(GRID_POINTS)
        unreal.log("DECOR PIXELS wall decor on-vs-off: %d/%d changed (%.1f%%)"
                   % (changed, GRID_POINTS, ratio * 100.0))
        # A：同机位只切 bDecorEnabled，画面必须真的变 —— 这是"摆件真的画出来了"的硬证据。
        if ratio < DECOR_DIFF_FAIL:
            unreal.log_error("DECOR !! 切换 bDecorEnabled 几乎没有改变画面 —— 摆件没有被画出来")
            ok = False

        # B：**变化的那批像素**（= 摆件覆盖的位置）在开着摆件那张里的平均亮度与色调。
        lit = [on[i] for i, m in enumerate(mask) if m]
        if lit:
            luma = sum(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in lit) / float(len(lit))
            red = sum(p[0] for p in lit) / float(len(lit))
            green = sum(p[1] for p in lit) / float(len(lit))
            blue = sum(p[2] for p in lit) / float(len(lit))
            unreal.log("DECOR PIXELS decor pixels: luma=%.1f rgb=(%.1f, %.1f, %.1f)"
                       % (luma, red, green, blue))
            if luma < DECOR_LUMA_FAIL:
                unreal.log_error("DECOR !! 摆件覆盖处平均亮度只有 %.1f —— 八成是顶点色搬成了黑" % luma)
                ok = False
            # 引擎默认材质是中性灰（R ≈ B）；本材质是暖调（顶点色 × 暖常数）。
            # 这一条抓的正是"母材质没勾 bUsedWithInstancedStaticMeshes ⇒ 静默换成默认材质"。
            if red <= blue * 1.15:
                unreal.log_error("DECOR !! 摆件覆盖处不是暖调（rgb=%.1f, %.1f, %.1f）—— 材质八成被换成了默认灰"
                                 % (red, green, blue))
                ok = False

    a, b = s.get("gate_no_road"), s.get("gate_road")
    if a and b:
        changed = sum(1 for m in changed_mask(a, b) if m)
        ratio = changed / float(GRID_POINTS)
        unreal.log("DECOR PIXELS gate road-vs-none: %d/%d changed (%.1f%%)"
                   % (changed, GRID_POINTS, ratio * 100.0))
        if ratio < GATE_DIFF_FAIL:
            unreal.log_error("DECOR !! 开拱前后画面几乎没变 —— 门那一家没长出来")
            ok = False

    # 数值侧的配套：门开出来之后锚点与实例都必须涨（"密度由锚点数量决定"）。
    before = STATE["counts"].get("no road")
    after = STATE["counts"].get("road")
    if before and after:
        unreal.log("DECOR COUNTS no-road anchors=%d gate=%d instances=%d -> road anchors=%d gate=%d instances=%d doors=%d"
                   % (before[0], before[3], before[1], after[0], after[3], after[1], after[2]))
        if after[2] <= 0:
            unreal.log_error("DECOR !! 画路没开出拱，下面那条是空判据")
            ok = False
        # ⚠️ 必须看**门那一家**的锚点，不能看总数：开拱同时会把门口净空与路面上的
        # 墙脚锚点排掉，两边数量可以刚好抵消（实测碰到过：20 → 20）。
        elif after[3] <= before[3]:
            unreal.log_error("DECOR !! 开拱之后门那一家的锚点没有变多 —— 它没接上")
            ok = False

    unreal.log("DECOR PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


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
            # 坑 ⑨：实例的可见集按主视口视角压，先让主视口驾驶这台相机。
            unreal.EditorLevelLibrary.pilot_level_actor(STATE["cap"])
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            # 坑 ⑧：一帧只打一次 capture_scene()，Lumen 的帧间历史才会真的往前走。
            # 挪过相机 / 切过开关之后历史等于清零，所以**每个机位都要重新预热满**。
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "decor_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("DECOR shot: decor_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    if STATE["house"]:
        STATE["house"].set_editor_property("bDecorEnabled", True)
        STATE["house"].call_method("RebuildHouse")
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("DECOR DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("DECOR FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
