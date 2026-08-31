# -*- coding: utf-8 -*-
"""窗（D8）的出图 + **像素判据**。

要回答的是三件只有像素能答、readback 与 `IsWindowDrawable()` 都答不了的事：

  A  **墙上真的破了洞**：洞数、砖数、三角形数在"洞根本没被切出来"时**全部照绿** ——
     洞不是几何，是墙材质用 OpacityMask 逐像素 discard 切出来的（裁决三：避免所有真几何洞）。
     所以这里拍**同一机位、只切 `bWindowsEnabled`** 的两张，逐像素比。
  B  **洞是洞，不是一块贴上去的补丁**：从洞里看到的是屋内（背光的内墙面），必须比外墙**暗**。
     变化像素在"开窗"那张里的平均亮度必须显著低于"关窗"那张 —— 这一条抓的是"洞没切开、
     只是多了一圈砖"那种半死不活的状态。
  C  **判据本身是活的**：最后一段**故意把世界侧弄坏** —— 把墙材质换成 Opaque 的
     `M_TinyGladeRoof`，洞在画面上当场消失，而所有数值断言纹丝不动。判据 A 必须在那一刻
     掉到阈值以下、`GetWindowUndrawableReason()` 必须开口说话。**两条都没反应就说明这道门是假的。**

用法（tag 决定文件名前缀）::

    set TG_SHOT_TAG=after
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotWindow.py"

产物：`Saved/TinyGladeShots/window_<tag>_*.png`，判据打在日志的 `WINDOW PIXELS` 行。

--- 踩过的坑（全部来自状态文件的坑表，与 TinyGladeShotDecor.py 同一份）-----------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：唯一的翻回办法是给捕获组件自己的
    `post_process_settings` 再覆盖一次（它在引擎那两行之后才 apply）。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**。准备段整个包在 try 里。
 ⑦ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState** ⇒ **PPV 里的曝光一条都不生效**。
 ⑧ ⚠️ **修了 ⑦ 才会触发 ⑧，两条必须一起加。** 有 ViewState 之后 Lumen 才真的跑起来，
    而它的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒ 间接光恒为零。
    对本脚本是致命的：判据 B 量的就是"洞里比墙暗多少"，间接光归零会把两边一起压黑。
 ⑨ ⚠️ 脚本一律调 `Get*UndrawableReason()`，**不要**调 `is_*_drawable()` —— 后者不可画时
    返回 `None`，`str(None) == "None"` 会伪造出一句像模像样的原因。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v1")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

# 判据的采样格与容差：与 `TinyGladeShotDecor.py` / `TinyGladeShotVine.py` 同一套。
GRID_X, GRID_Y = 48, 27
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
ZERO_FAIL = 0.005

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None, "house": None,
         "samples": {}, "counts": {}, "notes": {}, "rt": None, "comp": None, "cap": None,
         "spawned": [], "step": 0, "phase": 0, "wall_mat": None, "opaque_mat": None}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def make_window(edge, center_s, width, sill_z, height):
    w = unreal.CSHouseWindow()
    w.set_editor_property("edge_index", edge)
    w.set_editor_property("center_s", float(center_s))
    w.set_editor_property("width", float(width))
    w.set_editor_property("sill_z", float(sill_z))
    w.set_editor_property("height", float(height))
    return w


def note(name):
    h = STATE["house"]
    # ⚠️ 坑 ⑨：调原因版，不调 is_window_drawable()。
    why = str(h.get_window_undrawable_reason())
    STATE["counts"][name] = (h.get_window_count(), h.get_window_reject_count(),
                             h.get_frame_brick_count(), h.get_opening_count())
    STATE["notes"][name] = why
    unreal.log("WINDOW %-16s windows=%d rejected=%d bricks=%d openings=%d drawable=%s%s"
               % ((name,) + STATE["counts"][name]
                  + (why == "", "" if why == "" else (" —— " + why))))


def set_windows(on, name, frames=True, wall_mat=None):
    """切窗开关。

    ⚠️ **判据那几张必须把门框砖关掉（`frames=False`）。** 第一轮标定就栽在这上面：
    砖是一条独立的实例通路、材质也是自己的，开窗时它跟着一起出现 —— 于是"同机位切窗"的
    差异率里混着一大片砖，35.2% 的差异**在墙材质换成 Opaque（洞消失）之后仍有 33.0%**。
    那道门量的根本不是洞。关掉砖之后，两张之间**只剩被 discard 的那些像素**。
    """
    STATE["house"].set_editor_property("bWindowsEnabled", on)
    STATE["house"].set_editor_property("bFrameEnabled", frames)
    STATE["house"].set_editor_property("WallMaterial", wall_mat if wall_mat else STATE["wall_mat"])
    STATE["house"].call_method("RebuildHouse")
    note(name)


def broken(on, name):
    """**故意破坏世界侧**：把墙材质换成 Opaque 的 `M_TinyGladeRoof`。

    洞是 OpacityMask 切出来的 —— 换成 Opaque 之后画面上一个洞都没有，而窗数、砖数、
    洞数、零阻塞四条断言**纹丝不动**。判据 A 必须在这一刻掉到阈值以下、
    `GetWindowUndrawableReason()` 必须开口，否则这道门是假的。
    """
    set_windows(on, name, frames=False, wall_mat=STATE["opaque_mat"])


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    house = find("House_Road")
    if not (ground and house):
        unreal.log_error("WINDOW FAILED: no Ground_Demo / House_Road")
        return None
    STATE["ground"], STATE["house"] = ground, house
    STATE["wall_mat"] = house.get_editor_property("WallMaterial")
    STATE["opaque_mat"] = unreal.EditorAssetLibrary.load_asset("%s/M_TinyGladeRoof" % PKG)
    if not STATE["opaque_mat"]:
        unreal.log_error("WINDOW FAILED: 找不到 M_TinyGladeRoof（判据 C 的破坏实验要用它）")
        return None

    # **不画路**：拱一开就把整面南墙切满，窗全被谓词拒掉（D6 门拱优先），A 那两张就没得比了。
    ground.reset_paint()
    # ⚠️ **藤蔓与摆件必须关掉，否则判据量的不是洞。**（第一轮标定实测栽在这里）
    # 藤蔓读 `CurrentOpenings` 避让墙洞（`VineHoleClearance`，TG 的 `ivy_grower` 读 `PrevWallHoles`
    # 那条的对位物）⇒ **切一次窗，整面墙的藤会重新排一遍**。于是"同机位只切 bWindowsEnabled"
    # 的差异率里混着几百段藤：实测把墙材质换成 Opaque（洞当场消失）之后差异率**仍有 32.8%**，
    # 那 32.8% 全是藤在动。摆件同理（门口净空会随洞集合变）。
    house.set_editor_property("bVineEnabled", False)
    house.set_editor_property("bDecorEnabled", False)
    house.set_editor_property("Windows", [
        make_window(0, 160.0, 78.0, 90.0, 110.0),
        make_window(0, 300.0, 78.0, 90.0, 110.0),
        make_window(0, 440.0, 78.0, 90.0, 110.0),
    ])
    house.call_method("RebuildHouse")
    note("setup")

    loc = house.get_actor_location()
    foot = house.get_editor_property("FootprintSize")
    wall_h = house.get_editor_property("WallHeight")
    wall_y = loc.y - foot.y * 0.5          # 南墙外表面（边 0 的 Start.y）
    sill_z = loc.z + 90.0                  # 与上面那三扇窗的 SillZ 同一个数
    mid_z = sill_z + 55.0                  # 洞正中

    # 机位一律从房子参数反算，不手调 —— 手调的机位在房子一改尺寸之后就全废了。
    # 贴脸：正对最左那扇窗（边 0 的 S = 160 ⇒ 世界 X = loc.x − foot.x/2 + 160）。
    win_x = loc.x - foot.x * 0.5 + 160.0
    cam_close = unreal.Vector(win_x, wall_y - 300.0, mid_z)
    aim_close = unreal.Vector(win_x, wall_y, mid_z)
    # 全景：三扇窗连同整面墙 + 屋顶。
    cam_wide = unreal.Vector(loc.x - foot.x * 0.9, wall_y - 900.0, loc.z + wall_h * 1.15)
    aim_wide = unreal.Vector(loc.x, loc.y, loc.z + wall_h * 0.45)

    CAM_CLOSE = (cam_close, look_at(cam_close, aim_close))
    CAM_WIDE = (cam_wide, look_at(cam_wide, aim_wide))

    # (名字, 拍之前要做的事, 机位)。每一对必须**同机位、只切一个开关**，否则差异率没有意义。
    #
    # 前四张是交付图（砖开着，就是线上那副样子）；后四张是**判据图**（砖关掉，两张之间只剩
    # 被 discard 的像素）。分开是因为砖会把判据淹掉，见 `set_windows` 上面那段。
    STATE["plan"] = [
        ("close_off", lambda: set_windows(False, "close off"), CAM_CLOSE),
        ("close_on", lambda: set_windows(True, "close on"), CAM_CLOSE),
        ("wide_off", lambda: set_windows(False, "wide off"), CAM_WIDE),
        ("wide_on", lambda: set_windows(True, "wide on"), CAM_WIDE),
        ("hole_off", lambda: set_windows(False, "hole off", frames=False), CAM_CLOSE),
        ("hole_on", lambda: set_windows(True, "hole on", frames=False), CAM_CLOSE),
        # 判据 C：故意把世界侧弄坏（墙材质换 Opaque），看 A 会不会红。
        ("broken_off", lambda: broken(False, "broken off"), CAM_CLOSE),
        ("broken_on", lambda: broken(True, "broken on"), CAM_CLOSE),
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
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_close, CAM_CLOSE[1])
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


def luma(px):
    return 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]


def zero_ratio(samples):
    """精确 (0,0,0) 占比 —— 坑 ⑧ 有没有被预热掉的**指纹**。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


FIRST_TICK = 45      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 6           # 切开关 / 换材质 → 重建房子 + 组件重注册，留几帧
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
# 不是"取一个能绿的小数"。标定实测（2026-08-30，藤蔓/摆件关掉之后的干净一轮）：
#   交付图  贴脸机位切 `bWindowsEnabled` 的差异率  **5.1%**
#   交付图  全景机位同上                           **1.7%**
#   A       砖也关掉、只剩洞的差异率               **5.1%**
#   B       洞覆盖处的亮度落差（关窗 − 开窗）      **31.7**
# 四条都取一半。物理含义：洞在屏幕上的覆盖面至少还剩一半、洞至少还是比墙暗那么多的一半。
#
# ⚠️ **差异率只有个位数是正常的，别嫌它小**：洞里看到的是同一种材质的内墙面，与外墙的亮度
# 差本来就不大，`PIXEL_DELTA = 12` 会把其中一半滤掉。判据的力量不在绝对值，在**破坏实验**：
# 把墙材质换成 Opaque 之后这个数掉到 **0.0%**，5.1% 与 0.0% 之间隔着 51 倍。
#
# ⚠️ **两个第一轮踩过的坑，都会让判据量到别的东西**（改阈值之前先读这两条）：
#   ① **门框砖**：砖是独立实例路、材质也自己一套，开窗时跟着一起出现。用交付图那一对当 A，
#      差异率 35.2% 看着很健康 —— 直到破坏实验把洞弄没，差异率**仍有 33.0%**。那全是砖。
#   ② **藤蔓**：藤读 `CurrentOpenings` 避让墙洞 ⇒ 切一次窗整面墙的藤重排一遍。关掉砖之后
#      差异率仍有 34.5%、破坏后仍有 32.8%，那全是藤在动。摆件同理。
#   ⇒ 判据那几张必须**藤/摆件/砖全关**，只剩被 discard 的像素。
WINDOW_DIFF_FAIL = 0.025    # 交付图 贴脸 = 5.1% / 2
WIDE_DIFF_FAIL = 0.008      # 交付图 全景 = 1.7% / 2
HOLE_DIFF_FAIL = 0.025      # A = 5.1% / 2
HOLE_DARKEN_FAIL = 15.8     # B = 31.7 / 2


def diff_ratio(a, b):
    return sum(1 for m in changed_mask(a, b) if m) / float(GRID_POINTS)


def report():
    s = STATE["samples"]
    ok = True

    # ---- 先把两条"回执"判据做掉（不需要像素）----
    # 健康态必须闭嘴、破坏态必须开口。第二条正是 `IsWindowDrawable` 里那条 Masked 判据的执行面。
    h = STATE["house"]
    h.set_editor_property("bFrameEnabled", True)
    h.set_editor_property("bWindowsEnabled", True)
    h.set_editor_property("WallMaterial", STATE["opaque_mat"])
    h.call_method("RebuildHouse")
    note("broken_full")
    h.set_editor_property("WallMaterial", STATE["wall_mat"])
    h.call_method("RebuildHouse")
    note("healthy_full")

    if STATE["notes"].get("healthy_full", "x") != "":
        unreal.log_error("WINDOW !! 健康状态下 GetWindowUndrawableReason() 却在报错：%s"
                         % STATE["notes"].get("healthy_full"))
        ok = False
    if STATE["notes"].get("broken_full", "") == "":
        unreal.log_error("WINDOW !! 墙材质换成 Opaque 之后 GetWindowUndrawableReason() 还是空的 —— "
                         "那条 Masked 判据没起作用")
        ok = False

    for name in sorted(s):
        z = zero_ratio(s[name])
        unreal.log("WINDOW PIXELS %-12s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("WINDOW !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑧）"
                             % (name, z * 100.0))
            ok = False

    # ---- 交付图：同机位切窗，画面必须真的变（贴脸 + 全景各一对）----
    for pair, fail, what in (("close", WINDOW_DIFF_FAIL, "贴脸"), ("wide", WIDE_DIFF_FAIL, "全景")):
        a, b = s.get(pair + "_off"), s.get(pair + "_on")
        if not (a and b):
            continue
        ratio = diff_ratio(a, b)
        unreal.log("WINDOW PIXELS %-5s on-vs-off: %.1f%% changed" % (pair, ratio * 100.0))
        if ratio < fail:
            unreal.log_error("WINDOW !! %s机位切窗几乎没有改变画面" % what)
            ok = False

    # ---- A / B：**砖关掉**的那一对，两张之间只剩被 discard 的像素 ----
    hole_off, hole_on = s.get("hole_off"), s.get("hole_on")
    hole_ratio = None
    if hole_off and hole_on:
        hole_ratio = diff_ratio(hole_off, hole_on)
        unreal.log("WINDOW PIXELS hole-only on-vs-off: %.1f%% changed" % (hole_ratio * 100.0))
        if hole_ratio < HOLE_DIFF_FAIL:
            unreal.log_error("WINDOW !! 关掉门框砖之后切窗几乎没有改变画面 —— 洞根本没有被切出来")
            ok = False

        # B：洞覆盖处必须**变暗** —— 从洞里看到的是背光的屋内。
        # 这一条抓的是"洞没切开、只是墙面颜色变了"那种半死不活的状态。
        mask = changed_mask(hole_off, hole_on)
        lit = [(hole_off[i], hole_on[i]) for i, m in enumerate(mask) if m]
        if lit:
            off_luma = sum(luma(a) for a, _ in lit) / float(len(lit))
            on_luma = sum(luma(b) for _, b in lit) / float(len(lit))
            unreal.log("WINDOW PIXELS hole pixels: wall luma=%.1f -> hole luma=%.1f (drop %.1f)"
                       % (off_luma, on_luma, off_luma - on_luma))
            if off_luma - on_luma < HOLE_DARKEN_FAIL:
                unreal.log_error("WINDOW !! 洞覆盖处没有变暗（落差只有 %.1f）—— 洞里看到的应该是背光的屋内"
                                 % (off_luma - on_luma))
                ok = False

    # ---- C：**故意破坏世界侧**之后，A 必须红 ----
    #
    # 这一段不是在测窗，是在测**判据 A 本身是不是活的**。墙材质换成 Opaque 之后洞在画面上
    # 消失，而窗数 / 砖数 / 洞数一个都不变（下面那行日志会把它们打出来对照）——
    # 差异率若照旧过阈，说明 A 量到的根本不是洞。第一轮标定就是这么发现"A 其实在量砖"的。
    b_off, b_on = s.get("broken_off"), s.get("broken_on")
    if b_off and b_on:
        broken_ratio = diff_ratio(b_off, b_on)
        unreal.log("WINDOW PIXELS broken (opaque wall) on-vs-off: %.1f%% changed (healthy was %s)"
                   % (broken_ratio * 100.0,
                      "n/a" if hole_ratio is None else ("%.1f%%" % (hole_ratio * 100.0))))
        if broken_ratio >= HOLE_DIFF_FAIL:
            unreal.log_error("WINDOW !! 把墙材质换成 Opaque 之后判据 A 依然是绿的 —— 这道门是假的，"
                             "它量到的不是洞")
            ok = False
        else:
            unreal.log("WINDOW gate is live: 破坏世界侧之后判据 A 从 %.1f%% 掉到 %.1f%%（阈值 %.1f%%）"
                       % (0.0 if hole_ratio is None else hole_ratio * 100.0,
                          broken_ratio * 100.0, HOLE_DIFF_FAIL * 100.0))

    # 数值侧的配套：破坏前后**窗数与砖数一个都不变** —— 这正是"只靠数值断言看不出来"的证据。
    healthy = STATE["counts"].get("healthy_full")
    bad = STATE["counts"].get("broken_full")
    if healthy and bad:
        unreal.log("WINDOW COUNTS healthy windows=%d bricks=%d openings=%d | broken windows=%d bricks=%d openings=%d"
                   % (healthy[0], healthy[2], healthy[3], bad[0], bad[2], bad[3]))
        if healthy != bad:
            unreal.log_error("WINDOW !! 破坏实验把数值也改了 —— 那就证明不了'数值断言对这种失效是盲的'")
            ok = False

    unreal.log("WINDOW PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


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
            # 实例的可见集按主视口视角压，先让主视口驾驶这台相机（门框砖走 GPU 实例路）。
            unreal.EditorLevelLibrary.pilot_level_actor(STATE["cap"])
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            # 坑 ⑧：一帧只打一次 capture_scene()，Lumen 的帧间历史才会真的往前走。
            # 挪过相机 / 切过开关之后历史等于清零，所以**每个机位都要重新预热满**。
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "window_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("WINDOW shot: window_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    # 收尾：把破坏实验改过的世界还原干净，别把 Opaque 墙留给下一个脚本。
    if STATE["house"]:
        STATE["house"].set_editor_property("WallMaterial", STATE["wall_mat"])
        STATE["house"].set_editor_property("bWindowsEnabled", True)
        STATE["house"].set_editor_property("bFrameEnabled", True)
        STATE["house"].set_editor_property("bVineEnabled", True)
        STATE["house"].set_editor_property("bDecorEnabled", True)
        STATE["house"].set_editor_property("Windows", [])
        STATE["house"].call_method("RebuildHouse")
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("WINDOW DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("WINDOW FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
