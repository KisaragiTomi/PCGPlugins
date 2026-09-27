"""Create or reload the dedicated stair/terrace connection demo and capture it."""
import math
from pathlib import Path
import traceback
import unreal

PKG = '/PCGPlugins/HouseTest'
MAP = PKG + '/L_StairsTerraceConnectionDemo'
OUT = Path(unreal.Paths.project_saved_dir()) / 'StairsConnections_20260920'
OUT.mkdir(exist_ok=True)
STATE = {'ticks': 0, 'rigs': []}


def look_at(a, b):
    d = b-a
    return unreal.Rotator(pitch=math.degrees(math.atan2(d.z, math.hypot(d.x,d.y))),
                          yaw=math.degrees(math.atan2(d.y,d.x)), roll=0)


def setup():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(MAP):
        assert levels.load_level(MAP)
        by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
        house, wood, stone = [by_label[k] for k in ('Connected_Terrace', 'Stairs_Wood_Connected', 'Stairs_Stone_Connected')]
        assert wood.get_terrace_connection_count() == 1
        assert stone.get_terrace_connection_count() == 1
        assert house.get_terrace_entrance_count() == 2
        unreal.log('STAIRS CONNECTION RELOAD OK')
    else:
        assert levels.new_level(MAP)
        ground = actors.spawn_actor_from_class(unreal.CSGroundActor, unreal.Vector(0,0,-1))
        ground.get_tiny_glade_mesh_component().set_visibility(False)
        floor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0,0,-30))
        floor.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube'))
        floor.set_actor_scale3d(unreal.Vector(45,45,0.5))
        floor.static_mesh_component.set_material(0, unreal.load_asset('/Engine/BasicShapes/BasicShapeMaterial'))
        bp = unreal.load_asset(PKG + '/BP_TinyGladeHouse')
        house = actors.spawn_actor_from_class(bp.generated_class(), unreal.Vector())
        house.set_actor_label('Connected_Terrace')
        house.set_editor_property('footprint_size', unreal.Vector2D(700,550))
        house.set_editor_property('wall_height', 330)
        house.set_editor_property('roof_pitch', 0)
        house.rebuild_house()
        stairs = []
        for name, style, width, points in (
                ('Stairs_Wood_Connected', unreal.CSStairsRailing.WOODEN, 180,
                 [(-360,-1100,15),(-230,-670,160),(-80,-295,325)]),
                ('Stairs_Stone_Connected', unreal.CSStairsRailing.LOW, 150,
                 [(370,75,330),(650,150,175),(1050,300,15)])):
            stair = actors.spawn_actor_from_class(unreal.CSStairsActor, unreal.Vector())
            stair.set_actor_label(name)
            stair.set_editor_property('width', width)
            stair.set_editor_property('railing', style)
            stair.get_spline().set_spline_points([unreal.Vector(*p) for p in points], unreal.SplineCoordinateSpace.WORLD, True)
            stair.rebuild_stairs()
            assert stair.get_terrace_connection_count() == 1, name
            stairs.append(stair)
        wood, stone = stairs
        for yaw, intensity, shadows in ((-40,6,True),(45,2,False),(135,2,False),(225,2,False)):
            sun = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0,0,1500), unreal.Rotator(pitch=-45, yaw=yaw, roll=0))
            sun.light_component.set_editor_property('intensity', intensity)
            sun.light_component.set_editor_property('cast_shadows', shadows)
        sky = actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0,0,800))
        sky.light_component.set_editor_property('intensity', 1.0)
        actors.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector())

    assert house.get_terrace_entrance_count() == 2
    house.push_roof_height(160, True)
    assert wood.get_terrace_connection_count() == 0 and stone.get_terrace_connection_count() == 0
    assert house.get_terrace_entrance_count() == 0
    house.push_roof_height(-160, True)
    assert wood.get_terrace_connection_count() == 1 and stone.get_terrace_connection_count() == 1
    assert house.get_terrace_entrance_count() == 2
    wood.set_actor_location(unreal.Vector(-1500,0,0), False, False)
    assert wood.get_terrace_connection_count() == 0 and house.get_terrace_entrance_count() == 1
    wood.set_actor_location(unreal.Vector(), False, False)
    assert wood.get_terrace_connection_count() == 1 and house.get_terrace_entrance_count() == 2
    levels.save_current_level()
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    for name, loc, target, fov in (
            ('overview', unreal.Vector(1500,-1800,1400), unreal.Vector(150,-220,220), 54),
            ('entrance', unreal.Vector(80,-740,720), unreal.Vector(-80,-255,320), 54),
            ('reverse', unreal.Vector(1050,-500,820), unreal.Vector(365,75,305), 52)):
        rt = unreal.RenderingLibrary.create_render_target2d(world, 1400,1000,unreal.TextureRenderTargetFormat.RTF_RGBA8)
        cap = actors.spawn_actor_from_class(unreal.SceneCapture2D, loc, look_at(loc,target))
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property('texture_target',rt)
        comp.set_editor_property('capture_source',unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property('capture_every_frame',False)
        comp.set_editor_property('capture_on_movement',False)
        comp.set_editor_property('fov_angle',fov)
        pp=unreal.PostProcessSettings()
        for k in ('min','max'):
            pp.set_editor_property('override_auto_exposure_'+k+'_brightness',True)
            pp.set_editor_property('auto_exposure_'+k+'_brightness',1.0)
        pp.set_editor_property('override_dynamic_global_illumination_method',True)
        pp.set_editor_property('dynamic_global_illumination_method',unreal.DynamicGlobalIlluminationMethod.LUMEN)
        comp.set_editor_property('post_process_settings',pp)
        STATE['rigs'].append((name,comp,rt))
    STATE['world']=world
    unreal.log('STAIRS CONNECTION QA READY')


def tick(dt):
    STATE['ticks']+=1
    if STATE['ticks'] < 100:
        return
    try:
        for name,comp,rt in STATE['rigs']:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(STATE['world'],rt,str(OUT),name+'.png')
        unreal.log('STAIRS CONNECTION QA OK')
    except Exception:
        unreal.log_error(traceback.format_exc())
    unreal.unregister_slate_post_tick_callback(STATE['handle'])
    unreal.SystemLibrary.quit_editor()


try:
    setup()
    STATE['handle']=unreal.register_slate_post_tick_callback(tick)
except Exception:
    unreal.log_error(traceback.format_exc())
    unreal.SystemLibrary.quit_editor()
