#!/usr/bin/env python3
"""Import the console tutorial (Common/Tutorial) into ported/tutorial.

The tutorial classes are copied unchanged except for their #include lines:
`stdafx.h` becomes `TutorialHost.h` (the stand-in for the engine types the
tutorial talks to), and includes of engine headers outside the Tutorial folder
are commented out because TutorialHost.h declares what they provided.
TutorialMode/FullTutorialMode are engine glue (MultiPlayerGameMode subclasses)
and are replaced by src/TutorialSession.cpp, so they are not imported.

Also generated from the original headers, for TutorialHost.h to include:
* generated/TileIds.inc     - Tile::*_Id constants (Tile.h)
* generated/ItemIds.inc     - Item::*_Id constants (Item.h)
* generated/ItemPointers.inc- Item::* pointer names with their registered IDs
* generated/InstanceOf.inc  - enum eINSTANCEOF (Class.h)
* generated/TileConstants.inc - the tile-class constants the hints use
* generated/AppEnums.inc    - eGameSetting and EControllerActions (App_enums.h)
* generated/RecipyGroups.inc, JoyButtons.inc, JoypadMap.inc - see generated()

Run with --check to fail when ported/tutorial differs from the source.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'source_full/Minecraft.Client/Common/Tutorial'
WORLD = ROOT / 'source_full/Minecraft.World'
TARGET = ROOT / 'ported/tutorial'
SKIP = {'TutorialMode.cpp', 'TutorialMode.h', 'FullTutorialMode.cpp', 'FullTutorialMode.h'}


def rewrite(text):
    out = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped == '#include "stdafx.h"':
            line = line.replace('"stdafx.h"', '"TutorialHost.h"')
        elif re.match(r'#include\s+"\.\.', stripped):
            line = '// TutorialHost.h: ' + line.lstrip()
        out.append(line)
    return ''.join(out)


def world(name):
    text = (WORLD / name).read_text(encoding='utf-8-sig', errors='replace')
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)


def generated():
    files = {}
    tile_h, item_h = world('Tile.h'), world('Item.h')
    files['TileIds.inc'] = ''.join(f'static const int {m[1]}_Id = {m[2]};\n' for m in
                                   re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', tile_h))
    item_ids = {m[1]: int(m[2]) for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', item_h)}
    files['ItemIds.inc'] = ''.join(f'static const int {k}_Id = {v};\n' for k, v in item_ids.items())
    # Item::name = (new Class(N ...)) registers ID 256 + N.
    body = world('Item.cpp')
    body = body[body.index('void Item::staticCtor()'):]
    pointers = []
    for m in re.finditer(r'Item::(\w+)\s*=\s*\(?\s*(?:\(\w+\s*\*\)\s*)?\(?\s*new\s+\w+\s*\(\s*(\d+)', body):
        name, iid = m[1], 256 + int(m[2])
        if name in item_ids and item_ids[name] != iid:
            raise SystemExit(f'{name}: {iid} != {item_ids[name]}')
        pointers.append((name, iid))
    declared = set(re.findall(r'static\s+\w+\s*\*\s*(\w+)\s*;', item_h))
    files['ItemPointers.inc'] = ''.join(f'ITEM_POINTER({n}, {i})\n' for n, i in pointers if n in declared)
    cls = world('Class.h')
    enum = re.search(r'enum eINSTANCEOF\s*\{.*?\};', cls, re.S)
    files['InstanceOf.inc'] = enum[0] + '\n'
    consts = []
    for header, names in (('SandStoneTile.h', ['TYPE_DEFAULT', 'TYPE_HEIROGLYPHS', 'TYPE_SMOOTHSIDE']),
                          ('TallGrass.h', ['DEAD_SHRUB', 'TALL_GRASS', 'FERN']),
                          ('StoneSlabTile.h', ['STONE_SLAB', 'SAND_SLAB', 'WOOD_SLAB', 'COBBLESTONE_SLAB',
                                               'BRICK_SLAB', 'SMOOTHBRICK_SLAB', 'NETHERBRICK_SLAB', 'QUARTZ_SLAB']),
                          ('TreeTile.h', ['DARK_TRUNK', 'BIRCH_TRUNK']),
                          ('WallTile.h', ['TYPE_NORMAL', 'TYPE_MOSSY']),
                          ('QuartzBlockTile.h', ['TYPE_DEFAULT', 'TYPE_CHISELED', 'TYPE_LINES_Y',
                                                 'TYPE_LINES_X', 'TYPE_LINES_Z'])):
        text = world(header)
        cname = header[:-2]
        members = []
        for n in names:
            m = re.search(rf'static const int {n}\s*=\s*(\d+)\s*;', text)
            if not m:
                raise SystemExit(f'{cname}::{n} not found')
            members.append(f'    static const int {n} = {m[1]};\n')
        consts.append(f'struct {cname}\n{{\n' + ''.join(members) + '};\n')
    files['TileConstants.inc'] = ''.join(consts)
    enums = (ROOT / 'source_full/Minecraft.Client/Common/App_enums.h').read_text(errors='replace')
    files['AppEnums.inc'] = ''.join(re.search(rf'enum {name}\s*\{{.*?\}};', enums, re.S)[0] + '\n'
                                    for name in ('eGameSetting', 'EControllerActions'))
    recipy = re.search(r'enum _eGroupType\s*\{.*?\}', world('Recipy.h'), re.S)
    files['RecipyGroups.inc'] = recipy[0] + ';\n'
    # PS3 4J_Input.h button bits and the DefineActions() layouts MAP_STYLE_0-2
    # (circle/cross not swapped), which InputConstraint compares through
    # GetGameJoypadMaps and the Controls menu selects.
    client = ROOT / 'source_full/Minecraft.Client/PS3'
    buttons = re.findall(r'#define\s+(_360_JOY_BUTTON_\w+)\s+(0x[0-9A-Fa-f]+)',
                         (client / '4JLibs/inc/4J_Input.h').read_text(errors='replace'))
    files['JoyButtons.inc'] = ''.join(f'#define {n} {v}\n' for n, v in buttons)
    actions = (client / 'PS3_Minecraft.cpp').read_text(errors='replace')
    actions = actions[actions.index('void DefineActions(void)'):]
    actions = actions[:actions.index('\n}\n')]
    actions = re.sub(r'if\(InputManager\.IsCircleCrossSwapped\(\)\)\s*\{.*?\}\s*else', '', actions, flags=re.S)
    maps = re.findall(r'SetGameJoypadMaps\(MAP_STYLE_(\d),\s*(\w+),\s*([^)]*)\)', actions)
    files['JoypadMap.inc'] = ''.join(f'JOYPAD_MAP({s}, {a}, {" ".join(b.split())})\n' for s, a, b in maps)
    return {'generated/' + k: v for k, v in files.items()}


def expected():
    files = {}
    for path in sorted(SOURCE.iterdir()):
        if path.suffix in ('.cpp', '.h') and path.name not in SKIP:
            files[path.name] = rewrite(path.read_text(encoding='utf-8-sig', errors='strict'))
    # Tutorial::setHintCompleted deletes hints through TutorialHint*, which
    # has no virtual destructor in the source (undefined behaviour; the
    # derived hints' members leak).
    ctor = '\tTutorialHint(eTutorial_Hint id, Tutorial *tutorial, int descriptionId, eHintType type, bool allowFade = true);\n'
    if ctor not in files['TutorialHint.h']:
        raise SystemExit('TutorialHint.h: constructor not found')
    files['TutorialHint.h'] = files['TutorialHint.h'].replace(
        ctor, ctor + '\tvirtual ~TutorialHint() {} // port fix: deleted through the base class\n')
    # Tutorial::tick compares m_bSceneIsSplitscreen, which only the Xbox
    # branch ever assigns: initialise it with hasRequestedUI.
    init = '\thasRequestedUI = false;\n'
    if files['Tutorial.cpp'].count(init) != 1:
        raise SystemExit('Tutorial.cpp: hasRequestedUI initialisation not found')
    files['Tutorial.cpp'] = files['Tutorial.cpp'].replace(
        init, init + '\tm_bSceneIsSplitscreen = false; // port fix: read before it is set outside _XBOX\n')
    files.update(generated())
    return files


def main():
    check = '--check' in sys.argv
    stale = []
    for name, text in expected().items():
        target = TARGET / name
        if check:
            if not target.exists() or target.read_text() != text:
                stale.append(name)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
    if stale:
        raise SystemExit('ported/tutorial is stale: ' + ', '.join(stale))
    print('ported/tutorial matches the source' if check else 'imported ported/tutorial')


main()
