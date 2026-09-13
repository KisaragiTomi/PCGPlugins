"""Lit roof: native roof_colors layer 0 and 5x5 normal/damage atlas.

Source: _nani_instanced_roof b903e43ffb3da915 VS/PS. The mesh bake supplies
native position-derived UV0 and a top-face vertex R mask. Brightness, weather
amount and seed hashing adapt TG's theme/lighting system to this UE scene.
"""
import unreal
PKG='/PCGPlugins/HouseTest'
TEX=PKG+'/TinyGladeAsset/Textures'
MEL=unreal.MaterialEditingLibrary


def build():
    mat=unreal.load_asset(PKG+'/M_TinyGladeRoof')
    if not mat:
        mat=unreal.AssetToolsHelpers.get_asset_tools().create_asset('M_TinyGladeRoof',PKG,unreal.Material,unreal.MaterialFactoryNew())
    for _ in range(16):
        if not MEL.get_num_material_expressions(mat): break
        MEL.delete_all_material_expressions(mat)
    assert not MEL.get_num_material_expressions(mat)
    for k,v in dict(used_with_instanced_static_meshes=True,two_sided=False,tangent_space_normal=True,
        blend_mode=unreal.BlendMode.BLEND_OPAQUE,shading_model=unreal.MaterialShadingModel.MSM_DEFAULT_LIT).items():mat.set_editor_property(k,v)
    def node(cls,**props):
        n=MEL.create_material_expression(mat,getattr(unreal,cls),0,0)
        for k,v in props.items(): n.set_editor_property(k,v)
        return n
    def link(a,b,pin,out=''):
        assert MEL.connect_material_expressions(a,out,b,pin)
    def custom(code,inputs,kind='FLOAT3'):
        pins=[]
        for name in inputs:
            p=unreal.CustomInput();p.set_editor_property('input_name',name);pins.append(p)
        n=node('MaterialExpressionCustom',code=code,description='TG roof '+kind,
            output_type=getattr(unreal.CustomMaterialOutputType,'CMOT_'+kind),inputs=pins)
        for name,src in inputs.items():link(src,n,name)
        return n
    def scalar(name,value):return node('MaterialExpressionScalarParameter',parameter_name=name,default_value=value)
    def sample(name,tex,uv,normal=False):
        t=unreal.load_asset(TEX+'/'+tex);assert t,tex
        n=node('MaterialExpressionTextureSampleParameter2D',parameter_name=name,texture=t,
            sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if normal else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        link(uv,n,'UVs');return n
    wp=node('MaterialExpressionWorldPosition');vn=node('MaterialExpressionVertexNormalWS')
    rnd=node('MaterialExpressionPerInstanceRandom');uv=node('MaterialExpressionTextureCoordinate',coordinate_index=0)
    vc=node('MaterialExpressionVertexColor')
    atlas=custom('''float2 seed=float2(round(saturate(R)*5.0),round(frac(R*17.137+0.317)*5.0));
return (UV+seed)*0.2;''',{'UV':uv,'R':rnd},'FLOAT2')
    offset=custom('return WP*0.001+(R<0.5 ? frac(R*13.71)*0.4 : 0.0);',{'WP':wp,'R':rnd})
    cx=sample('RoofColor','roof_colors_layer00',custom('return P.yz;',{'P':offset},'FLOAT2'))
    cy=sample('RoofColor','roof_colors_layer00',custom('return P.xz;',{'P':offset},'FLOAT2'))
    dmg=sample('TileDamage','roof_tile_damage',atlas);nrm=sample('TileNormal','roof_tile_normal',atlas,True)
    color=custom('''
float2 w=float2(dot(CX.rgb,float3(.2126,.7152,.0722))+abs(N.x),
                dot(CY.rgb,float3(.2126,.7152,.0722))+abs(N.y));
w=max(w-max(w.x,w.y)+.2,0.0);
float3 base=(CX.rgb*w.x+CY.rgb*w.y)/max(w.x+w.y,1e-5)*.6;
float variation=lerp(.9,1.3,saturate(1.0-R*1.4));
float wear=(1.0-Damage.r)*Weather;
return lerp(base*variation,base*float3(.78,.83,.56),wear)*Brightness;
''',{'CX':cx,'CY':cy,'N':vn,'R':rnd,'Damage':dmg,'Weather':scalar('RoofWeathering',.12),'Brightness':scalar('RoofBrightness',1.25)})
    normal=custom('return normalize(lerp(float3(0,0,1),N.rgb,VC.r*Strength));',
        {'N':nrm,'VC':vc,'Strength':scalar('TileNormalStrength',.7)})
    for n,prop in [(color,'BASE_COLOR'),(normal,'NORMAL'),(scalar('RoofRoughness',.86),'ROUGHNESS'),(scalar('RoofSpecular',.25),'SPECULAR')]:
        assert MEL.connect_material_property(n,'',getattr(unreal.MaterialProperty,'MP_'+prop))
    MEL.layout_material_expressions(mat);MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat,False)
    return mat


if __name__=='__main__':build()

