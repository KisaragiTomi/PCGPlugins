# -*- coding: utf-8 -*-
"""
门框砖"块数跳变时不露缝"的对比图 —— B 那条改动的**图像**证据。
穷举版的证据是断言：`PCGPlugins.ComputeShaderGenerator.House.FrameBrickOverlap`
（扫 10001 个弧长、穿过 38 次块数跳变，钉住砖缝恒为负）。这里只是把它拍出来给人看。

要证的事：**砖数变了，拱缘上也不会冒出缝。**
做法：只动 FrameBrickLength 一个量 —— 先扫一遍找出恰好跨过一次块数跳变的两个**相邻**取值
（步长 0.05 cm，砖本身大 0.2% 都不到，肉眼分不出），在同一机位各拍一张；再把砌法换回改动前的
正缝参数（FrameBrickGap 1.5 / 不胀）拍同样两张。四张图里只有"正缝"那一组的拱缘是一格一格断开的，
而且断口位置会随块数跳变整条挪一遍 —— 那正是运行时看到的闪跳。

产物：Saved/TinyGladeShots/seam_{legacy,bloat}_{lo,hi}.png（同机位同分辨率，可直接逐像素比）

--- 踩过的坑（全部来自 TinyGladeShotLighting.py 的文件头，别重踩）-------------------
 ① `unreal.Rotator` 的参数序是 **(roll, pitch, yaw)**，一律用关键字参数。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`；`-ExecutePythonScript`
    跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot`（本工程起来是 4 分屏）。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`，否则导出的 png 是 HDR 内容。
 ⑤ 离屏 SceneCapture 被引擎**写死**关掉 Lumen，只有捕获组件自己的 `post_process_settings`
    能覆盖回来（它在那两行赋值之后才 apply）。
 ⑥ GPU 实例的可见集按主视口视角逐帧 compact，每张图先让主视口**驾驶**捕获相机、等几帧再拍。
 ⑦ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState**，于是眼适应整条链不跑，
    **PPV 里的曝光一条都不生效**（实测把 EV100 改 2.83 倍，出图每个分位数逐位不变）。
    与坑 ⑤ 症状字面相同、根因完全不同，别拿坑 ⑤ 的办法去修它。
 ⑧ ⚠️ **修了 ⑦ 才会触发 ⑧，两条必须一起加。** 有了 ViewState，坑 ⑤ 翻回来的 Lumen 才
    第一次真的跑起来；而 Lumen 的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒
    间接光恒为零 ⇒ 没被太阳直射的 lit 表面全部精确 (0,0,0)。对本脚本尤其致命：拱缘朝北、
    整条砖带都在背光里 —— 不预热的话四张图的拱缘一律纯黑，"缝"与"不缝"字面上无法区分。
    修法：每张图导出前预热 `WARMUP_CAPTURES` 次，**一帧一次**（同一帧连打不推进帧间历史）。
"""
import math
import os

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 2400, 1350

# 改动后 / 改动前的两套砌法。名字直接进文件名。
BLOAT_SET = ("bloat", 0.0, 1.1)     # 排布不留缝 + 沿曲线胀 10% ⇒ 净缝 −2.6 cm
LEGACY_SET = ("legacy", 1.5, 1.0)   # 改动前的出厂值 ⇒ 净缝 +1.5 cm

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()
spawned = []


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


ground = find("Ground_Demo")
house = find("House_Road")
if not (ground and house):
    unreal.log_error("SEAM SHOT FAILED: demo actors missing")
    raise SystemExit

# 画一条路，否则没有门拱、也就没有门框砖可看（与 TinyGladeShotLighting.py 同一笔）。
loc = house.get_actor_location()
ground.reset_paint()
ground.begin_paint_stroke()
for i in range(17):
    y = loc.y - 800.0 + i * 100.0
    ground.apply_paint_stroke(unreal.Vector(loc.x, y, ground.sample_height(unreal.Vector2D(loc.x, y))))
ground.end_paint_stroke()


def apply_frame(gap, bloat, length):
    """砌法三件套一起写再重建；返回这次砌出来的砖数。"""
    house.set_editor_property("FrameBrickGap", gap)
    house.set_editor_property("FrameBrickBloat", bloat)
    house.set_editor_property("FrameBrickLength", length)
    house.call_method("RebuildHouse")
    return house.get_frame_brick_count()


# ---- 找块数跳变：**每套砌法各找各的** --------------------------------------------
# 两套砌法的槽距不同（27.5×scale vs 26×scale），跳变点本来就落在不同的砖长上；拿正缝那套的
# 跳变点去拍胀大那套，拍到的只是"没跳变"，证不了任何事。所以各扫各的，各拍自己跨过跳变的那一对。
# 步长 0.05 cm 是刻意的：两张图之间砖长只差 0.2%，画面上任何可见差别都只能来自块数。
STEPS = [round(24.0 + i * 0.05, 2) for i in range(81)]


def find_jump(gap, bloat):
    """返回相邻一对砖长 (lo, hi) 及其块数，取扫描范围内块数跳得最狠的那一对。"""
    counts = [apply_frame(gap, bloat, length) for length in STEPS]
    best = None
    for i in range(len(STEPS) - 1):
        delta = abs(counts[i + 1] - counts[i])
        if delta > 0 and (best is None or delta > best[0]):
            best = (delta, STEPS[i], STEPS[i + 1], counts[i], counts[i + 1])
    return best


JOBS = []
for tag, gap, bloat in (LEGACY_SET, BLOAT_SET):
    found = find_jump(gap, bloat)
    if found is None:
        unreal.log_error("SEAM SHOT FAILED: no brick-count jump for '%s' in the swept range" % tag)
        raise SystemExit
    _, len_lo, len_hi, cnt_lo, cnt_hi = found
    unreal.log("SEAM %s jump: length %.2f -> %.2f cm (%.2f%%), bricks %d -> %d"
               % (tag, len_lo, len_hi, (len_hi / len_lo - 1.0) * 100.0, cnt_lo, cnt_hi))
    JOBS.append(("%s_lo" % tag, gap, bloat, len_lo))
    JOBS.append(("%s_hi" % tag, gap, bloat, len_hi))

# ---- 机位：与 TinyGladeShotLighting.py 的 "archframe" 逐字相同（已知能框住三个拱）----
cam_loc = unreal.Vector(loc.x - 60.0, loc.y - 620.0, 170.0)
aim = unreal.Vector(loc.x, loc.y - 250.0, 150.0)
dx, dy, dz = aim.x - cam_loc.x, aim.y - cam_loc.y, aim.z - cam_loc.z
cam_rot = unreal.Rotator(roll=0.0,
                         pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))),
                         yaw=math.degrees(math.atan2(dy, dx)))

# 见坑 ⑤：把捕获自己那份 PP 覆盖建起来，Lumen 才会回到出图里。
CAP_PP = unreal.PostProcessSettings()
CAP_PP.set_editor_property("override_dynamic_global_illumination_method", True)
CAP_PP.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
CAP_PP.set_editor_property("override_reflection_method", True)
CAP_PP.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

rt = unreal.RenderingLibrary.create_render_target2d(
    world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_loc, cam_rot)
comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property("texture_target", rt)
comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property("fov_angle", 60.0)
comp.set_editor_property("capture_every_frame", False)
comp.set_editor_property("capture_on_movement", False)
comp.set_editor_property("post_process_settings", CAP_PP)
comp.set_editor_property("post_process_blend_weight", 1.0)
# 坑 ⑦：没有 ViewState ⇒ 眼适应不跑 ⇒ 曝光一条都不生效。
# ⚠️ 开了它就会继承坑 ⑧，必须同时有下面的 `WARMUP_CAPTURES` 预热。
comp.set_editor_property("always_persist_rendering_state", True)
spawned.append(cap)

STATE = {"ticks": 0, "handle": None, "job": 0, "phase": 0, "samples": {}}
FIRST_TICK = 40      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 5           # 换砌法要重建整栋房子 + 让驾驶视口跟上，留几帧
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

# 判据的采样格与容差。⚠️ 这张图是 2400 × 1350，格子按同样的密度加密到 72 × 40。
GRID_X, GRID_Y = 72, 40
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
# 精确 (0,0,0) 的容许上限，与 `TinyGladeShotSofteningStats.py` 的 `ZERO_FAIL` 同一个数。
ZERO_FAIL = 0.005
# 两套砌法同机位差异率的下限。
#
# 标定依据（管线修好后实测）：legacy_lo vs bloat_lo = **6.7%**（193/2880）。
# 这张贴脸图里三个拱缘只占画面的一小块，所以这个数天然远小于岩壳那条 57.9% ——
# 取实测值的**一半**（3.35%），物理含义是"拱缘上被砌法改动到的像素至少还剩一半"。
#
# 复现性（两次独立的编辑器启动，同一份代码）：`TinyGladeShotRockShell.py` 的两条判据
# **逐位相同**（750/1296 与 71/1296 各两次）；其余脚本的差异像素数在 ±3 个采样点之内摆动，
# 只有裙边噪声那条 `pair3q` 摆到 331 → 372（相对 12%，因为噪声改的是**几何**，Lumen 的
# 收敛路径跟着变）。取实测值的一半留出的余量比这个摆幅大一个量级。
SET_DIFF_FAIL = 0.0335


def sample_pixels():
    out = []
    for gy in range(GRID_Y):
        y = int((gy + 0.5) * H / GRID_Y)
        for gx in range(GRID_X):
            x = int((gx + 0.5) * W / GRID_X)
            col = unreal.RenderingLibrary.read_render_target_pixel(world, rt, x, y)
            out.append((col.r, col.g, col.b))
    return out


def diff_ratio(a, b):
    changed = sum(1 for pa, pb in zip(a, b)
                  if max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > PIXEL_DELTA)
    return changed / float(len(a)), changed


def zero_ratio(samples):
    """精确 (0,0,0) 占比 —— 坑 ⑧ 有没有被预热掉的**指纹**。拱缘整条在背光里，
    这个数不为零时四张图的判读**全部作废**（黑的是捕获的 bug，不是砖缝）。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


def report():
    ok = True
    s = STATE["samples"]
    for key in sorted(s):
        z = zero_ratio(s[key])
        unreal.log("SEAM PIXELS %-14s zero=%.3f%%" % (key, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("SEAM !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑧）"
                             % (key, z * 100.0))
            ok = False
    a, b = s.get("legacy_lo"), s.get("bloat_lo")
    if a and b:
        ratio, changed = diff_ratio(a, b)
        unreal.log("SEAM PIXELS %-14s legacy-vs-bloat: %d/%d changed (%.1f%%)"
                   % ("lo", changed, GRID_POINTS, ratio * 100.0))
        # 这条判据回答的是"门框砖真的画在了屏幕上"。
        # ⚠️ 它**故意不去断言"胀大那组的缝比正缝小"**：驱动方看过 `seam_compare.png`，
        # 上下两排的差别肉眼读不出，把它写成阈值就是在给噪声定标准。
        # "不露缝"的证明在穷举断言 `House.FrameBrickOverlap` 上（10001 个弧长、38 次块数跳变），
        # 图这边只需要证明砖**在画面里**：两套砌法的槽距差 5.5%（27.5 vs 26）+ 砖数差 6 块，
        # 拱缘上每块砖的位置都挪了 —— 砖若一块都没画出来，这个差异率会塌到 0。
        # 这正是石阶那个坑（`StairMesh` 恒 NULL、画面全黑、所有 readback 断言全绿）的正面执行面。
        if ratio < SET_DIFF_FAIL:
            unreal.log_error("SEAM !! 两套砌法的画面几乎相同 —— 门框砖没有被画出来")
            ok = False
    unreal.log("SEAM PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    job = STATE["job"]
    if job < len(JOBS):
        name, gap, bloat, length = JOBS[job]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            bricks = apply_frame(gap, bloat, length)
            unreal.log("SEAM %s: gap=%.2f bloat=%.3f length=%.2f bricks=%d" % (name, gap, bloat, length, bricks))
            # 见坑 ⑥：实例的可见集按主视口视角压，先让主视口驾驶这台相机。
            unreal.EditorLevelLibrary.pilot_level_actor(cap)
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            # 坑 ⑧：一帧只打一次，Lumen 的帧间历史才会真的往前走；换砌法等于清历史，
            # 所以**每张图都要重新预热满**。
            comp.capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "seam_%s.png" % name)
                STATE["samples"][name] = sample_pixels()
                unreal.log("shot: seam_%s.png（预热 %d 帧）" % (name, WARMUP_CAPTURES))
                STATE["job"], STATE["phase"] = job + 1, 0
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    # 收尾必须把砌法写回出厂值，否则这次实验的参数会跟着关卡存盘漂出去。
    apply_frame(BLOAT_SET[1], BLOAT_SET[2], 26.0)
    ground.reset_paint()
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.log("SEAM SHOT DONE")
    unreal.SystemLibrary.quit_editor()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
