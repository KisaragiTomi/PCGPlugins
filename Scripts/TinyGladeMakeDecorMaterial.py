# -*- coding: utf-8 -*-
"""
给演示关卡的房子接上装饰摆件（D12 的**锚点那一半**）：建一张母材质 + 把网格/材质/参数
烘进 BP CDO 与关卡实例。

--- 为什么必须自己建材质 ------------------------------------------------------------
`clutter/` 那 58 张网格**一张贴图都没有** —— `Content/TinyGlade/Textures/` 实有 459 张，
与 `MaterialInstances/` 的 459 个 MI 一一对应，clutter 一个都不在其中（对照文档 §7.3/§7.6）。
它们的颜色全部烘在**顶点流**里，TG 侧的消费者是 `M_TG_VertexColor`。

而 `M_TG_VertexColor` **没有勾 `used_with_instanced_static_meshes`**（`M_TG_Texture` 那份
同样的缺陷已于 2026-08-31 补勾，`M_TG_VertexColor` 至今没有消费者、因此原样留着），
而摆件走的是 `UCSGpuInstancedMeshComponent` 的实例路径 ⇒ 引擎会把材质**静默换成默认材质**，
症状与"根本没绑材质"逐像素相同（一片灰），**而所有 readback 断言照绿**。
石阶那个坑（`StairMesh`/`StairMaterial` 恒 NULL、画面黑块、单测全绿）就是这一枪的第一次。
`ACSHouseActor::IsDecorDrawable` 把"母材质勾没勾"做成了显式判据；这份脚本是那条判据的供给侧。

⚠️ `M_TG_VertexColor` / `M_TG_Texture` 的**材质图**不要动 —— 它们是 gallery 的看图材质，
459 个 MI 挂在 `M_TG_Texture` 下面，改节点/参数会波及整个提取资产库。

⚠️ **但"不要改"不包括 usage 标志与着色模型**（原措辞已随裁决六作废，2026-08-31 订正）：
`M_TG_Texture` 本体已经翻成 `MSM_DefaultLit`（2026-08-30 裁决六：交付路径上不许 unlit），
并已补上 `bUsedWithInstancedStaticMeshes`（2026-08-31）。那两条都属于"不设就静默换材质 /
静默不受光"的一类开关，**该补就补**，与"别动材质图"是两件事。摆件仍然自建母材质，
理由是上面那条顶点色 boost，不是母材质本身还坏着。

--- 顶点色从哪来，为什么要乘一个大于 1 的系数 -----------------------------------
`CSHouseVine::BuildBaseMesh`（摆件复用它读基础网格）把源网格的顶点色流搬进快照，
没有色流时退成白色。

⚠️ **直接拿它当 base color 会得到一排黑影**（实测踩过）。源 GLB 的 `COLOR_0` 是
**线性空间的 float3**，而 TG 的木头在线性域里本来就很暗 —— 直接从
`extracted/meshes/clutter/*.glb` 量出来的均值：
`barrel` R .041 / G .038 / B .024（峰值 .277）、`crates_w_flowers` .081/.071/.050、
`firewood` .233/.179/.125、`birdnest` .184/.164/.079。
导入器把这些值**原样**存成字节（不做 sRGB 编码），所以材质里读到的就是 0.04 这个量级；
在本工程钉死的曝光下，一个 barrel 几乎精确黑（第一次出图实测：摆件覆盖处平均亮度 26.8，
且精确 (0,0,0) 占比从 0.154% 涨到 0.694%）。

所以这里乘 `COLOR_BOOST` 再 0..1 夹断：barrel 回到线性 0.12（≈ sRGB 0.38）的中深棕，
`firewood` / `birdnest` 回到中调。**不去改导入设置** —— 459 张贴图与 474 张网格都是同一批
导入的，为一家改导入管线会波及全库。
网格没有色流时快照退白 ⇒ 乘完夹断仍是白，读起来是"这张网格没顶点色"而不是一片洋红。

--- 尺寸 ---------------------------------------------------------------------------
clutter 网格实测 100–250 cm（`barrel` 106×106×175、`stall_veggies` 206×122×148），
而墙高 300 cm ⇒ 原尺寸摆上去一个桶半堵墙。`DecorScale = 0.5` 是按这个量出来的，
理由与数字都写在 `CSHouseActor.h` 的字段注释里，这里只把它烘进资产。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript=".../TinyGladeMakeDecorMaterial.py" \\
        -unattended -nopause -abslog=<独立日志>

日志自诊断：最后一行是 `DECOR SETUP OK` 或 `DECOR SETUP FAILED`。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

CLUTTER = "/Game/TinyGlade/Meshes/clutter/%s/StaticMeshes/%s"

# 三家各自的 `ClutterMeshes` 读集（TG 的每个 autoclutter 生产者都有自己的一组）。
# 计划 D12 点名的「箱子/水果摊」在提取资产里就是 `crates_w_flowers` / `stall_veggies`。
GATE_MESHES = ["crates_w_flowers", "stall_veggies"]
WALLFOOT_MESHES = ["firewood", "barrel", "birdhouse"]
# TG 的 `add_birdnests` 那一家。只放 `birdnest`：`birdhouse` 是一根 216 cm 的杆，
# 站在屋脊上读起来像穿模（第一轮出图看到了），摆到墙脚才对。
ROOF_MESHES = ["birdnest"]

# 顶点色的提亮系数。依据是从 `extracted/meshes/clutter/*.glb` 量出来的 `COLOR_0` 均值
#（见模块文档头）：×3 把 barrel 从线性 .041 拉到 .12（≈ sRGB 0.38）。
COLOR_BOOST = 2.5
COLOR_FLOOR = 0.05

FAILURES = []


def log(msg):
    unreal.log("DECOR %s" % msg)


def fail(msg):
    FAILURES.append(msg)
    unreal.log_error("DECOR !! %s" % msg)


def load_meshes(names):
    out = []
    for n in names:
        m = unreal.EditorAssetLibrary.load_asset(CLUTTER % (n, n))
        if not m:
            fail("网格缺失 %s" % (CLUTTER % (n, n)))
            continue
        box = m.get_bounding_box()
        size = box.max - box.min
        # 把实测尺寸打进日志：`DecorScale` 的取值依据就是这几行，换资产必须先复核。
        log("mesh %-18s size=(%.1f, %.1f, %.1f) pivotZ=%.1f" % (n, size.x, size.y, size.z, box.min.z))
        out.append(m)
    return out


def per_instance_tint(mat, x, y, lo, hi):
    """
    逐实例的明暗微差。**这一项依赖"随机数是身份哈希"那条纪律**：
    `CSHouseDecor` 的 `Random01` 由 (家族, 锚点 id, 种子) 算出，同一件摆件恒等；
    取 `InterlockedAdd` 槽位的话，房子每重建一次全场就换一次颜色 —— 而且**不会有任何断言
    报红**（S1 在石阶上栽过一次）。这张材质就是那条纪律的消费者。

    ⚠️ 读的是 `PerInstanceRandom + VertexColor.A`，不是裸的 `PerInstanceRandom`（裁决六 ③）：
    烘成 StaticMesh 之后**没有实例了**，`PerInstanceRandom` 恒等于 0，这一支会塌成
    `lerp(0.78, 1.22, 0)`，整场摆件烘出来是同一个色 —— 不报错，只有像素看得见。
    两条路互斥地为零（实例路上传基础网格时 A 被清零，烘焙路把随机数写进每个实例全部顶点的 A），
    相加之后取值逐位相同。通道字典在 `CSGpuInstancedMeshComponent` 类注释。
    ⚠️ 与上面 base color 那条 `vcol` 不冲突：字典把 **RGB** 留给资产自带的色流，**A** 归逐实例随机。
    """
    rnd_live = MEL.create_material_expression(mat, unreal.MaterialExpressionPerInstanceRandom, x - 220, y)
    rnd_vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, x - 220, y + 60)
    rnd = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, x, y)
    MEL.connect_material_expressions(rnd_live, "", rnd, "A")
    MEL.connect_material_expressions(rnd_vcol, "A", rnd, "B")
    lerp = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, x + 250, y)
    a = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y + 120)
    a.set_editor_property("r", lo)
    b = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y + 180)
    b.set_editor_property("r", hi)
    MEL.connect_material_expressions(a, "", lerp, "A")
    MEL.connect_material_expressions(b, "", lerp, "B")
    MEL.connect_material_expressions(rnd, "", lerp, "Alpha")
    return lerp


def build_material(name):
    path = "%s/%s" % (PKG, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mat = TOOLS.create_asset(name, PKG, unreal.Material, unreal.MaterialFactoryNew())
    if not mat:
        fail("建不出材质 %s" % name)
        return None

    # **这一行是这份脚本存在的理由**：没勾它的材质在 UCSGpuInstancedMeshComponent 上
    # 会被引擎静默换成默认材质，画面一片灰而所有 readback 断言照绿。
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    # clutter 里有草垛、篮子这类薄壳件，背面不画会漏；两面画一次性堵掉。
    mat.set_editor_property("two_sided", True)

    # 顶点色 × COLOR_BOOST，再 0..1 夹断。系数大于 1 的理由与实测值见模块文档头：
    # 源 `COLOR_0` 是线性域的深色木头（barrel 均值 .041），原样用就是一排黑影。
    vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -900, 0)
    boost = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, 200)
    boost.set_editor_property("r", COLOR_BOOST)
    lifted = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -620, 40)
    MEL.connect_material_expressions(vcol, "RGB", lifted, "A")
    MEL.connect_material_expressions(boost, "", lifted, "B")
    # 再加一个地板色。两个作用：① barrel 那类均值 .04 的网格光靠乘法仍然偏黑；
    # ② 资产里恰好没有色流时快照退白，乘完会过亮 —— 加法项把两头都拉回可读区间。
    # **只用 Multiply / Add 这两个节点**：`MaterialExpressionClamp` 的输入引脚名在
    # Python 侧接不上（传 "" 会静默失败），实测的症状是 base color 整条断掉、摆件全黑。
    floor_c = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -900, 300)
    floor_c.set_editor_property("constant", unreal.LinearColor(COLOR_FLOOR, COLOR_FLOOR * 0.72, COLOR_FLOOR * 0.46, 1.0))
    base = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -430, 40)
    MEL.connect_material_expressions(lifted, "", base, "A")
    MEL.connect_material_expressions(floor_c, "", base, "B")

    tint = per_instance_tint(mat, -900, 460, 0.78, 1.22)
    mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, 0)
    MEL.connect_material_expressions(base, "", mul, "A")
    MEL.connect_material_expressions(tint, "", mul, "B")
    MEL.connect_material_property(mul, "", unreal.MaterialProperty.MP_BASE_COLOR)

    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -250, 260)
    rough.set_editor_property("r", 0.82)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    log("built %s" % name)
    return mat


gate = load_meshes(GATE_MESHES)
wallfoot = load_meshes(WALLFOOT_MESHES)
roof = load_meshes(ROOF_MESHES)
decor_mat = build_material("M_TinyGladeDecor")

DECOR_PROPS = (
    ("bDecorEnabled", True),
    ("DecorGateMeshes", gate),
    ("DecorWallFootMeshes", wallfoot),
    ("DecorRoofMeshes", roof),
    ("DecorMaterial", decor_mat),
    # 默认值的依据都写在 `CSHouseActor.h` / `CSHouseDecor.h` 的字段注释里；这里只烘进资产。
    ("DecorWallFootSpacing", 150.0),
    ("DecorEaveSpacing", 220.0),
    ("DecorMinSpacing", 90.0),
    ("DecorRoadReject", 0.3),
    ("DecorScale", 0.5),
    ("DecorScaleJitter", 0.16),
    ("DecorSeed", 7),
)

bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeHouse" % PKG)
if bp:
    cdo = unreal.get_default_object(bp.generated_class())
    for name, value in DECOR_PROPS:
        try:
            cdo.set_editor_property(name, value)
        except Exception as exc:
            # ⚠️ UE Python 对**不存在的属性抛异常**，脚本会从那一行起整段不执行，
            # 而前面的日志看着一切正常（坑表里那条）。所以逐条 try 并把失败记下来。
            fail("CDO 写不进 %s：%s" % (name, exc))
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    log("assigned to BP CDO")
else:
    fail("找不到 %s/BP_TinyGladeHouse" % PKG)

A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
houses = 0
for level in ("L_HouseGroundDemo",):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    for a in A.get_all_level_actors():
        if "House" not in a.get_class().get_name():
            continue
        # 实测陷阱：CDO 的默认值不传播到已存在的实例，实例上必须再写一份。
        for name, value in DECOR_PROPS:
            try:
                a.set_editor_property(name, value)
            except Exception as exc:
                fail("%s 写不进 %s：%s" % (a.get_actor_label(), name, exc))
        a.call_method("RebuildHouse")
        # ⚠️ 坑 ⑩：一律调 `get_decor_undrawable_reason()`，**不要** `is_decor_drawable()` ——
        # UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值，不可画时拿到 `None`，
        # `str(None) == "None"` 会伪造出一句像模像样的原因，恰好在唯一需要它的时候失效。
        reason = str(a.get_decor_undrawable_reason())
        drawable = (reason == "")
        log("%s decor anchors=%d instances=%d drawable=%s%s"
            % (a.get_actor_label(), a.get_decor_anchor_count(), a.get_decor_instance_count(),
               drawable, "" if drawable else (" —— " + reason)))
        if not drawable:
            fail("%s 的摆件画不出来：%s" % (a.get_actor_label(), reason))
        houses += 1
    unreal.EditorLoadingAndSavingUtils.save_current_level()

if houses == 0:
    fail("演示关卡里一栋房子都没找到")

unreal.log("DECOR SETUP %s (%d houses, %d failures)"
           % ("OK" if not FAILURES else "FAILED", houses, len(FAILURES)))
