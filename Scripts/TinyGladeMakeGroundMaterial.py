# -*- coding: utf-8 -*-
"""原位重建 TG 三层地面：草地 → 深色土 → 浅色土路。

依据 _terrain_editing_floor PS 第 422–454、520 行：三张颜色图加一张
高度混合图；不同 smoothstep 区间产生深色路缘，高度低谷露出深色土斑。
_paths_path_mask_visual CS 最后一行的 dirt_mask 是各类路径遮罩的 max。
本项目目前只有 R 通道土路，因此 Road 与 DirtMask 都由 VertexColor.R 提供。

UV0 = 地面局部 XY / UVWorldPeriod（默认 500 cm），等价于 TG 米坐标 * 0.2。
再乘原版各层的 0.8 / 1.2 / 0.5 / 0.4，不能让四张图共用同一个平铺频率。
提取清单中 dirtpath_heightmap 是 BC7_SRGB；保留其 sRGB 采样，不按名字改色域。
草地图必须用 dirtpath_grass；grass_patch_summer 是带白色透明边框的草叶卡片。

保留材质与实例资产身份、已有 Grass/Dirt 参数覆盖，不切换或保存关卡。
详细证据和用户参考图见 Docs/TinyGlade/CSGroundMaterial.md。
"""
import unreal

PKG = '/PCGPlugins/HouseTest'
TEX = PKG + '/TinyGladeAsset/Textures'
MEL = unreal.MaterialEditingLibrary

GROUND_HLSL = '''
struct TGGroundLayers
{
    float HeightWeight(float mask, float height)
    {
        float t = saturate(sqrt(saturate(mask)) * 0.6);
        float grassHeight = 0.15 * (1.0 - t);
        float dirtHeight = height * t;
        float cutoff = max(grassHeight, dirtHeight) - 0.1;
        float dirt = max(dirtHeight - cutoff, 0.0);
        return dirt / max(max(grassHeight - cutoff, 0.0) + dirt, 1e-6);
    }
};
TGGroundLayers layers;
float road = layers.HeightWeight(Road, Height);
float soil = layers.HeightWeight(DirtMask, Height);
float soilAlpha = smoothstep(0.2, 0.65, soil);
float pathAlpha = smoothstep(0.6, 1.0, road);
// 原版先铺深色土，再覆盖浅土；低高度区域会从浅土中露出深土。
return lerp(lerp(Grass, DarkDirt * 1.1, soilAlpha), Dirt * 1.1, pathAlpha);
'''


def build():
    layers = [('Grass', 'dirtpath_grass', 0.8),
              ('DarkDirt', 'dirtpath_2', 1.2),
              ('Dirt', 'dirtpath_1', 0.5),
              ('GroundBlendHeight', 'dirtpath_heightmap', 0.4)]
    textures = {name: unreal.load_asset(TEX + '/' + asset) for name, asset, _ in layers}
    for name, texture in textures.items():
        if not texture:
            raise RuntimeError('Missing TG ground texture: ' + name)
        if not texture.get_editor_property('srgb'):
            raise RuntimeError('Expected original BC7_SRGB sampling: ' + texture.get_path_name())

    mat = unreal.load_asset(PKG + '/M_TinyGladeGround')
    if not mat:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            'M_TinyGladeGround', PKG, unreal.Material, unreal.MaterialFactoryNew())
    # 本版本批量删节点一次只移除部分节点；循环清空，但不删除材质资产本身。
    for _ in range(16):
        if MEL.get_num_material_expressions(mat) == 0:
            break
        MEL.delete_all_material_expressions(mat)
    if MEL.get_num_material_expressions(mat):
        raise RuntimeError('Could not clear the ground graph')
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)

    def node(cls, x, y):
        return MEL.create_material_expression(mat, cls, x, y)

    uv = node(unreal.MaterialExpressionTextureCoordinate, -1100, 0)
    uv.set_editor_property('coordinate_index', 0)
    samples = {}
    for i, (name, _, scale) in enumerate(layers):
        tiling = node(unreal.MaterialExpressionMultiply, -870, i * 230)
        tiling.set_editor_property('const_b', scale)
        assert MEL.connect_material_expressions(uv, '', tiling, 'A')
        sample = node(unreal.MaterialExpressionTextureSampleParameter2D, -620, i * 230)
        sample.set_editor_property('parameter_name', name)
        sample.set_editor_property('texture', textures[name])
        sample.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        assert MEL.connect_material_expressions(tiling, '', sample, 'UVs')
        samples[name] = sample
    vertex = node(unreal.MaterialExpressionVertexColor, -620, 970)
    blend = node(unreal.MaterialExpressionCustom, -220, 50)
    blend.set_editor_property('description', 'TG Ground: grass, dark soil, light path')
    blend.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    blend.set_editor_property('code', GROUND_HLSL)
    inputs = []
    for name in ['Grass', 'DarkDirt', 'Dirt', 'Height', 'Road', 'DirtMask']:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        inputs.append(item)
    blend.set_editor_property('inputs', inputs)
    for name in ['Grass', 'DarkDirt', 'Dirt']:
        assert MEL.connect_material_expressions(samples[name], 'RGB', blend, name)
    assert MEL.connect_material_expressions(samples['GroundBlendHeight'], 'R', blend, 'Height')
    for name in ['Road', 'DirtMask']:
        assert MEL.connect_material_expressions(vertex, 'R', blend, name)
    assert MEL.connect_material_property(blend, '', unreal.MaterialProperty.MP_BASE_COLOR)

    rough = node(unreal.MaterialExpressionConstant, -220, 320)
    rough.set_editor_property('r', 0.95)
    assert MEL.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    assert unreal.EditorAssetLibrary.save_loaded_asset(mat)

    # 补齐已存在的地面实例参数；旧 Grass/Dirt 覆盖值保留。
    mi = unreal.load_asset(PKG + '/MI_TinyGladeGround')
    if mi and mi.get_editor_property('parent') == mat:
        for name in ['DarkDirt', 'GroundBlendHeight']:
            MEL.set_material_instance_texture_parameter_value(mi, name, textures[name])
        MEL.update_material_instance(mi)
        assert unreal.EditorAssetLibrary.save_loaded_asset(mi)
    unreal.log('TG GROUND THREE LAYERS SAVED: ' + mat.get_path_name())
    return mat


if __name__ == '__main__':
    build()
