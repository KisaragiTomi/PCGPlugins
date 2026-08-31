# -*- coding: utf-8 -*-
"""**`WARMUP_CAPTURES` 的标定脚本**：Lumen 要连拍几帧，出图才既不吐纯零、又不再变。

全部出图脚本的那个常数由本脚本定，改它之前先重跑这里。

--- 第一档：不再吐精确零（2026-08-30，本脚本第一次的任务）------------------------
症状判死的过程：黑的是**地面**（板缝里透出来的那部分），精确为零**只发生在**
"离屏 capture + 有 ViewState + GI 翻回 Lumen + 只抓一帧"这一种组合上。
同机位实测：单帧 17.23% / 连拍 198 帧 0.00% / GI=None 0.00%。
阶梯实测：**1 次 19.25% / 8 次 0.007% / 16 次起 0.000%**。
⇒ 当初取 32 的依据全在这一档。**但这一档只能证明"不是黑图"。**

--- 第二档：画面不再变（2026-08-31 补，本脚本这次的任务）-------------------------
⚠️ **32 是下限不是上限。** 本轮实测曲线（同机位、以 w384 为基准，48 × 27 采样格、
单通道 > 12/255 —— 与各条门**同一个口径**，各用各的口径等于没标定）：

    N      zero%       diff-vs-w384
    1      28.858%     52.623%
    8       1.080%     52.469%
    16      0.000%     45.293%   ← 第一档到此就满足了，第二档还差一半画面
    32      0.000%      0.540%   ← 7/1296，与最薄那条门（zero > 0.5%）同量级
    64      0.000%      0.000%
    128     0.000%      0.000%   ← 取它
    192     0.000%      0.077%   ← 1/1296 = 采样噪声底，不是没收敛
    256     0.000%      0.000%
    384     0.000%      —（基准）

另一个**更难收**的场景（上一轮修好法线解包之后量的拖动态）：w32 距自己的 w384 差 **2.04%**，
w128 只差 **0.015%**。两个场景指向同一个结论。

本项目所有像素判据量的正是"差异点占比"，所以预热不足会给每一条门搭上一个同量级的
**系统误差**（实测 0.5%~2%），而好几条门的信号本身就在个位数百分比。

⇒ **全部出图脚本统一 `WARMUP_CAPTURES = 128`**：它是收敛平台（64 起）往里一整个倍频，
留给比本场景收得慢的那些。**没有分档**：实测一次 capture_scene() ≈ 35 ms（1081 次 / 38 s），
32 → 128 每张图只多约 3.4 s —— 代价不足以换掉判据的可信度。
`TG_SHOT_WARMUP` 只留给"今天只想快速看一眼"的纯记录图。

本脚本**自己算这条曲线**，不只是导图：每一路在第 N 次捕获时取一份采样格，收尾时报出
「zero%」与「相对最长那一路的差异点占比」两列（日志里的 `SEAMWARM curve` 行）。
判读方式：挑第一个让第二列落进采样噪声底、且**它的前一档也已经落进去**的 N ——
只看单点会被 192 那一格的 0.077% 骗到，以为还没收敛。


⚠️ 九路是**串行**推进的（tick 里那个 `break`）：一帧只喂一路，几路的 Lumen 历史才不会
互相借用。代价是总帧数 = sum(N) ≈ 1081，一次标定几分钟 —— 别为了快改成并行，
那样量出来的曲线是几路共享历史的结果，正好把要测的东西测没了。

用法（真编辑器 + tick）::

    UnrealEditor.exe <uproject> -ExecCmds="py <本文件>" -nosplash -abslog=<独立日志>
"""
import math

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900
WARMUPS = [1, 8, 16, 32, 64, 128, 192, 256, 384]
# 采样格与容差与出图脚本的判据**同一套**（48 × 27 = 1296 点、单通道 > 12/255）——
# 标定量出来的数必须与被标定的那些门可比，各用各的口径等于没标定。
GRID_X, GRID_Y = 48, 27
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
SAMPLES = {}

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "rigs": [], "spawned": [], "ground": None, "world": None,
         "loc": None, "rot": None}


def log(msg):
    unreal.log("SEAMWARM %s" % msg)


def look_at(frm, to):
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()
    ground = next((a for a in ACTORS.get_all_level_actors()
                   if "Ground" in a.get_class().get_name() and "Shaper" not in a.get_class().get_name()), None)
    shapers = [a for a in ACTORS.get_all_level_actors() if "Shaper" in a.get_class().get_name()]
    if not ground or not shapers:
        unreal.log_error("SEAMWARM FAILED: no ground/shaper")
        return None
    STATE["ground"] = ground
    hub = shapers[0].get_actor_location()

    sun_yaw = 0.0
    for a in ACTORS.get_all_level_actors():
        if a.get_class().get_name() == "DirectionalLight":
            sun_yaw = a.get_actor_rotation().yaw
            break
    sx, sy = math.cos(math.radians(sun_yaw)), math.sin(math.radians(sun_yaw))

    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(41):
        t = -1600.0 + i * 80.0
        x, y = hub.x + sx * t, hub.y + sy * t
        ground.apply_paint_stroke(unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y))))
    ground.end_paint_stroke()

    def at(along, up):
        x, y = hub.x + sx * along, hub.y + sy * along
        return unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y)) + up)

    STATE["loc"] = at(2600.0, 260.0)
    STATE["rot"] = look_at(STATE["loc"], at(0.0, -120.0))

    for n in WARMUPS:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
            unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, STATE["loc"], STATE["rot"])
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", 55.0)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        pp = unreal.PostProcessSettings()
        pp.set_editor_property("override_dynamic_global_illumination_method", True)
        pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
        pp.set_editor_property("override_reflection_method", True)
        pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
        comp.set_editor_property("post_process_settings", pp)
        comp.set_editor_property("post_process_blend_weight", 1.0)
        comp.set_editor_property("always_persist_rendering_state", True)
        STATE["spawned"].append(cap)
        STATE["rigs"].append([n, comp, rt, 0, False])
    return world


FIRST_TICK = 60


def sample_pixels(rt):
    """采样格上的 RGB。逐像素读 144 万次太慢，48 × 27 = 1296 个点足以给出稳定的差异率。"""
    out = []
    for gy in range(GRID_Y):
        y = int((gy + 0.5) * H / GRID_Y)
        for gx in range(GRID_X):
            x = int((gx + 0.5) * W / GRID_X)
            col = unreal.RenderingLibrary.read_render_target_pixel(STATE["world"], rt, x, y)
            out.append((col.r, col.g, col.b))
    return out


def report():
    """两列：zero%（第一档）与「相对最长那一路的差异点占比」（第二档）。"""
    ref_n = max(WARMUPS)
    ref = SAMPLES.get(ref_n)
    log("curve  N     zero%%     diff-vs-w%d" % ref_n)
    for n in WARMUPS:
        cur = SAMPLES.get(n)
        if not cur:
            continue
        zero = sum(1 for c in cur if c[0] == 0 and c[1] == 0 and c[2] == 0) / float(len(cur))
        if ref and len(ref) == len(cur):
            diff = sum(1 for a, b in zip(cur, ref)
                       if max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) > PIXEL_DELTA)
            log("curve  %-5d %7.3f%%  %7.3f%% (%d/%d)"
                % (n, zero * 100.0, diff * 100.0 / float(len(cur)), diff, len(cur)))
        else:
            log("curve  %-5d %7.3f%%  —" % (n, zero * 100.0))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    done = True
    for rig in STATE["rigs"]:
        n, comp, rt, shots, exported = rig
        if exported:
            continue
        done = False
        comp.capture_scene()
        rig[3] = shots + 1
        if rig[3] >= n:
            unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR,
                                                          "seamwarm_warm%03d.png" % n)
            SAMPLES[n] = sample_pixels(rt)
            rig[4] = True
            log("shot: seamwarm_warm%03d.png（第 %d 次 capture_scene）" % (n, rig[3]))
        break            # 一帧只推进一路，几路的 Lumen 历史才不会互相借用
    if not done:
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    log("DONE")
    unreal.SystemLibrary.quit_editor()


try:
    STATE["world"] = build()
except Exception as exc:
    import traceback
    unreal.log_error("SEAMWARM FAILED in build(): %s\n%s" % (exc, traceback.format_exc()))
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
