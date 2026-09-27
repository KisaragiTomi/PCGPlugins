"""Create the authored player stair entry beside BP_TinyGladeHouse/Ground/Window.

Treads, walls and arch rings share TG brick. stairs_step belongs to the separate
terrain stairs system (reverse engineering appendix D §2.2/3/8).
"""
import unreal

PATH = '/PCGPlugins/HouseTest/BP_TinyGladeStairs'


def get_stair_material(save=True):
    """Shared masonry projection with the reference's grey/green stone palette."""
    pkg = '/PCGPlugins/HouseTest'
    mel = unreal.MaterialEditingLibrary
    parent = unreal.load_asset(pkg + '/M_TinyGladeBrick')
    texture = unreal.load_asset(pkg + '/TinyGladeAsset/Textures/brick_colors_layer03')
    assert parent and texture
    parent.set_editor_property('used_with_instanced_static_meshes', True)
    parent.set_editor_property('used_with_nanite', True)
    mel.recompile_material(parent)
    # Finish shader compilation before validation/capture, including Nanite use.
    mel.get_statistics(parent)
    name = 'MI_TinyGladeStairsBrick'
    material = unreal.load_asset(pkg + '/' + name) if unreal.EditorAssetLibrary.does_asset_exist(pkg + '/' + name) else None
    if not material:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, pkg, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    assert material
    mel.set_material_instance_parent(material, parent)
    # UE 5.7's setter returns false even after writing; verify the actual value.
    mel.set_material_instance_texture_parameter_value(material, 'BrickColor', texture)
    assert mel.get_material_instance_texture_parameter_value(material, 'BrickColor') == texture
    mel.update_material_instance(material)
    mel.get_statistics(material)
    if save:
        assert unreal.EditorAssetLibrary.save_loaded_asset(parent, False)
        assert unreal.EditorAssetLibrary.save_loaded_asset(material, False)
    return material


def main():
    bp = unreal.load_asset(PATH) if unreal.EditorAssetLibrary.does_asset_exist(PATH) else None
    if not bp:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property('parent_class', unreal.CSStairsActor)
        bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            'BP_TinyGladeStairs', '/PCGPlugins/HouseTest', unreal.Blueprint, factory)
    assert bp and bp.generated_class(), PATH
    cdo = unreal.get_default_object(bp.generated_class())
    assert isinstance(cdo, unreal.CSStairsActor), 'Existing asset has the wrong parent'
    mesh = unreal.load_asset('/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick')
    wood = unreal.load_asset('/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/wooden_plank')
    masonry = get_stair_material()
    assert mesh and wood and masonry
    cdo.set_editor_property('brick_mesh', mesh)
    # The extracted mesh's gallery material repeats in each scaled brick's local
    # coordinates. Player stairs need the same world-projected masonry as frames.
    cdo.set_editor_property('brick_material', masonry)
    cdo.set_editor_property('wood_mesh', wood)
    cdo.set_editor_property('railing', unreal.CSStairsRailing.WOODEN)
    cdo.set_editor_property('solid_to_ground', True)
    cdo.set_editor_property('arched_support', True)
    cdo.set_editor_property('connect_terraces', True)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    assert unreal.EditorAssetLibrary.save_loaded_asset(bp, False)
    unreal.log('STAIRS BLUEPRINT SAVED ' + PATH)
    return bp


if __name__ == '__main__':
    main()
