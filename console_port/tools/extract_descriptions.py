#!/usr/bin/env python3
"""Generate item/tile description string IDs from Tile::staticCtor and Item::staticCtor.

Reproduces what the tutorial hints read:

* Item::items[id]->getDescriptionId(int iData) (LookAtTileHint titles).
  - Tiles are wrapped by TileItem, whose getDescriptionId(iData) asks the tile
    class (WoodTile, Sapling, TreeTile, LeafTile, TallGrass, StoneSlabTile,
    WoodSlabTile, StoneMonsterTile, SmoothStoneBrickTile override it with their
    *_NAMES arrays), unless Tile::staticCtor installs a MultiTextureTileItem
    (nameExtensions[iData], 0 when out of range) or AnvilTileItem (iData >> 2).
  - Items return their setDescriptionId value (SkullItem uses NAMES[iData]).
* Item::items[id]->getUseDescriptionId(): the tile's or item's
  setUseDescriptionId value (TileItem forwards to the tile).

Output: a table of (id, description, use description, name array, rule).
Sapling::getDescriptionId only clamps negative data; the growth bit (8) would
read past SAPLING_NAMES in the original, so the generated lookup clamps to the
array like the other tiles.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
WORLD = ROOT / 'source_full/Minecraft.World'
STRINGS = ROOT / 'source_full/Minecraft.Client/PS3Media/strings.h'


def read(name):
    text = (WORLD / name).read_text(encoding='utf-8-sig', errors='replace')
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)


string_ids = {m[1]: int(m[2]) for m in re.finditer(r'#define\s+(IDS_\w+)\s+(\d+)', STRINGS.read_text())}


def ids_constants(header):
    return {m[1]: int(m[2]) for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', read(header))}


tile_ids = ids_constants('Tile.h')
item_ids = ids_constants('Item.h')


def name_array(cls, array):
    source = read(cls + '.cpp')
    m = re.search(rf'{cls}::{array}\s*\[[^]]*\]\s*=\s*\{{([^}}]*)\}}', source, re.S)
    if not m:
        raise SystemExit(f'missing {cls}::{array}')
    names = re.findall(r'IDS_\w+', m[1])
    for n in names:
        if n not in string_ids:
            raise SystemExit(f'unknown string {n}')
    return names


def body(source, name):
    start = source.index(f'void {name}::staticCtor()')
    return source[start:source.index('\nvoid ', start + 10)]


def last(pattern, statement):
    found = re.findall(pattern, statement)
    return found[-1] if found else None


# Tile registrations.
TILE_OVERRIDES = {
    'WoodTile': ('WOOD_NAMES', 'clamp'),
    'Sapling': ('SAPLING_NAMES', 'clamp'),
    'TreeTile': ('TREE_NAMES', 'mask3'),
    'LeafTile': ('LEAF_NAMES', 'mask3'),
    'TallGrass': ('TALL_GRASS_TILE_NAMES', 'clamp'),
    'StoneSlabTile': ('SLAB_NAMES', 'clamp'),
    'WoodSlabTile': ('SLAB_NAMES', 'clamp'),
    'StoneMonsterTile': ('STONE_MONSTER_NAMES', 'clamp'),
    'SmoothStoneBrickTile': ('SMOOTH_STONE_BRICK_NAMES', 'clamp'),
}
rows = {}
tile_body = body(read('Tile.cpp'), 'Tile')
for statement in tile_body.split(';'):
    reg = re.search(r'Tile::(\w+)\s*=.*?new\s+(\w+)\s*\(([^)]*)\)', statement, re.S)
    if not reg:
        continue
    name, cls, args = reg[1], reg[2], reg[3]
    first = args.split(',')[0].strip()
    if re.fullmatch(r'\d+', first):
        tid = int(first)
    elif re.fullmatch(r'Tile::\w+_Id', first):
        tid = tile_ids[first[6:-3]]
    elif name in tile_ids:
        tid = tile_ids[name]
    else:
        raise SystemExit(f'unresolved tile {name}')
    desc = last(r'setDescriptionId\((IDS_\w+)\)', statement)
    use = last(r'setUseDescriptionId\((IDS_\w+)\)', statement)
    rule, names = 'plain', []
    if cls in TILE_OVERRIDES:
        array, rule = TILE_OVERRIDES[cls]
        names = name_array(cls, array)
    rows[tid] = dict(name=name, desc=desc, use=use, rule=rule, names=names)

# Item::items[] wrappers Tile::staticCtor installs for tiles.
for statement in tile_body.split(';'):
    m = re.search(r'Item::items\[Tile::(\w+)_Id\]\s*=.*?new\s+(\w+)\s*\(([^;]*)', statement, re.S)
    if not m:
        continue
    tid, cls = tile_ids[m[1]], m[2]
    if cls == 'MultiTextureTileItem':
        arr = re.search(r'(\w+)::(\w+_NAMES|BLOCK_NAMES)\b', m[3])
        rows[tid]['rule'] = 'clamp'
        rows[tid]['names'] = name_array(arr[1], arr[2])
    elif cls == 'AnvilTileItem':
        rows[tid]['rule'] = 'anvil'
        rows[tid]['names'] = name_array('AnvilTile', 'ANVIL_NAMES')

# Item registrations.
item_body = body(read('Item.cpp'), 'Item')
for statement in item_body.split(';'):
    reg = re.search(r'Item::(\w+)\s*=.*?new\s+(\w+)\s*\(\s*(\d+)', statement, re.S)
    if not reg:
        continue
    name, cls = reg[1], reg[2]
    iid = item_ids.get(name, 256 + int(reg[3]))
    if iid != 256 + int(reg[3]):
        raise SystemExit(f'{name}: {iid} != 256+{reg[3]}')
    desc = last(r'setDescriptionId\((IDS_\w+)\)', statement)
    use = last(r'setUseDescriptionId\((IDS_\w+)\)', statement)
    rule, names = 'plain', []
    if cls == 'SkullItem':
        rule, names = 'clamp', name_array('SkullItem', 'NAMES')
    rows[iid] = dict(name=name, desc=desc, use=use, rule=rule, names=names)


def sid(name):
    return string_ids[name] if name else -1


RULES = {'plain': 0, 'clamp': 1, 'mask3': 2, 'anvil': 3}
out = ['// Generated by tools/extract_descriptions.py from Tile::staticCtor and Item::staticCtor.',
       '// Do not edit by hand.',
       '#include "ItemDescriptions.h"',
       'namespace console {',
       'namespace {',
       'struct Row{int id,description,use,rule,count;int names[8];};',
       'constexpr Row kRows[]{']
for key, row in sorted(rows.items()):
    names = [sid(n) for n in row['names']]
    padded = names + [-1] * (8 - len(names))
    out.append(f' {{{key},{sid(row["desc"])},{sid(row["use"])},{RULES[row["rule"]]},{len(names)},'
               f'{{{",".join(map(str, padded))}}}}}, // {row["name"]}')
out += ['};',
        'const Row* find(int id){for(const auto& r:kRows)if(r.id==id)return &r;return nullptr;}',
        '}',
        'int consoleDescriptionId(int id,int data){',
        ' const Row* r=find(id);if(!r)return -1;',
        ' if(r->rule==0 || r->count==0)return r->description;',
        ' int index=data;',
        ' if(r->rule==2)index=data&3;',
        ' else if(r->rule==3)index=data>>2;',
        ' if(index<0 || index>=r->count)index=0;',
        ' return r->names[index];',
        '}',
        'int consoleUseDescriptionId(int id){const Row* r=find(id);return r?r->use:-1;}',
        '}',
        '']
args = [a for a in sys.argv[1:] if a != '--check']
target = Path(args[0]) if args else ROOT / 'ported/ItemDescriptions.cpp'
if '--check' in sys.argv:
    if not target.exists() or target.read_text() != '\n'.join(out):
        raise SystemExit(f'{target} is stale; rerun {Path(__file__).name}')
else:
    target.write_text('\n'.join(out))
print(f'{len(rows)} descriptions')
