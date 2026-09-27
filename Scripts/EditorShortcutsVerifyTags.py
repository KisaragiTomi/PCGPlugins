# -*- coding: utf-8 -*-
"""
EditorShortcuts 的 tag 开关：生成 `BPFL_Tags` 里的四个 Toggle 函数，并端到端验收。

做两件事：

1. **生成**：`UEditorShortcutSetupLibrary::CreateTagLibrary` 往
   `/PCGPlugins/EditorShortcuts_BPFL/BPFL_Tags` 里生成 `ToggleCSSW` / `ToggleUA` / `TogglePick` / `ToggleRef`
   （同名函数重建，资产里别的东西不动）。项目 ini 里四条绑定都指向这个类。

2. **验收**（走真的绑定：`UEditorShortcutSubsystem::ExecuteBindingByLabel`，也就是按键那条路，带撤销事务）：
   A. 没有就加、有就删：同一个 actor 连按两次，tag 先出现再消失。
   B. 每次改动在 actor 身上贴一个临时文字：加 = 绿色 "+Tag"，删 = 红色 "-Tag"，
      只给真改了的 actor 冒（数量 = 改动数），过了 `TagIndicatorSeconds` 自己消失。
      **位置 = 该 actor 包围盒的最上方**（顶面正中，底边贴着顶面往上长），**宽度 = 包围盒**
      从视口方向看的宽度，正面朝着视口。判据按文字实际的朝向反算，视口在哪都成立。
   C. **撤销不诈尸**：按一次 → 等文字消失 → Ctrl+Z（`TRANSACTION UNDO`）→ tag 被撤掉、文字**不会**回来。
      绑定默认在 `FScopedTransaction` 里跑，而 `UWorld::SpawnActor` 在事务里会 `ModifyLevel`，
      没把事务藏起来的话，撤销会把一个已经自毁的文字 actor 连同关卡的 actor 表一起恢复。
   D. 四条绑定都能按（`ValidateAllBindings` 无报错 + 每条实际按一次再按回来）。

跑法（要真渲染 + slate tick，所以是完整编辑器而不是 -ExecutePythonScript）::

    UnrealEditor-Cmd.exe <uproject> -ExecCmds="py <本文件>" -unattended -nosplash -RenderOffscreen

判定看日志：每条 `[PASS]` / `[FAIL]`，末尾 `TAGVERIFY OK` / `TAGVERIFY FAILED`。不存关卡。
⚠️ 找文字 actor 不能用 `EditorActorSubsystem.get_all_level_actors()`：它按设计跳过 transient 和
不在大纲里的 actor（`EditorActorSubsystem.cpp:383-386`），而指示器两样都是 —— 用 `GameplayStatics`。
"""
import math
import time

import unreal

LIB = "/PCGPlugins/EditorShortcuts_BPFL/BPFL_Tags"
TAGS = ["CSSW", "UA", "Pick", "Ref"]
LABEL = {t: "Toggle %s" % t for t in TAGS}
LEVEL = "/PCGPlugins/HouseTest/L_HouseGroundDemo"

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
SHORTCUTS = unreal.get_editor_subsystem(unreal.EditorShortcutSubsystem)
SETTINGS = unreal.get_default_object(unreal.EditorShortcutSettings)

FAILS = []
PASSES = [0]
STATE = {"handle": None, "ticks": 0, "step": 0, "t0": 0.0, "world": None, "target": None, "orig_tags": []}


def check(label, ok, detail=""):
    if ok:
        PASSES[0] += 1
        unreal.log("[PASS] %s %s" % (label, detail))
    else:
        FAILS.append(label)
        unreal.log_error("[FAIL] %s %s" % (label, detail))


def tags_of(actor):
    return [str(t) for t in actor.get_editor_property("tags")]


def indicators():
    """Live tag labels: actors whose root is a TextRenderComponent saying +X / -X."""
    out = []
    for a in unreal.GameplayStatics.get_all_actors_of_class(STATE["world"], unreal.Actor):
        root = a.get_editor_property("root_component")
        if not isinstance(root, unreal.TextRenderComponent):
            continue
        text = str(root.get_editor_property("text"))
        if text[:1] in ("+", "-"):
            color = root.get_editor_property("text_render_color")
            out.append((text, (color.r, color.g, color.b), a))
    return out


def check_placement(label_actor, target, text):
    """文字朝着视口、站在 actor 包围盒顶面正中、宽度 = 包围盒（按文字自己的朝向反算期望值）。"""
    origin, extent = target.get_actor_bounds(False)
    got = label_actor.get_actor_location()
    yaw = math.radians(label_actor.get_actor_rotation().yaw)
    fx, fy = math.cos(yaw), math.sin(yaw)            # 文字 +X = 正面法线（TextRender 的 TangentZ）

    # 正面必须朝着视口相机：背过去的文字会被单面材质剔掉，人在视口里什么都看不到，
    # 而位置、大小断言照样全绿 —— 第二版就是这么漏过去的。
    cam = STATE["cam"]
    to_cam = unreal.Vector(cam.x - got.x, cam.y - got.y, 0.0)
    facing = (fx * to_cam.x + fy * to_cam.y) / max(to_cam.length(), 1e-3)
    check("B %s 正面朝着视口（否则被背面剔除、看不见）" % text, facing > 0.99, "cos = %.4f" % facing)

    ax, ay = -fy, fx                                 # 文字的宽度方向
    box_w = 2.0 * (extent.x * abs(ax) + extent.y * abs(ay))
    want = unreal.Vector(origin.x, origin.y, origin.z + extent.z + 1.0)
    off = (got - want).length()
    check("B %s 站在 actor 包围盒顶面的正中" % text, off < 1.5,
          "偏差 %.2f cm（文字 %s，包围盒中心 %s 半尺寸 %s）" % (off, got, origin, extent))

    root = label_actor.get_editor_property("root_component")
    align = str(root.get_editor_property("vertical_alignment"))
    check("B %s 从顶面往上长（底边对齐，整个文字都在包围盒上方）" % text, "BOTTOM" in align.upper(), align)

    size = root.get_text_local_size()                # (0, 宽, 高)
    w, h = size.y, size.z
    check("B %s 宽度 = 包围盒从视口方向看的宽度" % text, abs(w - box_w) <= box_w * 0.001 + 0.5,
          "文字 %.1f×%.1f cm，包围盒宽 %.1f cm" % (w, h, box_w))


def press(tag):
    """The same path a key press takes: the binding, with its undo transaction.

    视口相机每次按之前钉回已知机位：文字朝向跟的是视口，判据要拿它反算。"""
    unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(STATE["cam"], STATE["cam_rot"])
    ACTORS.set_selected_level_actors([STATE["target"]])
    return SHORTCUTS.execute_binding_by_label(LABEL[tag])


def seconds_since(t0):
    return time.time() - t0


# ---------------------------------------------------------------------------
# 1) 生成 + 静态校验（同步）
# ---------------------------------------------------------------------------

def setup():
    path = unreal.EditorShortcutSetupLibrary.create_tag_library(LIB, TAGS, unreal.EditorShortcutTagOp.TOGGLE, True)
    check("CreateTagLibrary 生成了 BPFL_Tags", bool(path), "-> %s" % path)

    problems = list(SHORTCUTS.validate_all_bindings())
    check("四条绑定全部可用（ValidateAllBindings 无报错）", not problems, "; ".join(str(p) for p in problems))

    bindings = SETTINGS.get_editor_property("bindings")
    for tag in TAGS:
        b = next((b for b in bindings if str(b.get_editor_property("label")) == LABEL[tag]), None)
        target = str(b.get_editor_property("target_class")) if b else "<没有这条绑定>"
        check("绑定 %s 指向 BPFL_Tags" % LABEL[tag], b is not None and "BPFL_Tags" in target, target)

    unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
    STATE["world"] = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    level_actors = ACTORS.get_all_level_actors()
    target = next((a for a in level_actors if a.get_class().get_name() == "StaticMeshActor"), None)
    target = target or next((a for a in level_actors if a.get_actor_label() == "House_Pillar"), None)
    if not target:
        check("关卡里找到一个测试 actor", False)
        return False
    STATE["target"] = target
    STATE["orig_tags"] = tags_of(target)
    # 视口相机放在目标的斜前上方（不沿任何轴，免得朝向错一半也蒙对）
    origin, extent = target.get_actor_bounds(False)
    reach = max(extent.x, extent.y, extent.z) * 4.0 + 300.0
    cam = unreal.Vector(origin.x - reach * 0.8, origin.y - reach * 0.6, origin.z + reach * 0.35)
    d = origin - cam
    STATE["cam"] = cam
    STATE["cam_rot"] = unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(d.z, math.hypot(d.x, d.y))),
                                      yaw=math.degrees(math.atan2(d.y, d.x)))
    unreal.log("TAGVERIFY 测试对象 %s（%s），原有 tag = %s"
               % (target.get_actor_label(), target.get_class().get_name(), STATE["orig_tags"]))
    for tag in TAGS:
        if tag in STATE["orig_tags"]:
            unreal.log_warning("TAGVERIFY 测试对象本来就带 %s —— 那一条的“加/删”顺序会反过来" % tag)
    check("开始前场上没有残留的文字 actor", not indicators(), str([i[0] for i in indicators()]))
    return True


# ---------------------------------------------------------------------------
# 2) 需要真 tick 的部分（文字的寿命走 core ticker，撤销要在事务结束之后）
# ---------------------------------------------------------------------------

LIFETIME = float(SETTINGS.get_editor_property("tag_indicator_seconds"))
WAIT = LIFETIME + 1.0


def step_press_once():
    had = "CSSW" in tags_of(STATE["target"])
    ok = press("CSSW")
    has = "CSSW" in tags_of(STATE["target"])
    check("A 按一次：没有就加上", ok and (not had) and has, "had=%s has=%s ok=%s" % (had, has, ok))
    live = indicators()
    check("B 冒出正好一个文字，写着 +CSSW、绿色",
          len(live) == 1 and live[0][0] == "+CSSW" and live[0][1][1] > 200 and live[0][1][0] < 50,
          str([(t, c) for t, c, _ in live]))
    if live:
        # get_all_level_actors 恰好就是按"transient 或不进大纲"过滤的（见文件头），
        # 所以"它看不见这个文字"= 文字是 transient / 不进大纲 —— 不会被存进关卡，也不在大纲里闪。
        listed = [a for a in ACTORS.get_all_level_actors() if a == live[0][2]]
        check("B 文字是 transient、不进大纲（编辑器的关卡 actor 列表看不见它）", not listed)
        check_placement(live[0][2], STATE["target"], live[0][0])


def step_after_lifetime():
    live = indicators()
    check("B 过了 %.1f s 文字自己消失" % LIFETIME, not live, str([t for t, _, _ in live]))


def step_undo():
    unreal.SystemLibrary.execute_console_command(STATE["world"], "TRANSACTION UNDO")
    has = "CSSW" in tags_of(STATE["target"])
    check("C 撤销把 tag 撤掉了", not has, "tags=%s" % tags_of(STATE["target"]))


def step_after_undo():
    live = indicators()
    check("C 撤销之后文字没有诈尸", not live, str([t for t, _, _ in live]))


def step_toggle_twice():
    press("CSSW")
    first = indicators()
    press("CSSW")
    has = "CSSW" in tags_of(STATE["target"])
    check("A 再按一次：有就删掉", not has, "tags=%s" % tags_of(STATE["target"]))
    live = indicators()
    minus = [x for x in live if x[0] == "-CSSW"]
    check("B 删除冒的是红色 -CSSW",
          len(minus) == 1 and minus[0][1][0] > 200 and minus[0][1][1] < 50,
          str([(t, c) for t, c, _ in live]))
    if minus:
        check_placement(minus[0][2], STATE["target"], minus[0][0])
    check("B 两次按键共冒两个文字（加一个、删一个）", len(first) == 1 and len(live) == 2,
          "first=%d now=%d" % (len(first), len(live)))


def step_all_bindings():
    for tag in TAGS[1:]:
        before = tag in tags_of(STATE["target"])
        ok1 = press(tag)
        mid = tag in tags_of(STATE["target"])
        ok2 = press(tag)
        after = tag in tags_of(STATE["target"])
        check("D %s 按两下：%s → %s → %s" % (LABEL[tag], before, mid, after),
              ok1 and ok2 and mid != before and after == before)


def step_finish():
    live = indicators()
    check("收尾：所有文字都已消失", not live, str([t for t, _, _ in live]))
    now = tags_of(STATE["target"])
    check("收尾：测试对象的 tag 与开始前一致", sorted(now) == sorted(STATE["orig_tags"]),
          "%s vs %s" % (now, STATE["orig_tags"]))


PLAN = [
    (0.0, step_press_once),
    (WAIT, step_after_lifetime),
    (0.0, step_undo),
    (0.5, step_after_undo),
    (0.0, step_toggle_twice),
    (0.0, step_all_bindings),
    (WAIT, step_finish),
]


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < 30:          # 关卡刚载入，先让编辑器转几帧
        STATE["t0"] = time.time()
        return
    step = STATE["step"]
    if step < len(PLAN):
        wait, fn = PLAN[step]
        if seconds_since(STATE["t0"]) < wait:
            return
        try:
            fn()
        except Exception:
            import traceback
            check("步骤 %s 没抛异常" % fn.__name__, False, traceback.format_exc())
        STATE["step"] += 1
        STATE["t0"] = time.time()
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    unreal.log("========== SUMMARY ==========")
    unreal.log("passed=%d failed=%d" % (PASSES[0], len(FAILS)))
    for f in FAILS:
        unreal.log_error("  failed: %s" % f)
    unreal.log("TAGVERIFY FAILED" if FAILS else "TAGVERIFY OK")
    unreal.SystemLibrary.quit_editor()


try:
    ready = setup()
except Exception:
    import traceback
    check("setup 没抛异常", False, traceback.format_exc())
    ready = False

if ready:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
else:
    unreal.log("TAGVERIFY FAILED")
    unreal.SystemLibrary.quit_editor()
