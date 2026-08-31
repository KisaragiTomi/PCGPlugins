# -*- coding: utf-8 -*-
"""给演示关卡的 GPU 石阶补上 TG 那颗 15% 概率的小石子。

`StairPebbleMesh` 与 `StairMesh` 一样，**留空所有断言照绿而画面上什么都没有** ——
这正是石阶网格在两张演示关卡里空了一整轮的那个坑，所以必须由一个脚本显式赋上。

⚠️ 脚本会把值烘进 **BP CDO 与关卡实例两处**（坑表里的那条）：
只改 C++ 默认值、或只改 CDO，关卡里已存在的实例都不会变。

--- 历史 ------------------------------------------------------------------------
这个脚本原来还干第二件事：清空塑形物自持的旧路石阶调色板 `StepMeshes`，让演示关卡只画
**一套**石阶。旧路（`ACSGroundShaperActor::RebuildSteps` + `CSShaperSteps` +
`CSGroundSteps.usf`）已随 2026-08-30「裁决一」第二步整条删除，连同 `StepMeshes` 这个属性 ——
"只画一套"从此是**代码事实**，不再需要脚本去维持一份配置，那几行因此一并删掉。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
# 原件最长轴实测 1.352 m ⇒ TG 的 mix(0.2, 0.4) 就是 27–54 cm，正是 `StairPebbleSize` 的默认值。
PEBBLE_MESH = "/Game/TinyGlade/Meshes/stairs_pebble/StaticMeshes/stairs_pebble"

A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

pebble = unreal.EditorAssetLibrary.load_asset(PEBBLE_MESH)
if not pebble:
    unreal.log_error("DEMOSTEPS FAILED: %s 不存在" % PEBBLE_MESH)
    raise SystemExit

# ⚠️ 石子材质**不在这里选**：石阶材质归 `TinyGladeSetupStairs.py` / `TinyGladeMakeStoneMaterial.py`，
#    两个脚本写同一个属性的话画面就取决于谁后跑（坑表里那条已经在石阶材质上踩过一次）。
#    石子跟石阶同一块石头，直接取关卡里石阶**当前**用的那份，不新增一个属性所有者。
unreal.log("DEMOSTEPS pebble=%s" % pebble)

# ---- BP CDO：新拖进关卡的实例从这里取默认值 ----
ground_bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeGround" % PKG)
if ground_bp:
    cdo = unreal.get_default_object(ground_bp.generated_class())
    cdo.set_editor_property("StairPebbleMesh", pebble)
    cdo.set_editor_property("StairPebbleMaterial", cdo.get_editor_property("StairMaterial"))
    unreal.EditorAssetLibrary.save_loaded_asset(ground_bp)
    unreal.log("DEMOSTEPS ground CDO done")

# ---- 关卡实例：CDO 的默认值**不传播到已存在的实例**，必须逐个再写一份 ----
peppered = 0
for level in ("L_TerrainOpsDemo", "L_HouseGroundDemo"):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    for a in A.get_all_level_actors():
        name = a.get_class().get_name()
        if "Ground" in name and "Shaper" not in name:
            mat = a.get_editor_property("StairMaterial")
            if not mat:
                # 留空 ⇒ 引擎默认面材质（灰白棋盘格）。石子小，不至于毁图，但它是"跑漏了
                # TinyGladeSetupStairs.py"的确切指纹，别让它无声通过。
                unreal.log_warning("DEMOSTEPS %s/%s 石阶材质为空 —— 先跑 TinyGladeSetupStairs.py"
                                   % (level, a.get_actor_label()))
            a.set_editor_property("StairPebbleMesh", pebble)
            a.set_editor_property("StairPebbleMaterial", mat)
            a.call_method("RebuildStairs")
            peppered += 1
            unreal.log("DEMOSTEPS %s/%s pebble mesh + material set" % (level, a.get_actor_label()))
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log("DEMOSTEPS %s saved" % level)

# 自诊断的日志断言（同 STAIRSET / LIGHTING 那几个脚本的口径）：两张关卡各一个地面。
if peppered >= 2:
    unreal.log("DEMOSTEPS OK peppered=%d" % peppered)
else:
    unreal.log_error("DEMOSTEPS FAILED peppered=%d" % peppered)
