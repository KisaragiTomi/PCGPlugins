# -*- coding: utf-8 -*-
"""演示关卡的石阶出图：整条爬坡 + 一张能看见小石子的近景。

回答的是一件只有像素能答的事：TG 那颗 15% 的小石子（`stairs_pebble`，27–54 cm）
**看不看得见**。判据是 `after` / `nopebble` 两个 tag 的**同机位对照** —— 石子与石阶共用
同一份石材，单张图上分不出哪块是石子。

历史：这个脚本原来还有一个 `before` tag，把塑形物自持的旧路石阶调色板临时写回去，
拍"两套石阶叠在一起"的对照。旧路（`RebuildSteps` + `CSShaperSteps` + `CSGroundSteps.usf`）
已随 2026-08-30「裁决一」第二步整条删除，"只有一套"从此是代码事实，那个 tag 随之作废。

用法（tag 决定文件名前缀）::

    set TG_SHOT_TAG=after      # 常规：GPU 石阶 + 小石子
    set TG_SHOT_TAG=nopebble   # 同 after，但 StairPebbleChance = 0 ⇒ 石子的同机位对照组
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotDemoSteps.py"

产物：`Saved/TinyGladeShots/steps_<tag>_*.png`。

--- 出图管线的口径，逐条照 `TinyGladeShotSoftening.py` --------------------------
 ① `unreal.Rotator` 参数序是 (roll, pitch, yaw)，一律关键字参数。
 ② 必须真编辑器 + `-ExecCmds="py …"`：`-ExecutePythonScript` 跑完即退，tick 回调不触发。
 ③ 离屏 `SceneCapture2D`，不用 `HighResShot`（本工程是 4 分屏）。
 ④ `create_render_target2d` 显式传 `RTF_RGBA8`。
 ⑤ 离屏捕获被引擎写死关掉 Lumen ⇒ 用捕获组件自己的 `post_process_settings` 只翻回
    GI/反射两项，其余交给世界 PPV（否则量的是脚本不是关卡）。
 ⑥ 准备段整个包在 try 里，否则抛异常 ⇒ 编辑器永不退出。
 ⑧ + ⑨ **缺一不可**：`always_persist_rendering_state = True` 让捕获真的有 ViewState
    （否则曝光一条都不生效），**而它一开，Lumen 就第一次真的跑起来**，
    而 Lumen 的最终聚集靠帧间历史 ⇒ 单帧 `capture_scene()` 的间接光恒为零、背光面全黑。
    所以每个机位导出前必须**一帧一次**、共 `WARMUP_CAPTURES` 次预热。只加 ⑧ 会让图更糟。
 ⑦ ⚠️ 已知未决缺陷：`L_TerrainOpsDemo` 的 GPU 实例在离屏 capture 下**可能只画出一个固定
    子集**（状态文件「离屏 SceneCapture 画不出 GPU 实例」，真因仍开放）。
    所以这组图是**辅证**；"石子存在"的主证据是回归里的 readback 断言（`pebbles=…`）。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "after")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "rigs": [], "spawned": [], "ground": None,
         "shaper": None, "world": None}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find_ground():
    for a in ACTORS.get_all_level_actors():
        name = a.get_class().get_name()
        if "Ground" in name and "Shaper" not in name:
            return a
    return None


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find_ground()
    shapers = [a for a in ACTORS.get_all_level_actors() if "Shaper" in a.get_class().get_name()]
    if not ground or not shapers:
        unreal.log_error("STEPSHOT FAILED: ground=%s shapers=%d" % (ground, len(shapers)))
        return None
    shaper = shapers[0]
    STATE["ground"] = ground
    STATE["shaper"] = shaper

    # `nopebble` 是石子的**同机位对照组**：只把概率关成 0，别的一个字节都不动
    # （石子与石阶共用石材，单张图上分不出哪块是石子 —— 判据只能是这一对图的差）。
    ground.set_editor_property("StairPebbleChance", 0.0 if TAG.startswith("nopebble") else 0.15)
    shaper.rebuild_terrain()

    pebble_mesh = ground.get_editor_property("StairPebbleMesh")
    unreal.log("STEPSHOT tag=%s stairMesh=%s pebbleMesh=%s"
               % (TAG, ground.get_editor_property("StairMesh"), pebble_mesh))

    hub = shaper.get_actor_location()
    radius = shaper.get_editor_property("Radius")
    falloff = shaper.get_editor_property("FalloffDistance")

    # 机位方位角只取太阳 yaw（改灯只改 pitch/强度，yaw 不动）—— 否则改前/改后不是同机位。
    sun_yaw = 0.0
    for a in ACTORS.get_all_level_actors():
        if a.get_class().get_name() == "DirectionalLight":
            sun_yaw = a.get_actor_rotation().yaw
            break
    sx, sy = math.cos(math.radians(sun_yaw)), math.sin(math.radians(sun_yaw))
    px, py = -sy, sx

    # 路照 TinyGladeShotSoftening.py 那一笔：沿最陡方向穿过土台中心，两侧裙边都长石阶。
    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(41):
        t = -1600.0 + i * 80.0
        x, y = hub.x + sx * t, hub.y + sy * t
        ground.apply_paint_stroke(unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y))))
    ground.end_paint_stroke()
    ground.rebuild_stairs()     # 落笔之后才知道哪几段等值线要摆（内部也会被落笔触发，这里明写一次）

    peb = ground.debug_read_stair_pebbles_sync()
    stairs = ground.debug_read_stairs_sync()
    unreal.log("STEPSHOT gpuStairs=%s pebbles=%s"
               % (stairs[0] if isinstance(stairs, tuple) else stairs,
                  peb[0] if isinstance(peb, tuple) else peb))

    def at(along, side, up):
        x, y = hub.x + sx * along + px * side, hub.y + sy * along + py * side
        return unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y)) + up)

    steep = radius + 0.5 * falloff   # smoothstep 极值点：最陡处，石阶最密

    cams = [
        # A 整条石阶：三四分俯看一整段爬坡，这个尺度最好读整条等值线的连续性。
        ("run", at(-steep + 320.0, 620.0, 380.0), at(-steep, 0.0, 60.0)),
        # B 小石子：**斜上方俯看踏面**。石子落在弦上、石阶被 `StairEmbed` 往上坡推了 30 cm，
        # 所以石子恒在每级的下坡侧（踏步鼻前方那条缝里）—— 从坡下平视会被自己的台阶挡住，
        # 必须俯看。
        # ⚠️ 第一版把机位怼到 130 cm 处平视，画面塞满白石头 ⇒ 自动曝光被一片纯白拖爆，
        #    石子和踏面过曝成同一块白，什么都读不出来。留一块草地在画面里把曝光钉住。
        # ⚠️ 石子与石阶**共用同一份石材**（本来就该如此），所以"看得见"这件事的判据是
        #    `pebble` / `nopebble` 两个 tag 的**同机位对照**，不是单张图上找石头。
        # ⚠️ 机位与 `run` **完全相同**，只把 FOV 从 55 收到 26 —— 换机位试了两版都过曝：
        #    只要画面被白石头填满，自动曝光就把石子和踏面一起推到同一块白。`run` 的构图
        #    自带半幅草地把曝光钉住，长焦裁进去仍然保留一条草边，这是唯一稳的做法。
        ("pebble", at(-steep + 320.0, 620.0, 380.0), at(-steep + 40.0, 60.0, 70.0)),
        # C 全景：一眼看出整个裙边上的石阶分布。
        ("wide", at(2600.0, 2100.0, 1300.0), at(0.0, 0.0, 0.0)),
    ]

    # 坑 ⑤：只翻回 GI/反射方法两项。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    for name, loc, aim in cams:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
            unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, loc, look_at(loc, aim))
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", 26.0 if name == "pebble" else 55.0)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        comp.set_editor_property("post_process_settings", cap_pp)
        comp.set_editor_property("post_process_blend_weight", 1.0)
        # 坑 ⑧：没有 ViewState ⇒ 眼适应整条链不跑 ⇒ PPV 里的曝光一条都不生效。
        # 开了它就必须配下面的预热（坑 ⑨），两条缺一不可。
        comp.set_editor_property("always_persist_rendering_state", True)
        STATE["rigs"].append([name, comp, rt, 0, False])
        STATE["spawned"].append(cap)
        unreal.log("STEPSHOT cam %-7s at (%.0f, %.0f, %.0f)" % (name, loc.x, loc.y, loc.z))
    return world


FIRST_TICK = 60      # 世界侧收敛（天光实时捕获、Lumen 场景）要几十帧
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


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    # 一帧只推进**一个**机位：几路同时连拍会互相抢帧，谁都攒不满自己的历史。
    for rig in STATE["rigs"]:
        name, comp, rt, shots, exported = rig
        if exported:
            continue
        comp.capture_scene()
        rig[3] = shots + 1
        if rig[3] >= WARMUP_CAPTURES:
            unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR,
                                                         "steps_%s_%s.png" % (TAG, name))
            rig[4] = True
            unreal.log("STEPSHOT shot: steps_%s_%s.png（预热 %d 帧）" % (TAG, name, rig[3]))
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("STEPSHOT DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception as exc:
    unreal.log_error("STEPSHOT FAILED in build(): %s" % exc)
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
