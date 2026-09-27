# -*- coding: utf-8 -*-
"""Nanite 截面 HLOD 测试关卡的内存归因：在几个节点各打一份 memreport -full。

    0 baseline      关卡加载完、什么都没做
    1 captures      建出截图脚本同时最多挂的 4 个常驻渲染状态的 1080p SceneCapture，各预热 60 帧
    1b cap_released 销毁它们、GC 后
    2 hlod          BuildHLOD（GPU 截面 + 可见性剔除）
    3 bake          BakeHLOD（重分 UV + 材质烘焙 + 贴图 + Nanite SM；不存盘）
    4 released      ClearHLOD、GC 后

memreport 是延迟命令（下一帧才执行、写文件），所以每个节点发出命令后先等几帧再往下走，
否则报告里会混进下一步的状态。

用法::

    UnrealEditor-Cmd.exe <uproject> /PCGPlugins/NaniteCutHLOD/L_NaniteCutHLODTest -ExecCmds="py <本文件>" -unattended -nosplash -stdout -AbsLog=<log>

报告在 Saved/Profiling/MemReports/ 下；日志里 `NCH MEM <节点> -> <文件>` 标出每个节点对应哪份报告。
"""
import math
import os
import time
import traceback

import unreal

LEVEL = "/PCGPlugins/NaniteCutHLOD/L_NaniteCutHLODTest"
W, H = 1920, 1080
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"handle": None, "gen": None, "done": False, "busy": False, "spawned": []}
REPORT_DIR = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "Profiling", "MemReports")


def log(msg):
    unreal.log("NCH MEM " + msg)


def world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def newest_report():
    newest, stamp = None, 0.0
    for root, _dirs, files in os.walk(REPORT_DIR):
        for name in files:
            if name.endswith(".memreport"):
                path = os.path.join(root, name)
                if os.path.getmtime(path) > stamp:
                    newest, stamp = path, os.path.getmtime(path)
    return newest


def checkpoint(name):
    """生成器：发 memreport，等它（延迟命令）真的写完文件再返回。"""
    before = newest_report()
    unreal.SystemLibrary.execute_console_command(world(), "memreport -full")
    after = before
    for _ in range(600):
        yield
        after = newest_report()
        if after != before:
            break
    for _ in range(3):
        yield
    log("%s -> %s" % (name, after if after != before else "(no new report)"))


def make_capture(pos, target, fov, wire):
    dx, dy, dz = target.x - pos.x, target.y - pos.y, target.z - pos.z
    rot = unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))), yaw=math.degrees(math.atan2(dy, dx)))
    rt = unreal.RenderingLibrary.create_render_target2d(world(), W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, pos, rot)
    STATE["spawned"].append(cap)
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", fov)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("always_persist_rendering_state", True)
    if wire:
        flag = unreal.EngineShowFlagsSetting()
        flag.set_editor_property("show_flag_name", "Wireframe")
        flag.set_editor_property("enabled", True)
        comp.set_editor_property("show_flag_settings", [flag])
    return comp


def frames(n):
    for _ in range(n):
        yield


def run():
    w = world()
    if LEVEL not in w.get_path_name():
        raise RuntimeError("关卡没打开：" + w.get_path_name())
    actors = ACTORS.get_all_level_actors()
    # 旧版截图脚本存进关卡的临时捕获：只在本会话里删掉（不存盘），基线才干净。
    stale = [a for a in actors if a.get_actor_label().startswith("NCH_TempCapture_")]
    for a in stale:
        ACTORS.destroy_actor(a)
    if stale:
        log("removed %d stale temp capture actor(s) for this session" % len(stale))
        unreal.SystemLibrary.collect_garbage()
    hlod = next(a for a in actors if a.get_actor_label() == "NCH_HLOD")
    center = hlod.get_actor_location()

    for _ in frames(60):
        yield
    yield from checkpoint("0_baseline")

    comps = []
    for i in range(4):
        a = math.radians(i * 33.0)
        dist = 9000.0 if i < 2 else 3000.0
        pos = unreal.Vector(center.x + dist * math.cos(a), center.y + dist * math.sin(a), center.z + dist * 0.3)
        comps.append(make_capture(pos, center, 90.0 if i < 2 else 60.0, False))
    for _ in range(60):
        for c in comps:
            c.capture_scene()
        yield
    yield from checkpoint("1_captures")

    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    STATE["spawned"] = []
    comps = []
    unreal.SystemLibrary.collect_garbage()
    for _ in frames(30):
        yield
    yield from checkpoint("1b_cap_released")

    hlod.build_hlod()
    for _ in frames(10):
        yield
    yield from checkpoint("2_hlod")

    t0 = time.time()
    hlod.bake_hlod()
    log("bake took %.1f s" % (time.time() - t0))
    for _ in frames(10):
        yield
    yield from checkpoint("3_bake")

    hlod.clear_hlod()
    unreal.SystemLibrary.collect_garbage()
    for _ in frames(60):
        yield
    unreal.SystemLibrary.collect_garbage()
    for _ in frames(10):
        yield
    yield from checkpoint("4_released")


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
    log("DONE" if ok else "FAILED")
    unreal.SystemLibrary.quit_editor()


def tick(_delta):
    # 重入保护：BakeHLOD 里材质烘焙的进度框会泵 Slate tick。
    if STATE["done"] or STATE["busy"]:
        return
    STATE["busy"] = True
    try:
        next(STATE["gen"])
    except StopIteration:
        finish(True)
    except Exception:
        unreal.log_error("NCH MEM exception\n" + traceback.format_exc())
        finish(False)
    finally:
        STATE["busy"] = False


try:
    STATE["gen"] = run()
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
except Exception:
    unreal.log_error("NCH MEM exception\n" + traceback.format_exc())
    finish(False)
