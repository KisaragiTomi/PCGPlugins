# -*- coding: utf-8 -*-
"""
往 `L_HouseGroundDemo` 摆 6 棵 TG 树：叶卡挂 `MI_TinyGladeCanopy`，树干用资产自带的树皮材质。

--- 为什么用 mid_tree_v3/v4/v5 而不是 central_tree ---------------------------------
叶卡材质要的三个 UV 通道（角码 / prim_center.xy / prim_center.z）只有 `*_leafcards` 那一族有，
而 `central_tree_leafcards` **没有配对的 `central_tree_trunk`** —— `central_tree` 是 canopy+trunk
合并资产，它的 canopy slot 是同一棵树的**低模叶卡版**（244 卡、只有 1 个 UV 通道且 UV0 全 0），
既喂不了这张材质，摆上去还会变成一团灰色实体把高模叶卡整个盖住（2026-09-09 出图时踩过）。
`mid_tree_v3/v4/v5` 三棵是 leafcards + trunk 成对的，实测 leafcards 都是 3 个 UV 通道。

--- 可重跑 -------------------------------------------------------------------------
所有 actor 的 label 带 `TGTree_` 前缀，脚本开头先按前缀删掉上一轮自己摆的，再摆新的。
**不动关卡里任何已有 actor**（两栋房子 / 地面 / 光照 / 后处理 / 窗户标记）。

⚠️ 本脚本**会存盘关卡** —— 与 `TinyGladeMakeWallMaterials.py` 那种「有意不存关卡」的脚本不同。

--- 贴地 ---------------------------------------------------------------------------
地面是 `BP_TinyGladeGround_C`，会被 `BP_GroundShaper1` 塑形，不能假定 z=0。
先向下打一条射线取地面高度；打不中（程序化网格可能没碰撞）就退回 z=0 并在日志里说明。
**2026-09-09 实测 6 次全部打不中** ⇒ 程序化地面没有碰撞体，z 全部退回 0。出图看下来
z=0 本身是对的（落在草地上的那棵贴得很好），错的只有 XY。

⚠️⚠️ **待修：PLAN 里的 6 个 XY 有 5 个落在草地外，树悬在空中。**
根因是**拿 actor 包围盒当草地范围**：`Ground_Demo` 的 bounds extent 是 (1802, 1802, 2000)，
但那只是包围盒，实际草地是一块小得多的不规则区域。俯瞰图（`demotree_aerial.png`）里
只有 `(-450,-1050)` 那棵落在草地边缘上，其余 5 棵全悬空。

**已知确实在地面上的点**（可作为重选位置的锚）：
    House_Road      (891, 1565)
    House_Pillar    (2200, 1600)
    BP_GroundShaper1 (987, 1387)
    TG_Start        (1100, 700)
真实草地范围要读 `Ground_Demo` 的**组件** bounds 或程序化网格顶点，不能再用 actor bounds。
`scratchpad/probe_ground.py` 就是干这个的，但它没跑成 —— 见下。

⚠️ **2026-09-09 起编辑器起不来，位置修不了**（与本脚本无关的既有状态）：
`CSGroundCover.usf:82` 的 `CoverHeightOffset` 绑不到 `FCSGroundCoverScatterCS::FParameters`，
global shader 编译失败 ⇒ 引擎启动即 fatal。**源码是一致的**（`CSGroundCover.cpp:57` 有这个
SHADER_PARAMETER、:278 也在赋值），是编译出来的二进制里 FParameters 还是旧版 ——
即「改了没编译」那条顽疾。**修法是重编译项目 C++**（Development Editor，别用 -Rebuild）。
在那之前本脚本重跑也没用。
"""
import json
import unreal

LEVEL = "/PCGPlugins/HouseTest/L_HouseGroundDemo"
MESHES = "/PCGPlugins/HouseTest/TinyGladeAsset/Meshes"
MI_CANOPY = "/PCGPlugins/HouseTest/MI_TinyGladeCanopy"
PREFIX = "TGTree_"
OUT_JSON = r"C:\Users\KLW\AppData\Local\Temp\claude\D--MyProject-UnrealProject-UETest574-2\663fbe13-5a7c-4be3-a860-830f2ae635f7\scratchpad\place_trees.json"

# (树种, x, y, yaw, scale) —— 避开两栋房子（891,1565 / 2200,1600）与玩家起点（1100,700），
# 都落在地面 ±1800 的范围内，从起点看过去有远近层次。
PLAN = [
    ("mid_tree_v5", -1150, 250, 20, 1.00),
    ("mid_tree_v3", -450, -1050, 140, 1.10),
    ("mid_tree_v4", 350, -1250, 250, 0.90),
    ("mid_tree_v3", 1500, -350, 75, 0.95),
    ("mid_tree_v4", -1250, 1150, 300, 1.05),
    ("mid_tree_v5", -150, 1350, 190, 0.85),
]

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
res = {"placed": [], "removed": 0, "traced": 0, "fallback_z0": 0, "failures": []}


def log(m):
    unreal.log("PLACETREE %s" % m)


def fail(m):
    res["failures"].append(m)
    unreal.log_error("PLACETREE !! %s" % m)


def ground_z(world, x, y):
    """向下打射线取地面高度；打不中就退回 0（程序化地面未必有碰撞）。"""
    try:
        hit = unreal.SystemLibrary.line_trace_single(
            world, unreal.Vector(x, y, 3000.0), unreal.Vector(x, y, -1000.0),
            unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
            unreal.DrawDebugTrace.NONE, True)
        if hit:
            res["traced"] += 1
            return hit.to_tuple()[4].z          # impact_point
    except Exception as e:
        fail("射线失败 (%s,%s)：%s" % (x, y, e))
    res["fallback_z0"] += 1
    return 0.0


les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not les.load_level(LEVEL):
    fail("关卡加载失败 %s" % LEVEL)
else:
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    # 先清掉上一轮自己摆的，脚本才可重跑
    for a in ACTORS.get_all_level_actors():
        try:
            if a.get_actor_label().startswith(PREFIX):
                ACTORS.destroy_actor(a)
                res["removed"] += 1
        except Exception:
            pass

    mi = unreal.EditorAssetLibrary.load_asset(MI_CANOPY)
    if mi is None:
        fail("材质实例缺失 %s" % MI_CANOPY)

    for i, (kind, x, y, yaw, scale) in enumerate(PLAN):
        leaf = unreal.EditorAssetLibrary.load_asset("%s/%s_leafcards" % (MESHES, kind))
        trunk = unreal.EditorAssetLibrary.load_asset("%s/%s_trunk" % (MESHES, kind))
        if leaf is None or trunk is None:
            fail("网格缺失 %s（叶卡 %s / 树干 %s）" % (kind, leaf is not None, trunk is not None))
            continue
        z = ground_z(world, x, y)
        loc = unreal.Vector(x, y, z)
        rot = unreal.Rotator(roll=0, pitch=0, yaw=yaw)   # ⚠️ Rotator 是 (roll, pitch, yaw)
        sc = unreal.Vector(scale, scale, scale)

        # 叶卡与树干必须同位置 / 同旋转 / 同缩放，否则冠与干对不上
        t = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, loc, rot)
        t.set_actor_scale3d(sc)
        t.static_mesh_component.set_editor_property("static_mesh", trunk)
        t.set_actor_label("%s%02d_%s_trunk" % (PREFIX, i, kind))

        c = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, loc, rot)
        c.set_actor_scale3d(sc)
        c.static_mesh_component.set_editor_property("static_mesh", leaf)
        if mi is not None:
            c.static_mesh_component.set_material(0, mi)
        c.set_actor_label("%s%02d_%s_canopy" % (PREFIX, i, kind))

        res["placed"].append({"kind": kind, "loc": [x, y, round(z, 1)], "yaw": yaw, "scale": scale})

    if not les.save_current_level():
        fail("关卡存盘失败")
    else:
        log("关卡已存盘 %s" % LEVEL)

log("摆了 %d 棵，清掉旧的 %d 个 actor，射线命中 %d / 退回 z=0 %d"
    % (len(res["placed"]), res["removed"], res["traced"], res["fallback_z0"]))

with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(res, f, ensure_ascii=False, indent=2)
print("PLACETREE_DONE placed=%d failures=%d" % (len(res["placed"]), len(res["failures"])))
