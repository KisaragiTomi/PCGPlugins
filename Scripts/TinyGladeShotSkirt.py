# -*- coding: utf-8 -*-
"""塑形物裙边摆件（D12 锚点层的**第五家**）的出图 + **像素判据**。

要回答的是三件只有像素能答、readback 与 `GetSkirtDecorUndrawableReason()` 都答不了的事：

  A  **摆件真的出现在土台裙边上**：那条 drawable 判据只证明"组件/快照/实例源/材质都在且材质
     支持实例化"，它证明不了那十几个实例有没有变成屏幕上的颜色。所以这里拍**同一机位、
     只切 `bSkirtDecorEnabled`** 的两张，逐像素比 —— 差异像素率就是"它画出来了"的硬证据。
  B  **摆件不是一片灰、也不是一团黑**：母材质没勾 `bUsedWithInstancedStaticMeshes` 时引擎会
     **静默换成默认材质**（一片灰，症状与"没绑材质"逐像素相同）；顶点色搬错时是一团黑。
     两种失效都能让 A 的差异率很高，所以另量一条：变化像素的**平均亮度**要够高，且**暖调**
     （R > B）—— 引擎默认材质是中性灰，`R ≈ B`。
  C  ⚠️ **故意破坏世界侧的对照**（这一节是本脚本存在的第二个理由）：第三张图把
     `SkirtDecorMeshes` **清空**再拍。它与"关掉开关"那张必须**几乎逐像素相同**
     （差异率落在采样噪声底），否则说明 A 量到的差异根本不是摆件带来的 ——
     近七轮每轮都靠这一步抓到假门或真 bug（上一轮山墙机位就拍进了隔壁房子的白墙，
     读出 0.0% 却与藤无关）。清空网格同时会让 drawable 判据报红，那句原因也一并打进日志。

--- 为什么在 `L_HouseGroundDemo` 上拍，而不是有现成土台的 `L_TerrainOpsDemo` ----------
状态文件那条「离屏 SceneCapture 画不出 GPU 实例」**只在 `L_TerrainOpsDemo` 复现**
（216 级石阶），同一个组件类在 `L_HouseGroundDemo` 里的 114 块门框砖与整片藤蔓四个机位
全画对了。裙边摆件走的正是那条 GPU 实例路，所以这里**现场 spawn 一座塑形物**到
`L_HouseGroundDemo` 上，用那张已知能出图的关卡。真因仍然开放，这只是绕开它。

用法（tag 决定文件名前缀）::

    set TG_SHOT_TAG=v1
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotSkirt.py"

产物：`Saved/TinyGladeShots/skirt_<tag>_*.png`，判据打在日志的 `SKIRT PIXELS` 行。

--- 踩过的坑（全部来自状态文件的坑表）-----------------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：唯一的翻回办法是给捕获组件自己的
    `post_process_settings` 再覆盖一次（它在引擎那两行之后才 apply）。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**。准备段整个包在 try 里。
 ⑦ `USceneCaptureComponent` 默认没有 ViewState ⇒ **PPV 里的曝光一条都不生效**。
 ⑧ 修了 ⑦ 才会触发 ⑧：有 ViewState 之后 Lumen 才真跑，而它靠**帧间历史** ⇒
    `capture_scene()` 只抓一帧的话间接光恒为零。必须**一帧一次**地预热到 128。
 ⑨ CDO 的默认值**不传播到同会话 spawn 的实例** —— spawn 出来的塑形物必须逐条再写一份。
 ⑩ 判据一律调 `get_*_undrawable_reason()`，**不要** `is_*_drawable()`：UE Python 把
    "bool 返回值 + 一个 out 参数"收成单一返回值，不可画时拿到 `None`，
    `str(None) == "None"` 会伪造出一句像模像样的原因，恰好在唯一需要它的时候失效。
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

# 现场 spawn 的那座土台。摆位与尺寸是**一起**选的，四条约束同时成立（改任何一个都要重算）：
#   · 影响半径 R + F = 650，中心 (1500, 2400) ⇒ 影响圆 x∈[850,2150] y∈[1750,3050]，
#     整个落在 64 格 × 50 cm = 3200 cm 的地面矩形内 —— **裙边一出界就完了**：出界处
#     `SampleHeight` 退回 actor Z，那一段摆件会浮在没有地面的空中（第一版就是这么拍砸的）；
#   · 与两栋房（(1100,1600) / (2200,1600)）的距离是 894 / 1063 > 650 ⇒ 房子不站在裙边上；
#   · 锚点环半径 R + F × BandT = 498 ⇒ 环直径 996；机位离环心 1400 cm 时近侧那段弧只有
#     900 cm 远，画面宽度约 940 cm ⇒ **一件摆件占三四个采样列**，判据 A 才有分辨力。
#   · ⚠️ **土台不能再大了**：地面只有 3200 cm 见方，中间还站着两栋房。第二、三版用
#     R=350/F=450（影响圆 1600 宽）时，能放相机的余地只剩几百 cm，画面被土台占满而
#     每件摆件只剩两三个采样点，差异率掉到 0.3~0.8%。
MOUND_XY = (1500.0, 2400.0)
MOUND_R, MOUND_F, MOUND_LIFT = 250.0, 400.0, 380.0
CLUTTER = "/Game/TinyGlade/Meshes/clutter/%s/StaticMeshes/%s.%s"
SKIRT_MESHES = ["barrel", "firewood", "basket"]
SKIRT_MATERIAL = "%s/M_TinyGladeDecor.M_TinyGladeDecor" % PKG

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None, "mound": None,
         "meshes": [], "samples": {}, "counts": {}, "rt": None, "comp": None, "cap": None,
         "spawned": [], "step": 0, "phase": 0}


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
    g = STATE["ground"]
    why = str(g.get_skirt_decor_undrawable_reason())
    STATE["counts"][name] = (g.get_skirt_decor_anchor_count(), g.get_skirt_decor_instance_count(), why)
    unreal.log("SKIRT %-14s anchors=%d instances=%d drawable=%s%s"
               % (name, STATE["counts"][name][0], STATE["counts"][name][1],
                  why == "", "" if why == "" else (" —— " + why)))


def set_skirt(on, name):
    STATE["ground"].set_editor_property("bSkirtDecorEnabled", on)
    STATE["ground"].call_method("RebuildSkirtDecor")
    note(name)


def break_world(name):
    """**故意破坏世界侧**：把网格表清空（判据 C）。开关仍然是开着的 ——
    破坏的是世界，不是断言，这正是这一步要证明的东西。"""
    STATE["ground"].set_editor_property("bSkirtDecorEnabled", True)
    STATE["ground"].set_editor_property("SkirtDecorMeshes", [])
    STATE["ground"].call_method("RebuildSkirtDecor")
    note(name)


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    if not ground:
        unreal.log_error("SKIRT FAILED: no Ground_Demo")
        return None
    STATE["ground"] = ground

    meshes = [unreal.load_asset(CLUTTER % (n, n, n)) for n in SKIRT_MESHES]
    material = unreal.load_asset(SKIRT_MATERIAL)
    if not all(meshes) or not material:
        unreal.log_error("SKIRT FAILED: 缺资产 meshes=%s material=%s"
                         % ([n for n, m in zip(SKIRT_MESHES, meshes) if m is None], material))
        return None
    STATE["meshes"] = meshes

    ground.reset_paint()
    # ⚠️ **披挂岩壳在这一张里必须关掉**（拍砸过一次）：壳正好披在裙边那条带上，
    # 一整圈灰蓝色石板把摆件遮掉大半，剩下的几件贴在石板缝里几乎读不出来（实测差异率
    # 从 2.9% 掉到 0.3%）。壳有自己的出图脚本（`TinyGladeShotRockShell.py`），
    # 这一张的被测对象是摆件 —— 与单测里"量岩壳时把石阶关掉"是同一条纪律。
    STATE["shell_was"] = ground.get_editor_property("bRockShell")
    ground.set_editor_property("bRockShell", False)
    ground.call_method("RebuildRockShell")

    # 现场 spawn 一座土台（坑 ⑨：CDO 默认值不传播到实例，逐条再写一份）。
    mound = ACTORS.spawn_actor_from_class(
        unreal.load_class(None, "%s/BP_GroundShaper.BP_GroundShaper_C" % PKG),
        unreal.Vector(MOUND_XY[0], MOUND_XY[1], 0.0))
    if not mound:
        unreal.log_error("SKIRT FAILED: spawn BP_GroundShaper 失败")
        return None
    for name, value in (("Ground", ground), ("Radius", MOUND_R),
                        ("FalloffDistance", MOUND_F), ("LiftHeight", MOUND_LIFT)):
        mound.set_editor_property(name, value)
    mound.call_method("RebuildTerrain")
    STATE["mound"] = mound
    STATE["spawned"].append(mound)

    # 参数显式写死，不靠 CDO 默认值（同回归脚本那一节的理由）。
    ground.set_editor_property("SkirtDecorMeshes", meshes)
    ground.set_editor_property("SkirtDecorMaterial", material)
    # ⚠️ **间距在这一张里调密到 130（交付默认是 260），这是有意的、也只影响这一张**：
    # 演示土台的锚点环只有 498 cm 半径，按 260 cm 一圈只摆得下 12 个锚 ⇒ 6 件 ⇒ 相机看得见的
    # 那半圈只剩两三件，判据 A 于是没有分辨力（实测 0.5%）。本脚本量的是
    # **"那些实例有没有变成屏幕上的颜色"**，与一圈摆几件无关；密度那一条由回归脚本
    # （用的是交付默认 260）与单测 `SkirtDensityFollowsAnchorCount` 各自钉着。
    # 最小间距跟着降到 110：不降的话它比弧步长（2π×498/24 ≈ 130）还大，相邻的会被互相挤掉。
    ground.set_editor_property("SkirtDecorSpacing", 130.0)
    ground.set_editor_property("SkirtDecorBandT", 0.62)
    ground.set_editor_property("SkirtDecorMinSpacing", 110.0)
    ground.set_editor_property("SkirtDecorScale", 0.6)
    ground.set_editor_property("SkirtDecorScaleJitter", 0.18)
    ground.set_editor_property("SkirtDecorSeed", 11)
    ground.set_editor_property("bSkirtDecorEnabled", True)
    ground.call_method("RebuildSkirtDecor")
    note("setup")

    # 机位从土台参数反算，不手调 —— 手调的机位在土台一改尺寸之后就全废了。
    # 压低 + 拉近：裙边摆件只有 60~100 cm 高，高机位下每件只占一两个采样点，
    # 差异率会被稀释到噪声底附近（判据 A 于是失去分辨力）。
    cx, cy = MOUND_XY
    # ⚠️ **机位必须留在地面矩形之内** —— 这是踩出来的（前三版都拍砸了，各砸各的）：
    #   · v1/v2 放在土台正北、退到环外 520~950 cm（y ≈ 3750~4180），那已经出了地面
    #     （y > 3200）⇒ 相机悬在没有地面的地方、以掠角看地面的断口，画面 2/3 是一大片贴图；
    #   · v3 从东北角看，机位对了，但土台太大、退得太远 ⇒ 每件摆件只剩两三个采样点（0.3%）；
    #   · v4 拉近了，土台却把整幅画面占满，摆件全被挤到画框外（0.8%）。
    # 定版：**从正东压低到 420 cm 平看**，离环心 1400 cm（地面东边界留得下）。
    # 正东方向是这张关卡上离地面边界最远的一条（3200 − 1500 = 1700），也正因此才站得开。
    cam = unreal.Vector(cx + 1400.0, cy, 420.0)
    aim = unreal.Vector(cx, cy, 170.0)
    CAM = (cam, look_at(cam, aim))

    # (名字, 拍之前要做的事, 机位)。三张**必须同机位**，否则差异率没有意义。
    STATE["plan"] = [
        ("skirt_off", lambda: set_skirt(False, "off"), CAM),
        ("skirt_on", lambda: set_skirt(True, "on"), CAM),
        ("skirt_broken", lambda: break_world("broken"), CAM),
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
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam, CAM[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    # 坑 ⑦ + ⑧ 必须成对：开 ViewState，再靠 WARMUP_CAPTURES 一帧一次地推进 Lumen 的帧间历史。
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


def ratio_changed(a, b):
    return sum(1 for m in changed_mask(a, b) if m) / float(GRID_POINTS)


def zero_ratio(samples):
    """精确 (0,0,0) 占比 —— 坑 ⑧ 有没有被预热掉的**指纹**。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


FIRST_TICK = 45      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 6           # 切开关 → 重摆 + 组件重注册，留几帧
# 坑 ⑧ 的执行面。**128 不是保守取整，32 是真的不够**（2026-08-31 重标定，整表见
# `TinyGladeShotSeamWarmup.py` 的文件头）：zero% 在 16 次就到 0，但"画面不再变"要到 64 次
# 才收敛（w32 相对 w384 还差 0.540%），与最薄那条门同量级。
# ⚠️ 必须**一帧一次**：同一帧连打 128 次不推进帧间历史，等于没预热。
WARMUP_CAPTURES = int(os.environ.get("TG_SHOT_WARMUP", "128"))

# 判据的阈值。口径与本项目其余出图脚本一致：**先量真值、再取实测值的一半**。
# 标定实测（2026-08-31，资产与机位定版后）：
#   A  切 `bSkirtDecorEnabled` 的差异率     **5.9%**（77/1296）
#   B  摆件覆盖处平均亮度                   **56.8**，rgb=(62.3, 56.0, 40.6)
#   C  故意破坏（清空网格） vs 关掉开关     **0.077%**（1/1296，= 采样噪声底）
# A 取实测值的一半；B 的失效模式不是"变少"而是"变黑"（房子那一轮真撞到过：base color 被
# `MaterialExpressionClamp` 接断之后摆件全黑，luma 只剩 19.2），同样取一半。
# C 是**上界**不是下界：破坏之后画面必须与"关掉"几乎相同，0.8% 给采样与 Lumen 抖动留余量。
SKIRT_DIFF_FAIL = 0.029   # A = 5.9% / 2
SKIRT_LUMA_FAIL = 28.0    # B = 56.8 / 2
BREAK_DIFF_FAIL = 0.008   # C 的上界（实测 0.077%）


def report():
    s = STATE["samples"]
    ok = True
    for name in sorted(s):
        z = zero_ratio(s[name])
        unreal.log("SKIRT PIXELS %-14s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("SKIRT !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑧）"
                             % (name, z * 100.0))
            ok = False

    off, on, broken = s.get("skirt_off"), s.get("skirt_on"), s.get("skirt_broken")
    if off and on:
        mask = changed_mask(off, on)
        changed = sum(1 for m in mask if m)
        r = changed / float(GRID_POINTS)
        unreal.log("SKIRT PIXELS on-vs-off: %d/%d changed (%.1f%%)" % (changed, GRID_POINTS, r * 100.0))
        # A：同机位只切开关，画面必须真的变 —— 这是"摆件真的画出来了"的硬证据。
        if r < SKIRT_DIFF_FAIL:
            unreal.log_error("SKIRT !! 切换 bSkirtDecorEnabled 几乎没有改变画面 —— 摆件没有被画出来")
            ok = False

        # B：**被摆件盖得最实的那批采样点**在开着那张里的平均亮度与色调。
        #
        # ⚠️ 不能拿"全部变化像素"求平均（`TinyGladeShotDecor.py` 那张可以，因为房子周围的摆件
        # 又大又多、几乎每个变化点都整点落在摆件上）。裙边这一张的摆件只有 60~85 cm，
        # 一个采样点往往**半件摆件半片草**，均值于是被草的绿色拉过去 —— 实测 rgb=(15, 44, 24)，
        # 暖调那一条报红，而画面里摆件明明是对的。所以先按 |Δ| 排序，只取盖得最实的四分之一。
        changed_idx = [i for i, m in enumerate(mask) if m]
        changed_idx.sort(key=lambda i: max(abs(on[i][k] - off[i][k]) for k in range(3)), reverse=True)
        lit = [on[i] for i in changed_idx[:max(4, len(changed_idx) // 4)]]
        if lit:
            luma = sum(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in lit) / float(len(lit))
            red = sum(p[0] for p in lit) / float(len(lit))
            green = sum(p[1] for p in lit) / float(len(lit))
            blue = sum(p[2] for p in lit) / float(len(lit))
            unreal.log("SKIRT PIXELS skirt pixels (盖得最实的 %d/%d 点): luma=%.1f rgb=(%.1f, %.1f, %.1f)"
                       % (len(lit), len(changed_idx), luma, red, green, blue))
            if luma < SKIRT_LUMA_FAIL:
                unreal.log_error("SKIRT !! 摆件覆盖处平均亮度只有 %.1f —— 八成是顶点色搬成了黑" % luma)
                ok = False
            # 引擎默认表面材质是中性灰（R ≈ B）；本材质是暖调（顶点色 × 暖常数）。
            # 这一条抓的正是"母材质没勾 bUsedWithInstancedStaticMeshes ⇒ 静默换成默认材质"。
            if red <= blue * 1.15:
                unreal.log_error("SKIRT !! 摆件覆盖处不是暖调（rgb=%.1f, %.1f, %.1f）—— 材质八成被换成了默认灰"
                                 % (red, green, blue))
                ok = False

    # C：**故意破坏世界侧**的对照。清空网格与关掉开关是两条互不相干的路，
    # 画面必须几乎逐像素相同；不相同就说明 A 量到的差异另有来源（上一轮就是这么抓到假门的）。
    if off and broken:
        rb = ratio_changed(off, broken)
        unreal.log("SKIRT PIXELS broken-vs-off (故意破坏对照): %.3f%% changed" % (rb * 100.0))
        if rb > BREAK_DIFF_FAIL:
            unreal.log_error("SKIRT !! 清空网格之后画面与「关掉摆件」差了 %.3f%% —— A 量到的差异另有来源"
                             % (rb * 100.0))
            ok = False
    if on and broken:
        rob = ratio_changed(on, broken)
        unreal.log("SKIRT PIXELS broken-vs-on: %.3f%% changed（应当与 on-vs-off 同量级）" % (rob * 100.0))
        if rob < SKIRT_DIFF_FAIL:
            unreal.log_error("SKIRT !! 清空网格没有让画面变回去 —— 撤实例源那条路可能没走通")
            ok = False

    # 数值侧的配套：破坏之后 drawable 判据必须报红（门是活的）。
    broken_counts = STATE["counts"].get("broken")
    if broken_counts:
        if broken_counts[1] != 0:
            unreal.log_error("SKIRT !! 清空网格之后还剩 %d 件摆件 —— 撤实例源那条路没走通" % broken_counts[1])
            ok = False
        if broken_counts[2] == "":
            unreal.log_error("SKIRT !! 清空网格之后 drawable 判据仍然说'画得出来' —— 那道门是空的")
            ok = False

    unreal.log("SKIRT PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


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
            # 实例的可见集按主视口视角压，先让主视口驾驶这台相机（同 TinyGladeShotDecor.py）。
            unreal.EditorLevelLibrary.pilot_level_actor(STATE["cap"])
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            # 坑 ⑧：一帧只打一次 capture_scene()，Lumen 的帧间历史才会真的往前走。
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "skirt_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("SKIRT shot: skirt_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    if STATE["ground"]:
        STATE["ground"].set_editor_property("SkirtDecorMeshes", STATE["meshes"])
        STATE["ground"].set_editor_property("bSkirtDecorEnabled", True)
        STATE["ground"].set_editor_property("bRockShell", STATE.get("shell_was", True))
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("SKIRT DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("SKIRT FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
