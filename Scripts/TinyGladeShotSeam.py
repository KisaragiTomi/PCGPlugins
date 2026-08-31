# -*- coding: utf-8 -*-
"""D7 接缝（裁决二）的改前/改后同机位对照图 + **一次故意破坏世界侧的对照实验**。

一次编辑器运行出五组图，五组共用**同一个世界、同一批相机**，相机一帧都不动 ——
所以任意两组之间的像素差异只可能来自那一组自己改的那一件事。

  · `off`     两栋房都 `bSeamEnabled = False`，**邻居从捕获里隐藏**
  · `on`      两栋房都 `bSeamEnabled = True`，邻居同样隐藏
              ⇒ `off` ↔ `on` 的差异**只可能**是 House_Road 自己身上的接缝：
                 插进邻居里的那截墙被 clip 掉 + 轮廓交点上立起来的两根砖柱。
  · `apart`   **故意破坏世界侧**：接缝照旧开着、代码一行没变、邻居照旧隐藏，
              只把邻居**挪出 footprint**（1400 cm）—— 两栋房不再相交。
              ⇒ 这一组必须**逐像素退回 `off`**。差异不塌回去就说明这道门量的根本不是接缝。
  · `pairoff` / `pairon`  邻居**可见**的那一对，就是交付里要的"两栋相交的房子，改前/改后"。

--- 为什么非要那个 `apart` 组不可 -------------------------------------------------
上一轮 D8 的门第一版看着很像回事（差异率 35.2% → 33.0%），实际在测藤蔓重排而不是测洞 ——
**"两张图不一样"从来不能证明"不一样的地方是我说的那个东西"**。能证的只有一条：
把世界侧那件事拿掉，差异必须塌回噪声底。`apart` 就是那一枪：它不碰任何代码路径
（接缝逻辑照跑），只让世界不再满足触发条件。

⚠️ **邻居必须从捕获里隐藏**（`hidden_actors`），否则 `apart` 组里那栋挪走 1400 cm 的房子
本身就会占满画面，差异塌不下去，实验白做。这条自带检错：万一 `hidden_actors` 没生效，
`apart ↔ off` 会大得离谱而不是接近零，一眼就看得出来。

--- 踩过的坑（逐条抄自 `TinyGladeShotSoftening.py`，理由完整版在那里）-----------------
 ① `unreal.Rotator` 的参数序是 **(roll, pitch, yaw)**，一律关键字参数。
 ② 必须真编辑器 + `-ExecCmds="py <脚本>"`；`-ExecutePythonScript` 跑完即退，tick 不触发。
 ③ 离屏 `SceneCapture2D`，不用 `HighResShot`（本工程是 4 分屏）。
 ④ `create_render_target2d` 显式 `RTF_RGBA8`。
 ⑤ 离屏 capture 被引擎写死关掉 Lumen ⇒ 用捕获自己的 `post_process_settings` 翻回来。
 ⑥ 准备段整个包在 try 里，异常也要走到 Quit（否则编辑器永不退出）。
 ⑧ `always_persist_rendering_state = True` —— 没有 ViewState 时 PPV 的曝光**一条都不生效**。
 ⑨ 导出前每个机位先 `capture_scene()` 预热 `WARMUP_CAPTURES` 次 —— Lumen 的最终聚集靠帧间
    历史，一帧抓出来的间接光恒为零（症状像"几何有洞"）。**⑧ 与 ⑨ 缺一不可**：只做 ⑧ 会触发 ⑨。

⚠️ **不改演示关卡**：邻居是脚本临时生成、跑完销毁的，`.umap` 一个字节都不动
（坑表："脚本会把值烘进 BP CDO 与关卡实例两处" / "脚本把空值存进 .umap 而 umap 不在 git 内"）。

产物：`Saved/TinyGladeShots/seam_<phase>_<cam>.png`；判据用
`Scripts/TinyGladeShotSeamStats.py`（PIL + numpy，编辑器外跑）。
"""
import math

import os

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "cams": [], "spawned": [], "world": None,
         "house": None, "other": None, "home": None, "phase": 0, "shots": 0, "exported": 0}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    for a in ACTORS.get_all_level_actors():
        if a.get_actor_label() == label:
            return a
    return None


def set_seam(on):
    for house in (STATE["house"], STATE["other"]):
        house.set_editor_property("bSeamEnabled", on)
        house.rebuild_house()


def log_seam(where):
    h, o = STATE["house"], STATE["other"]
    unreal.log("SEAM %-8s A(corners=%d cuts=%d seam_bricks=%d frame_bricks=%d) B(corners=%d seam_bricks=%d)"
               % (where, h.get_seam_corner_count(), h.get_seam_cut_count(), h.get_seam_brick_count(),
                  h.get_frame_brick_count(), o.get_seam_corner_count(), o.get_seam_brick_count()))
    # 数值全绿而画面上什么都没有，本项目栽过两次。原因版，不调 is_seam_drawable()：
    # 后者不可画时返回 None，`str(None) == "None"` 会伪造出一句像模像样的原因。
    unreal.log("SEAM %-8s drawable_reason='%s'" % (where, h.get_seam_undrawable_reason()))


# ---- 五个阶段。相机一帧都不动，世界只按下面这一列改 ----
def phase_off():
    STATE["other"].set_actor_location(STATE["home"], False, False)
    set_seam(False)
    hide_neighbour(True)
    log_seam("off")


def phase_on():
    STATE["other"].set_actor_location(STATE["home"], False, False)
    set_seam(True)
    hide_neighbour(True)
    log_seam("on")


def phase_apart():
    """故意破坏**世界侧**：接缝开着、代码没动，只是两栋房不再相交 ⇒ 必须逐像素退回 off。"""
    home = STATE["home"]
    STATE["other"].set_actor_location(unreal.Vector(home.x + 1400.0, home.y, home.z), False, False)
    set_seam(True)
    hide_neighbour(True)
    log_seam("apart")


def phase_pairoff():
    STATE["other"].set_actor_location(STATE["home"], False, False)
    set_seam(False)
    hide_neighbour(False)
    log_seam("pairoff")


def phase_pairon():
    STATE["other"].set_actor_location(STATE["home"], False, False)
    set_seam(True)
    hide_neighbour(False)
    log_seam("pairon")


def phase_off2():
    """**噪声底**：与 `off` 逐字相同的世界，再拍一遍。

    没有它，"破坏组塌到 0.1%"这句话没有刻度 —— 塌到 0.1% 到底是"塌回去了"还是"还剩一点"，
    只有拿"什么都没改时同一对图差多少"来比才答得出。相机、世界、开关全不动，重拍一次。
    """
    phase_off()


PHASES = [("off", phase_off), ("on", phase_on), ("apart", phase_apart),
          ("off2", phase_off2), ("pairoff", phase_pairoff), ("pairon", phase_pairon)]


def hide_neighbour(hidden):
    """把邻居整栋从捕获里摘掉。

    ⚠️ **不能走 `set_editor_property("hidden_actors", ...)`**（实测）：`SceneCapture2D` 的组件是
    默认子对象，引擎判它是 template，那一句直接抛
    `Property 'HiddenActors' ... cannot be edited on templates`。走 BlueprintCallable 的
    `hide_actor_components()` 就没有这条限制（它填的是 `HiddenComponents`，另一张表）。

    ⚠️ **必须在 `rebuild_house()` 之后调**：改属性 ⇒ `RerunConstructionScripts` ⇒ 卸载并重建
    全部实例组件（坑表那条），先隐藏后重建的话隐藏的是一批已经被销毁的组件，画面上什么都没变。
    """
    for _name, comp, _rt in STATE["cams"]:
        comp.clear_hidden_components()
        if hidden:
            comp.hide_actor_components(STATE["other"], True)


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    house = find("House_Road")
    if not (ground and house):
        unreal.log_error("SEAM FAILED: no ground/house")
        return None

    # 画面里**只留接缝砖**：擦掉路 ⇒ 没有门 ⇒ 没有门框砖。这样 off↔on 的差异里
    # 不会混进"门开了/关了"这类与接缝无关的东西。
    ground.reset_paint()
    house.set_editor_property("Windows", [])
    house.set_editor_property("FootprintSize", unreal.Vector2D(600.0, 400.0))
    house.rebuild_house()

    loc = house.get_actor_location()
    bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeHouse" % PKG)
    if not bp:
        unreal.log_error("SEAM FAILED: no BP_TinyGladeHouse")
        return None
    # ⚠️ 必须从 BP CDO 生成：墙材质/砖网格/砖材质那一整套烘在 CDO 上，裸 actor 全是 None。
    other = ACTORS.spawn_actor_from_object(bp, unreal.Vector(loc.x + 250.0, loc.y + 150.0, loc.z))
    if not other:
        unreal.log_error("SEAM FAILED: neighbour did not spawn")
        return None
    other.set_actor_label("House_SeamNeighbour")
    other.set_editor_property("FootprintSize", unreal.Vector2D(500.0, 500.0))
    other.rebuild_house()

    STATE["house"], STATE["other"] = house, other
    STATE["home"] = other.get_actor_location()
    STATE["spawned"].append(other)

    # 轮廓交点（600x400 与 500x500 偏移 (250,150)）：局部 (+300, −100) 与 (0, +200)。
    # 相机放在 (+300, −100) 那一根柱子的**角平分外侧** —— 也就是两栋房都在外面的那个象限，
    # 正是两条裁剪断口露头、砖柱要遮的地方。
    corner = unreal.Vector(loc.x + 300.0, loc.y - 100.0, loc.z)
    diag = math.sqrt(0.5)
    cams = [
        # 贴脸看那根砖柱与被裁掉的墙段。
        ("corner",
         unreal.Vector(corner.x + diag * 620.0, corner.y - diag * 620.0, loc.z + 260.0),
         unreal.Vector(corner.x, corner.y, loc.z + 150.0)),
        # 三四分远景：两栋房与整条缝一起进画面（`pair*` 那一对就是交付要的图）。
        ("wide",
         unreal.Vector(corner.x + diag * 1500.0, corner.y - diag * 1500.0, loc.z + 900.0),
         unreal.Vector(loc.x + 120.0, loc.y + 60.0, loc.z + 120.0)),
    ]

    # 坑 ⑤：只翻回 GI/反射方法两项，其余交给世界 PPV（否则量的是脚本不是关卡）。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    for name, cam_loc, aim in cams:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
            unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_loc, look_at(cam_loc, aim))
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", 55.0)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        comp.set_editor_property("post_process_settings", cap_pp)
        comp.set_editor_property("post_process_blend_weight", 1.0)
        # 坑 ⑧：没有 ViewState 时 PPV 的曝光一条都不生效（症状与坑 ⑤ 字面相同、修法完全不同）。
        comp.set_editor_property("always_persist_rendering_state", True)
        STATE["cams"].append((name, comp, rt))
        STATE["spawned"].append(cap)
        unreal.log("SEAM cam %-7s at (%.0f, %.0f, %.0f)" % (name, cam_loc.x, cam_loc.y, cam_loc.z))
    return world


FIRST_TICK = 60      # 世界侧收敛（天光实时捕获、Lumen 场景）要几十帧，早拍就是一张暗图
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
    if STATE["phase"] >= len(PHASES):
        finish()
        return

    label, setup = PHASES[STATE["phase"]]
    if STATE["shots"] == 0 and STATE["exported"] == 0:
        # 坑 ⑥ 的另一半：tick 里抛异常**不会**停下这个回调 —— 它会每帧再抛一次，
        # 编辑器永远走不到 Quit，任务看着像挂死（本脚本第一版就是这么卡满 10 分钟的）。
        try:
            setup()
        except Exception as exc:
            unreal.log_error("SEAM FAILED in phase '%s': %s" % (label, exc))
            finish()
            return

    # 一帧只推进**一个**机位：几路同时连拍会互相抢帧，谁都攒不满自己的历史。
    name, comp, rt = STATE["cams"][STATE["exported"]]
    comp.capture_scene()
    STATE["shots"] += 1
    if STATE["shots"] >= WARMUP_CAPTURES:
        unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR,
                                                     "seam_%s_%s.png" % (label, name))
        unreal.log("SEAM shot: seam_%s_%s.png（预热 %d 帧）" % (label, name, STATE["shots"]))
        STATE["shots"] = 0
        STATE["exported"] += 1
        if STATE["exported"] >= len(STATE["cams"]):
            STATE["exported"] = 0
            STATE["phase"] += 1


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("SEAM DONE")
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception as exc:
    unreal.log_error("SEAM FAILED in build(): %s" % exc)
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
