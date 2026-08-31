# -*- coding: utf-8 -*-
"""墙面藤蔓（D13）的出图 + **像素判据**。

要回答的是六件只有像素能答、readback 与 `GetVineUndrawableReason()` 都答不了的事：

  A  **藤真的出现在墙上**：可画性只证明"组件/快照/实例源/材质都在且材质支持实例化"，
     它证明不了那 300 多个实例有没有变成屏幕上的颜色。所以拍**同一机位、只切
     `bVineEnabled`** 的两张，逐像素比 —— 差异像素率就是"它画出来了"的硬证据。
  B  **`ivy_branch` 没有法线也没有 UV 这条被解决了**：没解决的话藤是一条**纯黑剪影**
     （零长切线 ⇒ 光照全灭），而剪影同样会让 A 的差异率很高。所以另外量一条：
     变化像素的**平均亮度**必须显著高于纯黑，且色相偏绿。
  C  **藤避让墙洞**：画路开六个拱之后，拱那一片的藤给洞让位（绕过去，不是整根砍掉）。
  D  **第二档的三件事真的改变了画面**（山墙 / 跨墙 / 花）：同机位、只切那三个开关。
  E  **花真的被画出来了**：花是第三个调色板，它有一条独立的失败模式 ——
     记录排出来了、counter 也对，但组件没有基础网格 ⇒ 屏幕上一朵都没有。
  F  **季节标量真的走到了材质里**：`Season` 这个参数名是 Python 与 C++ 之间的**字符串约定**，
     写错时 `SetScalarParameterValue` **不报错**、季节静默停在夏天。只有像素能抓到它。

用法（tag 决定文件名前缀）::

    set TG_SHOT_TAG=v2
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotVine.py"

**故意破坏实验**（验证门是活的，每轮必做一次）::

    set TG_VINE_BREAK=vine|tier2|flower|season      # 破坏**世界侧**，不动阈值

产物：`Saved/TinyGladeShots/vine_<tag>_*.png`，判据打在日志的 `VINE PIXELS` 行。

--- 踩过的坑（全部来自状态文件的坑表）-----------------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：唯一的翻回办法是给捕获组件自己的
    `post_process_settings` 再覆盖一次（它在引擎那两行之后才 apply）。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**。准备段整个包在 try 里。
 ⑦ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState** ⇒ **PPV 里的曝光一条都不生效**。
    与坑 ⑤ 症状字面相同、根因完全不同。
 ⑧ ⚠️ **修了 ⑦ 才会触发 ⑧，两条必须一起加。** 有 ViewState 之后 Lumen 才真的跑起来，
    而它的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒ 间接光恒为零 ⇒ 没被直射的
    lit 表面全部精确 (0,0,0)。对本脚本是**致命**的：判据 B 量的就是"藤不是黑的"，
    不预热的话背光那一面的藤本来就全黑，B 会得出完全相反的结论。
 ⑨ 藤走的是 GPU 实例路（`UCSGpuInstancedMeshComponent`）。状态文件那条"离屏 capture 画不出
    GPU 实例"只在 `L_TerrainOpsDemo` 的石阶上复现过，`L_HouseGroundDemo` 的 114 块门框砖
    四个机位全画对了 —— 本脚本用的正是后者那张关卡，并沿用"先让主视口驾驶捕获相机"的缓解手段。
 ⑩ 一律调 `Get*UndrawableReason()`，**不要** `is_*_drawable()` —— 后者不可画时返回 `None`，
    `str(None) == "None"` 会伪造出一句像模像样的原因。
 ⑪ 一次运行里连拍多个世界状态：改完世界要先空转 `SETTLE` 帧再开始预热。
 ⑫ UE Python 枚举比较：`str(枚举).endswith("X")` **恒假**、`int(枚举)` 直接 TypeError。
    一律用 `unreal.CSRidgeAxis.X` 直接比对象（山墙在哪两面取决于脊向）。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v2")
# 故意破坏实验：破坏的一律是**世界侧**（开关、资产引用），**绝不动下面那几个阈值**。
BREAK = os.environ.get("TG_VINE_BREAK", "").strip().lower()
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

# 判据的采样格与容差：与 `TinyGladeShotRockShell.py` 同一套（48 × 27 = 1296 点、单通道 > 12/255）。
GRID_X, GRID_Y = 48, 27
GRID_POINTS = GRID_X * GRID_Y
PIXEL_DELTA = 12
# 精确 (0,0,0) 的容许上限，与 `TinyGladeShotSofteningStats.py` 的 `ZERO_FAIL` 同一个数。
ZERO_FAIL = 0.005

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None, "house": None,
         "samples": {}, "counts": {}, "rt": None, "comp": None, "cap": None, "spawned": [],
         "step": 0, "phase": 0, "flower_mesh": None}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def apply(name, **props):
    """写一批属性 → 重建 → 把三个计数打进日志（判据红了先看这几行）。"""
    h = STATE["house"]
    for key, value in props.items():
        h.set_editor_property(key, value)
    h.call_method("RebuildHouse")
    counts = (h.get_vine_segment_count(), h.get_vine_leaf_count(), h.get_vine_flower_count())
    STATE["counts"][name] = counts
    unreal.log("VINE state %-14s branches=%d leaves=%d flowers=%d %s"
               % (name, counts[0], counts[1], counts[2],
                  " ".join("%s=%s" % kv for kv in sorted(props.items()))))


# --- 各机位拍之前要把世界摆成什么样 -------------------------------------------
# ⚠️ 每一对（off/on、tier1/tier2、summer/autumn）**必须只差被测的那一个开关**，
#    差第二个的话差异率就不再是"这一件事画出来了"的证据。

def vine_off():
    apply("wall_off", bVineEnabled=False)


def vine_on():
    apply("wall_on", bVineEnabled=(BREAK != "vine"))


def tier1():
    """第一档的形态：不上山墙、不跨墙、不开花。三个开关一起关，是"第二档整体"的对照组。"""
    apply("gable_tier1", bVineEnabled=True, bVineClimbGable=False,
          VineJumpChance=0.0, VineFlowerChance=0.0)


def tier2():
    apply("gable_tier2", bVineClimbGable=(BREAK != "tier2"),
          VineJumpChance=(0.0 if BREAK == "tier2" else 0.5),
          VineFlowerChance=(0.0 if BREAK == "tier2" else 0.10))


def flower_off():
    apply("flower_off", bVineClimbGable=True, VineJumpChance=0.5, VineFlowerChance=0.0)


def flower_on():
    # 概率拉满：判据要量的是"花画得出来"，不是"这次掷中了几朵"。
    # 破坏实验拆的是**网格引用**（世界侧），不是概率 —— 概率为 0 时连记录都不排，
    # 那证明不了"记录排了但画不出来"这条真正危险的失败模式。
    if BREAK == "flower":
        apply("flower_on", VineFlowerChance=1.0, VineFlowerMesh=None)
    else:
        apply("flower_on", VineFlowerChance=1.0, VineFlowerMesh=STATE["flower_mesh"])


def season_summer():
    apply("season_summer", VineSeason=unreal.CSVineSeason.SUMMER)


def season_autumn():
    # 破坏实验：季节**不换**（等价于 C++ 里 `Season` 参数名写错、静默停在夏天）。
    apply("season_autumn",
          VineSeason=(unreal.CSVineSeason.SUMMER if BREAK == "season" else unreal.CSVineSeason.AUTUMN))


def arch_none():
    apply("arch_no_road", VineSeason=unreal.CSVineSeason.SUMMER, VineFlowerChance=0.10)


def paint_road():
    g, h = STATE["ground"], STATE["house"]
    loc = h.get_actor_location()
    g.begin_paint_stroke()
    for i in range(17):
        y = loc.y - 800.0 + i * 100.0
        g.apply_paint_stroke(unreal.Vector(loc.x, y, g.sample_height(unreal.Vector2D(loc.x, y))))
    g.end_paint_stroke()
    h.call_method("RebuildHouse")
    counts = (h.get_vine_segment_count(), h.get_vine_leaf_count(), h.get_vine_flower_count())
    STATE["counts"]["arch_road"] = counts
    unreal.log("VINE state %-14s doors=%d branches=%d leaves=%d flowers=%d"
               % ("arch_road", h.get_open_door_count(), counts[0], counts[1], counts[2]))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    house = find("House_Road")
    if not (ground and house):
        unreal.log_error("VINE FAILED: no Ground_Demo / House_Road")
        return None
    STATE["ground"], STATE["house"] = ground, house
    STATE["flower_mesh"] = house.get_editor_property("VineFlowerMesh")

    ground.reset_paint()
    house.call_method("RebuildHouse")

    # 渲染侧的自诊断先打一行 —— 判据红了先看这行，它能一句话区分"没画出来"与"画了但看不见"。
    # ⚠️ 坑 ⑩：调原因版，**别调 is_vine_drawable()**。
    why = str(house.get_vine_undrawable_reason())
    drawable = (why == "")
    unreal.log("VINE drawable=%s%s" % (drawable, "" if drawable else (" —— " + why)))
    if not drawable:
        unreal.log_error("VINE !! 藤画不出来：%s" % why)
    if BREAK:
        unreal.log_warning("VINE ⚠️ 故意破坏实验：TG_VINE_BREAK=%s —— 对应那条门**应当报红**" % BREAK)

    loc = house.get_actor_location()
    foot = house.get_editor_property("FootprintSize")
    wall_h = house.get_editor_property("WallHeight")

    # 机位一律从房子参数反算，不手调 —— 手调的机位在房子一改尺寸之后就全废了。
    # 南墙（-Y 那面）正对着路，也是拱开在的地方。
    cam_wall = unreal.Vector(loc.x - foot.x * 0.20, loc.y - foot.y * 0.5 - 420.0, loc.z + wall_h * 0.45)
    aim_wall = unreal.Vector(loc.x + foot.x * 0.10, loc.y - foot.y * 0.5, loc.z + wall_h * 0.45)
    cam_arch = unreal.Vector(loc.x - 60.0, loc.y - foot.y * 0.5 - 320.0, loc.z + 170.0)
    aim_arch = unreal.Vector(loc.x, loc.y - foot.y * 0.5, loc.z + 150.0)
    # 花与季节都在藤的上半截 —— 机位贴近南墙的墙头，别把镜头浪费在墙脚。
    cam_near = unreal.Vector(loc.x - foot.x * 0.1, loc.y - foot.y * 0.5 - 240.0, loc.z + wall_h * 0.80)
    aim_near = unreal.Vector(loc.x - foot.x * 0.1, loc.y - foot.y * 0.5, loc.z + wall_h * 0.78)

    # 山墙那两面取决于**脊向**（坑 ⑫：枚举直接比对象，别 str/int）。
    # 脊沿 X ⇒ 跨度在 Y ⇒ 山墙是 ±X 那两面；脊沿 Y 则反过来。
    #
    # ⚠️ 机位是**斜四分之三**、不是正对山墙：正对着拍时相机落在 `House_Pillar` 那栋房子里
    #    （演示关卡两栋房沿 +X 排开、相距约 1200 cm），实测拍出来是一张糊满整帧的白墙 ——
    #    tier1/tier2 两张**逐位相同**，判据读出 0.0% 却与藤蔓毫无关系。这正是任务书里
    #    "第一版判据拉错轴、把缺陷掩盖了"那一类：**门是绿是红都要先确认它拍到了被测对象**。
    #    斜机位同时框住山墙三角与檐墙，跨墙那几段（藤绕过转角）也正好在画面里。
    ridge_x = (house.get_editor_property("RidgeAxis") == unreal.CSRidgeAxis.X)
    if ridge_x:
        gable_c = unreal.Vector(loc.x + foot.x * 1.2, loc.y - foot.y * 1.4, loc.z + wall_h * 1.5)
    else:
        gable_c = unreal.Vector(loc.x - foot.x * 1.4, loc.y + foot.y * 1.2, loc.z + wall_h * 1.5)
    gable_a = unreal.Vector(loc.x, loc.y, loc.z + wall_h * 0.5)
    unreal.log("VINE gable side: ridge_along_%s cam=(%.0f, %.0f, %.0f)"
               % ("X" if ridge_x else "Y", gable_c.x, gable_c.y, gable_c.z))

    CAM_WALL = (cam_wall, look_at(cam_wall, aim_wall))
    CAM_ARCH = (cam_arch, look_at(cam_arch, aim_arch))
    CAM_NEAR = (cam_near, look_at(cam_near, aim_near))
    CAM_GABLE = (gable_c, look_at(gable_c, gable_a))

    # (名字, 拍之前要做的事, 机位)。每一对必须**同机位、只切一个开关**。
    STATE["plan"] = [
        ("wall_off", vine_off, CAM_WALL),
        ("wall_on", vine_on, CAM_WALL),
        ("gable_tier1", tier1, CAM_GABLE),
        ("gable_tier2", tier2, CAM_GABLE),
        ("flower_off", flower_off, CAM_NEAR),
        ("flower_on", flower_on, CAM_NEAR),
        ("season_summer", season_summer, CAM_NEAR),
        ("season_autumn", season_autumn, CAM_NEAR),
        ("arch_no_road", arch_none, CAM_ARCH),
        ("arch_road", paint_road, CAM_ARCH),
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
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_wall, CAM_WALL[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    # 坑 ⑦：没有 ViewState ⇒ 眼适应不跑 ⇒ 曝光一条都不生效。
    # ⚠️ 开了它就会继承坑 ⑧，必须同时有下面的 `WARMUP_CAPTURES` 预热。
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


def zero_ratio(samples):
    """精确 (0,0,0) 占比 —— 坑 ⑧ 有没有被预热掉的**指纹**。"""
    return sum(1 for p in samples if p[0] == 0 and p[1] == 0 and p[2] == 0) / float(len(samples))


FIRST_TICK = 45      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 6           # 切开关 / 画路 → 重建房子 + 组件重注册，留几帧
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

# 六条判据的阈值。
#
# 标定口径与本项目其余出图脚本同一条：**先量出真值，再取实测值的一半**
# （物理含义 = "被测对象在屏幕上的覆盖面至少还剩一半"），不是取一个能绿的小数。
# 本轮实测（w128、L_HouseGroundDemo、House_Road，2026-08-31）与故意破坏时的读数：
#
#   判据  量什么                    实测      破坏后    取值
#   A     切 bVineEnabled           27.4%     —        13.7%
#   B     藤覆盖处平均亮度           99.1      —        48（量的是"不是纯黑"，最不敏感的一条）
#   C     开拱前后                  70.9%     —        30.3%（第一档同一条量到 61.0%，取低的那次的一半）
#   D     第一档 vs 第二档           2.9%      见日志    1.4%
#   E     花开 / 关                  8.6%      见日志    4.3%
#   F     夏 / 秋                    7.0%      见日志    3.5%
#
# ⚠️ D 的信号天生就小（山墙三角 + 转角那几段只占画面一小块，第二档也只多 43 段枝）——
#    它是这六条里**最薄**的一条，别再往下调。真要更厚只能换机位，不能换阈值。
VINE_DIFF_FAIL = 0.137    # A
VINE_LUMA_FAIL = 48.0     # B
ARCH_DIFF_FAIL = 0.303    # C
TIER2_DIFF_FAIL = 0.014   # D
FLOWER_DIFF_FAIL = 0.043  # E
SEASON_DIFF_FAIL = 0.035  # F


def pair(name, a_key, b_key, threshold, why):
    """一对同机位图的差异率判据。返回是否通过。"""
    a, b = STATE["samples"].get(a_key), STATE["samples"].get(b_key)
    if not (a and b):
        unreal.log_error("VINE !! %s：少了 %s / %s 之一，判据没法算" % (name, a_key, b_key))
        return False
    changed = sum(1 for m in changed_mask(a, b) if m)
    ratio = changed / float(GRID_POINTS)
    unreal.log("VINE PIXELS %-12s %s vs %s: %d/%d changed (%.1f%%) 门 %.1f%%"
               % (name, a_key, b_key, changed, GRID_POINTS, ratio * 100.0, threshold * 100.0))
    if ratio < threshold:
        unreal.log_error("VINE !! %s —— %s" % (name, why))
        return False
    return True


def report():
    s = STATE["samples"]
    ok = True
    for name in sorted(s):
        z = zero_ratio(s[name])
        unreal.log("VINE PIXELS %-16s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("VINE !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑧）"
                             % (name, z * 100.0))
            ok = False

    # A：同机位只切 bVineEnabled，画面必须真的变 —— 这是"藤真的画出来了"的硬证据。
    ok &= pair("A 藤在不在", "wall_off", "wall_on", VINE_DIFF_FAIL,
               "切换 bVineEnabled 几乎没有改变画面 —— 藤没有被画出来")

    # B：**变化的那批像素**（= 藤覆盖的位置）在开着藤那张里的平均亮度与色相。
    # `ivy_branch` 只有 `Vertex_Position`，法线与 UV 是 `CSHouseVine::BuildBaseMesh` 现补的；
    # 补失败的症状是藤变成一条**纯黑剪影**，而剪影同样能让 A 的差异率很高 ——
    # 所以必须再量一条"它不是黑的"。
    off, on = s.get("wall_off"), s.get("wall_on")
    if off and on:
        mask = changed_mask(off, on)
        lit = [on[i] for i, m in enumerate(mask) if m]
        if lit:
            luma = sum(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in lit) / float(len(lit))
            red = sum(p[0] for p in lit) / float(len(lit))
            green = sum(p[1] for p in lit) / float(len(lit))
            blue = sum(p[2] for p in lit) / float(len(lit))
            unreal.log("VINE PIXELS vine pixels: luma=%.1f rgb=(%.1f, %.1f, %.1f)"
                       % (luma, red, green, blue))
            if luma < VINE_LUMA_FAIL:
                unreal.log_error("VINE !! 藤覆盖处平均亮度只有 %.1f —— 八成是零长法线画成了黑剪影"
                                 % luma)
                ok = False
            if green <= max(red, blue):
                unreal.log_error("VINE !! 藤覆盖处不是绿的（rgb=%.1f, %.1f, %.1f）—— 材质八成被换成了默认材质"
                                 % (red, green, blue))
                ok = False

    # D/E/F：第二档那三件事各自的执行面。
    ok &= pair("D 第二档", "gable_tier1", "gable_tier2", TIER2_DIFF_FAIL,
               "开关第二档（山墙 / 跨墙 / 花）几乎没有改变画面")
    ok &= pair("E 花", "flower_off", "flower_on", FLOWER_DIFF_FAIL,
               "开关花几乎没有改变画面 —— 花没有被画出来")
    ok &= pair("F 季节", "season_summer", "season_autumn", SEASON_DIFF_FAIL,
               "换季节几乎没有改变画面 —— `Season` 标量八成没走到材质里")
    # C：开拱之后画面必须变（洞被 clip 切出来 + 藤给洞让位 + 门框砖出现）。
    ok &= pair("C 拱", "arch_no_road", "arch_road", ARCH_DIFF_FAIL, "开拱前后画面几乎没变")

    # 花的计数：像素判据之外再钉一条 —— 记录排没排出来与画没画出来是**两件事**。
    flowers_on = STATE["counts"].get("flower_on", (0, 0, 0))[2]
    unreal.log("VINE COUNTS %s" % " ".join(
        "%s=(%d,%d,%d)" % (k, v[0], v[1], v[2]) for k, v in sorted(STATE["counts"].items())))
    if flowers_on <= 0:
        unreal.log_error("VINE !! 花的记录一朵都没排出来（概率拉满仍是 0）")
        ok = False

    # 拱吃掉了多少藤 —— 第二档要改善的正是这个数，出图时一并记账。
    base = STATE["counts"].get("arch_no_road", (0, 0, 0))[0]
    road = STATE["counts"].get("arch_road", (0, 0, 0))[0]
    if base > 0:
        unreal.log("VINE ARCHKEEP 六拱全开保留率 %.3f（%d / %d 段）" % (road / float(base), road, base))

    unreal.log("VINE PIXEL VERDICT %s%s"
               % ("OK" if ok else "FAILED", ("（故意破坏 %s，红是对的）" % BREAK) if BREAK else ""))


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
            # 坑 ⑨：实例的可见集按主视口视角压，先让主视口驾驶这台相机。
            unreal.EditorLevelLibrary.pilot_level_actor(STATE["cap"])
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
        elif phase >= SETTLE:
            # 坑 ⑧：一帧只打一次 capture_scene()，Lumen 的帧间历史才会真的往前走。
            # 挪过相机 / 切过开关之后历史等于清零，所以**每个机位都要重新预热满**。
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "vine_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("VINE shot: vine_%s_%s.png（预热 %d 帧）" % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    if STATE["house"]:
        # 关卡里的房子恢复出厂设定：本脚本一路上把六七个属性来回切过，留着会污染下一个脚本。
        STATE["house"].set_editor_property("bVineEnabled", True)
        STATE["house"].set_editor_property("bVineClimbGable", True)
        STATE["house"].set_editor_property("VineJumpChance", 0.5)
        STATE["house"].set_editor_property("VineFlowerChance", 0.10)
        STATE["house"].set_editor_property("VineFlowerMesh", STATE["flower_mesh"])
        STATE["house"].set_editor_property("VineSeason", unreal.CSVineSeason.SUMMER)
        STATE["house"].call_method("RebuildHouse")
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("VINE DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("VINE FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
