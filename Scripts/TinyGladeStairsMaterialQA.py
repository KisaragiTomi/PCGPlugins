"""Compare stair masonry at fixed lighting/cameras; optionally repair generated demos.

Run with -StairsMaterialApply to save the BP default and migrate missing/gallery
overrides in our three demo maps. -StairsMaterialReload validates saved assets
without rebuilding the material. Deliberate material overrides are preserved.
"""
import importlib.util
import json
import math
from pathlib import Path
import traceback
import unreal

PKG = '/PCGPlugins/HouseTest'
OUT = Path(unreal.Paths.project_saved_dir()) / 'StairsMaterial_20260920'
OUT.mkdir(exist_ok=True)
APPLY = '-StairsMaterialApply' in unreal.SystemLibrary.get_command_line()
RELOAD = '-StairsMaterialReload' in unreal.SystemLibrary.get_command_line()
STATE = {'ticks': 0, 'rigs': []}


def path(obj):
    return obj.get_path_name() if obj else None


def repair(actor, material):
    old = actor.get_editor_property('brick_material')
    if old and 'MI_TG_MeshProjected_brick_colors' not in old.get_name():
        return False
    actor.set_editor_property('brick_material', material)
    actor.rebuild_stairs()
    return True


def material_graph(material):
    mel = unreal.MaterialEditingLibrary
    nodes = {}
    def visit(node):
        if not node or node.get_path_name() in nodes:
            return
        item = {'type':node.get_class().get_name()}
        nodes[node.get_path_name()] = item
        for key in ('code','texture','parameter_name','default_value','constant'):
            try:
                value = node.get_editor_property(key)
                item[key] = path(value) if isinstance(value,unreal.Object) else str(value)
            except Exception:
                pass
        inputs = mel.get_inputs_for_material_expression(material,node)
        item['inputs'] = [path(i) for i in inputs]
        for i in inputs:
            visit(i)
    roots = {}
    for key in ('BASE_COLOR','EMISSIVE_COLOR','OPACITY_MASK','ROUGHNESS','MATERIAL_ATTRIBUTES'):
        node = mel.get_material_property_input_node(material,getattr(unreal.MaterialProperty,'MP_'+key))
        roots[key] = path(node)
        visit(node)
    return {'roots':roots,'nodes':nodes,'uses_attributes':material.get_editor_property('use_material_attributes')}


def setup():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    spec = importlib.util.spec_from_file_location('stairs_bp', Path(__file__).with_name('TinyGladeSetupStairsBlueprint.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    material = unreal.load_asset(PKG + '/MI_TinyGladeStairsBrick') if RELOAD else module.get_stair_material(save=False)
    assert material
    parent = material.get_editor_property('parent')
    (OUT / 'material-graph.json').write_text(json.dumps(material_graph(parent),indent=2),encoding='utf-8')
    unreal.log('STAIR MATERIAL STATS '+str(unreal.MaterialEditingLibrary.get_statistics(material)))
    migrations = []
    if APPLY:
        module.main()
        for name in ('L_RoofTerraceStairsDemo', 'L_StairsTerraceConnectionDemo', 'L_RoofStairsArchDemo'):
            assert levels.load_level(PKG + '/' + name)
            changed = False
            for actor in actors.get_all_level_actors():
                if isinstance(actor, unreal.CSStairsActor):
                    changed = repair(actor, material) or changed
                    migrations.append({'map':name, 'actor':actor.get_actor_label(),
                                       'material':path(actor.get_editor_property('brick_material'))})
            if changed:
                assert levels.save_current_level()
    assert levels.load_level(PKG + '/L_RoofStairsArchDemo')
    stair = next(a for a in actors.get_all_level_actors() if a.get_actor_label() == 'BP_ArchedStairs')
    bp = unreal.load_asset(PKG + '/BP_TinyGladeStairs')
    cdo = unreal.get_default_object(bp.generated_class())
    mesh = stair.get_editor_property('brick_mesh')
    components = [c for c in stair.get_components_by_class(unreal.CSGpuInstancedMeshComponent)
                  if c.get_editor_property('base_mesh') == mesh]
    assert components
    report = {'applied': APPLY, 'reloaded':RELOAD, 'actor_material': path(stair.get_editor_property('brick_material')),
              'blueprint_material': path(cdo.get_editor_property('brick_material')),
              'mesh_material': path(mesh.get_material(0)), 'target_material': path(material),
              'resolved_materials': [path(c.get_material(0)) for c in components]}
    report['nanite'] = [c.is_nanite_render_path() for c in components]
    report['parent'] = path(parent)
    report['texture'] = path(unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(material,'BrickColor'))
    report['parent_nanite_usage'] = parent.get_editor_property('used_with_nanite')
    report['migrations'] = migrations
    report['geometry'] = [stair.get_step_count(), stair.get_brick_count()]
    unreal.log('STAIR MATERIAL AUDIT ' + json.dumps(report))
    (OUT / ('reload.json' if RELOAD else ('applied.json' if APPLY else 'before.json'))).write_text(json.dumps(report, indent=2), encoding='utf-8')
    if APPLY or RELOAD:
        assert report['parent_nanite_usage']
        assert report['texture'].endswith('/brick_colors_layer03.brick_colors_layer03')
        assert stair.get_editor_property('brick_material') == material
        assert cdo.get_editor_property('brick_material') == material
        assert all(c.get_material(0) == material for c in components)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    for name, loc, target, fov in (
        ('arches', unreal.Vector(100,-2350,700), unreal.Vector(0,-500,410), 53),
        ('stairs', unreal.Vector(1200,-1700,1100), unreal.Vector(150,-500,440), 54)):
        d = target-loc
        rotation = unreal.Rotator(pitch=math.degrees(math.atan2(d.z, math.hypot(d.x,d.y))),
                                 yaw=math.degrees(math.atan2(d.y,d.x)), roll=0)
        capture = actors.spawn_actor_from_class(unreal.SceneCapture2D, loc, rotation)
        comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
        rt = unreal.RenderingLibrary.create_render_target2d(world,1400,1000,unreal.TextureRenderTargetFormat.RTF_RGBA8)
        for key, value in {'texture_target':rt, 'capture_source':unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,
                           'capture_every_frame':False, 'capture_on_movement':False, 'fov_angle':fov}.items():
            comp.set_editor_property(key,value)
        pp = unreal.PostProcessSettings()
        for key in ('min','max'):
            pp.set_editor_property('override_auto_exposure_'+key+'_brightness',True)
            pp.set_editor_property('auto_exposure_'+key+'_brightness',1.0)
        pp.set_editor_property('override_dynamic_global_illumination_method',True)
        pp.set_editor_property('dynamic_global_illumination_method',unreal.DynamicGlobalIlluminationMethod.LUMEN)
        comp.set_editor_property('post_process_settings',pp)
        STATE['rigs'].append((name,comp,rt))
    STATE.update(world=world, stair=stair, material=material, geometry=report['geometry'])


def tick(dt):
    STATE['ticks'] += 1
    if STATE['ticks'] not in (100,180):
        return
    try:
        suffix = 'after' if APPLY or RELOAD or STATE['ticks'] == 180 else 'before'
        assert [STATE['stair'].get_step_count(),STATE['stair'].get_brick_count()] == STATE['geometry']
        for name, comp, rt in STATE['rigs']:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(STATE['world'],rt,str(OUT),name+'-'+suffix+'.png')
        if STATE['ticks'] == 100 and not (APPLY or RELOAD):
            assert repair(STATE['stair'],STATE['material'])
            return
        unreal.log('STAIR MATERIAL QA PASSED applied='+str(APPLY)+' reload='+str(RELOAD))
    except Exception:
        unreal.log_error(traceback.format_exc())
    unreal.unregister_slate_post_tick_callback(STATE['handle'])
    unreal.SystemLibrary.quit_editor()


try:
    setup()
    STATE['handle'] = unreal.register_slate_post_tick_callback(tick)
except Exception:
    unreal.log_error(traceback.format_exc())
    unreal.SystemLibrary.quit_editor()
