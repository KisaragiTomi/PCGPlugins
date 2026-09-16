"""Install the reference terracotta roof on the BP and current level houses.

Does not switch levels. Save the current map after visual validation.
The source roof_tile / lod1 / backface / roof_spire assets remain available.
"""
import importlib.util
from pathlib import Path
import unreal


def module(name):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m


def main():
    # The extracted roof_tile_damage.png is scaled to 0..76 (see that script); the material needs TG's real mask.
    module('TinyGladeFixBc4Textures').main()
    mat=module('TinyGladeMakeRoofMaterial').build()
    mesh=module('TinyGladeMakeRoofMesh').build(mat)
    bp=unreal.load_asset('/PCGPlugins/HouseTest/BP_TinyGladeHouse');assert bp
    actors=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
            if isinstance(a,unreal.CSHouseActor)]
    # The GPU component snapshots the source once; changing an asset in place
    # needs a different source identity before reassigning the rebuilt tile.
    source=unreal.load_asset('/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/roof_tile')
    for a in actors:
        if a.get_editor_property('RoofTileMesh')==mesh:
            a.set_editor_property('RoofTileMesh',source)
            a.rebuild_house()
    for a in [unreal.get_default_object(bp.generated_class())]+actors:
        for name,value in [('RoofTileMesh',mesh),('RoofMaterial',mat),('bRoofTilesEnabled',True),
            ('bRoofTileSwapAxes',True),('RoofTileThickness',6.0),('RoofFinialMesh',None),('RoofFinialMaterial',None)]:
            a.set_editor_property(name,value)
    unreal.EditorAssetLibrary.save_loaded_asset(bp,False)
    for a in actors:
        a.rebuild_house()
        assert a.get_roof_tile_count()>0 and not a.get_roof_tile_undrawable_reason(),a.get_actor_label()
        assert a.get_roof_finial_count()==0,a.get_actor_label()
        unreal.log('TG ROOF wired '+a.get_actor_label()+' tiles='+str(a.get_roof_tile_count()))
    unreal.log('TG ROOF SETUP OK')


if __name__=='__main__':main()
