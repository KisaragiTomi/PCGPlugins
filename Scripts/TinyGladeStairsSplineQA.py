"""Verify saved stair BP follows spline edits on subsequent editor frames.

No RebuildStairs/reevaluate call or getter-triggered flush is used after edits.
The demo map and blueprint remain unchanged on disk.
"""
import json
import math
from pathlib import Path
import traceback
import unreal

PKG = '/PCGPlugins/HouseTest'
OUT = Path(unreal.Paths.project_saved_dir()) / 'StairsSplineLive_20260920'
OUT.mkdir(exist_ok=True)
STATE = {'ticks':0,'samples':[]}


def setup():
    bp = unreal.load_asset(PKG+'/BP_TinyGladeStairs')
    assert bp
    # Deliberately disable construction-on-drag: spline notifications must work.
    bp.set_editor_property('run_construction_script_on_drag',False)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    assert levels.load_level(PKG+'/L_RoofStairsArchDemo')
    stair = next(a for a in actors.get_all_level_actors() if a.get_actor_label() == 'BP_ArchedStairs')
    assert stair.get_class() == bp.generated_class()
    stair.enter_resize_mode()
    for handle in stair.get_resize_handles():
        handle.get_component_by_class(unreal.StaticMeshComponent).set_hidden_in_game(False)
    loc, target = unreal.Vector(1200,-1700,1100), unreal.Vector(150,-500,440)
    d = target-loc
    rot = unreal.Rotator(pitch=math.degrees(math.atan2(d.z,math.hypot(d.x,d.y))),
                        yaw=math.degrees(math.atan2(d.y,d.x)),roll=0)
    capture = actors.spawn_actor_from_class(unreal.SceneCapture2D,loc,rot)
    comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
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
    STATE.update(stair=stair,spline=stair.get_spline(),comp=comp,rt=rt,world=world)


def capture(name):
    STATE['comp'].capture_scene()
    unreal.RenderingLibrary.export_render_target(STATE['world'],STATE['rt'],str(OUT),name+'.png')


def tick(dt):
    STATE['ticks'] += 1
    if STATE['ticks'] not in (100,130,160,190):
        return
    try:
        stair, spline = STATE['stair'], STATE['spline']
        # A query here must not be what triggers the rebuild: the queue must
        # already be drained by the regular editor world tick before we query.
        assert not stair.is_actor_tick_enabled(), 'Editor tick did not drain spline rebuild'
        upload = stair.get_upload_count()
        STATE['samples'].append({'frame':STATE['ticks'],'uploads':upload,'bricks':stair.get_brick_count()})
        if STATE['ticks'] == 100:
            capture('spline-before')
            STATE['original'] = spline.get_location_at_spline_point(1,unreal.SplineCoordinateSpace.LOCAL)
        else:
            assert upload > STATE['samples'][-2]['uploads'], 'Spline edit did not update rendered stairs'
        if STATE['ticks'] < 190:
            amount = (STATE['ticks']-100)//30+1
            spline.set_location_at_spline_point(1,STATE['original']+unreal.Vector(0,-100*amount,20*amount),unreal.SplineCoordinateSpace.LOCAL,True)
            assert stair.is_actor_tick_enabled(), 'Spline notification did not wake stairs'
            return
        capture('spline-after')
        for handle in stair.get_resize_handles():
            assert (handle.get_actor_location()-handle.compute_canonical_world_location()).length() < 0.01
        report = {'blueprint':PKG+'/BP_TinyGladeStairs','construction_on_drag':False,
                  'manual_rebuild_calls':0,'drag_frames':STATE['samples'],'handles_follow':True}
        (OUT/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        unreal.log('STAIRS SPLINE LIVE QA PASSED '+json.dumps(report))
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
