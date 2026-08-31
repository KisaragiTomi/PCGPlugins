# -*- coding: utf-8 -*-
"""
拍链 A 裙边噪声的对照图 —— **同一次会话里**先关后开，机位逐字相同。

要回答的两件事：
  A  单座土台的裙边有没有被啃出起伏（原型 noisebysourcestress 的观感项）；
  B  两座相接土台的**折痕**还在不在（计划裁决六派给噪声的活）。

为什么不用 `TG_SHOT_TAG` 分两次跑（`TinyGladeShotLighting.py` 那套）：那套要求两次跑
"必须用同一份机位代码"，靠人保证。这里两组图在同一次会话里出，机位是同一批相机 actor，
连浮点都一样 —— 对照唯一的变量就只剩 `SkirtNoiseAmount`。

用法（必须是**真编辑器** + `-ExecCmds="py ..."`，见坑 ②）：

  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotSkirtNoise.py" -abslog=<独立日志>

产物：Saved/TinyGladeShots/skirt_{noiseoff,noiseon}_*.png

--- 踩过的坑（全部来自状态文件「踩过的坑」表）-------------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
    写错的症状：相机朝天，出图一张纯渐变，像"渲染没出来"。
 ② 必须用真编辑器配 `-ExecCmds="py <脚本>"`。`-ExecutePythonScript` 跑完即退，
    tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 默认把 Lumen 整个关掉**，而且是引擎写死的
    （`SceneCaptureRendering.cpp` 的 `SetupViewFamilyForSceneCapture` 在 blend 完世界 PPV
    **之后**直接写 `DynamicGlobalIlluminationMethod = None`）。翻回来的唯一办法是给捕获
    组件自己的 `post_process_settings` 再覆盖一次 —— 它是在那两行之后才 apply 的。
 ⑥ **石阶（GPU 实例）在离屏捕获里画不对**，是状态文件里那条已知未修的缺陷，且正好复现在
    `L_TerrainOpsDemo`。本脚本因此把石阶（地面的 `StairMesh`）关掉，只拍地面本体 ——
    本轮改动改的就是高度场，石阶不在判读范围内。
 ⑦ **`SCS_NORMAL` 在 RGBA8 目标上出的是一张纯白图**（第一版试过，作废）。折痕是法线的
    不连续，本来最该用法线图读，但那条路要 16f 目标，导出的 png 又变成看图工具打不开的
    HDR 内容（坑 ④ 的另一面）。
 ⑧ **别为了拉对比去压太阳的俯角**（第二版试过，作废）：接合线沿 Y，把光压成沿 X 的掠射
    确实让折痕两侧一明一暗，但整整一座土台连同半张画面一起黑掉，什么都读不出来。
    用关卡自己那盏（俯角 42°、偏航 140°）反而两座都有明暗过渡。折痕在这里读的是
    **谷底那条线直不直**，不是明暗界。
 ⑨ 自动曝光要**钉死**（min = max）：两组图之间唯一该变的是几何，让自动曝光各自去适应
    等于把亮度也变成变量，那张对照就不能比了。状态文件那条"曝光偏置逐张给"说的是
    不同机位之间，不是同机位的两次。
 ⑩ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState**，于是眼适应整条链不跑，
    **PPV 里的曝光一条都不生效** —— 上面坑 ⑨ 那三行钉曝光在补这一行之前是**空操作**
    （实测把 EV100 改 2.83 倍，出图每个分位数逐位不变）。与坑 ⑤ 症状字面相同、根因不同。
 ⑪ ⚠️ **修了 ⑩ 才会触发 ⑪，两条必须一起加。** 有了 ViewState，坑 ⑤ 翻回来的 Lumen 才
    第一次真的跑起来；而 Lumen 的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒
    间接光恒为零 ⇒ 没被太阳直射的 lit 表面全部精确 (0,0,0)。对本脚本尤其致命：
    折痕读的就是**谷底那条线**，而谷底正是最不容易被直射的地方 —— 不预热的话折痕整条黑掉，
    看着像"噪声把地面啃穿了"。修法：每个机位导出前预热 `WARMUP_CAPTURES` 次，**一帧一次**。
"""
import math
import os

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

# 两座中心的间距。**不是石阶用例的 800**：权重 (1−S) 把噪声按剖面高度关小，
# 两座重叠到接合处已经爬到 S≈0.84（相距 800）时噪声在折痕上只剩 16% 权重，实测折痕纹丝不动。
# 1200 时接合处 S≈0.16，噪声接近满权重 —— 这才是这条改动该被判读的场景。
SEPARATION = 1200.0
NOISE_AMOUNT = 0.5

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()
spawned = []


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
if not (ground and shaper):
    unreal.log_error("SHOT FAILED: demo actors missing")
    raise SystemExit

# ---- 摆位：两座对称跨在地面中心上，接合线正好落在 mid_x ----
g_loc = ground.get_actor_location()
span_x = ground.get_editor_property("NumCellsX") * ground.get_editor_property("CellSize")
span_y = ground.get_editor_property("NumCellsY") * ground.get_editor_property("CellSize")
mid_x = g_loc.x + span_x * 0.5
mid_y = g_loc.y + span_y * 0.5
unreal.log("ground origin=(%.0f, %.0f) span=(%.0f, %.0f) mid=(%.0f, %.0f)"
           % (g_loc.x, g_loc.y, span_x, span_y, mid_x, mid_y))

shaper.set_actor_location(unreal.Vector(mid_x - SEPARATION * 0.5, mid_y, 0.0), False, False)
second = ACTORS.spawn_actor_from_class(
    unreal.load_class(None, "%s/BP_GroundShaper.BP_GroundShaper_C" % PKG),
    unreal.Vector(mid_x + SEPARATION * 0.5, mid_y, 0.0))
if not second:
    unreal.log_error("SHOT FAILED: could not spawn the second mound")
    raise SystemExit
spawned.append(second)

MOUNDS = (shaper, second)
# CDO 的默认值不传播到同会话 spawn 的实例 —— 实例上必须再写一份（状态文件实测陷阱）。
for m in MOUNDS:
    for name, value in (("Ground", ground), ("Radius", 300.0), ("FalloffDistance", 400.0),
                        ("LiftHeight", 300.0), ("SkirtNoiseWavelength", 300.0),
                        ("SkirtNoiseSeed", 0), ("SecondaryLiftScale", 0.021)):
        m.set_editor_property(name, value)


def set_noise(amount):
    for m in MOUNDS:
        m.set_editor_property("SkirtNoiseAmount", amount)
        m.call_method("RebuildTerrain")


# ---- 关掉石阶：出图只判读地面本体（坑 ⑥）----
# 这里原来还要关第二条：塑形物自持的旧路石阶（`StepMeshes` + `RebuildSteps`），以及一段
# 量它"噪声开/关各摆出多少级"的探针。旧路已随 2026-08-30「裁决一」第二步整条删除，
# 场上只剩地面自己这一条，两段随之去掉。
ground.set_editor_property("StairMesh", None)
ground.call_method("RebuildStairs")

# ---- 灯：用关卡自己那盏，一个字都不改（见坑 ⑧）。没有才补，且两组图同样处理 ----
suns = [a for a in ACTORS.get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]
if not suns:
    unreal.log_warning("no DirectionalLight in the level; spawning a fallback sun")
    sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000),
                                        unreal.Rotator(roll=0.0, pitch=-42.0, yaw=140.0))
    sun.light_component.set_editor_property("intensity", 6.0)
    spawned.append(sun)
    suns = [sun]
unreal.log("sun '%s' pitch=%.1f yaw=%.1f"
           % (suns[0].get_actor_label(), suns[0].get_actor_rotation().pitch, suns[0].get_actor_rotation().yaw))

# ---- 机位：接合线沿 Y 走，所以横跨它（沿 X）看最能读出折痕 ----
cam_pair = unreal.Vector(mid_x, mid_y - 2600.0, 1500.0)
cam_junction = unreal.Vector(mid_x + 120.0, mid_y - 1750.0, 900.0)
cam_top = unreal.Vector(mid_x, mid_y, 3400.0)
COLOR = unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR

SHOTS = [
    # 两座相接的全景：裙边整体被啃成什么样。
    ("pair3q", cam_pair, look_at(cam_pair, unreal.Vector(mid_x, mid_y, 120.0)), COLOR),
    # 抬高俯冲、正对接合线：折痕从近处一路延伸到远处，直/碎在这张里最直观。
    ("junction", cam_junction, look_at(cam_junction, unreal.Vector(mid_x, mid_y + 300.0, 60.0)), COLOR),
    # 俯视：折痕到底还是不是一条直线，这张一眼可判（掠射光把它画成一条明暗界）。
    ("overhead", cam_top, unreal.Rotator(roll=0.0, pitch=-90.0, yaw=90.0), COLOR),
]

# 见坑 ⑤：把捕获自己那份 PP 覆盖建起来，Lumen 才会回到出图里。
CAP_PP = unreal.PostProcessSettings()
CAP_PP.set_editor_property("override_dynamic_global_illumination_method", True)
CAP_PP.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
CAP_PP.set_editor_property("override_reflection_method", True)
CAP_PP.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
CAP_PP.set_editor_property("override_lumen_surface_cache_resolution", True)
CAP_PP.set_editor_property("lumen_surface_cache_resolution", 1.0)
# 曝光钉死（见坑 ⑨）：min = max 让两组图落在同一档，几何才是唯一的变量。
CAP_PP.set_editor_property("override_auto_exposure_bias", True)
CAP_PP.set_editor_property("auto_exposure_bias", 2.5)
CAP_PP.set_editor_property("override_auto_exposure_min_brightness", True)
CAP_PP.set_editor_property("auto_exposure_min_brightness", 0.05)
CAP_PP.set_editor_property("override_auto_exposure_max_brightness", True)
CAP_PP.set_editor_property("auto_exposure_max_brightness", 0.05)

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
    comp.set_editor_property("post_process_settings", CAP_PP)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    # 坑 ⑩：没有 ViewState ⇒ 上面那三行钉曝光是空操作。⚠️ 开了它就会继承坑 ⑪，
    # 必须同时有下面的 `WARMUP_CAPTURES` 预热，只加这一行会把图弄得更糟。
    comp.set_editor_property("always_persist_rendering_state", True)
    RIGS.append((name, cap, comp, rt))
    spawned.append(cap)

# 一次会话里跑两遍同一批机位，唯一变量是 SkirtNoiseAmount。
SEQUENCE = [(mode, index) for mode, _ in (("noiseoff", 0.0), ("noiseon", NOISE_AMOUNT))
            for index in range(len(RIGS))]
AMOUNTS = {"noiseoff": 0.0, "noiseon": NOISE_AMOUNT}

STATE = {"ticks": 0, "handle": None, "mode": None, "job": 0, "phase": 0, "samples": {}}
FIRST_TICK = 40      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 5           # 换噪声档要重建两座地形、换机位要让驾驶视口跟上，留几帧
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

# 判据的采样格与容差：与 `TinyGladeShotRockShell.py` 同一套（48 × 27 = 1296 点、单通道 > 12/255）。
GRID_X, GRID_Y = 48, 27
PIXEL_DELTA = 12
GRID_POINTS = GRID_X * GRID_Y


def sample_pixels(rt):
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
    """精确 (0,0,0) 占比 —— 坑 ⑪ 有没有被预热掉的**指纹**，同时是差异率的校正项：
    两张图同一处都精确为零时 `diff_ratio` 会读成"没变"，所有差异率因此被系统性低估。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


# 精确零的容许上限，与 `TinyGladeShotSofteningStats.py` 的 `ZERO_FAIL` 同一个数。
ZERO_FAIL = 0.005
# 噪声开/关同机位差异率的下限，**逐机位**。
#
# 标定依据（管线修好后实测，`skirt_noise*` 那一轮）：
#   pair3q 25.5% (331/1296) · junction 15.8% (205/1296) · overhead 6.2% (80/1296)
# 三个机位差一个量级是**几何决定的**，不是噪声：裙边在 pair3q 里占大半张画面，
# 在俯视图里只是两个圆环的边缘。拿一个数套三个机位，要么对 overhead 恒真、
# 要么对 pair3q 苛刻到会被合法的观感调整撞红。
#
# 一律取实测值的**一半**：物理含义是"噪声改动的屏幕覆盖面至少还剩一半"。
#
# 复现性（两次独立的编辑器启动，同一份代码）：`TinyGladeShotRockShell.py` 的两条判据
# **逐位相同**（750/1296 与 71/1296 各两次）；其余脚本的差异像素数在 ±1 个采样点之内摆动，
# 只有裙边噪声那条 `pair3q` 摆到 331 → 372（相对 12%，因为噪声改的是**几何**，Lumen 的
# 收敛路径跟着变）。取实测值的一半留出的余量比这个摆幅大一个量级。
#
# ⚠️ **`pair3q` 被降级成"只出图、不设门"，这是标定出来的，不是偷懒。**
# 故意破坏实验（把 `noiseon` 的幅度也设成 0，两组图在几何上完全相同）实测：
# junction **0.2%**、overhead **0.0%**，而 pair3q 仍有 **15.5%**。
# 那 15.5% 与噪声无关，是**捕获本身的不对称**：三个机位各有自己的捕获组件，
# `noiseoff` 那一轮是该组件第一次攒 Lumen 历史，`noiseon` 那一轮它已经带着上一轮的历史，
# 于是同一台相机的第二张天然更收敛一点。这个基线（15.5%）离 pair3q 的信号（25.5~28.7%）
# 太近，中间还压着 12% 的运行间摆幅 —— 拿它当门只会给出一条**分辨不出信号与基线**的判据，
# 而那正是"为了绿而放宽到没有意义"的另一种形态。
# 折痕本来也不该在这张全景里读（见坑 ⑧：读的是谷底那条线直不直），两个真正的判读位是
# junction 与 overhead，它们的信噪比分别是 15.9% : 0.2% 与 6.0% : 0.0%。
DIFF_FAIL = {"junction": 0.079, "overhead": 0.030}


def report():
    ok = True
    s = STATE["samples"]
    for key in sorted(s):
        z = zero_ratio(s[key])
        unreal.log("SKIRT PIXELS %-20s zero=%.3f%%" % (key, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("SKIRT !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑪）"
                             % (key, z * 100.0))
            ok = False
    for name, _, _, _ in RIGS:
        a, b = s.get("noiseoff_%s" % name), s.get("noiseon_%s" % name)
        if not (a and b):
            continue
        ratio, changed = diff_ratio(a, b)
        limit = DIFF_FAIL.get(name)
        unreal.log("SKIRT PIXELS %-20s noise off-vs-on: %d/%d changed (%.1f%%)%s"
                   % (name, changed, GRID_POINTS, ratio * 100.0,
                      "" if limit is not None else "  [只出图，不设门 —— 见 DIFF_FAIL 上面那段]"))
        if limit is None:
            continue
        # 这条判据回答的是"噪声真的画在了屏幕上"，不是"buffer 里有数"。
        if ratio < limit:
            unreal.log_error("SKIRT !! %s 开关噪声几乎没有改变画面 —— 裙边噪声没有被画出来" % name)
            ok = False
    unreal.log("SKIRT PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    job = STATE["job"]
    if job < len(SEQUENCE):
        mode, rig_index = SEQUENCE[job]
        name, cap, comp, rt = RIGS[rig_index]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            if STATE["mode"] != mode:
                STATE["mode"] = mode
                set_noise(AMOUNTS[mode])
                unreal.log("mode=%s SkirtNoiseAmount=%.2f  junction h=%.1f cm"
                           % (mode, AMOUNTS[mode], ground.sample_height(unreal.Vector2D(mid_x, mid_y))))
            unreal.EditorLevelLibrary.pilot_level_actor(cap)
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            # 坑 ⑪：一帧只打一次，Lumen 的帧间历史才会真的往前走。换机位/换噪声档等于清历史，
            # 所以**每个 (机位 × 档) 都要重新预热满**，不能几路同时连拍互相抢帧。
            comp.capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR,
                                                             "skirt_%s_%s.png" % (mode, name))
                STATE["samples"]["%s_%s" % (mode, name)] = sample_pixels(rt)
                unreal.log("shot: skirt_%s_%s.png（预热 %d 帧）" % (mode, name, WARMUP_CAPTURES))
                STATE["job"], STATE["phase"] = job + 1, 0
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    ground.reset_paint()
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.log("SHOT DONE (skirt noise)")
    unreal.SystemLibrary.quit_editor()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
