# -*- coding: utf-8 -*-
"""Repair the TG house material assignments without resetting scene parameters.

Run in the target house level after creating a filesystem asset snapshot.
Only the shared brick graph, wall normal basis, wall relief texture pair and
house finial/pillar bindings are changed. Existing wall clipping stays intact.
"""
import runpy
from pathlib import Path
import unreal

PKG = '/PCGPlugins/HouseTest'
SCRIPTS = Path(unreal.Paths.project_dir()) / 'Plugins/PCGPlugins/Scripts'
MEL = unreal.MaterialEditingLibrary
brick = runpy.run_path(str(SCRIPTS/'TinyGladeMakeBrickMaterial.py'))['build']()
wall_code = runpy.run_path(str(SCRIPTS/'TinyGladeMakeWallMaterials.py'))['NORMAL_HLSL']
wall = unreal.load_asset(PKG+'/M_TinyGladeWall')
expressions = [o for o in unreal.ObjectIterator(unreal.MaterialExpression)
               if o.get_path_name().startswith(wall.get_path_name()+':')]
# Depending on the engine version, expressions belong to the editor-only object.
if not expressions:
    expressions = [o for o in unreal.ObjectIterator(unreal.MaterialExpression)
                   if o.get_path_name().startswith(PKG+'/M_TinyGladeWall.')]
normal = next(o for o in expressions if isinstance(o,unreal.MaterialExpressionCustom)
              and o.get_editor_property('description') == 'TGWallNormal')
vn = next(o for o in expressions if isinstance(o,unreal.MaterialExpressionVertexNormalWS))
normal.set_editor_property('code',wall_code)
inputs = list(normal.get_editor_property('inputs'))
if 'N' not in [str(i.get_editor_property('input_name')) for i in inputs]:
    n_input=unreal.CustomInput()
    n_input.set_editor_property('input_name','N')
    inputs.append(n_input)
    normal.set_editor_property('inputs',inputs)
assert MEL.connect_material_expressions(vn,'',normal,'N')
wall.set_editor_property('tangent_space_normal',False)
MEL.recompile_material(wall)
unreal.EditorAssetLibrary.save_loaded_asset(wall)

mi=unreal.load_asset(PKG+'/MI_TinyGladeWall')
for pname,tname in [('BrickNormal','stone_floor_2_normal'),('BrickHeight','stone_floor_2_height')]:
    texture=unreal.load_asset(PKG+'/TinyGladeAsset/Textures/'+tname)
    if not texture: raise RuntimeError('Missing texture '+tname)
    MEL.set_material_instance_texture_parameter_value(mi,pname,texture)
MEL.set_material_instance_scalar_parameter_value(mi,'ProtrudeLevel',1.06)
MEL.update_material_instance(mi)
unreal.EditorAssetLibrary.save_loaded_asset(mi)

def apply(house):
    house.set_editor_property('FrameMaterial',brick)
    house.set_editor_property('PillarMaterial',brick)
    house.set_editor_property('RoofFinialMaterial',brick)
    house.set_editor_property('PillarYawJitter',0.035)

bp=unreal.load_asset(PKG+'/BP_TinyGladeHouse')
apply(unreal.get_default_object(bp.generated_class()))
unreal.EditorAssetLibrary.save_loaded_asset(bp)
count=0
for house in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if not isinstance(house,unreal.CSHouseActor): continue
    apply(house)
    house.call_method('RebuildHouse')
    count+=1
world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
# SaveCurrentLevel uses the interactive checkout path and can silently return
# false in an unattended editor; SaveMap targets the same, already loaded map.
assert unreal.EditorLoadingAndSavingUtils.save_map(world,world.get_path_name().split('.')[0])
print('TG HOUSE MATERIAL REPAIR OK: %d houses; brick, wall normals, relief pair and finials'%count)
