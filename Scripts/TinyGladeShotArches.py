# -*- coding: utf-8 -*-
"""
拍连拱的验证图 —— clip 路线唯一非看不可的一步。

单测能验 CPU 侧的裁剪场，回归能验门的开关判定，但"材质到底有没有把洞 discard 出来"
只有像素能回答。

**不要走编辑器视口 + HighResShot**：视口默认不是实时渲染的，而且这个工程起来是 4 分屏
布局，`HighResShot` 抓的是当前激活的那一格（可能是张空的正交面板）—— 症状是日志里只有
`Cmd: HighResShot`、Saved/Screenshots 下要么没有文件、要么一张全黑。这里改用离屏
SceneCapture2D：不依赖任何视口状态，同步出图，`UnrealEditor-Cmd`（真 RHI）下就能跑。

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=".../TinyGladeShotArches.py" \\
      -unattended -nopause -nosplash

产物：Saved/TinyGladeShots/*.png
"""
import unreal

# ⚠️ unreal.Rotator 的构造参数是 (roll, pitch, yaw)，**不是** (pitch, yaw, roll)。
#    写错的症状极具迷惑性：Rotator(-6, 90, 0) 会把 90 当成 pitch，相机直接朝正上方看天，
#    出图是一张纯渐变，看起来像"渲染没出来"。这里一律用关键字参数，别再靠位置传。

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.EditorLevelLibrary.get_editor_world()


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


ground = find("Ground_Demo")
house = find("House_Road")
if not (ground and house):
    unreal.log_error("SHOT FAILED: demo actors missing")
    raise SystemExit

# ---- 把路画上，否则没有门可看（回归脚本收尾时会 ResetPaint）----
loc = house.get_actor_location()
ground.reset_paint()
ground.begin_paint_stroke()
for i in range(17):
    y = loc.y - 800.0 + i * 100.0
    z = ground.sample_height(unreal.Vector2D(loc.x, y))
    ground.apply_paint_stroke(unreal.Vector(loc.x, y, z))
ground.end_paint_stroke()
house.call_method("RebuildHouse")
unreal.log("doors open: %d" % house.get_open_door_count())

# ---- 这张演示关卡是脚本建的，一盏灯都没有：不补光的话打光通路只会出全黑 ----
spawned = []
sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000), unreal.Rotator(roll=0.0, pitch=-38.0, yaw=150.0))
sun.light_component.set_editor_property("intensity", 6.0)
spawned.append(sun)
spawned.append(ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)))
sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(loc.x, loc.y, 1500))
sky.light_component.set_editor_property("intensity", 1.0)
sky.light_component.set_editor_property("real_time_capture", True)
spawned.append(sky)


SHOTS = [
    # 南墙外朝 +Y 看：南北两面长墙的连拱正对镜头。
    ("arches_lit", unreal.Vector(loc.x, loc.y - 950.0, 250.0), unreal.Rotator(roll=0.0, pitch=-6.0, yaw=90.0),
     unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR),
    # 同机位的 base color：不受光照影响，洞的剪影最干净。
    ("arches_basecolor", unreal.Vector(loc.x, loc.y - 950.0, 250.0), unreal.Rotator(roll=0.0, pitch=-6.0, yaw=90.0),
     unreal.SceneCaptureSource.SCS_BASE_COLOR),
    # 贴近一个拱：看洞缘是不是解析光滑的，以及洞口内壁有没有把断口遮住。
    # 判别用：正上方俯视整块地面。地面是 32 m 见方，这一张要是也空的，就不是机位问题。
    ("overhead", unreal.Vector(1600.0, 1600.0, 3000.0), unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0),
     unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR),
    ("arch_closeup", unreal.Vector(loc.x - 120.0, loc.y - 330.0, 130.0), unreal.Rotator(roll=0.0, pitch=-4.0, yaw=78.0),
     unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR),
]

# 捕获组件预先建好并注册，等引擎跑几帧（场景/流送/代理都就位）再触发 —— 在
# UnrealEditor-Cmd 里同步调 capture_scene() 出的是全黑，连引擎标准 Cube 都拍不到，
# 所以这个脚本必须用**真编辑器**跑：
#   UnrealEditor.exe <project> -ExecCmds="py <此脚本>"
# （-ExecutePythonScript 会在脚本返回后立刻退出编辑器，tick 根本没机会跑。）
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
    RIGS.append((name, cap, comp, rt))
    spawned.append(cap)

STATE = {"ticks": 0, "handle": None}


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] == 40:
        for name, cap, comp, rt in RIGS:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "%s.png" % name)
            unreal.log("shot: %s.png" % name)
    if STATE["ticks"] < 70:
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    for a in spawned:
        ACTORS.destroy_actor(a)
    unreal.log("SHOT DONE")
    unreal.SystemLibrary.quit_editor()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
