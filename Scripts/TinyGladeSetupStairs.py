# -*- coding: utf-8 -*-
"""
给 GPU 石阶补上基础网格与材质 —— 两张演示关卡里它们**一直都是 NULL**。

2026-08-30 实测发现：`Ground_Demo.StairMesh` 与 `StairMaterial` 在
`L_TerrainOpsDemo` / `L_HouseGroundDemo` 里都没被赋过值。S1/S2 的验收全部走 readback 断言
（因为"离屏 capture 画不出 GPU 实例"那条缺陷），而 **readback 证明的是实例变换存在，
对"画的是哪个基础网格"一个字都没说** —— 于是石阶在画面里是一撮黑块，测试却全绿。

这也是那条"截图缺陷"的头号新嫌疑：基础网格为空时，画出来的本来就该是垃圾，
之前几轮却一直在渲染路径里找原因。补上之后重拍即可证伪或坐实。

⚠️ 脚本会把值烘进 **BP CDO 与关卡实例两处**（状态文件坑表里的那条）：
只改 C++ 默认值、或只改 CDO，关卡里已存在的实例都不会变。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
STAIR_MESH = "/Game/TinyGlade/Meshes/stairs_step/StaticMeshes/stairs_step"      # 实测 100×100×100 cm 居中立方体
# ⚠️ 顺序陷阱：`TinyGladeMakeStoneMaterial.py` 会把石阶换成三平面石材 `M_TinyGladeStone`，
#    而本脚本一跑就会把材质覆盖回去。所以这里**优先取石材**，取不到才退回门框砖那份常数灰 ——
#    否则两个脚本谁后跑谁说了算，画面会随执行顺序变，而且两次都"跑成功了"。
STAIR_MAT_CANDIDATES = ["%s/M_TinyGladeStone" % PKG, "%s/M_TinyGladeBrick" % PKG]

mesh = unreal.EditorAssetLibrary.load_asset(STAIR_MESH)
mat = next((m for m in (unreal.EditorAssetLibrary.load_asset(p) for p in STAIR_MAT_CANDIDATES) if m), None)
unreal.log("STAIRSET mesh=%s mat=%s" % (mesh, mat))
if not mesh:
    unreal.log_error("STAIRSET FAILED: %s 不存在" % STAIR_MESH)
    raise SystemExit
if not mat:
    unreal.log_error("STAIRSET FAILED: 石阶材质一个都不存在 %s" % STAIR_MAT_CANDIDATES)
    raise SystemExit


def apply_embed(obj, where):
    """把埋深钉回闭式解 `StairEmbed = StairBlockSize.X / 2`（依据写在 `CSGroundActor.h` 的属性注释里）。

    ⚠️ 不能只改 C++ 默认值：关卡里已存在的实例与 BP CDO 各存了一份，改了默认值它们纹丝不动
       —— 症状是"代码改了、重编了、画面一模一样"。
    ⚠️ 也不能在这里写死一个数：埋深是从**本实例自己的** `StairBlockSize.X` 反算的，
       写死会在有人改块进深的那天悄悄失配（悬空量回到 `(X/2 − e)·坡度 > 0`，石墙重现）。
    """
    try:
        size = obj.get_editor_property("StairBlockSize")
        embed = size.x * 0.5
        obj.set_editor_property("StairEmbed", embed)
        unreal.log("STAIRSET %-28s StairEmbed = %.1f  (= StairBlockSize.X %.0f / 2)" % (where, embed, size.x))
    except Exception as exc:
        unreal.log_warning("STAIRSET %s embed skipped: %s" % (where, exc))

# ---- BP CDO：新拖进关卡的实例从这里取默认值 ----
bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeGround" % PKG)
if bp:
    cdo = unreal.get_default_object(bp.generated_class())
    cdo.set_editor_property("StairMesh", mesh)
    cdo.set_editor_property("StairMaterial", mat)
    apply_embed(cdo, "CDO")
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    unreal.log("STAIRSET CDO done")

# ---- 关卡实例：CDO 的默认值**不传播到已存在的实例**，必须逐个再写一份 ----
A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for level in ("L_TerrainOpsDemo", "L_HouseGroundDemo"):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    n = 0
    for a in A.get_all_level_actors():
        name = a.get_class().get_name()
        if "Ground" not in name or "Shaper" in name:
            continue
        a.set_editor_property("StairMesh", mesh)
        a.set_editor_property("StairMaterial", mat)
        apply_embed(a, "%s/%s" % (level, a.get_actor_label()))
        n += 1
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log("STAIRSET %s -> %d ground actors" % (level, n))

unreal.log("STAIRSET DONE")
