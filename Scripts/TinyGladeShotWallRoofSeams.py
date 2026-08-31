# -*- coding: utf-8 -*-
"""
拍「墙-顶三处收边」的改前 / 改后对照图 —— 同机位，其中一张是**室内朝上**。

三处各有一个判读位：
  ① 檐口楔形缝（功能性）：`in_eave` 从室内平视檐墙顶、`in_up` 从室内朝正上方。
     改前墙顶是平的 Z = WallHeight，与屋面底之间留一条外 0 内 T·tan(pitch) 的空腔；
     ⚠️ 这条缝**几何上被墙外棱那条零宽度相切封着**，所以图上未必真能看见蓝天，
     判读点是"墙顶到屋面底之间那条明暗不连续的带"在改后消失。硬证据在单测
     `PCGPlugins.ComputeShaderGenerator.House.EaveSealed`（逐点体积判据），不在图上。
  ② 山墙斜边共面：`gable_out`。改前山墙顶边与屋面底共面 ⇒ 沿两条斜边一条亮线（z-fight）。
  ③ 屋脊互穿：`ridge_out`。改前两块坡板过冲互穿，脊上一路交叉小尖（35°/12 cm 板 ≈ 7 cm）。
     这一处改前改后**肉眼差别最大**，先看它。

用法（两次跑必须用同一份机位代码，否则对照无效）：

  set TG_SHOT_TAG=before
  UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotWallRoofSeams.py"

产物：Saved/TinyGladeShots/seam_<tag>_*.png

--- 踩过的坑（前五条来自状态文件那张表，后三条是本轮新踩的）-----------------------
 ① `unreal.Rotator` 的构造参数序是 **(roll, pitch, yaw)** —— 一律用关键字参数。
    写错的症状：相机朝天、出图一张纯渐变，看着像"渲染没出来"。
 ② 必须用**真编辑器** + `-ExecCmds="py <脚本>"`。`-ExecutePythonScript` 跑完即退，
    tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏，抓的是空面板。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：`SceneCaptureRendering.cpp` 的
    `SetupViewFamilyForSceneCapture` 在 blend 完世界 PPV **之后**直接写
    `DynamicGlobalIlluminationMethod = None`，于是 PPV 里所有 Lumen 参数在出图里一条都不生效。
    唯一的翻盘办法是给捕获组件**自己**的 `post_process_settings` 再覆盖一次（它在那两行之后 apply）。
    室内那两张全靠间接光，这一条不做就是两张纯黑。
 ⑥ 读 UENUM 属性别用 `str(枚举).endswith("X")`（恒假，整组机位跟着转 90°），也别用
    `int(枚举)`（TypeError）。要和枚举值比，且 Python 侧的类型名**去掉 E 前缀**：
    C++ 的 `ECSRidgeAxis` 在这里叫 `unreal.CSRidgeAxis`。
 ⑦ 脚本在注册 tick **之前**抛异常 ⇒ 编辑器永远不退（`-ExecCmds` 不会因为 py 报错关窗），
    上层只能等超时。所以准备阶段整段包在 try 里，出错自己 quit_editor。
 ⑧ 本轮**没有改任何 C++ 属性默认值**，所以 `TinyGladeSetupFrame.py` /
    `TinyGladeMakeGroundMaterial.py` 这次不需要重跑（几何是每次重求值现生成的）。
"""
import math
import os
import traceback
import unreal

TAG = os.environ.get("TG_SHOT_TAG", "after")

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H_PIX = 1600, 900
FIRST_TICK = 40      # Lumen 的 surface cache 要几十帧才收敛，早拍就是一张暗图
GAP = 10


def setup():
    ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()
    spawned = []

    house = next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == "House_Road"), None)
    if not house:
        raise RuntimeError("House_Road missing")

    # 机位全部从房子自己的属性反算 —— 手调的机位在参数一变就全废，而对照图最怕两次机位不一样。
    loc = house.get_actor_location()
    fp = house.get_editor_property("footprint_size")
    wall_h = float(house.get_editor_property("wall_height"))
    wall_t = float(house.get_editor_property("wall_thickness"))
    pitch = float(house.get_editor_property("roof_pitch"))
    overhang = float(house.get_editor_property("roof_overhang"))
    ridge_along_x = (house.get_editor_property("ridge_axis") == unreal.CSRidgeAxis.X)   # 见坑 ⑥

    ridge_len = float(fp.x if ridge_along_x else fp.y)
    span = float(fp.y if ridge_along_x else fp.x)
    half_span = span * 0.5
    ridge_z = wall_h + math.tan(math.radians(pitch)) * half_span
    unreal.log("house loc=(%.0f, %.0f, %.0f) yaw=%.1f" % (loc.x, loc.y, loc.z, house.get_actor_rotation().yaw))
    unreal.log("house: fp=(%.0f, %.0f) H=%.0f T=%.0f pitch=%.0f overhang=%.0f ridgeX=%s ridgeZ=%.0f"
               % (fp.x, fp.y, wall_h, wall_t, pitch, overhang, ridge_along_x, ridge_z))

    def at(along, across, up):
        """脊向坐标 → 世界坐标（演示房子 yaw = 0，脊向只决定哪根世界轴是跨度）。"""
        dx, dy = (along, across) if ridge_along_x else (across, along)
        return unreal.Vector(loc.x + dx, loc.y + dy, loc.z + up)

    def look_at(frm, to):
        """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
        dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
        flat = math.hypot(dx, dy)
        return unreal.Rotator(roll=0.0,
                              pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                              yaw=math.degrees(math.atan2(dy, dx)))

    inner = half_span - wall_t          # 内墙面的跨度坐标
    cam_in_eave = at(-ridge_len * 0.30, -inner * 0.85, wall_h * 0.35)
    cam_in_up = at(-ridge_len * 0.22, 0.0, wall_h * 0.12)
    cam_ridge = at(-ridge_len * 1.20, half_span * 3.50, ridge_z + 25.0)
    cam_gable = at(ridge_len * 0.5 + 620.0, half_span * 1.60, wall_h * 1.15)
    cam_eave = at(-ridge_len * 0.18, (half_span + overhang) * 3.2, wall_h * 0.30)

    shots = [
        # ① 室内平视檐墙顶：判读位是"墙顶到屋面底"那条带。
        ("in_eave", cam_in_eave, look_at(cam_in_eave, at(0.0, inner, wall_h + 25.0)), 60.0, -2.0),
        # ① 室内**朝正上方**：一张图里同时看到两侧檐口、屋脊、两端山墙的接缝。
        ("in_up", cam_in_up, unreal.Rotator(roll=0.0, pitch=89.9, yaw=0.0), 105.0, -2.0),
        # ③ 屋脊：改前那一路交叉小尖就在这张里。
        ("ridge_out", cam_ridge, look_at(cam_ridge, at(0.0, 0.0, ridge_z)), 30.0, 0.0),
        # ② 山墙端：斜边与屋面底共面产生的亮线沿两条斜边走。
        ("gable_out", cam_gable, look_at(cam_gable, at(ridge_len * 0.5, 0.0, (wall_h + ridge_z) * 0.5)), 40.0, 0.0),
        # 外侧檐口：确认封口件全埋在屋面板里、没在外面鼓出来（顺带看檐口端面改成竖直后的轮廓）。
        ("eave_out", cam_eave, look_at(cam_eave, at(-ridge_len * 0.18, half_span, wall_h + 6.0)), 40.0, 0.0),
    ]

    # 室内那两张全靠间接光。补一盏点光让墙面读得出来 —— 改前改后**同样处理**，对照才成立。
    lamp = ACTORS.spawn_actor_from_class(unreal.PointLight, at(0.0, 0.0, wall_h * 0.30),
                                         unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
    lamp.set_actor_label("TG_SeamProbeLight")
    lamp.point_light_component.set_editor_property("intensity", 900.0)
    lamp.point_light_component.set_editor_property("attenuation_radius", 900.0)
    lamp.point_light_component.set_editor_property("cast_shadows", False)   # 只为读出形状，不引入新的阴影变量
    spawned.append(lamp)

    # 见坑 ⑤：把捕获自己那份 PP 覆盖建起来，Lumen 才会回到出图里。
    # ⑨ 曝光偏置是**逐张**的：墙面材质接近纯白，自动曝光把室内推到中灰就等于把墙推到饱和，
    #    两张室内图会一起糊成一片白（判读位正好在墙顶那条边上，糊了就什么都看不出）。
    def make_pp(exposure_bias):
        pp = unreal.PostProcessSettings()
        pp.set_editor_property("override_dynamic_global_illumination_method", True)
        pp.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
        pp.set_editor_property("override_reflection_method", True)
        pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
        pp.set_editor_property("override_lumen_surface_cache_resolution", True)
        pp.set_editor_property("lumen_surface_cache_resolution", 1.0)
        pp.set_editor_property("override_auto_exposure_bias", True)
        pp.set_editor_property("auto_exposure_bias", exposure_bias)
        return pp

    rigs = []
    for name, cam_loc, cam_rot, fov, bias in shots:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H_PIX, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_loc, cam_rot)
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", fov)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        comp.set_editor_property("post_process_settings", make_pp(bias))
        comp.set_editor_property("post_process_blend_weight", 1.0)
        unreal.log("cam %-10s loc=(%.0f, %.0f, %.0f) rot=(p %.1f, y %.1f) fov=%.0f"
                   % (name, cam_loc.x, cam_loc.y, cam_loc.z, cam_rot.pitch, cam_rot.yaw, fov))
        rigs.append((name, cap, comp, rt))
        spawned.append(cap)

    state = {"ticks": 0, "handle": None}

    def tick(delta):
        state["ticks"] += 1
        index = state["ticks"] - FIRST_TICK
        if index >= 0 and index // GAP < len(rigs):
            phase = index % GAP
            name, cap, comp, rt = rigs[index // GAP]
            if phase == 0:
                unreal.EditorLevelLibrary.pilot_level_actor(cap)
                unreal.EditorLevelLibrary.editor_invalidate_viewports()
            elif phase == 5:
                comp.capture_scene()
                unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, "seam_%s_%s.png" % (TAG, name))
                unreal.log("shot: seam_%s_%s.png" % (TAG, name))
        if state["ticks"] < FIRST_TICK + GAP * len(rigs) + 10:
            return
        unreal.unregister_slate_post_tick_callback(state["handle"])
        for a in spawned:
            ACTORS.destroy_actor(a)
        unreal.log("SHOT DONE tag=%s" % TAG)
        unreal.SystemLibrary.quit_editor()

    state["handle"] = unreal.register_slate_post_tick_callback(tick)


try:
    setup()
except Exception:                     # 见坑 ⑦
    unreal.log_error("SHOT FAILED\n" + traceback.format_exc())
    unreal.SystemLibrary.quit_editor()
