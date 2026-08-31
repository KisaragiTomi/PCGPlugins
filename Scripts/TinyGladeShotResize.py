# -*- coding: utf-8 -*-
"""D5 拉尺寸：一段**连续拉伸**的同机位逐帧图 —— 回答"派生物跟不跟得住"。

readback 断言能证明"拖完的一切等于全量重建"（回归里那条已经绿了），但它**答不了**
这一段里有没有肉眼可见的跳变：门拱重排、藤蔓重生、摆件换位、屋脊翻面，四件事都可能在
计数完全不变的情况下把画面掀一遍。所以这一节的判据必须是像素。

产物（`Saved/TinyGladeShots/`）：

  · `resize_on_f0..f8.png`    禁带**开**（默认口径）。同机位，Y 从 400 连续拉到 800。
  · `resize_off_f0..f8.png`   禁带**关**（对照 / 破坏实验之一）。同一段拉伸、同一机位。
  · `resize_on_no{frame,vine,decor}.png`  末尾尺寸上分别关掉一类派生物（破坏实验之二）。
  · `resize_on_f{2,5,8}_{rb,rel}.png`     同一帧的"全量重建"与"松手"孪生帧（判据 D / E）。

⚠️ **实测结论先写在这里，免得看图的人误判**：拖动**中**的画面与"同一状态松手后"的画面
能差 6.3–9.0% 像素，而 CPU 侧每一个量都相同（三角数、`GetWorldBoundsApprox`、脊向、
门/洞/砖/藤/摆件计数逐位相等，两图也**没有任何像素位移**）。差的全是阴影与受光：
背光墙整片压到精确 (0,0,0)（zero% 2.28% → 0.047%）。**几何跟得住，画面收敛以松手为准。**
根因未定位，且与世界状态无关（同一段重放换个任务次序就干净了），属于状态文件
「离屏 SceneCapture 画不出 GPU 实例（真因仍开放）」那一族。

判据由 `Scripts/TinyGladeShotResizeStats.py` 离线算（PIL + numpy）：

  A **相邻帧差异率**：禁带开时，除跳带那一帧外，逐帧差异应当是平滑的一小段；
    禁带关时，屋脊翻面的那一帧会顶出一个尖峰。**这一条同时就是"门是活的"的证明** ——
    翻轴在像素上看得见，所以"禁带开时没看见"才是有意义的。
  B **派生物可见性**：关掉门框砖 / 藤 / 摆件之后与同尺寸的基准帧比，差异率必须**明显不为零**。
    若某一类关掉后画面纹丝不动，说明这组图**根本没拍到它**，
    "它跟得住"就是一句空话（D8 那轮的教训：第一版判据量的是藤蔓重排，不是洞）。

用法（必须真编辑器 + `-ExecCmds`，见坑 ②）::

    UnrealEditor.exe <project> -ExecCmds="py <此文件>; Quit" -abslog=<独立日志>

--- 踩过的坑（沿用 `TinyGladeShotSoftening.py` 那份，逐条适用）---------------------
 ① `unreal.Rotator` 参数序是 (roll, pitch, yaw)，一律关键字参数。
 ② `-ExecutePythonScript` 跑完即退，tick 回调不触发；需要 tick 的脚本用真编辑器 + `-ExecCmds`。
    ⚠️ 另外它解析相对路径是相对**引擎 Binaries 目录**，不是项目目录 —— 一律给绝对路径。
 ③ 用离屏 `SceneCapture2D`，不要 `HighResShot`（本工程是 4 分屏）。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`。
 ⑤ 离屏 capture 被引擎写死关掉 Lumen ⇒ 捕获组件自己的 `post_process_settings` 覆盖回来。
 ⑥ 准备段整个包在 try 里，否则异常 ⇒ 编辑器永不退出。
 ⑧ `always_persist_rendering_state = True`，否则 PPV 里的曝光一条都不生效。
 ⑨ 导出前**一帧一次**、共 `WARMUP_CAPTURES` 次预热，否则 Lumen 间接光恒为零。
    ⚠️ ⑧ 与 ⑨ 缺一不可：只修 ⑧ 会触发 ⑨，方向相反。
 ⑪ **一次运行里连拍多个世界状态**：改完世界要先空转 `SETTLE_TICKS` 帧再开始预热，
    否则"刚挪过 actor"的那几帧会整面墙落在精确 (0,0,0)（本轮实测 zero% 0.04% → 2.3%），
    看着像"拖尺寸把墙弄没了"，实际几何逐位相同。预热喂的是 Lumen 的屏幕历史，
    喂不动阴影/Lumen 场景那一侧 —— 与坑 ⑨ 同症状、不同根因。
 ⑩ 一律调 `Get*UndrawableReason()`，**不要** `is_*_drawable()` —— 后者不可画时返回 `None`，
    `str(None) == "None"` 会伪造出一句像模像样的原因。
"""
import math
import os

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "jobs": [], "job": 0, "shots": 0, "setup": False,
         "spawned": [], "world": None, "house": None, "ground": None, "comp": None, "rt": None,
         "home": None, "settle": 0, "stage": "reset", "pushed": 0}

FIRST_TICK = 60          # 世界侧收敛（天光实时捕获、Lumen 场景）要几十帧
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
# ⚠️ **坑 ⑪（本轮新踩，别的出图脚本还没有这一条）**：一次运行里连拍**多个世界状态**时，
# 改完世界必须先**空转**若干帧再开始预热，不能改完下一帧就拍。
# 现场：同一段拉伸拍 21 个状态，凡是"拍照前刚挪过 actor"的那几帧，整面背光墙落在
# **精确 (0,0,0)**（zero% 从 0.04% 跳到 2.3%），而同尺寸、同轴向、三角数逐位相同的对照帧
# 干干净净 —— 探针实测 `dragged axis/tris == rebuilt axis/tris`，所以**不是几何**。
# 32 次预热喂的是 Lumen 的**屏幕**历史，喂不动阴影与 Lumen 场景那一侧的更新。
# 症状极具误导性：看着就是"拖尺寸把一面墙弄没了"。
SETTLE_TICKS = int(os.environ.get("TG_SHOT_SETTLE", "24"))

# 一段连续拉伸：Y 从 400 拉到 800，每帧 50 cm。X 恒 600 ⇒ 翻轴阈 690、禁带 [480, 720]，
# 阈值整个落在带里 —— 这正是要拍的那一段。
START = unreal.Vector2D(600.0, 400.0)
STEP_CM = 50.0
FRAMES = 9


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
    """把这一帧**实际**长出了什么打进日志：像素判据的配套解析量，两者互为旁证。

    ⚠️ 坑 ⑩：一律调原因版。`is_*_drawable()` 不可画时返回 None，str(None) 会伪造原因。
    """
    h = STATE["house"]
    size = h.get_editor_property("FootprintSize")
    reasons = []
    for label, getter in (("vine", h.get_vine_undrawable_reason),
                          ("decor", h.get_decor_undrawable_reason),
                          ("window", h.get_window_undrawable_reason)):
        why = str(getter())
        if why:
            reasons.append("%s:%s" % (label, why))
    unreal.log("RESIZE %-14s size=(%.1f, %.1f) axis=%s doors=%d openings=%d bricks=%d vines=%d decor=%d%s"
               % (name, size.x, size.y, h.get_editor_property("RidgeAxis"),
                  h.get_open_door_count(), h.get_opening_count(), h.get_frame_brick_count(),
                  h.get_vine_segment_count(), h.get_decor_instance_count(),
                  "" if not reasons else ("  undrawable: " + " ".join(reasons))))


def reset_house(band_on, frame=True, vine=True, decor=True):
    """把房子放回起点，并设好这一帧要开/关哪些派生物。"""
    h, home = STATE["house"], STATE["home"]
    h.set_editor_property("bFrameEnabled", frame)
    h.set_editor_property("bVineEnabled", vine)
    h.set_editor_property("bDecorEnabled", decor)
    h.set_editor_property("FootprintBandFraction", 0.20 if band_on else 0.0)
    h.set_actor_location(unreal.Vector(home.x, home.y, h.get_actor_location().z), False, False)
    h.set_editor_property("FootprintSize", START)
    h.set_editor_property("RidgeAxis", unreal.CSRidgeAxis.X)
    h.call_method("RebuildHouse")


def push_one_frame():
    """一帧的拖动量：对**南北两面墙各推半步**。

    ⚠️ **一个 tick 只推一帧**，不许在一个 tick 里把整段拖完（第二版栽在这里）：
    每次推拉都会重排门框砖的 GPU 实例源，十几次挤在同一帧里之后，离屏 capture 会
    间歇性地把整面山墙画成**精确 (0,0,0)**。而且它**与世界状态无关** ——
    同一段重放当第 0 个任务拍是 2.3%、当第 3 个任务拍是 0.07%，几何探针
    （`axis`/三角数，拖出来的 vs 全量重建的）逐位相同。这属于状态文件里
    「离屏 SceneCapture 画不出 GPU 实例（真因仍开放）」那一族，不是拉尺寸的缺陷。
    一 tick 一帧同时也更像真实拖动，本来就该这么写。
    """
    h = STATE["house"]
    h.push_edge(0, STEP_CM * 0.5)
    h.push_edge(2, STEP_CM * 0.5)


def recentre():
    """把中心平移回原位。

    ⚠️ **归位是取景需要，不是修 bug**（第一版没归位，栽得很惨）：每帧对两面墙各推半步，
    两步位移相等时中心不动 —— 但**跳带那一步不相等**（一面被吸在外沿上位移 0、另一面跳
    240），中心于是净偏 120 cm。不归位的话 9 次重放累积把房子推出了那条画好的路，
    后面几帧的门从 6 个掉到 3 个，看着像"拖尺寸把门弄丢了"，实际是脚本自己把房子挪走了。
    偏移本身是推拉该有的行为（对侧不动），纯平移也不改任何派生物的相对摆位。
    """
    h, home = STATE["house"], STATE["home"]
    h.set_actor_location(unreal.Vector(home.x, home.y, h.get_actor_location().z), False, False)
    h.reevaluate_site()


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground, house = find("Ground_Demo"), find("House_Road")
    if not (ground and house):
        unreal.log_error("RESIZE FAILED: no Ground_Demo / House_Road")
        return None
    STATE["ground"], STATE["house"] = ground, house
    STATE["home"] = house.get_actor_location()

    # 必须在**有门有砖有藤有摆件**的状态下拍：空房子那组图就算派生物整条路坏掉也一样好看。
    loc = house.get_actor_location()
    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(17):
        y = loc.y - 800.0 + i * 100.0
        ground.apply_paint_stroke(unreal.Vector(loc.x, y, ground.sample_height(unreal.Vector2D(loc.x, y))))
    ground.end_paint_stroke()

    # 机位：定死在房子斜前上方，**整段拉伸都不动**。距离按最终尺寸（800）留够余量，
    # 否则拉到后面房子会顶出画框，差异率里混进"取景变了"这一项。
    aim = unreal.Vector(loc.x, loc.y, loc.z + 150.0)
    cam = unreal.Vector(loc.x + 1450.0, loc.y - 1450.0, loc.z + 900.0)

    cap_pp = unreal.PostProcessSettings()          # 坑 ⑤：只翻回 GI/反射两项
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    actor = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam, look_at(cam, aim))
    comp = actor.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)      # 坑 ⑧
    STATE["comp"], STATE["rt"] = comp, rt
    STATE["spawned"].append(actor)
    unreal.log("RESIZE cam at (%.0f, %.0f, %.0f) aim (%.0f, %.0f, %.0f)"
               % (cam.x, cam.y, cam.z, aim.x, aim.y, aim.z))

    # 每条任务 = (名字, 目标帧号, 禁带开否, 关掉哪些派生物)。
    jobs = []
    # ① 禁带开：这一段就是交付要看的那组图。
    for i in range(FRAMES):
        jobs.append(("on_f%d" % i, i, True, {}))
    # ② 禁带关（破坏实验之一）：同一段拉伸，屋脊会在某个**平滑**帧原地翻 90°。
    #    这组图的价值不在"好看"，在于**证明这台相机看得见翻轴** —— 看不见的话
    #    ①"没看见翻轴"就是空话。
    for i in range(FRAMES):
        jobs.append(("off_f%d" % i, i, False, {}))
    # ③ 派生物可见性（破坏实验之二）：末尾尺寸上逐类关掉。差异率≈0 = 这组图没拍到它。
    jobs.append(("on_noframe", FRAMES - 1, True, {"frame": False}))
    jobs.append(("on_novine", FRAMES - 1, True, {"vine": False}))
    jobs.append(("on_nodecor", FRAMES - 1, True, {"decor": False}))
    # ④ **全量重建的孪生帧**（判据 D，最硬的一条）：同一帧再补一次 RebuildHouse 后重拍。
    #    "拖出来的画面与全量重建逐像素相同"是"派生物跟得住"能给出的最强形态 ——
    #    回归里那条 readback 断言只能证明**计数**相同，证不了摆位。
    for i in (2, 5, 8):
        jobs.append(("on_f%d_rb" % i, i, True, {"rebuild": True}))
    # ⑤ **松手帧**（判据 E）：同一帧再补一次 `push_edge(..., bFinished=True)`，也就是用户松开鼠标。
    #    与 ④ 的区别是它**不清滞回表**（`RebuildHouse` 会清），因此是真实交互的收尾状态。
    #    拖动**中**与松手后的画面差多少，就是"拖动期有多少东西还没收敛"的直接读数。
    for i in (2, 5, 8):
        jobs.append(("on_f%d_rel" % i, i, True, {"release": True}))
    STATE["jobs"] = jobs
    return world


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return

    if STATE["job"] >= len(STATE["jobs"]):
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        if STATE["ground"]:
            STATE["ground"].reset_paint()
        for a in STATE["spawned"]:
            ACTORS.destroy_actor(a)
        unreal.log("RESIZE DONE frames=%d" % len(STATE["jobs"]))
        unreal.SystemLibrary.quit_editor()
        return

    name, target, band_on, off = STATE["jobs"][STATE["job"]]

    if STATE["stage"] == "reset":
        reset_house(band_on, frame=off.get("frame", True), vine=off.get("vine", True),
                    decor=off.get("decor", True))
        STATE["pushed"], STATE["stage"] = 0, "push"
        return

    if STATE["stage"] == "push":
        if STATE["pushed"] < target:
            push_one_frame()                      # 一 tick 一帧，见 push_one_frame 的注释
            STATE["pushed"] += 1
            return
        recentre()
        if off.get("rebuild", False):
            STATE["house"].call_method("RebuildHouse")
        if off.get("release", False):
            STATE["house"].push_edge(2, 0.0, True)      # 松手：置 bForceFullRebuild，不清滞回表
        note(name)
        STATE["settle"], STATE["shots"], STATE["stage"] = 0, 0, "settle"
        return

    if STATE["stage"] == "settle":                # 坑 ⑪：改完世界先空转，再开始喂预热
        STATE["settle"] += 1
        if STATE["settle"] >= SETTLE_TICKS:
            STATE["stage"] = "shoot"
        return

    STATE["comp"].capture_scene()
    STATE["shots"] += 1
    if STATE["shots"] >= WARMUP_CAPTURES:
        unreal.RenderingLibrary.export_render_target(STATE["world"], STATE["rt"], OUT_DIR,
                                                     "resize_%s.png" % name)
        unreal.log("RESIZE shot: resize_%s.png（预热 %d 帧）" % (name, STATE["shots"]))
        STATE["job"] += 1
        STATE["stage"] = "reset"


try:                                   # 坑 ⑥：准备段整个包在 try 里
    STATE["world"] = build()
except Exception as exc:
    unreal.log_error("RESIZE FAILED in build(): %s" % exc)
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
