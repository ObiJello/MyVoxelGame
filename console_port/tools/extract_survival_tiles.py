#!/usr/bin/env python3
"""Extract survival block properties from the original Tile registry.

Reads original/reference-only/Tile.cpp (Tile::staticCtor) and Tile.h and writes
the destroy time and Material of every registered tile ID:

* destroy time: the last setDestroyTime(...) in the registration chain,
  setIndestructible() (-1), the class constructor's own setDestroyTime, the
  base tile's value for StairTile/WallTile, or Tile's default 0.
* material: an explicit Material:: constructor argument, else the class
  constructor initializer (following single-argument base constructors).

Tile classes are read from original/reference-only, then source_full; a class
that cannot be resolved is an error (nothing is guessed).
"""
from pathlib import Path
import re, sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'original/reference-only'

MATERIALS = ['air', 'grass', 'dirt', 'wood', 'stone', 'metal', 'heavyMetal', 'water', 'lava',
             'leaves', 'plant', 'replaceable_plant', 'sponge', 'cloth', 'fire', 'sand',
             'decoration', 'clothDecoration', 'glass', 'buildable_glass', 'explosive', 'coral',
             'ice', 'topSnow', 'snow', 'cactus', 'clay', 'vegetable', 'egg', 'portal', 'cake',
             'web', 'piston']


FULL = ROOT / 'source_full/Minecraft.World'


def read(name):
    path = SRC / name
    if not path.exists():
        path = FULL / name
    if not path.exists():
        return ''
    text = path.read_text(encoding='utf-8-sig', errors='replace')
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)


tile_h = read('Tile.h')
ids = {m[1]: int(m[2]) for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', tile_h)}
tile_cpp = read('Tile.cpp')
start = tile_cpp.index('void Tile::staticCtor()')
end = tile_cpp.index('\nvoid Tile::', start + 10)
body = tile_cpp[start:end]

class_cache = {}


def class_info(cls):
    """(material or None, own destroy time or None, reconstructed) for a class."""
    if cls in class_cache:
        return class_cache[cls]
    source = read(cls + '.cpp')
    result = (None, None, False)
    if source:
        own_time = None
        material = None
        base = None
        for ctor in re.finditer(cls + r'::' + cls + r'\s*\(([^)]*)\)\s*:\s*(\w+)\s*\(([^;{]*)\)\s*\{', source):
            init = ctor[3]
            m = re.search(r'Material::(\w+)', init)
            if m and material is None:
                material = m[1]
            elif base is None and ctor[2] not in ('Tile',):
                base = ctor[2]
            ctor_body = source[ctor.end():source.find('\n}', ctor.end())]
            t = re.search(r'setDestroyTime\(\s*([-\d.]+|INDESTRUCTIBLE_DESTROY_TIME)f?\s*\)', ctor_body)
            if t and own_time is None:
                own_time = -1.0 if t[1] == 'INDESTRUCTIBLE_DESTROY_TIME' else float(t[1])
        reconstructed = False
        if material is None and base is not None:
            material, base_time, reconstructed = class_info(base)
            if own_time is None:
                own_time = base_time
        result = (material, own_time, reconstructed)
    class_cache[cls] = result
    return result


rows = {}
unresolved = []
for statement in body.split(';'):
    reg = re.search(r'Tile::(\w+)\s*=.*?new\s+(\w+)\s*\(([^)]*)\)', statement, flags=re.S)
    if not reg:
        continue
    name, cls, args = reg[1], reg[2], reg[3]
    # The constructor's first argument is the registered ID; ClothTile's
    # constructor hard-codes it. Field names do not always match *_Id names.
    first = args.split(',')[0].strip()
    if re.fullmatch(r'\d+', first):
        tile_id = int(first)
    elif re.fullmatch(r'Tile::\w+_Id', first):
        tile_id = ids[first[6:-3]]
    elif name in ids:
        tile_id = ids[name]
    else:
        raise SystemExit(f'Unresolved tile id for {name}')
    times = [('-1' if v == 'INDESTRUCTIBLE_DESTROY_TIME' else v)
             for v in re.findall(r'setDestroyTime\(\s*([-\d.]+|INDESTRUCTIBLE_DESTROY_TIME)f?\s*\)', statement)]
    material_arg = re.search(r'Material::(\w+)', args)
    material, own_time, reconstructed = class_info(cls)
    if material_arg:
        material, reconstructed = material_arg[1], False
    base_arg = re.search(r'Tile::(\w+)', args)
    if 'setIndestructible' in statement:
        time = -1.0
    elif times:
        time = float(times[-1])
    elif own_time is not None:
        time = own_time
    elif cls in ('StairTile', 'WallTile') and base_arg:
        base_row = rows[ids[base_arg[1]]]
        time = base_row['time']
        if material is None:
            material, reconstructed = base_row['material'], base_row['reconstructed']
    else:
        time = 0.0
    if material is None:
        unresolved.append(f'{name} ({cls})')
        continue
    if material not in MATERIALS:
        raise SystemExit(f'Unknown material {material} for {name}')
    rows[tile_id] = dict(name=name, cls=cls, time=time, material=material, reconstructed=reconstructed)

if unresolved:
    raise SystemExit('Unresolved materials: ' + ', '.join(unresolved))
out = ['// Generated by tools/extract_survival_tiles.py from original Tile::staticCtor.',
       '// Do not edit by hand.',
       '#include "TileSurvival.h"', 'namespace console {', 'namespace {',
       'constexpr SurvivalTile kTiles[]{']
for tile_id, row in sorted(rows.items()):
    out.append(f' {{{tile_id},{row["time"]!r}f,SurvivalMaterial::{row["material"]},'
               f'{"true" if row["reconstructed"] else "false"}}}, // {row["name"]} ({row["cls"]})')
out += ['};', '}', 'const SurvivalTile* consoleSurvivalTile(int id){',
        ' for(const auto& tile:kTiles)if(tile.id==id)return &tile;', ' return nullptr;', '}', '}', '']
Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'ported/TileSurvival.cpp').write_text('\n'.join(out))
print(f'{len(rows)} tiles')
