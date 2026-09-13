# -*- coding: utf-8 -*-
"""Make the TG grass bend view direction legal in the vertex shader.

CameraVectorWS is a pixel-stage expression in this engine. TG grass VS uses
the normalized vector from a blade vertex to the camera for its bend factor.
Keep the pixel-stage specular CameraVectorWS and all grass coloring unchanged.
"""
import unreal

MEL=unreal.MaterialEditingLibrary
mat=unreal.load_asset('/PCGPlugins/HouseTest/TinyGladeAsset/Materials/M_TG_Grass')
expressions=[o for o in unreal.ObjectIterator(unreal.MaterialExpression)
             if o.get_path_name().startswith(mat.get_path_name()+':')]
targets=[]
for o in expressions:
    if not isinstance(o,unreal.MaterialExpressionCustom): continue
    if o.get_editor_property('description') != 'TG Blade Bend': continue
    names=MEL.get_material_expression_input_names(o)
    inputs=MEL.get_inputs_for_material_expression(mat,o)
    for name,source in zip(names,inputs):
        if name=='CV' and isinstance(source,unreal.MaterialExpressionCameraVectorWS):
            targets.append((o,name))
if targets:
    camera=MEL.create_material_expression(mat,unreal.MaterialExpressionCameraPositionWS,-1600,1700)
    position=MEL.create_material_expression(mat,unreal.MaterialExpressionWorldPosition,-1600,1850)
    position.set_editor_property('world_position_shader_offset',unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    direction=MEL.create_material_expression(mat,unreal.MaterialExpressionCustom,-1300,1750)
    direction.set_editor_property('description','TG vertex-to-camera direction')
    direction.set_editor_property('code','return normalize(Camera - Position);')
    direction.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    ins=[]
    for name in ['Camera','Position']:
        item=unreal.CustomInput()
        item.set_editor_property('input_name',name)
        ins.append(item)
    direction.set_editor_property('inputs',ins)
    assert MEL.connect_material_expressions(camera,'',direction,'Camera')
    assert MEL.connect_material_expressions(position,'',direction,'Position')
    for target,name in targets:
        assert MEL.connect_material_expressions(direction,'',target,name)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
print('TG GRASS CAMERA REPAIR OK: %d bend inputs replaced'%len(targets))
