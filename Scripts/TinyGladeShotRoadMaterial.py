# -*- coding: utf-8 -*-
"""
验地面**道路材质**：M_TinyGladeGround 按顶点色 R 混 grass ↔ dirt。

要回答的不是"路画上了没有"（俯视图早就看得见），而是这四件只有像素能答的：
  A  **贴地视角**下路读不读得出来 —— 俯视好看不代表玩家视角好看（掠射角下平铺周期会暴露）。
  B  **混合边缘的质量**：顶点色是逐顶点的，地面格 50 cm，混合是格内线性插值 ⇒
     天然有"每 50 cm 一段折线"的风险，贴脸看是块状阶梯还是连续过渡。
  C  **一笔的宽度与羽化**是否合理（笔刷参数的观感验收，不是功能验收）。
  D  路**爬上坡面**时对不对 —— 顶点色画在镜像格上、坡面高度是解析场，
     两者在裙边噪声（2026-08-30 落地）之后还合不合得上。

用法（tag 决定文件名前缀）：
  set TG_SHOT_TAG=road
  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotRoadMaterial.py"

产物：Saved/TinyGladeShots/road_<tag>_*.png

--- 踩过的坑（全部来自状态文件的坑表，逐条都真踩过）-------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
    写错的症状：相机朝天，出图一张纯渐变，像"渲染没出来"。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：`SceneCaptureRendering.cpp` 的
    `SetupViewFamilyForSceneCapture` 在 blend 完世界 PPV **之后**直接写
    `DynamicGlobalIlluminationMethod = None`，于是 PPV 里所有 Lumen 参数在出图里一条都不生效。
    唯一的翻回办法是给捕获组件自己的 `post_process_settings` 再覆盖一次（它在那两行之后才 apply）。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**（那条 Quit 再也执行不到）。所以准备段整个包在 try 里，
    异常也要走到 quit。
 ⑦ `L_TerrainOpsDemo` 的 GPU 实例石阶在离屏 capture 下画不对（状态文件「离屏 SceneCapture
    画不出 GPU 实例」，真因**仍未定位**）。本脚本关掉石阶再拍 —— 这条缺陷不是路面材质的问题，
    让它入画只会污染判读。
 ⑧ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState**，于是眼适应整条链不跑，
    **PPV 里的曝光一条都不生效**（实测把 EV100 改 2.83 倍，出图每个分位数逐位不变）。
    与坑 ⑤ 症状字面相同、根因完全不同，别拿坑 ⑤ 的办法去修它。
 ⑨ ⚠️ **修了 ⑧ 才会触发 ⑨，两条必须一起加。** 有了 ViewState，坑 ⑤ 翻回来的 Lumen 才
    第一次真的跑起来；而 Lumen 的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒
    间接光恒为零 ⇒ 没被太阳直射的 lit 表面全部精确 (0,0,0)。对本脚本尤其致命：判读位 B
    要看的是**路缘的混合过渡**，而掠射角下路缘正好落在半阴影里 —— 不预热的话那条边整条
    黑掉，看着像"顶点色混出了一条黑边"。修法：每个机位导出前预热 `WARMUP_CAPTURES` 次，
    **一帧一次**（同一帧连打不推进帧间历史）。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "road")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "jobs": [], "spawned": [], "ground": None,
         "job": 0, "phase": 0, "samples": {}, "world": None, "hub": None}

# 判据的采样格与容差：与 `TinyGladeShotRockShell.py` 同一套（48 × 27 = 1296 点、单通道 > 12/255）。
GRID_X, GRID_Y = 48, 27
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
# 精确 (0,0,0) 的容许上限，与 `TinyGladeShotSofteningStats.py` 的 `ZERO_FAIL` 同一个数。
ZERO_FAIL = 0.005


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


def try_set(obj, prop, value):
    """演示关卡的属性名随版本漂移过；缺了就跳过，别让准备段抛异常（坑 ⑥）。"""
    try:
        obj.set_editor_property(prop, value)
        return True
    except Exception as exc:
        unreal.log_warning("ROADMAT skip %s: %s" % (prop, exc))
        return False


def paint_road():
    """从平地起笔、径直穿过第一座土台的中心 —— 一笔同时覆盖 A/B/C/D 四个判读位。"""
    ground, hub = STATE["ground"], STATE["hub"]
    ground.begin_paint_stroke()
    for i in range(41):
        x = hub.x - 1600.0 + i * 80.0
        ground.apply_paint_stroke(
            unreal.Vector(x, hub.y, ground.sample_height(unreal.Vector2D(x, hub.y))))
    ground.end_paint_stroke()


def clear_road():
    STATE["ground"].reset_paint()


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


def diff_ratio(a, b):
    changed = sum(1 for pa, pb in zip(a, b)
                  if max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > PIXEL_DELTA)
    return changed / float(len(a)), changed


def zero_ratio(samples):
    """精确 (0,0,0) 占比 —— 坑 ⑨ 有没有被预热掉的**指纹**，同时是差异率的校正项：
    两张图同一处都精确为零时 `diff_ratio` 会读成"没变"，所有差异率因此被系统性低估。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find_ground()
    if not ground:
        unreal.log_error("ROADMAT FAILED: no ground actor")
        return None
    STATE["ground"] = ground

    mat = ground.get_editor_property("GroundMaterial")
    unreal.log("ROADMAT material=%s" % (mat.get_name() if mat else "<NONE — 路会看不见>"))

    # 坑 ⑦：石阶那条路在离屏 capture 下画不对，且与本次判读无关，关掉再拍。
    try_set(ground, "bEnableGpuStairs", False)
    for a in ACTORS.get_all_level_actors():
        if "Shaper" in a.get_class().get_name():
            try_set(a, "bEnableSteps", False)

    shapers = [a for a in ACTORS.get_all_level_actors() if "Shaper" in a.get_class().get_name()]
    unreal.log("ROADMAT shapers=%d" % len(shapers))

    # 路线：从平地起笔、径直穿过第一座土台的中心 —— 一笔同时覆盖 A/B/C/D 四个判读位。
    if shapers:
        hub = shapers[0].get_actor_location()
    else:
        hub = unreal.Vector(0.0, 0.0, 0.0)
    STATE["hub"] = hub

    ground.reset_paint()
    paint_road()

    top_z = ground.sample_height(unreal.Vector2D(hub.x, hub.y))
    unreal.log("ROADMAT hub=(%.0f, %.0f) top_z=%.1f" % (hub.x, hub.y, top_z))

    # 机位一律从路线反算，不手调 —— 手调的机位在土台一动之后就全废了。
    def on_road(t, up, side=0.0):
        x = hub.x - 1600.0 + t * 3200.0
        return unreal.Vector(x, hub.y + side, ground.sample_height(unreal.Vector2D(x, hub.y)) + up)

    cam_eye = on_road(0.06, 170.0)            # A：站在路头，顺着路看过去（掠射角）
    aim_eye = on_road(0.55, 120.0)
    cam_edge = on_road(0.20, 90.0, 260.0)     # B：贴脸看路缘，判混合边是不是块状阶梯
    aim_edge = on_road(0.24, 0.0)
    cam_slope = on_road(0.50, 700.0, -1500.0)  # D：侧看路爬上土台
    aim_slope = on_road(0.50, 60.0)
    cam_top = unreal.Vector(hub.x, hub.y, top_z + 2600.0)   # C：俯视看整笔的宽度与羽化

    shots = [
        ("eye", cam_eye, look_at(cam_eye, aim_eye)),
        ("edge", cam_edge, look_at(cam_edge, aim_edge)),
        ("slope", cam_slope, look_at(cam_slope, aim_slope)),
        ("overhead", cam_top, unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0)),
    ]
    # 判据要的对照位：`eye` 是掠射角（判读位 A）、`overhead` 是整笔（判读位 C）。
    # 这两张各拍一张**没路**的同机位对照 —— 差异率就是"路真的画在了屏幕上"的硬证据。
    # 只挑两张是因为 `edge`/`slope` 的机位本身就是从路线反算的，没路时它们指向一片没有语义的草地。
    PAIRED = ("eye", "overhead")

    # 坑 ⑤：把捕获自己那份 PP 覆盖建起来，Lumen 才会回到出图里。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rigs = {}
    for name, cam_loc, cam_rot in shots:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
            unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_loc, cam_rot)
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", 55.0)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        comp.set_editor_property("post_process_settings", cap_pp)
        comp.set_editor_property("post_process_blend_weight", 1.0)
        # 坑 ⑧：没有 ViewState ⇒ 眼适应不跑 ⇒ 曝光一条都不生效。
        # ⚠️ 开了它就会继承坑 ⑨，必须同时有下面的 `WARMUP_CAPTURES` 预热。
        comp.set_editor_property("always_persist_rendering_state", True)
        rigs[name] = (comp, rt)
        STATE["spawned"].append(cap)

    # 先拍没路的两张，再画路拍四张。`clear_road` / `paint_road` 挂在 job 上而不是在这里跑完，
    # 是因为每次改地面之后都要重新预热满 32 帧（坑 ⑨），顺序必须由 tick 驱动。
    STATE["jobs"] = [("clean_%s" % n, clear_road if i == 0 else None, rigs[n])
                     for i, n in enumerate(PAIRED)]
    STATE["jobs"] += [(n, paint_road if i == 0 else None, rigs[n])
                      for i, (n, _, _) in enumerate(shots)]
    return world


FIRST_TICK = 40      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 5           # 画路/擦路要重推顶点色 + 重建地形，留几帧
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
# 路的最小差异率，**逐机位**。
#
# 标定依据（管线修好后实测）：eye 68.9% (893/1296) · overhead 11.8% (153/1296)。
# 两者差近六倍是视角决定的：掠射角下这条路铺满了大半张画面，俯视图里它只是 64 m 见方
# 地面上的一条 3200 cm 长的窄带。一律取实测值的**一半** ——
# 物理含义是"路的屏幕覆盖面至少还剩一半"。
#
# 复现性（两次独立的编辑器启动，同一份代码）：`TinyGladeShotRockShell.py` 的两条判据
# **逐位相同**（750/1296 与 71/1296 各两次）；其余脚本的差异像素数在 ±3 个采样点之内摆动，
# 只有裙边噪声那条 `pair3q` 摆到 331 → 372（相对 12%，因为噪声改的是**几何**，Lumen 的
# 收敛路径跟着变）。取实测值的一半留出的余量比这个摆幅大一个量级。
DIFF_FAIL = {"eye": 0.344, "overhead": 0.059}


def report():
    ok = True
    s = STATE["samples"]
    for key in sorted(s):
        z = zero_ratio(s[key])
        unreal.log("ROADMAT PIXELS %-16s zero=%.3f%%" % (key, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("ROADMAT !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑨）"
                             % (key, z * 100.0))
            ok = False
    for name, limit in sorted(DIFF_FAIL.items()):
        a, b = s.get("clean_%s" % name), s.get(name)
        if not (a and b):
            continue
        ratio, changed = diff_ratio(a, b)
        unreal.log("ROADMAT PIXELS %-16s road on-vs-off: %d/%d changed (%.1f%%)"
                   % (name, changed, GRID_POINTS, ratio * 100.0))
        # 这条判据回答的是"顶点色真的被材质混成了土路、并且出现在画面上"，
        # 不是"顶点色 buffer 里有数" —— 后者 readback 早就绿了，而材质接错时它照样绿。
        if ratio < limit:
            unreal.log_error("ROADMAT !! %s 画路前后几乎没变 —— 路没有被画出来（材质没接上顶点色 R？）"
                             % name)
            ok = False
    unreal.log("ROADMAT PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    job = STATE["job"]
    if job < len(STATE["jobs"]):
        name, action, (comp, rt) = STATE["jobs"][job]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            if action:
                action()
        elif phase >= SETTLE:
            # 坑 ⑨：一帧只打一次，Lumen 的帧间历史才会真的往前走；换机位等于清历史，
            # 所以**每张图都要重新预热满**。
            comp.capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR,
                                                             "road_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels(rt)
                unreal.log("ROADMAT shot: road_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["job"], STATE["phase"] = job + 1, 0
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("ROADMAT DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception as exc:
    unreal.log_error("ROADMAT FAILED in build(): %s" % exc)
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
