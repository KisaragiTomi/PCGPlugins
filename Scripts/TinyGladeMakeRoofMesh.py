"""Bake the extracted TG tile's vertex deformation and procedural UVs for UE.

Input topology is copied verbatim from roof_tile.json (20 vertices, 36 triangles).
The native VS uses sign-based resizing, not uniform box scaling. Nominal row
dimensions and the 3 cm lap lift below adapt that deformation to our 16.9 cm rows.
No source asset, LOD proxy or backface mesh is overwritten.
"""
import json
import math
from pathlib import Path
import unreal

MESH = '/PCGPlugins/HouseTest/SM_TinyGladeRoofTile'


def build(material):
    source = Path(__file__).resolve().parents[1] / 'Docs/TinyGlade/evidence/roof_tile-source.json'
    data = json.loads(source.read_text(encoding='utf-8'))
    original = data['Vertex_Position']['buffer']
    indices = data['indices']['buffer']
    # Native VS: max(min(max(scale), min(scale * normalization)), epsilon).
    half = (14.0, 13.52, 1.5)
    common = max(min(max(half), min(a*b for a,b in zip(half,(3.5892038,3.7037039,3.7037039)))), 1e-5)
    positions = []
    for v in original:
        p = [v[i] * (1.9764018 if i == 0 else 2.0) * common
             - (common-half[i]) * (1 if v[i] >= 0 else -1) for i in range(3)]
        p[2] += 3.0 if v[1] > .1 else 0.0
        # Canonical UE tile: X across row, +Y up slope, +Z top.
        positions.append((p[0], -p[1], p[2]))
    vertices, normals, uv, colors, triangles = [], [], [], [], []
    for start in range(0,len(indices),3):
        ids = indices[start:start+3]
        pts = [positions[i] for i in ids]
        a,b = [[pts[j][k]-pts[0][k] for k in range(3)] for j in (1,2)]
        n = (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
        length = math.sqrt(sum(x*x for x in n))
        n = tuple(x/length for x in n)
        # Source is clockwise-outward; the Y reflection makes this cross outward.
        base = len(vertices)
        for i,p in zip(ids,pts):
            v=original[i]
            vertices.append(unreal.Vector(*p)); normals.append(unreal.Vector(*n))
            side = -.475 < v[2] < .45
            uv.append(unreal.Vector2D(v[1]*.8+.5,v[2]*.9+.5) if side
                      else unreal.Vector2D(v[0]*.85+.5,v[1]*.92+.5))
            colors.append(unreal.LinearColor(1.0 if v[2] > .499 else 0.0,0,0,1))
        # UE's front-face winding is clockwise; keep the geometric outward normal.
        triangles.append(unreal.IntVector(base,base+2,base+1))
    buffers=unreal.GeometryScriptSimpleMeshBuffers(vertices=vertices,normals=normals,
        triangles=triangles,uv0=uv,vertex_colors=colors)
    dynamic=unreal.DynamicMesh(); dynamic.append_buffers_to_mesh(buffers)
    mesh=unreal.load_asset(MESH) if unreal.EditorAssetLibrary.does_asset_exist(MESH) else None
    if mesh:
        options=unreal.GeometryScriptCopyMeshToAssetOptions(enable_recompute_normals=False,enable_recompute_tangents=True)
        _,outcome=unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(dynamic,mesh,options,unreal.GeometryScriptMeshWriteLOD())
    else:
        options=unreal.GeometryScriptCreateNewStaticMeshAssetOptions(enable_recompute_normals=False,
            enable_recompute_tangents=True,enable_collision=False,enable_nanite=False)
        mesh,outcome=unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(dynamic,MESH,options)
    assert mesh and outcome==unreal.GeometryScriptOutcomePins.SUCCESS, str(outcome)
    mesh.set_material(0,material)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh,False)
    unreal.log('TG roof tile baked: '+str(mesh.get_bounding_box()))
    return mesh
