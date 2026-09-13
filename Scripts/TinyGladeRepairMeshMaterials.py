"""Repair extracted mesh surfaces using the reviewed material/attribute plan.

Run in the UETest574 editor. The plan and original-file backup must already exist
in Saved/TGMeshMaterials. The current editing level is never saved or switched.
"""
from pathlib import Path
import json,unreal

ROOT=Path(unreal.Paths.project_dir()).resolve()
OUT=ROOT/'Saved/TGMeshMaterials'
BASE='/PCGPlugins/HouseTest/TinyGladeAsset'
MAT=BASE+'/Materials';TEX=BASE+'/Textures'
PLAN=json.loads((ROOT/'Plugins/PCGPlugins/Scripts/TinyGladeMeshMaterialAssignments.json').read_text(encoding='utf-8'))
assert (OUT/'active.json').is_file(),'Create the reviewed backup first'
EAL=unreal.EditorAssetLibrary;MEL=unreal.MaterialEditingLibrary
TOOLS=unreal.AssetToolsHelpers.get_asset_tools()
changed=[]

def node(m,cls,**props):
    n=MEL.create_material_expression(m,getattr(unreal,cls),0,0)
    for k,v in props.items():n.set_editor_property(k,v)
    return n
def link(src,dst,pin='',out=''):
    assert MEL.connect_material_expressions(src,out,dst,pin),(src,dst,pin,out)
def output(n,m,prop,pin=''):
    assert MEL.connect_material_property(n,pin,getattr(unreal.MaterialProperty,'MP_'+prop))
def scalar(m,name,value):
    return node(m,'MaterialExpressionScalarParameter',parameter_name=name,default_value=float(value))
def color(m,value):
    return node(m,'MaterialExpressionConstant3Vector',constant=unreal.LinearColor(*value,1))
def multiply(m,a,b,aout='',bout=''):
    n=node(m,'MaterialExpressionMultiply');link(a,n,'A',aout);link(b,n,'B',bout);return n
def sample(m,name,texture):
    tex=unreal.load_asset(TEX+'/'+texture);assert tex,texture
    sampler=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR
    if not tex.get_editor_property('srgb'):
        sampler=(unreal.MaterialSamplerType.SAMPLERTYPE_MASKS if tex.get_editor_property('compression_settings')==unreal.TextureCompressionSettings.TC_MASKS
                 else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    return node(m,'MaterialExpressionTextureSampleParameter2D',parameter_name=name,texture=tex,sampler_type=sampler)
def material(name,foliage=False):
    p=MAT+'/'+name
    m=unreal.load_asset(p) if EAL.does_asset_exist(p) else TOOLS.create_asset(name,MAT,unreal.Material,unreal.MaterialFactoryNew())
    # Some inspected/editor-owned expressions can be rooted by UE/Python. Deleting
    # them triggers !IsRooted() in MarkAsGarbage; wire a new output subgraph instead.
    for k,v in {'use_material_attributes':False,'two_sided':True,'used_with_instanced_static_meshes':True,
                'tangent_space_normal':True,'shading_model':unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE if foliage else unreal.MaterialShadingModel.MSM_DEFAULT_LIT,
                'blend_mode':unreal.BlendMode.BLEND_MASKED if foliage else unreal.BlendMode.BLEND_OPAQUE}.items():m.set_editor_property(k,v)
    output(scalar(m,'Roughness',.86),m,'ROUGHNESS')
    output(color(m,(0,0,0)),m,'EMISSIVE_COLOR')
    return m
def finish(m):
    MEL.layout_material_expressions(m);MEL.recompile_material(m)
    assert EAL.save_loaded_asset(m,False);changed.append(m.get_path_name())
def custom(m,code,inputs):
    pins=[]
    for name in inputs:
        p=unreal.CustomInput();p.set_editor_property('input_name',name);pins.append(p)
    n=node(m,'MaterialExpressionCustom',description='Object-space texture projection',code=code,
           output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,inputs=pins)
    for name,src in inputs.items():link(src,n,name)
    return n

# The shared alpha material used to send colour to emissive on an Unlit surface.
# Preserve Tex / Alpha parameter names, and select the original mask channel.
m=material('M_TG_TextureMasked',True)
tex=sample(m,'Tex','white');mask=sample(m,'Alpha','white')
channel=node(m,'MaterialExpressionLinearInterpolate')
link(mask,channel,'A','R');link(mask,channel,'B','A');link(scalar(m,'MaskUseAlpha',0),channel,'Alpha')
opacity=multiply(m,tex,channel,'A')
output(tex,m,'BASE_COLOR','RGB');output(opacity,m,'OPACITY_MASK')
output(multiply(m,tex,scalar(m,'Transmission',.35),'RGB'),m,'SUBSURFACE_COLOR')
output(scalar(m,'LeafThickness',.6),m,'OPACITY')
m.set_editor_property('opacity_mask_clip_value',.35);finish(m)
bush=unreal.load_asset(MAT+'/MIA_bush')
MEL.set_material_instance_scalar_parameter_value(bush,'MaskUseAlpha',1.)
MEL.update_material_instance(bush);EAL.save_loaded_asset(bush,False);changed.append(bush.get_path_name())

# TG window panes are opaque room/pane cards. Use lit glass colour and highlights,
# retaining this opaque layout and the diamond pane texture.
m=material('M_TG_Glass')
tex=sample(m,'Tex','window_glass');output(tex,m,'BASE_COLOR','RGB')
output(scalar(m,'PaneRoughness',.2),m,'ROUGHNESS')
output(scalar(m,'PaneSpecular',.65),m,'SPECULAR');finish(m)

# Original UV-less meshes require projected colour; UE-generated UV0 can be a
# lightmap unwrap, which is unrelated to the game's procedural coordinates.
m=material('M_TG_MeshProjected')
tex=node(m,'MaterialExpressionTextureObjectParameter',parameter_name='Tex',texture=unreal.load_asset(TEX+'/brick_colors_layer00'))
world=node(m,'MaterialExpressionWorldPosition')
position=node(m,'MaterialExpressionTransformPosition',
    transform_source_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD,
    transform_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL)
link(world,position)
normal=node(m,'MaterialExpressionVertexNormalWS')
local_normal=node(m,'MaterialExpressionTransform',transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD,
    transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_LOCAL)
link(normal,local_normal)
surface=custom(m,
    'float3 w=pow(abs(normalize(N)),4); w/=max(dot(w,1),0.0001); float3 q=P/max(TileCm,1.0);\n'
    'return Texture2DSample(Tex,TexSampler,q.yz).rgb*w.x + Texture2DSample(Tex,TexSampler,q.xz).rgb*w.y + Texture2DSample(Tex,TexSampler,q.xy).rgb*w.z;',
    {'Tex':tex,'P':position,'N':local_normal,'TileCm':scalar(m,'TileCm',140)})
output(surface,m,'BASE_COLOR');finish(m)
for name,params in PLAN['projected_instances'].items():
    p=MAT+'/'+name
    mi=unreal.load_asset(p) if EAL.does_asset_exist(p) else TOOLS.create_asset(name,MAT,unreal.MaterialInstanceConstant,unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi,m)
    texture=unreal.load_asset(TEX+'/'+params['texture']);assert texture,params
    MEL.set_material_instance_texture_parameter_value(mi,'Tex',texture)
    MEL.set_material_instance_scalar_parameter_value(mi,'TileCm',params['tile_cm'])
    MEL.update_material_instance(mi);assert EAL.save_loaded_asset(mi,False);changed.append(mi.get_path_name())

m=material('M_TG_MeshLeafBillboard',True)
tex=sample(m,'Tex','bush_color_summer');mask=sample(m,'CanopyMask','canopy_alpha')
output(tex,m,'BASE_COLOR','RGB');output(mask,m,'OPACITY_MASK','R')
output(multiply(m,tex,scalar(m,'Transmission',.3),'RGB'),m,'SUBSURFACE_COLOR')
m.set_editor_property('opacity_mask_clip_value',.2);finish(m)

for name,tint,roughness in [('M_TG_MeshWater',(.045,.17,.19),.12),
                            ('M_TG_MeshIce',(.31,.46,.48),.22),
                            ('M_TG_MeshDistantFoliage',(.095,.19,.12),.95)]:
    m=material(name)
    output(color(m,tint),m,'BASE_COLOR');output(scalar(m,'SurfaceRoughness',roughness),m,'ROUGHNESS')
    output(scalar(m,'SurfaceSpecular',.6 if 'Foliage' not in name else .2),m,'SPECULAR');finish(m)

# clearing_meadow_plant: part 0 = stem, 1 = leaf, 2 = flower.
# All extracted thistle vertex colours are zero. The shader supplies green and
# a theme flower colour; thistle_flower supplies its alpha silhouette only.
m=material('M_TG_MeshMeadowPlant',True)
green=color(m,(.0843920559,.2200000137,.0725999996))
output(green,m,'BASE_COLOR');output(scalar(m,'SolidMask',1),m,'OPACITY_MASK')
output(multiply(m,green,scalar(m,'Transmission',.3)),m,'SUBSURFACE_COLOR')
output(scalar(m,'LeafThickness',.6),m,'OPACITY');finish(m)
m=material('M_TG_MeshThistleFlower',True)
flower=node(m,'MaterialExpressionVectorParameter',parameter_name='FlowerColor',
    default_value=unreal.LinearColor(.35,.11,.25,1))
tex=sample(m,'FlowerMask','thistle_flower')
output(flower,m,'BASE_COLOR','RGB');output(tex,m,'OPACITY_MASK','A')
output(multiply(m,flower,scalar(m,'Transmission',.25),'RGB'),m,'SUBSURFACE_COLOR')
m.set_editor_property('opacity_mask_clip_value',.5);finish(m)

meshes={}
for a in PLAN['assignments']:
    mesh=meshes.setdefault(a['mesh'],unreal.load_asset(a['mesh']))
    mat=unreal.load_asset(a['new']);assert mat,a
    mesh.set_material(a['slot'],mat)
for mesh in meshes.values():
    assert EAL.save_loaded_asset(mesh,False);changed.append(mesh.get_path_name())

# The generated gallery has explicit component overrides. Update matching old
# overrides as well so it shows the corrected mesh defaults.
gallery=unreal.load_object(None,BASE+'/Maps/TinyGladeGallery.TinyGladeGallery')
replacements={(a['mesh'],a['slot']):a for a in PLAN['assignments']}
override_count=0
for actor in unreal.GameplayStatics.get_all_actors_of_class(gallery,unreal.StaticMeshActor):
    comp=actor.static_mesh_component
    if not comp.static_mesh:continue
    for i,override in enumerate(comp.get_editor_property('override_materials')):
        a=replacements.get((comp.static_mesh.get_path_name(),i))
        if a and override and override.get_path_name()==a['old']:
            comp.set_material(i,unreal.load_asset(a['new']));override_count+=1
if override_count:
    assert unreal.EditorLoadingAndSavingUtils.save_packages([gallery.get_outermost()],False)
    changed.append(gallery.get_path_name())
(OUT/'applied.json').write_text(json.dumps({'ok':True,'assets':changed,'mesh_bindings':len(PLAN['assignments']),
    'gallery_overrides':override_count},indent=2),encoding='utf-8')
print('TG_MESH_MATERIALS_APPLIED',len(changed),'gallery overrides',override_count)
