"""Exercise reflected Blueprint functions on the saved stair BP and capture gizmos.

The map stays unchanged; only recompilation of the BP asset is saved. Handles
are transient and their hidden-in-game flag is lifted only for these captures.
"""
import json
import math
from pathlib import Path
import traceback
import unreal

PKG = '/PCGPlugins/HouseTest'
OUT = Path(unreal.Paths.project_saved_dir()) / 'StairsWidthHandle_20260920'
OUT.mkdir(exist_ok=True)
STATE = {'ticks':0}


def setup():
    bp = unreal.load_asset(PKG+'/BP_TinyGladeStairs')
    assert bp
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    assert unreal.EditorAssetLibrary.save_loaded_asset(bp,False)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    assert levels.load_level(PKG+'/L_RoofStairsArchDemo')
    stair = next(a for a in actors.get_all_level_actors() if a.get_actor_label() == 'BP_ArchedStairs')
    assert stair.get_class() == bp.generated_class()
    assert not stair.is_in_resize_mode()
    stair.enter_resize_mode()
    handles = stair.get_resize_handles()
    assert len(handles) == 2
    stair.enter_resize_mode()
    assert handles == stair.get_resize_handles()
    for handle in handles:
        assert handle.get_host() == stair
        handle.get_component_by_class(unreal.StaticMeshComponent).set_hidden_in_game(False)
    width = stair.get_editor_property('width')
    spline = stair.get_spline()
    points = [spline.get_location_at_spline_point(i,unreal.SplineCoordinateSpace.LOCAL)
              for i in range(spline.get_number_of_spline_points())]
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    loc, target = unreal.Vector(1200,-1700,1100), unreal.Vector(150,-500,440)
    d = target-loc
    rot = unreal.Rotator(pitch=math.degrees(math.atan2(d.z,math.hypot(d.x,d.y))),
                        yaw=math.degrees(math.atan2(d.y,d.x)),roll=0)
    capture = actors.spawn_actor_from_class(unreal.SceneCapture2D,loc,rot)
    comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt = unreal.RenderingLibrary.create_render_target2d(world,1400,1000,unreal.TextureRenderTargetFormat.RTF_RGBA8)
    for key,value in {'texture_target':rt,'capture_source':unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,
                      'capture_every_frame':False,'capture_on_movement':False,'fov_angle':54}.items():
        comp.set_editor_property(key,value)
    pp = unreal.PostProcessSettings()
    for key in ('min','max'):
        pp.set_editor_property('override_auto_exposure_'+key+'_brightness',True)
        pp.set_editor_property('auto_exposure_'+key+'_brightness',1.0)
    pp.set_editor_property('override_dynamic_global_illumination_method',True)
    pp.set_editor_property('dynamic_global_illumination_method',unreal.DynamicGlobalIlluminationMethod.LUMEN)
    comp.set_editor_property('post_process_settings',pp)
    STATE.update(stair=stair,handles=handles,width=width,points=points,world=world,comp=comp,rt=rt)


def tick(dt):
    STATE['ticks'] += 1
    if STATE['ticks'] not in (100,180):
        return
    try:
        stair = STATE['stair']
        suffix = 'before' if STATE['ticks'] == 100 else 'after'
        STATE['comp'].capture_scene()
        unreal.RenderingLibrary.export_render_target(STATE['world'],STATE['rt'],str(OUT),'width-'+suffix+'.png')
        if STATE['ticks'] == 100:
            handle = STATE['handles'][0]
            handle.add_actor_world_offset(handle.get_outer_normal_world()*60.0,False,False)
            applied = handle.consume_drag_to_host(True)
            assert abs(applied-60.0) < 0.01, applied
            assert abs(stair.get_editor_property('width')-STATE['width']-120.0) < 0.01
            return
        spline = stair.get_spline()
        for i,point in enumerate(STATE['points']):
            current = spline.get_location_at_spline_point(i,unreal.SplineCoordinateSpace.LOCAL)
            assert (current-point).length() < 0.001
        report = {'blueprint':PKG+'/BP_TinyGladeStairs','handle_count':len(stair.get_resize_handles()),
                  'initial_width':STATE['width'],'drag_cm':60,'final_width':stair.get_editor_property('width'),
                  'centreline_unchanged':True,'material':stair.get_editor_property('brick_material').get_path_name()}
        stair.exit_resize_mode()
        assert not stair.is_in_resize_mode() and not stair.get_resize_handles()
        report['exit_cleaned_handles'] = True
        (OUT/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        unreal.log('STAIRS WIDTH HANDLE QA PASSED '+json.dumps(report))
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
