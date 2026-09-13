# -*- coding: utf-8 -*-
"""
给演示关卡的房子接上**砖石承重柱**（TinyGladeHouse D9 表现层，2026-09-06 用户裁决 B 档）。

--- 为什么没有柱子网格 -------------------------------------------------------------
TG 的 `system_wall_constructor::construct_elevation_supports` 里那个函数叫
`assemble_stone_pillar` —— **装配**，不是"放置"。提取库里根本没有柱件，砖就是墙上那块
`brick`（1×1×1 居中、600 顶点的倒角石块）。所以这里也不引入新资产，与门框砖共用同一块，
连材质都复用 `M_TinyGladeBrick`：同一块石头没有理由做两份。

⚠️ **`PillarMaterial` 原来指向 `M_TinyGladeReveal`**（洞口内壁那张）。方盒时代那是个无所谓的
占位，换成砖之后就不对了 —— 内壁材质和石砌的取向不一样，砖会显得发灰发平。这份脚本把它
改指 `M_TinyGladeBrick`。

--- 与 TG 的对位 -------------------------------------------------------------------
本档落地的是 `util_cross_brick_pillar::construct_crossbrick_pillar`（交错叠砖）+
`construct_rectangle_brackets` / `_small_brackets`（顶部出挑的托架）。
不做的：`circle_*` 那一支（前提是圆 footprint，本项目的墙是刚性矩形，没有落点）、
`construct_wooden_pillar_w_stone_base`（木柱石基变体）。

⚠️ 光改 C++ 默认值不够：这份脚本把值烘进 BP CDO 与关卡实例，改完必须重跑一次
（与 `TinyGladeSetupFrame.py` / `TinyGladeMakeIvyMaterials.py` 同一条）。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
BRICK = "/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick"
BRICK_MAT = "%s/M_TinyGladeBrick" % PKG
BP = "%s/BP_TinyGladeHouse" % PKG

FAILURES = []


def log(msg):
    unreal.log("PILLAR %s" % msg)


def fail(msg):
    FAILURES.append(msg)
    unreal.log_error("PILLAR !! %s" % msg)


brick = unreal.EditorAssetLibrary.load_asset(BRICK)
if not brick:
    fail("没有 %s" % BRICK)
    raise SystemExit

box = brick.get_bounding_box()
size = box.max - box.min
log("brick mesh size=(%.1f, %.1f, %.1f) centre=(%.1f, %.1f, %.1f)"
    % (size.x, size.y, size.z, (box.max.x + box.min.x) * 0.5,
       (box.max.y + box.min.y) * 0.5, (box.max.z + box.min.z) * 0.5))

# 砖材质由 TinyGladeSetupFrame.py 建。这里只引用，**不重建** —— 两份脚本各建一次的话
# 后跑的那份会把先跑的那份的引用打断（delete_asset + create），门框砖会静默变默认棋盘格。
mat = unreal.EditorAssetLibrary.load_asset(BRICK_MAT)
if not mat:
    fail("没有 %s —— 先跑一次 TinyGladeSetupFrame.py（那份负责建砖材质）" % BRICK_MAT)
    raise SystemExit
if not mat.get_editor_property("used_with_instanced_static_meshes"):
    fail("%s 没勾 used_with_instanced_static_meshes：实例路径上会被引擎**静默**换成默认材质" % BRICK_MAT)

PILLAR_PROPS = (
    ("bPillarUseBricks", True),
    ("PillarBrickMesh", brick),
    ("PillarMaterial", mat),
    # 连续方形承压截面，逐层旋转同一块倒角石；不再交替缩窄到 72%。
    ("PillarSize", 34.0),
    ("PillarCourseHeight", 20.0),
    ("PillarYawJitter", 0.035),
    ("PillarBracketCourses", 3),
    ("PillarBracketOverhang", 0.55),
)


def apply(target, what):
    for name, value in PILLAR_PROPS:
        try:
            target.set_editor_property(name, value)
        except Exception as exc:
            fail("%s 写不进 %s：%s" % (what, name, exc))


bp = unreal.EditorAssetLibrary.load_asset(BP)
if bp:
    cdo = unreal.get_default_object(bp.generated_class())
    apply(cdo, "CDO")
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    log("assigned to BP CDO")
else:
    fail("找不到 %s" % BP)

# ⚠️ 实测陷阱：CDO 的默认值**不传播到已存在的实例**，关卡里的房子必须再写一份
#（与 TinyGladeMakeIvyMaterials.py 里那条坑同一个）。
SS = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
world = SS.get_editor_world()
houses = 0
if world:
    for a in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.CSHouseActor):
        apply(a, a.get_actor_label())
        a.call_method("RebuildHouse")
        log("%s pillars=%d" % (a.get_actor_label(), a.get_pillar_count()))
        houses += 1
    unreal.EditorLoadingAndSavingUtils.save_current_level()

if houses == 0:
    fail("关卡里一栋房子都没找到")

unreal.log("PILLAR SETUP %s (%d houses, %d failures)"
           % ("OK" if not FAILURES else "FAILED", houses, len(FAILURES)))
