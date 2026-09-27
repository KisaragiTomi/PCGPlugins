# -*- coding: utf-8 -*-
"""Nanite 截面 HLOD 的多角度对照出图：原始模型群 vs BP_NaniteCutHLOD 合并出的截面网格。

先跑 NaniteCutHLODSetup.py 建好测试关卡。一个编辑器会话里每帧推进一步：

  1. 打开 L_NaniteCutHLODTest，先清掉旧版本存进关卡的临时捕获（NCH_TempCapture_*），按模型群包围盒算好各机位。
  2. 源物体可见，线框机位每帧捕获一次（顺带推 Nanite 流送），对每个源网格 RequestNaniteResidency。
  3. BuildHLOD；有源不完整就再等一轮流送重建，最多 BUILD_ATTEMPTS 轮。之后同一份截面剔除关 / 开各建一次，
     数"后屋内部""隧道中段"这些外面绝对看不见的盒子里还剩几个 HLOD 三角（开着剔除应当是 0）。
  4. 线框机位先出两张 X 光：同一份截面剔除关（wire_nocull）/ 剔除开（wire_cull）。只拿 HLOD 比 ——
     Nanite 的线框会写深度、大平面填黑，不透视，跟 GPU 网格的线框不可比。拍完线框捕获就销毁。
  5. BakeHLOD：重新分 UV、用源材质烘 2048 的 BaseColor / Normal / Roughness，存成开 Nanite 的静态网格
     挂到 HLOD actor 下面；烘焙资产（AutoResult/）与关卡一起存盘（临时关掉的云先还原）。存盘时场景里
     没有任何临时捕获（坑 ⑩）。
  6. 实景机位每 LIT_BATCH 个一批：建捕获 → 依次切换、各自预热后导出 orig（ShowSources）→ baked（ShowHLOD，
     显示烘焙 SM）→ orig2 → baked2 → empty（源与 HLOD 都藏，给合成脚本算覆盖掩码）→ 销毁。orig2 / baked2
     是切走再切回来重拍的同一组，它们与第一次的差异就是各自那条渲染路径的噪声底（TSR 抖动、曝光重新收敛）。

产物：Saved/NaniteCutHLODShots/<机位>_<组>.png 与 shots.json（机位参数 + 构建结果）。
合成对照图：系统 Python 跑 NaniteCutHLODCompose.py。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecCmds="py <本文件>" -unattended -nosplash -stdout -AbsLog=<log>

日志里 `NCH SHOTS` 开头的是进度，最后一行 `NCH SHOTS DONE` / `NCH SHOTS FAILED`。

显存：常驻渲染状态的 1080p 捕获在这个场景里每个约 210 MB（TSR / Lumen / VSM / 体积云的逐视图历史、光追
场景缓冲），同时挂 11 个就是 2.3 GB。所以线框拍完即销毁、实景分批，同一时刻最多 LIT_BATCH 个。

对照要公平，所以捕获统一关掉 Lumen 与距离场 AO：HLOD 显示组件是 GPU 常驻网格，没有 Lumen
卡片也没有距离场，开着它们两组画面的间接光本来就不同，会被误读成几何差异。
SceneCapture 的坑沿用 TinyGladeShotRockShell.py 的坑表（①④⑥⑧⑨），代码里标号。
"""
import json
import math
import os
import time
import traceback

import unreal

LEVEL = "/PCGPlugins/NaniteCutHLOD/L_NaniteCutHLODTest"
HLOD_LABEL = "NCH_HLOD"
SOURCE_PREFIX = "NCH_Obj_"
OUT_DIR = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "NaniteCutHLODShots")

W, H = 1920, 1080          # 与 HLOD 的参考视图同宽：FOV 90 时 CutError 在切换距离上正好 1 像素
FIRST_TICKS = 20           # 等关卡与光照建好
STREAM_FRAMES = 60         # 每轮构建前推流送的帧数
BUILD_ATTEMPTS = 8
WARMUP_FRAMES = 72         # 坑 ⑨：一帧只 capture 一次
HLOD_MIN_SECONDS = 90.0    # 烘焙 SM 第一次上屏：扁平化材质的新排列要现编 shader，Nanite 构建也可能是异步的
ORIG_MIN_SECONDS = 15.0
LIT_BATCH = 4              # 实景捕获同时最多挂几个（每个约 210 MB 显存）
MAX_TICKS = 9000           # 兜底：状态机卡住也要退出
TEMP_CAPTURE_PREFIX = "NCH_TempCapture_"

# 机位：对准的目标、到目标的距离 (cm)、仰角°、方位角°（从目标指向相机）、水平 FOV°、是否另出线框。
# 线框是透视的（被挡住的边也画），近景那几个的线框就是"X 光"：原始里看得见被包住的物体，HLOD 里应当没有。
ANGLES = [
    dict(name="switch_front", target="group", distance=13500.0, elevation=12.0, azimuth=205.0, fov=90.0),
    dict(name="switch_high", target="group", distance=13500.0, elevation=40.0, azimuth=125.0, fov=90.0),
    dict(name="far_side", target="group", distance=18000.0, elevation=8.0, azimuth=30.0, fov=90.0),
    dict(name="mid", target="group", distance=8000.0, elevation=25.0, azimuth=300.0, fov=90.0),
    dict(name="house", target="house", distance=2600.0, elevation=28.0, azimuth=225.0, fov=60.0, wire=True),
    dict(name="cave", target="cave", distance=4300.0, elevation=55.0, azimuth=250.0, fov=60.0, wire=True),
    dict(name="cave_mouth", target="cave_mouth_w", distance=1600.0, elevation=6.0, azimuth=244.5, fov=60.0),
    dict(name="pile", target="pile", distance=1700.0, elevation=30.0, azimuth=160.0, fov=60.0, wire=True),
]

# 与 NaniteCutHLODSetup.py 一致：房子与岩块的局部尺寸（原点都在底面中心）。
HOUSE_BACK_ROOM = ((170.0, -380.0, 20.0), (580.0, 380.0, 430.0))    # 后屋内部，含内壁（向外放 5 cm）
HOUSE_FRONT_ROOM = ((-580.0, -380.0, 20.0), (130.0, 380.0, 430.0))
# 隧道中段。洞口是斜着穿出端面的，开口在 y 向被拉长到约 ±4.6 m，贴着第一道弯内侧的直线视线能擦到 x≈±600，
# 所以盒子只取 ±480：这里离两个洞口都隔着两道弯，直线视线过不来。
CAVE_MIDDLE = ((-480.0, -750.0, 80.0), (480.0, 750.0, 520.0))
# 逐物体计数（actor 包围盒里的 HLOD 三角，含它脚下的洞壁 / 地板）：外面绝对看不见的应当剔成 0。
COUNT_ITEMS = ("CaveDeep", "HouseBack", "CaveMouth", "CaveBend", "HouseFront")
CAVE_MOUTH_W = (-1500.0, 0.0, 280.0)                                  # 西洞口（隧道中心高度）

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
STATE = {"handle": None, "gen": None, "ticks": 0, "spawned": [], "done": False}


def log(msg):
    unreal.log("NCH SHOTS " + msg)


def look_at(frm, to):
    """坑 ①：unreal.Rotator 的位置参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    return unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def box_distance(p, lo, hi):
    d = [max(lo[i] - p[i], 0.0, p[i] - hi[i]) for i in range(3)]
    return math.sqrt(sum(x * x for x in d))


def make_capture(world, name, pos, rot, fov, wire):
    # 坑 ④：必须显式 RTF_RGBA8，默认浮点导出的 png 是 HDR 内容
    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, pos, rot)
    cap.set_actor_label(TEMP_CAPTURE_PREFIX + name)
    STATE["spawned"].append(cap)
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", fov)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("always_persist_rendering_state", True)   # 坑 ⑧：没有 ViewState 就没有眼适应与 TSR 历史
    pp = unreal.PostProcessSettings()
    pp.set_editor_property("override_dynamic_global_illumination_method", True)
    pp.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.NONE)
    pp.set_editor_property("override_reflection_method", True)
    pp.set_editor_property("reflection_method", unreal.ReflectionMethod.SCREEN_SPACE)
    comp.set_editor_property("post_process_settings", pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    if wire:
        flag = unreal.EngineShowFlagsSetting()
        flag.set_editor_property("show_flag_name", "Wireframe")
        flag.set_editor_property("enabled", True)
        comp.set_editor_property("show_flag_settings", [flag])
    return {"name": name, "comp": comp, "rt": rt, "wire": wire, "actor": cap}


def make_captures(world, defs):
    return [make_capture(world, d["name"], d["pos"], d["rot"], d["fov"], d["wire"]) for d in defs]


def destroy_captures(caps):
    """销毁捕获并放掉引用：注销时释放逐视图历史（显存），GC 再回收渲染目标。"""
    for c in caps:
        a = c["actor"]
        if a in STATE["spawned"]:
            STATE["spawned"].remove(a)
        ACTORS.destroy_actor(a)
    caps.clear()
    unreal.SystemLibrary.collect_garbage()


def capture(caps):
    for c in caps:
        c["comp"].capture_scene()


def warm(caps, frames, seconds=0.0):
    """预热：每帧每个捕获一次，至少 frames 帧且至少 seconds 秒。"""
    t0 = time.time()
    n = 0
    while n < frames or time.time() - t0 < seconds:
        capture(caps)
        n += 1
        yield
    log("  warmed %d frame(s) in %.1f s" % (n, time.time() - t0))


def export(world, caps, variant):
    for c in caps:
        unreal.RenderingLibrary.export_render_target(world, c["rt"], OUT_DIR, "%s_%s.png" % (c["name"], variant))
    log("  exported %d x %s" % (len(caps), variant))


def summarize(result):
    counts = {}
    tris = clusters = forced = 0
    for s in result.get_editor_property("sources"):
        key = str(s.get_editor_property("status")).split(".")[-1].split(":")[0].strip("<> ")
        counts[key] = counts.get(key, 0) + 1
        tris += s.get_editor_property("triangles")
        clusters += s.get_editor_property("clusters")
        forced += s.get_editor_property("forced_clusters")
    return counts, tris, clusters, forced


def full_res_triangles(mesh):
    try:
        desc = mesh.get_static_mesh_description(0)
        if desc is not None:
            return int(desc.get_triangle_count())
    except Exception:
        pass
    return int(mesh.get_num_triangles(0))


def union_bounds(actor_list):
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for a in actor_list:
        origin, extent = a.get_actor_bounds(False)
        for i, (o, e) in enumerate(((origin.x, extent.x), (origin.y, extent.y), (origin.z, extent.z))):
            lo[i] = min(lo[i], o - e)
            hi[i] = max(hi[i], o + e)
    return lo, hi


def local_box(actor, box):
    """actor 局部（只平移，测试件都没转）的盒子 → 世界盒子。"""
    p = actor.get_actor_location()
    (x0, y0, z0), (x1, y1, z1) = box
    b = unreal.Box()
    b.min = unreal.Vector(p.x + x0, p.y + y0, p.z + z0)
    b.max = unreal.Vector(p.x + x1, p.y + y1, p.z + z1)
    try:
        b.is_valid = True
    except Exception:
        pass
    return b


def cull_summary(hlod):
    r = hlod.get_editor_property("last_cull_result")
    return {k: r.get_editor_property(k) for k in ("triangles_before", "triangles_visible", "triangles_after", "num_views", "applied")}


def run():
    for name in os.listdir(OUT_DIR) if os.path.isdir(OUT_DIR) else []:
        if name.endswith(".png") or name.endswith(".json"):
            os.remove(os.path.join(OUT_DIR, name))
    os.makedirs(OUT_DIR, exist_ok=True)

    world = editor_world()
    if LEVEL not in world.get_path_name():
        LEVELS.load_level(LEVEL)
        for _ in range(5):
            yield
        world = editor_world()
    if LEVEL not in world.get_path_name():
        raise RuntimeError("关卡没打开：当前是 " + world.get_path_name())
    log("level %s" % world.get_path_name())

    # 坑 ⑩：旧版本在临时捕获还挂着时存了关卡，它们（连同 1080p 渲染目标）被存进了 .umap。先清掉，烘焙后随关卡存盘。
    actors = ACTORS.get_all_level_actors()
    stale = [a for a in actors if a.get_actor_label().startswith(TEMP_CAPTURE_PREFIX)]
    for a in stale:
        ACTORS.destroy_actor(a)
    if stale:
        log("removed %d stale temp capture actor(s) saved by an older run" % len(stale))
        actors = ACTORS.get_all_level_actors()
    hlod = next((a for a in actors if a.get_actor_label() == HLOD_LABEL), None)
    if hlod is None:
        raise RuntimeError("找不到 " + HLOD_LABEL + "（先跑 NaniteCutHLODSetup.py）")
    objects = [a for a in actors if a.get_actor_label().startswith(SOURCE_PREFIX)]
    excluded = [a for a in objects if a.get_actor_label().endswith("_Excluded")]
    sources = [a for a in objects if a not in excluded]

    # 云每帧都在动，两组画面前后拍，差分会把云读成差异。关掉拍，存盘前还原。
    clouds = [a.get_component_by_class(unreal.VolumetricCloudComponent) for a in actors if isinstance(a, unreal.VolumetricCloud)]
    for c in clouds:
        c.set_visibility(False)
    unreal.SystemLibrary.execute_console_command(world, "r.DistanceFieldAO 0")

    lo, hi = union_bounds(objects)
    center = unreal.Vector((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, lo[2] + (hi[2] - lo[2]) * 0.35)
    by_label = {a.get_actor_label(): a for a in objects}
    house = by_label.get(SOURCE_PREFIX + "House")
    cave = by_label.get(SOURCE_PREFIX + "Cave")
    pile = [a for a in objects if a.get_actor_label().startswith(SOURCE_PREFIX + "Pile_")]
    targets = {"group": center}
    for key, actor_list in (("house", [house] if house else []), ("cave", [cave] if cave else []), ("pile", pile)):
        if actor_list:
            tlo, thi = union_bounds(actor_list)
            targets[key] = unreal.Vector((tlo[0] + thi[0]) * 0.5, (tlo[1] + thi[1]) * 0.5, (tlo[2] + thi[2]) * 0.5)
    if cave:
        p = cave.get_actor_location()
        targets["cave_mouth_w"] = unreal.Vector(p.x + CAVE_MOUTH_W[0], p.y + CAVE_MOUTH_W[1], p.z + CAVE_MOUTH_W[2])
    regions = []
    if house:
        regions += [("house_back_room", local_box(house, HOUSE_BACK_ROOM)), ("house_front_room", local_box(house, HOUSE_FRONT_ROOM))]
    if cave:
        regions.append(("cave_middle", local_box(cave, CAVE_MIDDLE)))
    for a in objects:
        short = a.get_actor_label()[len(SOURCE_PREFIX):]
        if short.startswith(COUNT_ITEMS):
            ilo, ihi = union_bounds([a])
            b = unreal.Box()
            b.min = unreal.Vector(ilo[0] - 2.0, ilo[1] - 2.0, ilo[2] - 2.0)
            b.max = unreal.Vector(ihi[0] + 2.0, ihi[1] + 2.0, ihi[2] + 2.0)
            try:
                b.is_valid = True
            except Exception:
                pass
            regions.append(("item:" + short, b))
    target_bounds = {"group": (lo, hi)}
    for key, actor_list in (("house", [house] if house else []), ("cave", [cave] if cave else []), ("pile", pile),
                            ("cave_mouth_w", [cave] if cave else [])):
        if actor_list:
            target_bounds[key] = union_bounds(actor_list)
    log("group: %d objects (%d sources, %d excluded), bounds %s .. %s" % (
        len(objects), len(sources), len(excluded), [round(v) for v in lo], [round(v) for v in hi]))

    switch_distance = hlod.get_editor_property("switch_distance")
    pixel_error = hlod.get_editor_property("pixel_error")
    world_cut_error = unreal.CSNaniteCutOps.cut_error_for_screen_error(switch_distance, pixel_error, 1920.0, 90.0)

    lit_defs, wire_defs, meta = [], [], []
    for angle in ANGLES:
        name, dist, elev, azim, fov = angle["name"], angle["distance"], angle["elevation"], angle["azimuth"], angle["fov"]
        want_wire = angle.get("wire", False)
        target = targets.get(angle["target"])
        if target is None:
            log("camera %s: target %s missing, skipped" % (name, angle["target"]))
            continue
        e, a = math.radians(elev), math.radians(azim)
        pos = unreal.Vector(target.x + dist * math.cos(e) * math.cos(a), target.y + dist * math.cos(e) * math.sin(a),
                            target.z + dist * math.sin(e))
        rot = look_at(pos, target)
        lit_defs.append({"name": name, "pos": pos, "rot": rot, "fov": fov, "wire": False})
        if want_wire:
            wire_defs.append({"name": name + "_wire", "pos": pos, "rot": rot, "fov": fov, "wire": True})
        tlo, thi = target_bounds.get(angle["target"], (lo, hi))
        nearest = box_distance((pos.x, pos.y, pos.z), tlo, thi)
        lod_scale = W / (2.0 * math.tan(math.radians(fov) * 0.5))
        meta.append({"name": name, "target": angle["target"], "distance": dist, "elevation": elev, "azimuth": azim, "fov": fov,
                     "wire": want_wire, "nearest_cm": round(nearest, 1),
                     "cut_error_px_at_nearest": round(world_cut_error * lod_scale / max(nearest, 1.0), 2),
                     "cut_error_px_at_center": round(world_cut_error * lod_scale / dist, 2)})
        log("camera %s: dist %.0f, nearest object %.0f cm, cut error %.2f px at nearest" % (
            name, dist, nearest, meta[-1]["cut_error_px_at_nearest"]))

    # 流送与 X 光阶段只挂线框捕获（没有线框机位就借第一批实景机位推流送）。
    wire = make_captures(world, wire_defs)
    pump = wire if wire else make_captures(world, lit_defs[:LIT_BATCH])

    meshes = {}
    for a in sources:
        sm = a.static_mesh_component.static_mesh
        meshes[sm.get_path_name()] = sm
    mesh_tris = {path: full_res_triangles(sm) for path, sm in meshes.items()}
    full_res = sum(mesh_tris[a.static_mesh_component.static_mesh.get_path_name()] for a in sources)
    log("source meshes (full-res triangles): %s" % json.dumps({p.rsplit(".", 1)[-1]: n for p, n in mesh_tris.items()}))

    for _ in range(FIRST_TICKS):
        yield

    # ---- 流送 + 构建
    hlod.show_sources()
    build = None
    t_start = time.time()
    for attempt in range(1, BUILD_ATTEMPTS + 1):
        for sm in meshes.values():
            unreal.CSNaniteCutOps.request_nanite_residency(sm)
        for _ in warm(pump, STREAM_FRAMES):
            yield
        t0 = time.time()
        hlod.build_hlod()
        dt = time.time() - t0
        result = hlod.get_editor_property("last_result")
        counts, tris, clusters, forced = summarize(result)
        build = {"attempt": attempt, "seconds": round(dt, 3), "complete": bool(result.get_editor_property("all_complete")),
                 "status_counts": counts, "triangles": result.get_editor_property("written_triangles"),
                 "vertices": result.get_editor_property("written_vertices"), "clusters": clusters,
                 "forced_clusters": forced, "sources": hlod.get_editor_property("last_num_sources"),
                 "excluded": hlod.get_editor_property("last_num_excluded"),
                 "world_cut_error": hlod.get_editor_property("last_world_cut_error")}
        log("build %d: %s" % (attempt, json.dumps(build)))
        # 非 Nanite 的源（NOT_NANITE）永远不会"完成"，等流送只等 INCOMPLETE / NOT_READY。
        if not any(k in counts for k in ("INCOMPLETE", "NOT_READY")):
            break
    if build is None or build["triangles"] <= 0:
        raise RuntimeError("HLOD 没有产出三角形")
    log("build took %.1f s incl. streaming" % (time.time() - t_start))

    # ---- 同一份截面：剔除关 / 开各建一次，数看不见的区域里还剩几个三角，线框机位各拍一张 X 光
    boxes = [b for _, b in regions]
    hlod.set_editor_property("cull_hidden", False)
    hlod.build_hlod()
    counts_off = list(hlod.count_hlod_triangles_in_boxes(boxes)) if boxes else []
    if wire:
        for _ in warm(wire, WARMUP_FRAMES, 10.0):
            yield
        export(world, wire, "nocull")
    hlod.set_editor_property("cull_hidden", True)
    t0 = time.time()
    hlod.build_hlod()
    build["cull_seconds_incl_build"] = round(time.time() - t0, 3)
    counts_on = list(hlod.count_hlod_triangles_in_boxes(boxes)) if boxes else []
    if wire:
        for _ in warm(wire, WARMUP_FRAMES):
            yield
        export(world, wire, "cull")
    build["cull"] = cull_summary(hlod)
    build["regions"] = {name: {"cull_off": off, "cull_on": on} for (name, _), off, on in zip(regions, counts_off, counts_on)}
    log("cull: %s" % json.dumps(build["cull"]))
    log("regions (HLOD triangles whose centroid is inside): %s" % json.dumps(build["regions"]))
    if pump is not wire:
        destroy_captures(pump)
    destroy_captures(wire)

    # ---- 烘焙：静态网格 + 三张贴图挂到 HLOD actor 下面，资产与关卡存盘
    # 坑 ⑩：存盘时场景里不能有临时捕获，否则连同渲染目标一起进 .umap。
    if STATE["spawned"]:
        raise RuntimeError("存盘前还有 %d 个临时捕获没销毁" % len(STATE["spawned"]))
    t0 = time.time()
    hlod.bake_hlod()
    baked_mesh = hlod.get_editor_property("last_baked_mesh")
    baked_actor = hlod.get_editor_property("baked_actor")
    if baked_mesh is None or baked_actor is None:
        raise RuntimeError("BakeHLOD 没有产出")
    build["bake"] = {"mesh": baked_mesh.get_path_name(), "triangles": hlod.get_editor_property("last_bake_triangles"),
                     "seconds": round(hlod.get_editor_property("last_bake_seconds"), 2), "wall_seconds": round(time.time() - t0, 2),
                     "nanite": bool(unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem).get_nanite_settings(baked_mesh)
                                    .get_editor_property("enabled")),
                     "materials": [m.get_path_name() for m in [baked_mesh.get_material(0)] if m]}
    log("bake: %s" % json.dumps(build["bake"]))
    for c in clouds:
        c.set_visibility(True)
    bake_folder = baked_mesh.get_path_name().rsplit("/", 1)[0]
    unreal.EditorAssetLibrary.save_directory(bake_folder, only_if_is_dirty=False, recursive=True)
    LEVELS.save_current_level()
    for c in clouds:
        c.set_visibility(False)
    log("saved %s and the level" % bake_folder)

    # ---- 各组画面：实景机位分批，同一时刻最多挂 LIT_BATCH 个捕获
    first_baked = True
    for b in range(0, len(lit_defs), LIT_BATCH):
        lit = make_captures(world, lit_defs[b:b + LIT_BATCH])
        log("lit batch %s" % [c["name"] for c in lit])

        hlod.show_sources()
        for _ in warm(lit, WARMUP_FRAMES, ORIG_MIN_SECONDS):
            yield
        export(world, lit, "orig")

        hlod.show_hlod()
        for _ in warm(lit, WARMUP_FRAMES, HLOD_MIN_SECONDS if first_baked else ORIG_MIN_SECONDS):
            yield
        export(world, lit, "baked")
        first_baked = False

        hlod.show_sources()
        for _ in warm(lit, WARMUP_FRAMES):
            yield
        export(world, lit, "orig2")

        hlod.show_hlod()
        for _ in warm(lit, WARMUP_FRAMES):
            yield
        export(world, lit, "baked2")

        baked_actor.set_is_temporarily_hidden_in_editor(True)
        for _ in warm(lit, WARMUP_FRAMES):
            yield
        export(world, lit, "empty")
        hlod.show_sources()
        destroy_captures(lit)
    for c in clouds:
        c.set_visibility(True)

    summary = {"level": LEVEL, "width": W, "height": H, "switch_distance": switch_distance, "pixel_error": pixel_error,
               "full_res_source_triangles": full_res, "num_source_actors": len(sources), "num_excluded_objects": len(excluded),
               "group_bounds": [lo, hi], "group_center": [center.x, center.y, center.z], "build": build, "angles": meta}
    with open(os.path.join(OUT_DIR, "shots.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)
    final = build["cull"]["triangles_after"] if build["cull"]["applied"] else build["triangles"]
    log("full-res source triangles %d -> cut %d -> after hidden-triangle cull %d (%.1f%%)" % (
        full_res, build["triangles"], final, 100.0 * final / max(full_res, 1)))


def finish(ok):
    if STATE["done"]:
        return
    STATE["done"] = True
    if STATE["handle"] is not None:
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
    for a in STATE["spawned"]:
        try:
            ACTORS.destroy_actor(a)
        except Exception:
            pass
    if ok:
        log("DONE -> %s" % OUT_DIR)
    else:
        unreal.log_error("NCH SHOTS FAILED")
    unreal.SystemLibrary.quit_editor()


def tick(_delta):
    # 重入保护：BakeHLOD 里材质烘焙的进度框会泵 Slate tick，回调在生成器还没返回时又被调进来。
    if STATE["done"] or STATE.get("busy"):
        return
    STATE["ticks"] += 1
    STATE["busy"] = True
    try:
        next(STATE["gen"])
        if STATE["ticks"] > MAX_TICKS:
            unreal.log_error("NCH SHOTS timeout after %d ticks" % STATE["ticks"])
            finish(False)
    except StopIteration:
        finish(True)
    except Exception:
        unreal.log_error("NCH SHOTS exception\n" + traceback.format_exc())
        finish(False)
    finally:
        STATE["busy"] = False


# 坑 ⑥：准备段抛异常的话编辑器永远不退出。
try:
    STATE["gen"] = run()
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
except Exception:
    unreal.log_error("NCH SHOTS exception\n" + traceback.format_exc())
    finish(False)
