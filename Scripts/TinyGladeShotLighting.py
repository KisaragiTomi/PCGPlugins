# -*- coding: utf-8 -*-
"""
拍 D14（阴影 + 后处理）的对照图 —— 同机位、改前 / 改后各一组。

要回答的三件事，都只有像素能答：
  A  门框砖那条**实例化**路（UCSGpuInstancedMeshComponent）开 `CastShadow = true` 有没有影子；
  A' 房体/地面/柱子那条**非实例化**路（UCSMeshRenderComponent）有没有影子；
  B  补了 PostProcessVolume + ExponentialHeightFog 之后，阴影区是不是"压不黑、有颜色"。

2026-08-30 用这套图跑出来的答案：**A 与 A' 同一个结论** —— VSM 下两条都不投影，
CSM 下两条都投影。用户据此拍板关掉了 VSM（`Config/DefaultEngine.ini:52` = 0）。
⚠️ **那条裁决落地之后，原来那张 `house3q_csmonly`（同机位、只关 VSM）就退化成了
house3q 的副本**（实测差异 0.6% = 纯收敛噪声）。它已改成 `house3q_noshadow`
（同机位、只关 `r.ShadowQuality`），继续当"影子真的画出来了"的判据用。

用法（tag 决定文件名前缀，两次跑必须用同一份机位代码，否则对照无效）：

  set TG_SHOT_TAG=before
  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotLighting.py"

产物：Saved/TinyGladeShots/lit_<tag>_*.png

--- 踩过的坑（前四条来自状态文件，第五条是本轮新踩到的）---------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
    写错的症状：相机朝天，出图一张纯渐变，像"渲染没出来"。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 默认把 Lumen 整个关掉**，而且是引擎写死的：
    `SceneCaptureRendering.cpp` 的 `SetupViewFamilyForSceneCapture` 在 blend 完世界里的
    PostProcessVolume 之后，直接写
        View->FinalPostProcessSettings.DynamicGlobalIlluminationMethod = None;
        View->FinalPostProcessSettings.ReflectionMethod              = None;
    —— 于是 PPV 里所有 Lumen 相关的参数（LumenSkylightLeaking / DiffuseColorBoost）在出图里
    **一条都不生效**，症状是"改了 PPV 但两张图一模一样"。翻回来的唯一办法是给捕获组件
    自己的 `post_process_settings` 再覆盖一次：它是在那两行**之后**才 apply 的
    （同函数里的 `View->OverridePostProcessSettings(*PostProcessSettings, ...)`）。
 ⑥ GPU 实例（门框砖）的可见集由 `CSGpuInstancedMesh.usf` 的 InstanceCullCS 逐帧 compact，
    离屏捕获拿到的常常不是自己视角该看到的那一批（状态文件「离屏 SceneCapture 画不出 GPU
    实例」，真因未定位）。这里沿用 `TinyGladeShotStairs.py` 的缓解手段：每张图先让主视口
    **驾驶**捕获相机、等几帧再触发。房体/地面走的是另一条路（非实例化），不受此影响。
 ⑦ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState**，于是眼适应整条链不跑，
    **PPV 里的曝光一条都不生效**（实测把 EV100 改 2.83 倍，出图每个分位数逐位不变）。
    这对本脚本是致命的 —— 判读位 B 量的就是"阴影区压不压得黑"，而在补这一行之前
    `TinyGladeSetupLighting.py` 调的那一整套 PPV 曝光**在出图里一条都没生效**。
    与坑 ⑤ 症状字面相同、根因完全不同，别拿坑 ⑤ 的办法去修它。
 ⑧ ⚠️ **修了 ⑦ 才会触发 ⑧，两条必须一起加。** 有了 ViewState，坑 ⑤ 翻回来的 Lumen 才
    第一次真的跑起来；而 Lumen 的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒
    间接光恒为零 ⇒ 阴影区**精确 (0,0,0)**。判读位 B 会因此得出完全相反的结论
    （"阴影黑成一片"其实是没预热，不是 AO/天光的错）。修法：每个机位导出前预热
    `WARMUP_CAPTURES` 次，**一帧一次**（同一帧连打不推进帧间历史）。
"""
import os
import unreal

TAG = os.environ.get("TG_SHOT_TAG", "after")

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()
spawned = []


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


ground = find("Ground_Demo")
house = find("House_Road")
if not (ground and house):
    unreal.log_error("SHOT FAILED: demo actors missing")
    raise SystemExit

# ---- 画一条路，否则没有门拱、也就没有门框砖（实例化那条路）可看 ----
loc = house.get_actor_location()
ground.reset_paint()
ground.begin_paint_stroke()
for i in range(17):
    y = loc.y - 800.0 + i * 100.0
    ground.apply_paint_stroke(unreal.Vector(loc.x, y, ground.sample_height(unreal.Vector2D(loc.x, y))))
ground.end_paint_stroke()
house.call_method("RebuildHouse")
unreal.log("doors=%d frame_bricks=%d" % (house.get_open_door_count(), house.get_frame_brick_count()))

# ---- 灯：用关卡自己的那盏。只有一盏都没有时才补，且**改前改后同样处理**，对照才成立 ----
suns = [a for a in ACTORS.get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]
if not suns:
    unreal.log_warning("no DirectionalLight in the level; spawning a fallback sun")
    sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000),
                                        unreal.Rotator(roll=0.0, pitch=-38.0, yaw=150.0))
    sun.light_component.set_editor_property("intensity", 6.0)
    spawned.append(sun)
    suns = [sun]
sun_rot = suns[0].get_actor_rotation()
unreal.log("sun '%s' pitch=%.1f yaw=%.1f  lights=%d" %
           (suns[0].get_actor_label(), sun_rot.pitch, sun_rot.yaw, len(suns)))

# 阴影往光的**前进方向**落地。机位放在光的来向偏一侧，房子与它投在地上的影子同时进画 ——
# 这是"到底有没有影子"唯一不用猜的判读方式。机位从太阳朝向反算，不手调：手调的机位
# 在灯一动之后就全废了，而对照图最怕的就是两次机位不一样。
import math
sun_yaw = math.radians(sun_rot.yaw)
fwd = (math.cos(sun_yaw), math.sin(sun_yaw))       # 光前进方向（影子伸向这边）
side = (-fwd[1], fwd[0])


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def at(along, across, up):
    return unreal.Vector(loc.x + fwd[0] * along + side[0] * across,
                         loc.y + fwd[1] * along + side[1] * across,
                         loc.z + up)


# 瞄准点略偏向影子那侧，让房子占左半、影子占右半。
aim_body = at(300.0, 0.0, 150.0)
cam_3q = at(-1500.0, -1100.0, 900.0)
cam_low = at(-900.0, -700.0, 220.0)
cam_top = at(200.0, 0.0, 2600.0)
# 门拱贴脸：路是沿 Y 走的，南墙的拱正对 -Y。门框砖（实例化路）就在这张里。
cam_arch = unreal.Vector(loc.x - 60.0, loc.y - 620.0, 170.0)

SHOTS = [
    # 3/4 全景：房子 + 它投在地上的影子。A/A' 两条路的结论主要看这张。
    ("house3q", cam_3q, look_at(cam_3q, aim_body)),
    # 压低：地面上的影子占画面大半，阴影区"压不黑 / 有没有颜色"看这张（B 的判读位）。
    ("shadowfloor", cam_low, look_at(cam_low, at(900.0, 0.0, 0.0))),
    # 俯视：影子的轮廓与墙的轮廓能不能对上（歪了就是 bias/机位的问题，不是"没影子"）。
    ("overhead", cam_top, unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0)),
    # 门拱贴脸：门框砖是**实例化**组件，A 的直接判读位。
    ("archframe", cam_arch, look_at(cam_arch, unreal.Vector(loc.x, loc.y - 250.0, 150.0))),
    # ⚠️ 与上面 shadowfloor **同机位**，唯一区别是临时把阴影整个关掉。
    #
    # 这张图原本是 `house3q_csmonly`（临时 `r.Shadow.Virtual.Enable 0`），存在的理由是把
    # "关掉 VSM 能拿到什么"摆给驱动方拍板。**那次拍板已经落地**：
    # `Config/DefaultEngine.ini:52` 现在就是 `r.Shadow.Virtual.Enable=0`
    # ⇒ 那张图与 house3q 是同一个渲染配置，两者的差异实测只有 **0.6%**（8/1296，纯收敛噪声），
    # 作为"影子值多少钱"的证据已经归零，作为判据更是恒真。
    # 改成关 `r.ShadowQuality` 之后它变成一条**活的**判据：两张图同机位、同灯、同 PPV，
    # 唯一的变量是"画不画影子"，差异率因此就是影子在画面上占了多少。
    #
    # ⚠️ 机位从 house3q 换到了 **shadowfloor**：3/4 全景里影子只占 4.7% 的采样点
    # （实测 61/1296），判据太靠近噪声；shadowfloor 这个机位存在的唯一理由就是让地上的
    # 影子占画面大半，拿它当判据位信噪比高一个量级。
    ("shadowfloor_noshadow", cam_low, look_at(cam_low, at(900.0, 0.0, 0.0))),
]
NO_SHADOW_SHOT = "shadowfloor_noshadow"

# 见文件头坑 ⑤：把捕获自己那份 PP 覆盖建起来，Lumen 才会回到出图里。
CAP_PP = unreal.PostProcessSettings()
CAP_PP.set_editor_property("override_dynamic_global_illumination_method", True)
CAP_PP.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
CAP_PP.set_editor_property("override_reflection_method", True)
CAP_PP.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
CAP_PP.set_editor_property("override_lumen_surface_cache_resolution", True)
CAP_PP.set_editor_property("lumen_surface_cache_resolution", 1.0)

RIGS = []
for name, cam_loc, cam_rot in SHOTS:
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
    # 坑 ⑦：没有 ViewState ⇒ 眼适应不跑 ⇒ 世界 PPV 的曝光一条都不生效。
    # ⚠️ 开了它就会继承坑 ⑧，必须同时有下面的 `WARMUP_CAPTURES` 预热。
    comp.set_editor_property("always_persist_rendering_state", True)
    RIGS.append((name, cap, comp, rt))
    spawned.append(cap)

STATE = {"ticks": 0, "handle": None, "job": 0, "phase": 0, "samples": {}}
FIRST_TICK = 40      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 5           # 让驾驶视口跟上 + 让 `r.Shadow.Virtual.Enable` 的切换生效
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
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
# 精确 (0,0,0) 的容许上限，与 `TinyGladeShotSofteningStats.py` 的 `ZERO_FAIL` 同一个数。
ZERO_FAIL = 0.005
# 开/关阴影同机位差异率的下限。
#
# 标定依据（管线修好后实测，`shadowfloor` 机位）：**9.6%**（124/1296）。
# 取实测值的**一半**（4.8%），与本项目其余出图脚本同一条规则；物理含义是
# "影子在这张图上占的面积至少还剩一半"。
# ⚠️ 这个数比岩壳那条 57.9% 小一个量级是**几何决定的**：影子只是地面上的一条带，
# 而岩壳铺满整面坡。别拿一个数去套两条判据。
# 同机位换到 house3q 时实测只有 4.7%（61/1296）—— 那才是"阈值太靠近噪声"的样子，
# 所以判据位选了 shadowfloor。
SHADOW_DIFF_FAIL = 0.048


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
    """精确 (0,0,0) 占比 —— 坑 ⑧ 有没有被预热掉的**指纹**。B 的判读（阴影区压不压得黑）
    在这个数不为零时**全部作废**：那时候的黑是捕获的 bug，不是光照。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


def report():
    ok = True
    s = STATE["samples"]
    for key in sorted(s):
        z = zero_ratio(s[key])
        unreal.log("LIT PIXELS %-20s zero=%.3f%%" % (key, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("LIT !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑧）"
                             % (key, z * 100.0))
            ok = False
    a, b = s.get("shadowfloor"), s.get(NO_SHADOW_SHOT)
    if a and b:
        ratio, changed = diff_ratio(a, b)
        unreal.log("LIT PIXELS %-20s shadow on-vs-off: %d/%d changed (%.1f%%)"
                   % ("shadowfloor", changed, GRID_POINTS, ratio * 100.0))
        # 这条判据回答的是"影子真的落在了画面上"，不是"灯的 CastShadow 是 true"。
        # 两张图**同机位、同灯、同 PPV**，唯一的变量是 `r.Shadow.Virtual.Enable`；
        # 2026-08-30 实测两条 gpumesh 路在 VSM 下一个影子像素都画不出、CSM 下全有 ⇒
        # 差异率就是"影子占了多少画面"。它掉到 0 有两种可能，两种都必须报红：
        # 灯/组件的 CastShadow 被人关掉了，或者出图管线又退化成拍不出光照。
        if ratio < SHADOW_DIFF_FAIL:
            unreal.log_error("LIT !! 开关阴影的两张几乎相同 —— 影子没有被画出来")
            ok = False
    unreal.log("LIT PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    job = STATE["job"]
    if job < len(RIGS):
        name, cap, comp, rt = RIGS[job]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            # 见文件头坑 ⑥：实例的可见集是按主视口视角压的，先让主视口驾驶这台相机。
            unreal.EditorLevelLibrary.pilot_level_actor(cap)
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
            if name == NO_SHADOW_SHOT:
                unreal.SystemLibrary.execute_console_command(world, "r.ShadowQuality 0")
        elif phase >= SETTLE:
            # 坑 ⑧：一帧只打一次，Lumen 的帧间历史才会真的往前走；换机位等于清历史，
            # 所以**每张图都要重新预热满**。
            comp.capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR,
                                                             "lit_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels(rt)
                unreal.log("shot: lit_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                if name == NO_SHADOW_SHOT:
                    unreal.SystemLibrary.execute_console_command(world, "r.ShadowQuality 5")
                STATE["job"], STATE["phase"] = job + 1, 0
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    ground.reset_paint()
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.log("SHOT DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
