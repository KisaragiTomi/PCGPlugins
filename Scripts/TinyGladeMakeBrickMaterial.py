# -*- coding: utf-8 -*-
"""Build the shared masonry material in place, preserving all asset references.

TG wall_brick_lod0 gbuffer PS samples brick_colors; its VS adds a stable per-brick
UV offset. The exported brick has geometry normals, not a floor normal map.
Keep the negative instance-random visibility contract used by CSHouseFrame.usf.
"""
import unreal

PKG = '/PCGPlugins/HouseTest'
TEX = PKG + '/TinyGladeAsset/Textures'
MEL = unreal.MaterialEditingLibrary


def custom_input(name):
    value = unreal.CustomInput()
    value.set_editor_property('input_name',name)
    return value


def build():
    mat = unreal.load_asset(PKG + '/M_TinyGladeBrick')
    if not mat:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            'M_TinyGladeBrick', PKG, unreal.Material, unreal.MaterialFactoryNew())
    texture = unreal.load_asset(TEX + '/brick_colors_layer01')
    if not texture:
        raise RuntimeError('TG brick color texture is missing')
    for _ in range(16):
        if MEL.get_num_material_expressions(mat) == 0:
            break
        MEL.delete_all_material_expressions(mat)
    if MEL.get_num_material_expressions(mat):
        raise RuntimeError('Could not clear old brick material expressions')
    mat.set_editor_property('used_with_instanced_static_meshes', True)
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property('opacity_mask_clip_value', 0.5)

    def node(cls, x, y):
        return MEL.create_material_expression(mat, cls, x, y)

    wp = node(unreal.MaterialExpressionWorldPosition, -1000, 0)
    normal = node(unreal.MaterialExpressionVertexNormalWS, -1000, 150)
    rnd = node(unreal.MaterialExpressionPerInstanceRandom, -1000, 300)
    uv = node(unreal.MaterialExpressionCustom, -730, 0)
    uv.set_editor_property('description', 'TG stone color projection and stable brick variation')
    uv.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    uv.set_editor_property('code', '''
float3 a = abs(N);
float2 p = a.z > max(a.x,a.y) ? WP.xy : (a.x > a.y ? WP.yz : WP.xz);
// TG frame projection: world metres * 0.7 * 0.3, plus seeded UV offset.
return p * 0.0021 + float2(saturate(Random), saturate(Random));
''')
    uv.set_editor_property('inputs', [custom_input(n) for n in ['WP','N','Random']])
    for n,src in [('WP',wp),('N',normal),('Random',rnd)]:
        MEL.connect_material_expressions(src,'',uv,n)
    color = node(unreal.MaterialExpressionTextureSampleParameter2D,-480,0)
    color.set_editor_property('parameter_name','BrickColor')
    color.set_editor_property('texture',texture)
    color.set_editor_property('sampler_type',unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    MEL.connect_material_expressions(uv,'',color,'UVs')
    tint = node(unreal.MaterialExpressionCustom,-200,0)
    tint.set_editor_property('description','Subtle per-brick value variation')
    tint.set_editor_property('code','return Color * lerp(0.90, 1.10, saturate(Random));')
    tint.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    tint.set_editor_property('inputs',[custom_input(n) for n in ['Color','Random']])
    MEL.connect_material_expressions(color,'RGB',tint,'Color')
    MEL.connect_material_expressions(rnd,'',tint,'Random')
    MEL.connect_material_property(tint,'',unreal.MaterialProperty.MP_BASE_COLOR)
    rough = node(unreal.MaterialExpressionScalarParameter,-200,170)
    rough.set_editor_property('parameter_name','StoneRoughness')
    rough.set_editor_property('default_value',0.92)
    MEL.connect_material_property(rough,'',unreal.MaterialProperty.MP_ROUGHNESS)
    visible = node(unreal.MaterialExpressionCustom,-200,320)
    visible.set_editor_property('description','Corner-pier hidden brick sentinel')
    visible.set_editor_property('code','return saturate(Random + 1.0);')
    visible.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    visible.set_editor_property('inputs',[custom_input('Random')])
    MEL.connect_material_expressions(rnd,'',visible,'Random')
    MEL.connect_material_property(visible,'',unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    unreal.log('TG BRICK MATERIAL rebuilt in place: '+mat.get_path_name())
    return mat


if __name__ == '__main__':
    build()
