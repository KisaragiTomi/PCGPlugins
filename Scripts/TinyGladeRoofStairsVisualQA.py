"""Regenerate only L_RoofTerraceStairsDemo and capture roof/terrace/stair checks."""
import importlib.util
import math
from pathlib import Path
import traceback
import unreal

PKG = '/PCGPlugins/HouseTest'
OUT = Path(unreal.Paths.project_saved_dir()) / 'RoofStairs_20260920'
OUT.mkdir(exist_ok=True)
STATE = {'ticks': 0, 'handle': None, 'rigs': []}


def look_at(a, b):
    dx, dy, dz = b.x-a.x, b.y-a.y, b.z-a.z
    return unreal.Rotator(pitch=math.degrees(math.atan2(dz, math.hypot(dx, dy))),
                          yaw=math.degrees(math.atan2(dy, dx)), roll=0)


def setup():
    spec = importlib.util.spec_from_file_location('terrace_setup', Path(__file__).with_name('TinyGladeSetupRoofTerrace.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    material = module.build()
    bp = unreal.load_asset(PKG + '/BP_TinyGladeHouse')
    unreal.get_default_object(bp.generated_class()).set_editor_property('FlatRoofMaterial', material)
    unreal.EditorAssetLibrary.save_loaded_asset(bp, False)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(PKG + '/L_RoofTerraceStairsDemo'):
        unreal.EditorAssetLibrary.delete_asset(PKG + '/L_RoofTerraceStairsDemo')
    assert levels.new_level(PKG + '/L_RoofTerraceStairsDemo'), 'Could not create the dedicated validation map'
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.EditorLevelLibrary.get_editor_world()
    ground = actors.spawn_actor_from_class(unreal.CSGroundActor, unreal.Vector(0, 0, -1))
    ground.set_actor_label('ValidationGround')
    floor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, -30))
    floor.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube'))
    floor.set_actor_scale3d(unreal.Vector(45, 40, 0.5))
    floor.static_mesh_component.set_material(0, unreal.load_asset('/Engine/BasicShapes/BasicShapeMaterial'))
    houses = []
    for x, pitch, label in ((-650, 35, 'Roof_Pitched'), (350, 0, 'Roof_Terrace')):
        h = actors.spawn_actor_from_class(bp.generated_class(), unreal.Vector(x, 300, 0))
        h.set_actor_label(label)
        h.set_editor_property('footprint_size', unreal.Vector2D(700, 550))
        h.set_editor_property('wall_height', 330)
        h.set_editor_property('roof_pitch', pitch)
        h.set_editor_property('FlatRoofMaterial', material)
        h.rebuild_house()
        houses.append(h)
    # Live switch exercise uses the same production entry point as the roof handle.
    terrace = houses[1]
    assert terrace.is_flat_roof() and terrace.get_roof_tile_count() == 0
    terrace.push_roof_height(140, True)
    assert not terrace.is_flat_roof() and terrace.get_roof_tile_count() > 0
    terrace.push_roof_height(-140, True)
    assert terrace.is_flat_roof() and terrace.get_roof_tile_count() == 0
    for x, style, label in ((-900, unreal.CSStairsRailing.WOODEN, 'Stairs_Wood'),
                             (-200, unreal.CSStairsRailing.LOW, 'Stairs_Low'),
                             (500, unreal.CSStairsRailing.HIGH, 'Stairs_High')):
        stair = actors.spawn_actor_from_class(unreal.CSStairsActor, unreal.Vector(x, -650, 0))
        stair.set_actor_label(label)
        stair.set_editor_property('Railing', style)
        stair.set_editor_property('width', 150)
        stair.rebuild_stairs()
        assert stair.get_railing_piece_count() > 0
    ladder = actors.spawn_actor_from_class(unreal.CSStairsActor, unreal.Vector(1050, 200, 0))
    ladder.set_actor_label('Ladder_Steep')
    ladder.get_spline().set_spline_points([unreal.Vector(0,0,15), unreal.Vector(100,0,400)], unreal.SplineCoordinateSpace.LOCAL, True)
    ladder.rebuild_stairs()
    assert ladder.get_ladder_rung_count() > 0
    sun = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0,0,1500), unreal.Rotator(pitch=-45, yaw=-40, roll=0))
    sun.light_component.set_editor_property('intensity', 6.0)
    # Soft studio fills make the shaded faces inspectable in offscreen captures.
    for yaw in (45, 135, 225):
        fill = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0,0,1500), unreal.Rotator(pitch=-30, yaw=yaw, roll=0))
        fill.light_component.set_editor_property('intensity', 2.0)
        fill.light_component.set_editor_property('cast_shadows', False)
    sky = actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0,0,800))
    sky.light_component.set_editor_property('intensity', 1.0)
    sky.light_component.set_editor_property('real_time_capture', True)
    actors.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector())
    levels.save_current_level()
    shots = [('overview', unreal.Vector(1700,-2300,1800), unreal.Vector(0,0,170), 52),
             ('roof', unreal.Vector(-1350,-650,1000), unreal.Vector(-650,300,420), 46),
             ('terrace', unreal.Vector(1100,-600,1050), unreal.Vector(350,300,340), 45),
             ('stairs', unreal.Vector(600,-1600,1000), unreal.Vector(0,-500,140), 56)]
    for name, loc, target, fov in shots:
        rt = unreal.RenderingLibrary.create_render_target2d(world, 1400, 1000, unreal.TextureRenderTargetFormat.RTF_RGBA8)
        cap = actors.spawn_actor_from_class(unreal.SceneCapture2D, loc, look_at(loc,target))
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property('texture_target', rt)
        comp.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property('capture_every_frame', False)
        comp.set_editor_property('capture_on_movement', False)
        comp.set_editor_property('fov_angle', fov)
        pp = unreal.PostProcessSettings()
        pp.set_editor_property('override_auto_exposure_min_brightness', True)
        pp.set_editor_property('override_auto_exposure_max_brightness', True)
        pp.set_editor_property('auto_exposure_min_brightness', 1.0)
        pp.set_editor_property('auto_exposure_max_brightness', 1.0)
        pp.set_editor_property('override_dynamic_global_illumination_method', True)
        pp.set_editor_property('dynamic_global_illumination_method', unreal.DynamicGlobalIlluminationMethod.LUMEN)
        comp.set_editor_property('post_process_settings', pp)
        STATE['rigs'].append((name, comp, rt))
    STATE['world'] = world
    unreal.log('ROOF STAIRS QA READY')


def tick(dt):
    try:
        STATE['ticks'] += 1
        if STATE['ticks'] < 100:
            return
        for name, comp, rt in STATE['rigs']:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(STATE['world'], rt, str(OUT), name + '.png')
        (OUT / 'visual_qa_complete.txt').write_text('roof/terrace switch and three railing styles/ladder passed', encoding='utf-8')
        unreal.log('ROOF STAIRS QA OK')
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
