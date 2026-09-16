"""Import TG's single-channel (BC4_UNORM) data textures with their real 0..1 values.

History (2026-09-14): `D:/MyProject/Tiny Glade/extract/tex2png.py` decoded BC4 into the R channel of a
BGRA image and then called `img.convert("L")`, i.e. stored luminance 0.299*R. Every BC4 PNG in
`extracted/textures` topped out at 76 (checked file by file: old PNG == round(0.299 * decoded)).
The extractor now takes the R channel and the 17 PNGs below were regenerated. BC7 and RGBA8 exports
were bit-exact and are not affected.

TG samples these as BC4_UNORM, i.e. linear data. They are imported as **TC_Grayscale + sRGB off**:
exact 8-bit linear values (sampler type LinearGrayscale), mips built in linear space like TG's, so
every consumer reads TG's numbers without per-shader compensation. Consumers adjusted together
with this change:
  M_TinyGladeRoof   TileDamage: LinearGrayscale sampler, no sRGB re-encode    (TinyGladeMakeRoofMaterial.py)
  M_TinyGladeWall   BrickHeight: LinearGrayscale; BrickHeightGain 3.4 -> 1.0, the 3.4 was 255/76
                    compensation for the squashed PNG                          (TinyGladeMakeWallMaterials.py)
  M_TinyGladeStone  roughness TextureObject: LinearGrayscale                  (TinyGladeMakeStoneMaterial.py)
The TinyGladeGallery MIs (parent M_TG_Texture, colour sampler) cannot display linear textures; that
already applied to every linear texture there (normal maps, stone_floor_2_height) and M_TG_Texture
also drives rock-shell MIs, so it is left alone.

Run: exec in the editor, or call `fix(rel)` for one entry. A package that is already dirty is skipped
(it holds someone's unsaved edits) and reported by `main()`.
"""
import struct
import zlib
from pathlib import Path
import unreal

TG = Path(r'D:/MyProject/Tiny Glade')
EXTRACTED = TG / 'extracted/textures'
COMPILED = TG / 'compiled-assets/textures'
TEX = '/PCGPlugins/HouseTest/TinyGladeAsset/Textures'

# compiled-assets / extracted relative path (no extension) -> UE texture asset name
TEXTURES = {
    'roof/roof_tile_damage': 'roof_tile_damage',
    'stone_floor/stone_floor_height': 'stone_floor_height',
    'stone_floor/stone_floor_roughness': 'stone_floor_roughness',
    'stone_floor_2/stone_floor_height': 'stone_floor_2_height',
    'stone_floor_2/stone_floor_roughness': 'stone_floor_2_roughness',
    'stone_floor_3/stone_floor_height': 'stone_floor_3_height',
    'stone_floor_3/stone_floor_roughness': 'stone_floor_3_roughness',
    'wooden_floor/wooden_floor_height': 'wooden_floor_height',
    'wooden_floor/wooden_floor_roughness': 'wooden_floor_roughness',
    'wooden_floor_2/wooden_floor_height': 'wooden_floor_2_height',
    'wooden_floor_2/wooden_floor_roughness': 'wooden_floor_2_roughness',
    'wooden_floor_3/wooden_floor_height': 'wooden_floor_3_height',
    'wooden_floor_3/wooden_floor_roughness': 'wooden_floor_3_roughness',
    'wood_detail/edge_damage': 'edge_damage',
    'wood_detail/roughness': 'roughness',
    'terrain_height': 'terrain_height',
    'canopy_flowers_depth': 'canopy_flowers_depth',
}


def bc4_top_mip_endpoint_max(path):
    """Largest BC4 endpoint in mip 0 (tex2png.py documents the 8-byte header / 28-byte footer layout)."""
    raw = path.read_bytes()
    _, _, width, height, _, _, _, fmt, _, _ = struct.unpack_from('<iIIIIHHBBH', raw, len(raw) - 28)
    assert fmt == 139, '%s is VkFormat %d, not BC4_UNORM' % (path.name, fmt)
    blocks = ((width + 3) // 4) * ((height + 3) // 4)
    return max(max(raw[8 + i * 8], raw[9 + i * 8]) for i in range(blocks))


def grey_png_max(path):
    """Max pixel of an 8-bit greyscale, non-interlaced PNG (tex2png writes exactly that for BC4)."""
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, chunks, width, height = 8, [], 0, 0
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], 'big')
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if tag == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or colour != 0 or interlace:
                raise ValueError('%s: expected 8-bit greyscale non-interlaced PNG' % path.name)
        elif tag == b'IDAT':
            chunks.append(body)
        elif tag == b'IEND':
            break
        pos += 12 + length
    raw = zlib.decompress(b''.join(chunks))
    prev, best = bytearray(width), 0
    for y in range(height):
        base = y * (width + 1)
        kind, row = raw[base], bytearray(raw[base + 1:base + 1 + width])
        if kind == 1:
            for x in range(1, width):
                row[x] = (row[x] + row[x - 1]) & 255
        elif kind == 2:
            row = bytearray((a + b) & 255 for a, b in zip(row, prev))
        elif kind == 3:
            for x in range(width):
                row[x] = (row[x] + ((row[x - 1] if x else 0) + prev[x]) // 2) & 255
        elif kind == 4:
            for x in range(width):
                a, b, c = (row[x - 1] if x else 0), prev[x], (prev[x - 1] if x else 0)
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[x] = (row[x] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        best = max(best, max(row))
        prev = row
    return best


def fix(rel, save=True):
    """Re-import one texture from extracted/, set linear greyscale. Returns (asset path, status)."""
    name = TEXTURES[rel]
    asset_path = '%s/%s' % (TEX, name)
    png = EXTRACTED / (rel + '.png')
    expected, got = bc4_top_mip_endpoint_max(COMPILED / (rel + '.texture')), grey_png_max(png)
    if got < 0.5 * expected:
        raise RuntimeError('%s: PNG max %d but the BC4 data reaches %d; this is the squashed export, '
                           're-run extract/tex2png.py (fixed 2026-09-14)' % (png, got, expected))
    texture = unreal.load_asset(asset_path)
    assert texture, asset_path
    if any(p.get_name() == asset_path for p in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()):
        return asset_path, 'skipped: package already dirty'
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', str(png))
    task.set_editor_property('destination_path', TEX)
    task.set_editor_property('destination_name', name)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('replace_existing_settings', False)
    task.set_editor_property('automated', True)
    task.set_editor_property('save', False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.load_asset(asset_path)
    texture.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_GRAYSCALE)
    texture.set_editor_property('srgb', False)
    if save:
        unreal.EditorAssetLibrary.save_loaded_asset(texture, False)
    return asset_path, 'imported (png max %d, BC4 endpoint max %d)' % (got, expected)


def main(names=None):
    results = [fix(rel) for rel in TEXTURES if names is None or TEXTURES[rel] in names]
    for path, status in results:
        (unreal.log_warning if status.startswith('skipped') else unreal.log)('TG BC4: %s %s' % (path, status))
    return results


if __name__ == '__main__':
    main()
