# -*- coding: utf-8 -*-
"""
给演示关卡的房子接上墙面藤蔓（D13）：建两张母材质 + 把网格/材质/参数烘进 CDO 与关卡实例。

--- 为什么必须自己建材质，不能直接用 `MI_ivy_branch_color` -------------------------
`Content/TinyGlade/` 里藤蔓的贴图与材质实例是**齐的**（`MI_ivy_branch_color` /
`MI_{summer,autumn,winter}_ivy_leaf_color` / `MI_leaf_alpha`），但它们**全都挂在
`M_TG_Texture` 下**，而**本脚本落地那天**（2026-08-30 早于裁决六）那张母材质实测是：

  · `shading_model = MSM_UNLIT`                     ⇒ 藤完全不受光、不响应阴影；
  · `used_with_instanced_static_meshes = False`     ⇒ **实例路径上引擎静默换成默认材质**。

第二条才是致命的：症状与"根本没绑材质"逐像素相同（一片灰），而所有 readback 断言照绿 ——
石阶那个坑（`StairMesh`/`StairMaterial` 恒 NULL、画面黑块、单测全绿）的同一条。
`ACSHouseActor::IsVineDrawable` 因此把"母材质勾没勾 bUsedWithInstancedStaticMeshes"
做成了显式判据；这份脚本是那条判据的供给侧。

⚠️ **上面那两条如今都已经不成立了**（2026-08-31 订正，别再照抄这段当现状）：裁决六把
`M_TG_Texture` 本体翻成了 `MSM_DefaultLit`，同日又给它补上了 `bUsedWithInstancedStaticMeshes`。
自建这两张母材质的理由因此只剩**功能**那一半 —— 逐实例明暗微差、叶子的双面植被着色模型、
以及只有我们自己知道的那套柱面 UV，`M_TG_Texture` 一条都不提供。

⚠️ `M_TG_Texture` 的**材质图**仍然不要动：它是 gallery 的看图材质，459 个 MI 挂在它下面，
改节点/参数会波及整个提取资产库。（usage 标志与着色模型是另一件事，见上。）

--- ivy_branch 没有法线也没有 UV ---------------------------------------------------
`ivy_branch` 的顶点流只有 `Vertex_Position`（对照文档 §7.4 实测，编辑器内复核过：
12 顶点 / 6 三角，包围盒 min=(-50,-86.6,0) max=(100,86.6,100)）。
**这一条不是靠材质解决的** —— `CSHouseVine::BuildBaseMesh` 在建基础网格快照时
现补法线（相邻面法线累加 = 棱柱的逐面平法线）与柱面 UV（绕长度轴展开）。
所以这里的材质可以像任何常规材质一样直接采 UV0，不需要三平面、不需要自定义顶点工厂。

--- 三季叶：三张贴图 + 一个标量，不是三份材质资产 --------------------------------
`MI_{summer,autumn,winter}_ivy_leaf_color` 三张齐全（对照文档 §7 实测）。这里的落地做法是
**一张母材质里三张贴图按 `Season` 标量混**，`ACSHouseActor` 缓存一个 MID 只写那个标量。

为什么不做成三份材质实例、按季节换 `VineLeafMaterial`：换资产 = 在实例路上换一次材质绑定，
而季节是会被反复来回切的量；标量走 MID，连 `InstanceMaterial` 指针都不动。代价是三张贴图
恒定采样（叶子是小三角，这个代价不值一提）。

⚠️ **`Season` 这个参数名是与 C++ 的硬约定**：`ACSHouseActor::EnsureVineComponents` 写的就是
这个名字，改名的话季节会**静默**停在夏天（`SetScalarParameterValue` 对不存在的参数不报错）。

--- 🆕 又一条"静默换默认材质"（2026-08-31 现场踩到，第三条）-----------------------
**母材质编译失败 ⇒ 引擎照样静默换默认材质**，与"没勾 `bUsedWithInstancedStaticMeshes`"、
"根本没绑材质"三者**逐像素同症状**：叶子整片变成暗蓝黑，而 `GetVineUndrawableReason()`
一声不吭（它查的是资产上的标志，不是编译结果）。

现场经过：三季混合用 `MaterialExpressionClamp` 做 saturate，`connect_material_expressions`
到它的 `Input` 针脚**返回 False 但不抛异常**，材质因此带着 `(Node Clamp) Missing Clamp input`
编译失败，日志里只有一条 `LogMaterial: Warning ... Default Material will be used in game`。

两条修法都做了，缺一不可：
  ① 供给侧：所有连线一律走 `link()`，**检查返回值**（MEL 那一族函数返回 bool，失败不抛）；
     saturate 改用 `Min`/`Max` —— 它们的针脚是 A/B，是这一族里最没有歧义的两个。
  ② 消费侧：`ACSHouseActor::GetVineUndrawableReason()` 新增"母材质编译不出来"这一环。

⚠️ 与 `TinyGladeMakeStoneMaterial.py` 的取舍**不同**且是有意的：石阶用三平面，因为
`stairs_step` 是被逐实例**非均匀**缩放的单位立方体，跟 UV 走的贴图会被拉成条；
藤蔓的 UV 是我们自己按长度轴生成的柱面展开，缩放沿的正是那条轴，不会被拉花。

⚠️ 脚本把值烘进 **BP CDO 与关卡实例两处**：只改 CDO，关卡里已存在的实例不会变。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript=".../TinyGladeMakeIvyMaterials.py" \\
        -unattended -nopause -abslog=<独立日志>

日志自诊断：最后一行是 `IVY SETUP OK` 或 `IVY SETUP FAILED`。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

BRANCH_MESH = "/Game/TinyGlade/Meshes/ivy_branch/StaticMeshes/ivy_branch"
LEAF_MESH = "/Game/TinyGlade/Meshes/ivy_leaf/StaticMeshes/ivy_leaf"
FLOWER_MESH = "/Game/TinyGlade/Meshes/ivy_flower/StaticMeshes/ivy_flower"
BRANCH_TEX = "/Game/TinyGlade/Textures/ivy_branch_color"
FLOWER_TEX = "/Game/TinyGlade/Textures/ivy_flower_color"
# 顺序即 `ECSVineSeason` 的取值（0 夏 / 1 秋 / 2 冬）—— C++ 那边写的就是这个枚举的整数值。
SEASON_TEX = ("/Game/TinyGlade/Textures/summer_ivy_leaf_color",
              "/Game/TinyGlade/Textures/autumn_ivy_leaf_color",
              "/Game/TinyGlade/Textures/winter_ivy_leaf_color")

FAILURES = []


def log(msg):
    unreal.log("IVY %s" % msg)


def fail(msg):
    FAILURES.append(msg)
    unreal.log_error("IVY !! %s" % msg)


def link(src, out_name, dst, in_name):
    """
    连一条线并**检查返回值**。

    ⚠️ `MaterialEditingLibrary` 这一族函数针脚名写错时**返回 False、不抛异常**，
    材质于是带着编译错误存盘，引擎再**静默换成默认材质** —— 现场吃过一次，见文件头。
    """
    if not MEL.connect_material_expressions(src, out_name, dst, in_name):
        fail("连线失败：%s -> %s.%s（针脚名八成写错了）"
             % (src.get_class().get_name(), dst.get_class().get_name(), in_name))
        return False
    return True


def link_property(src, out_name, mat_property, what):
    if not MEL.connect_material_property(src, out_name, mat_property):
        fail("接不到材质属性：%s（%s）" % (what, src.get_class().get_name()))
        return False
    return True


def per_instance_tint(mat, x, y, lo, hi):
    """
    逐实例的明暗微差。**这一项依赖"随机数是身份哈希"那条纪律**：
    `CSHouseVine` 的 `Random01` 由 (墙号, 藤号, 段号, 种子) 算出，同一段藤恒等；
    取 `InterlockedAdd` 槽位的话，房子每重建一次全场藤就换一次颜色 —— 而且**不会有任何
    断言报红**（S1 在石阶上栽过一次）。这张材质就是那条纪律的消费者。

    ⚠️ 读的是 `PerInstanceRandom + VertexColor.A`，不是裸的 `PerInstanceRandom`（裁决六 ③）：
    烘成 StaticMesh 之后**没有实例了**，`PerInstanceRandom` 恒等于 0，这一支会塌成
    `lerp(lo, hi, 0)`，整墙的藤烘出来是同一个色 —— 不报错、不掉三角，只有像素看得见。
    两条路互斥地为零（实例路上传基础网格时 A 被清零，烘焙路把随机数写进每个实例全部顶点的 A），
    相加之后取值逐位相同。通道字典在 `CSGpuInstancedMeshComponent` 类注释。
    """
    rnd_live = MEL.create_material_expression(mat, unreal.MaterialExpressionPerInstanceRandom, x - 220, y)
    vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, x - 220, y + 60)
    rnd = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, x, y)
    link(rnd_live, "", rnd, "A")
    link(vcol, "A", rnd, "B")
    scale = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, x + 250, y)
    a = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y + 120)
    a.set_editor_property("r", lo)
    b = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y + 180)
    b.set_editor_property("r", hi)
    link(a, "", scale, "A")
    link(b, "", scale, "B")
    link(rnd, "", scale, "Alpha")
    return scale


def season_blend(mat, tex_paths, fallback_rgb, name):
    """
    三张季节贴图按 `Season` 标量混：lerp(summer, autumn, sat(t)) 再 lerp(·, winter, sat(t−1))。

    ⚠️ 参数名 `Season` 与 C++ 是硬约定（见文件头）。
    ⚠️ 贴图缺一张就整条退回常数色而不是留个黑洞：缺贴图时藤仍然该是绿的，
       一片洋红或一片黑会让出图那条"藤覆盖处平均亮度"的判据读出完全相反的结论。
    """
    samples = []
    for index, path in enumerate(tex_paths):
        tex = unreal.EditorAssetLibrary.load_asset(path)
        if not tex:
            fail("贴图缺失 %s，%s 退回常数色" % (path, name))
            const = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -1100, 0)
            const.set_editor_property("constant", fallback_rgb)
            return const
        node = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D,
                                              -1100, index * 240)
        node.set_editor_property("parameter_name", ("SummerTex", "AutumnTex", "WinterTex")[index])
        node.set_editor_property("texture", tex)
        samples.append(node)

    season = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1100, -260)
    season.set_editor_property("parameter_name", "Season")
    season.set_editor_property("default_value", 0.0)

    def sat(node, offset, y):
        """saturate(node − offset)。

        ⚠️ **不用 `MaterialExpressionClamp`**：它的针脚名接不上（`connect_material_expressions`
        返回 False 不抛异常），材质会带着 `Missing Clamp input` 编译失败、被静默换成默认材质。
        `Min`/`Max` 的针脚是 A/B，与算术节点同一套，最没有歧义。
        """
        src = node
        if offset:
            sub = MEL.create_material_expression(mat, unreal.MaterialExpressionSubtract, -900, y)
            sub.set_editor_property("const_b", float(offset))
            link(node, "", sub, "A")
            src = sub
        lo = MEL.create_material_expression(mat, unreal.MaterialExpressionMax, -800, y)
        lo.set_editor_property("const_b", 0.0)
        link(src, "", lo, "A")
        hi = MEL.create_material_expression(mat, unreal.MaterialExpressionMin, -700, y)
        hi.set_editor_property("const_b", 1.0)
        link(lo, "", hi, "A")
        return hi

    first = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -520, 0)
    link(samples[0], "", first, "A")
    link(samples[1], "", first, "B")
    link(sat(season, 0, -300), "", first, "Alpha")

    second = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -380, 0)
    link(first, "", second, "A")
    link(samples[2], "", second, "B")
    link(sat(season, 1, -140), "", second, "Alpha")
    return second


def build_material(name, tex_path, fallback_rgb, roughness, tint_lo, tint_hi, two_sided_foliage,
                   season_textures=None):
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
    # 藤是薄片状的东西，背面必须画 —— 不然从墙的斜侧看过去叶子会整片消失。
    mat.set_editor_property("two_sided", True)
    if two_sided_foliage:
        # 叶子用双面植被：光能透过叶片，背光那一侧才不是死黑（TG 的 M_TG_LeafCards 同一档）。
        mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)

    if season_textures:
        color_source = season_blend(mat, season_textures, fallback_rgb, name)
    else:
        tex = unreal.EditorAssetLibrary.load_asset(tex_path)
        if tex:
            sample = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -700, 0)
            sample.set_editor_property("texture", tex)
            color_source = sample
        else:
            # 退回常数而不是让脚本挂掉：**贴图缺失时藤仍然该是绿的**，一片洋红或一片灰会让
            # 出图的像素判据读出完全错误的结论。缺失本身进 FAILURES，日志里报红。
            fail("贴图缺失 %s，%s 退回常数色" % (tex_path, name))
            const = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -700, 0)
            const.set_editor_property("constant", fallback_rgb)
            color_source = const

    tint = per_instance_tint(mat, -700, 400, tint_lo, tint_hi)
    mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, 0)
    link(color_source, "", mul, "A")
    link(tint, "", mul, "B")
    link_property(mul, "", unreal.MaterialProperty.MP_BASE_COLOR, "BaseColor")

    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -250, 260)
    rough.set_editor_property("r", roughness)
    link_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS, "Roughness")

    if two_sided_foliage:
        # 透射色：叶子被背光穿透时的颜色，压暗一档的同色系。
        sss = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -250, 380)
        sss.set_editor_property("constant", unreal.LinearColor(0.16, 0.28, 0.08, 1.0))
        try:
            MEL.connect_material_property(sss, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
        except Exception as exc:
            unreal.log_warning("IVY 透射色接不上（不致命）：%s" % exc)

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    log("built %s (%s)" % (name, "三季混合" if season_textures else ("贴图 " + str(tex_path))))
    return mat


branch_mesh = unreal.EditorAssetLibrary.load_asset(BRANCH_MESH)
leaf_mesh = unreal.EditorAssetLibrary.load_asset(LEAF_MESH)
flower_mesh = unreal.EditorAssetLibrary.load_asset(FLOWER_MESH)
if not branch_mesh:
    fail("没有 %s" % BRANCH_MESH)
if not leaf_mesh:
    fail("没有 %s" % LEAF_MESH)
if not flower_mesh:
    fail("没有 %s" % FLOWER_MESH)

# 把实测到的长度轴打进日志：`CSHouseVine::BuildBaseMesh` 的换轴参数（枝 = 2、叶 = 1、花 = 2）
# 就是按这几行来的，资产一换必须先复核这几行再改 C++。
# 实测（2026-08-31）：ivy_branch (150.0, 173.2, 100.0) 长度在 +Z；ivy_leaf (66.7, 70.0, 21.5)
# 长度在 +Y；ivy_flower (76.0, 78.0, 38.4)、底面在 Z=0 ⇒ 簇沿 +Z 张开，换轴取恒等。
for mesh in (branch_mesh, leaf_mesh, flower_mesh):
    if not mesh:
        continue
    box = mesh.get_bounding_box()
    size = box.max - box.min
    log("mesh %-12s min=(%.1f, %.1f, %.1f) size=(%.1f, %.1f, %.1f)"
        % (mesh.get_name(), box.min.x, box.min.y, box.min.z, size.x, size.y, size.z))

branch_mat = build_material("M_TinyGladeIvyBranch", BRANCH_TEX,
                            unreal.LinearColor(0.19, 0.15, 0.10, 1.0), 0.85, 0.80, 1.15, False)
leaf_mat = build_material("M_TinyGladeIvyLeaf", None,
                          unreal.LinearColor(0.10, 0.24, 0.06, 1.0), 0.72, 0.72, 1.20, True,
                          season_textures=SEASON_TEX)
# 花也走双面植被：花瓣与叶片一样薄，背光那一侧不该是死黑。
flower_mat = build_material("M_TinyGladeIvyFlower", FLOWER_TEX,
                            unreal.LinearColor(0.62, 0.58, 0.30, 1.0), 0.60, 0.85, 1.15, True)

VINE_PROPS = (
    ("bVineEnabled", True),
    ("VineBranchMesh", branch_mesh),
    ("VineLeafMesh", leaf_mesh),
    ("VineBranchMaterial", branch_mat),
    ("VineLeafMaterial", leaf_mat),
    ("VineFlowerMesh", flower_mesh),
    ("VineFlowerMaterial", flower_mat),
    ("VineSeason", unreal.CSVineSeason.SUMMER),
    # 默认值的依据都写在 `CSHouseActor.h` 的字段注释里；这里只把它们烘进资产。
    ("VineStrandSpacing", 90.0),
    ("VineSegmentLength", 26.0),
    ("VineMaxSegments", 22),
    ("VineBloat", 1.15),
    ("VineThickness", 9.0),
    ("VineLeafSize", 26.0),
    ("VineLeafChance", 0.72),
    ("VineFlowerChance", 0.10),
    ("VineFlowerSize", 22.0),
    ("VineJumpChance", 0.5),
    ("bVineClimbGable", True),
)

bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeHouse" % PKG)
if bp:
    cdo = unreal.get_default_object(bp.generated_class())
    for name, value in VINE_PROPS:
        try:
            cdo.set_editor_property(name, value)
        except Exception as exc:
            # ⚠️ UE Python 对**不存在的属性抛异常**，脚本会从这一行起整段不执行，
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
        for name, value in VINE_PROPS:
            try:
                a.set_editor_property(name, value)
            except Exception as exc:
                fail("%s 写不进 %s：%s" % (a.get_actor_label(), name, exc))
        a.call_method("RebuildHouse")
        # ⚠️ 坑 ⑩：一律调 `get_vine_undrawable_reason()`，**不要** `is_vine_drawable()` ——
        # UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值，不可画时拿到 `None`，
        # `str(None) == "None"` 会伪造出一句像模像样的原因，恰好在唯一需要它的时候失效。
        reason = str(a.get_vine_undrawable_reason())
        drawable = (reason == "")
        log("%s vine branches=%d leaves=%d flowers=%d drawable=%s%s"
            % (a.get_actor_label(), a.get_vine_segment_count(), a.get_vine_leaf_count(),
               a.get_vine_flower_count(), drawable, "" if drawable else (" —— " + reason)))
        if not drawable:
            fail("%s 的藤画不出来：%s" % (a.get_actor_label(), reason))
        houses += 1
    unreal.EditorLoadingAndSavingUtils.save_current_level()

if houses == 0:
    fail("演示关卡里一栋房子都没找到")

unreal.log("IVY SETUP %s (%d houses, %d failures)"
           % ("OK" if not FAILURES else "FAILED", houses, len(FAILURES)))
