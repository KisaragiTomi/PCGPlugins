"""Build the stone terrace material and install it on the house BP/current level.

Run in the UE editor after compiling the roof-height changes. Existing roof tile
materials and house dimensions are preserved; saving the level is the caller's choice.
"""
import unreal

PKG = '/PCGPlugins/HouseTest'
MEL = unreal.MaterialEditingLibrary


def build():
    path = PKG + '/M_TinyGladeTerrace'
    mat = unreal.load_asset(path)
    if not mat:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            'M_TinyGladeTerrace', PKG, unreal.Material, unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    mat.set_editor_property('tangent_space_normal', False)
    mat.set_editor_property('used_with_instanced_static_meshes', True)

    def node(cls, **props):
        n = MEL.create_material_expression(mat, getattr(unreal, cls), 0, 0)
        for name, value in props.items():
            n.set_editor_property(name, value)
        return n

    def custom(code, inputs, kind):
        n = node('MaterialExpressionCustom', code=code,
                 output_type=getattr(unreal.CustomMaterialOutputType, 'CMOT_' + kind))
        pins = []
        for name in inputs:
            p = unreal.CustomInput()
            p.set_editor_property('input_name', name)
            pins.append(p)
        n.set_editor_property('inputs', pins)
        for name, source in inputs.items():
            assert MEL.connect_material_expressions(source, '', n, name)
        return n

    def texture(name, sampler):
        asset = unreal.load_asset(PKG + '/TinyGladeAsset/Textures/' + name)
        assert asset, name
        return node('MaterialExpressionTextureObject', texture=asset,
                    sampler_type=getattr(unreal.MaterialSamplerType, 'SAMPLERTYPE_' + sampler))

    wp = node('MaterialExpressionWorldPosition')
    uv = custom('return P.xy / 220.0;', {'P': wp}, 'FLOAT2')
    # The extracted stone-floor family contains height/seed/normal, no albedo.
    # Colour comes from a stone palette; the seed identifies individual paving stones.
    height = texture('stone_floor_height', 'LINEAR_GRAYSCALE')
    seed = texture('stone_floor_seed', 'COLOR')
    normal = texture('stone_floor_normal', 'NORMAL')
    color = custom('''
float H = Texture2DSample(HeightMap, HeightMapSampler, UV).r;
float3 S = Texture2DSample(SeedMap, SeedMapSampler, UV).rgb;
float V = dot(S, float3(0.299, 0.587, 0.114));
float Joint = smoothstep(0.08, 0.3, H);
return lerp(float3(0.16, 0.14, 0.13), float3(0.43, 0.40, 0.35) * lerp(0.8, 1.15, V), Joint);
''', {'UV': uv, 'HeightMap': height, 'SeedMap': seed}, 'FLOAT3')
    normal_ws = custom('return UnpackNormalMap(Texture2DSample(NormalMap, NormalMapSampler, UV));',
                       {'UV': uv, 'NormalMap': normal}, 'FLOAT3')
    rough = node('MaterialExpressionConstant', r=0.9)
    for expression, prop in ((color, 'BASE_COLOR'), (normal_ws, 'NORMAL'), (rough, 'ROUGHNESS')):
        assert MEL.connect_material_property(expression, '', getattr(unreal.MaterialProperty, 'MP_' + prop))
    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat, False)
    return mat


def main():
    mat = build()
    bp = unreal.load_asset(PKG + '/BP_TinyGladeHouse')
    assert bp
    unreal.get_default_object(bp.generated_class()).set_editor_property('FlatRoofMaterial', mat)
    unreal.EditorAssetLibrary.save_loaded_asset(bp, False)
    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if isinstance(actor, unreal.CSHouseActor):
            actor.set_editor_property('FlatRoofMaterial', mat)
            actor.rebuild_house()
    unreal.log('ROOF TERRACE SETUP OK')


if __name__ == '__main__':
    main()
