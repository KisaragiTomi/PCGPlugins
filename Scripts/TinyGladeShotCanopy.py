# -*- coding: utf-8 -*-
"""
`M_TinyGladeCanopy` 的视觉验收：搭一个最小场景（两棵树叶卡相互穿插）+ 三档对照出图 + 像素判据。

**为什么要三档而不是一张图**：材质编译通过 ≠ 机制生效。深度浮雕与 cone-cull 都只改深度/顶点，
单看一张图分不出「做了」和「没做」。所以每条机制各出一张把它关掉的对照，用像素差异率证明它在起作用：

    A full        全开（DepthBulge 60 / ConeCull 1 / NormalBulge 0.3）
    B nobulge     DepthBulge = 0        —— 与 A 的差异证明深度浮雕生效
    C nocull      ConeCullEnable = 0    —— 与 A 的差异证明 cone-cull 收缩生效

⚠️ 判据同时要防「材质被静默换成默认材质」：那种情况下三张图会**逐位相同**（差异率 0），
而所有连线断言照绿。所以「差异率 > 0」本身就是最重要的一条断言，不是锦上添花。

⚠️ **`unreal.Rotator(x, y, z)` 的参数顺序是 (Roll, Pitch, Yaw)**，不是 FRotator 成员顺序的
(Pitch, Yaw, Roll)。按后者写会得到一个翻着朝天的相机，图里全是天空而判据照样"通过"
（2026-09-09 实测）。本脚本一律用关键字参数写死。

⚠️ **必须新建空关卡**，不能在编辑器当前打开的关卡里 spawn：那张关卡自带体积云与光照，
云每帧都在动，diff_ratio 会把云的变化读成"机制生效"——第一版就是这样拿到 57% 的假阳性。
判据也因此改成只统计非天空像素。

坑清单沿用 `TinyGladeShotRockShell.py`（离屏 SceneCapture、RTF_RGBA8、捕获自带 PP 覆盖开 Lumen、
always_persist_rendering_state + 一帧一次 capture_scene 预热），不再重复解释，只在代码里标号。

⚠️ 关卡是临时的、**不存盘**：出图后 actor 全销毁。树网格来自提取资产库（473 个孤儿之一），
本脚本只在实例上覆盖材质，**不动 `central_tree_leafcards` 资产本身的材质槽**。
"""
import unreal

MI_PATH = "/PCGPlugins/HouseTest/MI_TinyGladeCanopy"
TEMP_LEVEL = "/Game/__CanopyShotTemp"
LEAFCARDS = "/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/central_tree_leafcards"
OUT_DIR = r"C:\Users\KLW\AppData\Local\Temp\claude\D--MyProject-UnrealProject-UETest574-2\663fbe13-5a7c-4be3-a860-830f2ae635f7\scratchpad\canopy_shots"

W, H = 1280, 720
GRID_X, GRID_Y = 48, 27
FIRST_TICK = 12          # 等编辑器把关卡与光照建好
SETTLE = 4               # 换参数后先空转几帧
WARMUP = 48              # Lumen 帧间历史预热（坑 ⑨：一帧只打一次）

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
MEL = unreal.MaterialEditingLibrary

# 三档：名字 -> {参数名: 值}
PLAN = [
    ("A_full", {"DepthBulge": 60.0, "ConeCullEnable": 1.0, "NormalBulge": 0.3}),
    ("B_nobulge", {"DepthBulge": 0.0, "ConeCullEnable": 1.0, "NormalBulge": 0.3}),
    ("C_nocull", {"DepthBulge": 60.0, "ConeCullEnable": 0.0, "NormalBulge": 0.3}),
]
DEFAULTS = {"DepthBulge": 60.0, "ConeCullEnable": 1.0, "NormalBulge": 0.3}

STATE = {"ticks": 0, "step": 0, "phase": 0, "spawned": [], "samples": {}, "ok": True}


def log(m):
    unreal.log("CANOPYSHOT %s" % m)


def err(m):
    STATE["ok"] = False
    unreal.log_error("CANOPYSHOT !! %s" % m)


def setup():
    # 空关卡：默认关卡自带体积云 + 光照，云一动 diff_ratio 就成了噪声计。
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(TEMP_LEVEL):
        unreal.EditorAssetLibrary.delete_asset(TEMP_LEVEL)
    if not les.new_level(TEMP_LEVEL):
        err("建不出临时关卡 %s" % TEMP_LEVEL)
        return None
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    mi = unreal.EditorAssetLibrary.load_asset(MI_PATH)
    if mi is None:
        err("材质实例缺失 %s" % MI_PATH)
        return None
    leaf = unreal.EditorAssetLibrary.load_asset(LEAFCARDS)
    if leaf is None:
        err("网格缺失 %s" % LEAFCARDS)
        return None
    STATE["mi"] = mi

    # 地面：没有它的话画面全是天空，自动曝光被天空拉爆、树读成一团黑（第一轮就栽在这）
    plane = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Plane")
    if plane is not None:
        g = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(175, 60, 0))
        g.static_mesh_component.set_editor_property("static_mesh", plane)
        g.set_actor_scale3d(unreal.Vector(80, 80, 1))
        STATE["spawned"].append(g)

    # 太阳：侧逆光（与相机 yaw 差约 120°），受光面与逆光边缘同时进画面
    # ⚠️ 光照参数保持第一轮那组（已实测能出画面）。第二轮同时改了 intensity /
    # atmosphere_sun_light / real_time_capture，结果三张图全黑 —— 一次只动一个变量。
    sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight,
                                        unreal.Vector(0, 0, 2000), unreal.Rotator(roll=0, pitch=-32, yaw=145))
    sun.light_component.set_editor_property("intensity", 8.0)
    STATE["spawned"].append(sun)
    sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 1500))
    sky.light_component.set_editor_property("intensity", 4.0)
    STATE["spawned"].append(sky)
    STATE["spawned"].append(
        ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)))

    # 两棵树，冠幅约 4.2 m、间距 3.5 m ⇒ 叶卡必然相互穿插，深度浮雕的交线才有的看
    for i, (loc, yaw) in enumerate([((0, 0, 0), 0.0), ((350, 120, 0), 137.0)]):
        a = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(*loc),
                                          unreal.Rotator(roll=0, pitch=0, yaw=yaw))
        comp = a.static_mesh_component
        comp.set_editor_property("static_mesh", leaf)
        comp.set_material(0, mi)
        STATE["spawned"].append(a)
        # ⚠️ 不要顺手把 `central_tree` 也摆上当树干：它的 slot0 是**同一棵树的低模叶卡版**
        # （488 tris = 244 卡，挂 gallery 材质 M_TG_Canopy），会变成一团灰色实体把高模叶卡
        # 整个盖住，看着像"材质没生效"。要树干得用 `*_trunk` 那一族单独的网格。

    # 坑 ⑤：捕获自己那份 PP 覆盖，否则离屏 SceneCapture 被引擎写死关掉 Lumen
    pp = unreal.PostProcessSettings()
    pp.set_editor_property("override_dynamic_global_illumination_method", True)
    pp.set_editor_property("dynamic_global_illumination_method",
                           unreal.DynamicGlobalIlluminationMethod.LUMEN)
    pp.set_editor_property("override_reflection_method", True)
    pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
    # ⚠️ 不要用 min=max 去钉死曝光：`auto_exposure_min/max_brightness` 是**亮度**不是 EV，
    # 两个都设 1.0 会把曝光压到接近 0，三张图全黑（2026-09-09 实测，离散度 2/255）。
    # 地面已经把天空占比压下去了，自动曝光在这个构图下足够稳，三档之间的残余亮度差
    # 由 diff_ratio 的 6/255 阈值吸收。

    # 坑 ④：必须显式 RTF_RGBA8，默认浮点导出的 png 是 HDR 内容
    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    # 对准两棵树重心 (175, 60, 900)，距离 22 m：视野宽约 18 m，两棵树占 65% 画面
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D,
                                        unreal.Vector(2080, -1040, 1100), unreal.Rotator(roll=0, pitch=-5, yaw=150))
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 45.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)   # 坑 ⑧
    STATE["rt"], STATE["comp"], STATE["world"] = rt, comp, world
    STATE["spawned"].append(cap)
    return world


def apply_params(params):
    mi = STATE["mi"]
    for k, v in params.items():
        MEL.set_material_instance_scalar_parameter_value(mi, k, float(v))
    MEL.update_material_instance(mi)


def sample():
    world, rt = STATE["world"], STATE["rt"]
    out = []
    for gy in range(GRID_Y):
        y = int((gy + 0.5) * H / GRID_Y)
        for gx in range(GRID_X):
            x = int((gx + 0.5) * W / GRID_X)
            c = unreal.RenderingLibrary.read_render_target_pixel(world, rt, x, y)
            out.append((c.r, c.g, c.b))
    return out


def diff_ratio(a, b, idx, thresh=6):
    """只在树覆盖的像素上比 —— 天空那部分不属于任何一条机制的作用域。"""
    if not idx:
        return 0.0, 0
    n = sum(1 for i in idx
            if abs(a[i][0] - b[i][0]) + abs(a[i][1] - b[i][1]) + abs(a[i][2] - b[i][2]) > thresh)
    return float(n) / len(idx), n


def is_sky(px):
    """天空是蓝的（b 明显大于 r）；树叶是橙/绿/暗的。空关卡里这条判别足够干净。"""
    return px[2] > px[0] + 12


def tree_mask(s):
    return [i for i, px in enumerate(s) if not is_sky(px)]


def spread(s):
    """画面的颜色离散度：材质被换成默认材质 / 全黑时这个数会塌到接近 0。"""
    rs = [p[0] for p in s]
    gs = [p[1] for p in s]
    return (max(rs) - min(rs)) + (max(gs) - min(gs))


def report():
    s = STATE["samples"]
    for name in ("A_full", "B_nobulge", "C_nocull"):
        if name not in s:
            err("缺图 %s" % name)
            return
    log("画面离散度 A=%d B=%d C=%d" % (spread(s["A_full"]), spread(s["B_nobulge"]), spread(s["C_nocull"])))
    if spread(s["A_full"]) < 40:
        err("A 几乎是纯色 —— 材质八成被静默换成了默认材质，或者相机没对着树")

    # 三档取并集：某一档把叶子收掉的地方，另一档仍算它是树的作用域
    idx = sorted(set(tree_mask(s["A_full"])) | set(tree_mask(s["B_nobulge"])) | set(tree_mask(s["C_nocull"])))
    cover = float(len(idx)) / (GRID_X * GRID_Y)
    log("树覆盖率 %.1f%%（%d/%d 采样点）" % (cover * 100, len(idx), GRID_X * GRID_Y))
    if cover < 0.12:
        err("画面里几乎没有树 —— 相机没对准（Rotator 参数顺序是 roll,pitch,yaw）")

    r_bulge, n_bulge = diff_ratio(s["A_full"], s["B_nobulge"], idx)
    r_cull, n_cull = diff_ratio(s["A_full"], s["C_nocull"], idx)
    log("深度浮雕 A-vs-B：%d/%d 树像素变化（%.2f%%）" % (n_bulge, len(idx), r_bulge * 100))
    log("cone-cull A-vs-C：%d/%d 树像素变化（%.2f%%）" % (n_cull, len(idx), r_cull * 100))

    # 阈值取得很低：这里要证的是「机制确实接进了管线」，不是「效果有多强」。
    # 两条都只改深度/顶点位置，在这个机位下本来就只影响叶簇边缘与背向卡片。
    if r_bulge < 0.005:
        err("关掉 DepthBulge 画面几乎没变 —— PDO 没接进管线")
    if r_cull < 0.005:
        err("关掉 ConeCullEnable 画面几乎没变 —— WPO 收缩没接进管线")
    log("VERDICT %s" % ("OK" if STATE["ok"] else "FAILED"))


def tick(_delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(PLAN):
        name, params = PLAN[step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            apply_params(params)
        elif phase >= SETTLE:
            STATE["comp"].capture_scene()          # 坑 ⑨：一帧一次
            if phase >= SETTLE + WARMUP - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "canopy_%s.png" % name)
                STATE["samples"][name] = sample()
                log("出图 canopy_%s.png（预热 %d 帧）" % (name, WARMUP))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    apply_params(DEFAULTS)                          # 把 MIC 恢复到默认档
    unreal.EditorAssetLibrary.save_asset(MI_PATH)
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    if unreal.EditorAssetLibrary.does_asset_exist(TEMP_LEVEL):
        unreal.EditorAssetLibrary.delete_asset(TEMP_LEVEL)
    log("DONE")
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    if setup() is None:
        log("准备失败，直接退出")
        unreal.SystemLibrary.quit_editor()
    else:
        STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
except Exception as exc:
    unreal.log_error("CANOPYSHOT !! 准备段抛异常：%s" % exc)
    unreal.SystemLibrary.quit_editor()
