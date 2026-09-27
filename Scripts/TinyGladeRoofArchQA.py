"""Install/reload the stair BP and VSM roof assets, then capture a dedicated demo."""
import importlib.util
import math
from pathlib import Path
import traceback
import unreal

PKG = '/PCGPlugins/HouseTest'
MAP = PKG + '/L_RoofStairsArchDemo'
OUT = Path(unreal.Paths.project_saved_dir()) / 'RoofStairsArch_20260920'
OUT.mkdir(exist_ok=True)
STATE = {'ticks': 0, 'rigs': []}
RELOAD = '-RoofStairsReload' in unreal.SystemLibrary.get_command_line()


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name+'.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def setup():
    if not RELOAD:
        material = unreal.load_asset(PKG+'/M_TinyGladeRoof')
        material.set_editor_property('used_with_nanite', True)
        unreal.MaterialEditingLibrary.recompile_material(material)
        unreal.EditorAssetLibrary.save_loaded_asset(material, False)
        module('TinyGladeMakeRoofMesh').build(material)
        stair_bp = module('TinyGladeSetupStairsBlueprint').main()
    else:
        stair_bp = unreal.load_asset(PKG+'/BP_TinyGladeStairs')
    assert stair_bp and isinstance(unreal.get_default_object(stair_bp.generated_class()), unreal.CSStairsActor)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(MAP):
        assert levels.load_level(MAP)
        by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
        house, stair = [by_label[n] for n in ('VSM_Roof', 'BP_ArchedStairs')]
    else:
        assert levels.new_level(MAP)
        ground = actors.spawn_actor_from_class(unreal.CSGroundActor, unreal.Vector(0,0,-1))
        ground.get_tiny_glade_mesh_component().set_visibility(False)
        floor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0,0,-30))
        floor.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube'))
        floor.set_actor_scale3d(unreal.Vector(55,55,0.5))
        floor.static_mesh_component.set_material(0,unreal.load_asset('/Engine/BasicShapes/BasicShapeMaterial'))
        house_bp = unreal.load_asset(PKG+'/BP_TinyGladeHouse')
        house = actors.spawn_actor_from_class(house_bp.generated_class(),unreal.Vector(0,1000,0))
        house.set_actor_label('VSM_Roof')
        house.set_editor_property('footprint_size',unreal.Vector2D(900,550))
        house.set_editor_property('wall_height',330)
        house.set_editor_property('roof_pitch',35)
        stair = actors.spawn_actor_from_class(stair_bp.generated_class(),unreal.Vector(0,-500,0))
        stair.set_actor_label('BP_ArchedStairs')
        stair.set_editor_property('width',180)
        stair.get_spline().set_spline_points([unreal.Vector(-700,0,15),unreal.Vector(0,0,435),unreal.Vector(700,0,855)],unreal.SplineCoordinateSpace.LOCAL,True)
        for yaw, intensity, shadows in ((-50,6,True),(40,1.5,False),(140,1.5,False),(230,1.5,False)):
            sun=actors.spawn_actor_from_class(unreal.DirectionalLight,unreal.Vector(0,0,1800),unreal.Rotator(pitch=-32,yaw=yaw,roll=0))
            sun.light_component.set_editor_property('intensity',intensity)
            sun.light_component.set_editor_property('cast_shadows',shadows)
        sky=actors.spawn_actor_from_class(unreal.SkyLight,unreal.Vector(0,0,1000))
        sky.light_component.set_editor_property('intensity',1.0)
        actors.spawn_actor_from_class(unreal.SkyAtmosphere,unreal.Vector())
    house.rebuild_house()
    stair.rebuild_stairs()
    assert stair.get_class() == stair_bp.generated_class()
    assert stair.get_step_count() > 20 and stair.get_brick_count() > 100
    before = stair.get_upload_count()
    stair.rebuild_stairs()
    assert stair.get_upload_count() == before, 'Idle rebuild should not reupload support geometry'
    assert not house.get_roof_tile_undrawable_reason(),house.get_roof_tile_undrawable_reason()
    tile_mesh = house.get_editor_property('roof_tile_mesh')
    tile = next(c for c in house.get_components_by_class(unreal.CSGpuInstancedMeshComponent)
                if c.get_editor_property('base_mesh') == tile_mesh)
    assert tile.is_nanite_render_path(), 'Roof must enter GPU Scene / VSM'
    assert tile.get_editor_property('cast_shadow')
    assert unreal.SystemLibrary.get_console_variable_int_value('r.Shadow.Virtual.Enable') == 1
    # Only VSM can contribute in the comparison captures; do not modify project settings.
    unreal.SystemLibrary.execute_console_command(house, 'r.Shadow.Virtual.ForceOnlyVirtualShadowMaps 1')
    unreal.SystemLibrary.execute_console_command(house, 'r.Shadow.Virtual.Cache 0')
    # Static scene capture does not tick every component like an interactive viewport.
    # Toggle the renderer's child as well as its owner for the explicit off comparison.
    proxies = [c for c in house.get_components_by_class(unreal.StaticMeshComponent)
               if c.get_editor_property('static_mesh') == tile_mesh]
    assert proxies, 'Nanite roof proxy is missing'
    for c in [tile]+proxies:
        unreal.log('ROOF SHADOW FLAGS '+c.get_name()+' cast='+str(c.get_editor_property('cast_shadow'))+
                   ' dynamic='+str(c.get_editor_property('cast_dynamic_shadow')))
        assert c.get_editor_property('cast_dynamic_shadow')
    unreal.log('ROOF VSM PATH OK; STAIR BLUEPRINT RELOAD='+str(RELOAD))
    levels.save_current_level()
    world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    for name,loc,target,fov in (
        ('ridge',unreal.Vector(0,320,660),unreal.Vector(0,1000,465),64),
        ('roof',unreal.Vector(850,300,1000),unreal.Vector(0,1000,450),56),
        ('arches',unreal.Vector(100,-2350,700),unreal.Vector(0,-500,410),53),
        ('stairs',unreal.Vector(1200,-1700,1100),unreal.Vector(150,-500,440),54),
        ('shadow',unreal.Vector(-1100,1300,1000),unreal.Vector(0,1050,120),58)):
        d=target-loc
        rotation=unreal.Rotator(pitch=math.degrees(math.atan2(d.z,math.hypot(d.x,d.y))),yaw=math.degrees(math.atan2(d.y,d.x)),roll=0)
        cap=actors.spawn_actor_from_class(unreal.SceneCapture2D,loc,rotation)
        comp=cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        rt=unreal.RenderingLibrary.create_render_target2d(world,1400,1000,unreal.TextureRenderTargetFormat.RTF_RGBA8)
        for k,v in {'texture_target':rt,'capture_source':unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,'capture_every_frame':False,'capture_on_movement':False,'fov_angle':fov}.items():comp.set_editor_property(k,v)
        pp=unreal.PostProcessSettings()
        for k in ('min','max'):
            pp.set_editor_property('override_auto_exposure_'+k+'_brightness',True)
            pp.set_editor_property('auto_exposure_'+k+'_brightness',1.0)
        pp.set_editor_property('override_dynamic_global_illumination_method',True)
        pp.set_editor_property('dynamic_global_illumination_method',unreal.DynamicGlobalIlluminationMethod.LUMEN)
        comp.set_editor_property('post_process_settings',pp)
        STATE['rigs'].append((name,comp,rt))
    STATE['world'],STATE['tile'],STATE['proxies']=world,tile,proxies


def tick(dt):
    STATE['ticks']+=1
    if STATE['ticks'] not in (100,140): return
    try:
        suffix='' if STATE['ticks']==100 else '-no-tile-shadow'
        for name,comp,rt in STATE['rigs']:
            if suffix and name not in ('roof','ridge','shadow'): continue
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(STATE['world'],rt,str(OUT),name+suffix+'.png')
        if STATE['ticks']==100:
            STATE['tile'].set_cast_shadow(False)
            for c in STATE['proxies']: c.set_cast_shadow(False)
            return
        STATE['tile'].set_cast_shadow(True)
        for c in STATE['proxies']: c.set_cast_shadow(True)
        unreal.log('ROOF ARCH QA PASSED')
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
