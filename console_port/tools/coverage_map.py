#!/usr/bin/env python3
"""Coverage of the console source by the port: docs/COVERAGE.md.

Every class in source_full/Minecraft.World and the shared Minecraft.Client
code is grouped by subsystem (from its base class chain) and marked:

  ported   the port has a file of that name (ported/, src/)
  partial  the port's code names the class (a reimplemented piece, a cited
           method or rule), but has no file for it
  missing  nothing in the port refers to it

"partial" is a lead, not a verdict: the per-subsystem notes in the
generated file say what is actually behind the numbers. Platform folders
(Xbox, PS3 system code, PSVita, Orbis, Durango, Windows64) are left out.

Run with --check to fail when docs/COVERAGE.md is out of date.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'source_full'
OUT = ROOT / 'docs/COVERAGE.md'
PORT_DIRS = [ROOT / 'src', ROOT / 'ported', ROOT / 'platform', ROOT / 'tools']
# The imported tutorial is the source itself.
IMPORTED = {p.stem for p in (ROOT / 'ported/tutorial').glob('*.cpp')}

# Base classes that decide a subsystem, checked along the inheritance chain.
BASES = [
    ('Packet', 'Networking'), ('Command', 'Commands'), ('Goal', 'Mob AI'),
    ('TileEntity', 'Block entities'), ('Tile', 'Blocks'), ('Item', 'Items'),
    ('Enchantment', 'Enchanting'), ('MobEffect', 'Effects'),
    ('Feature', 'World generation'), ('Layer', 'World generation'), ('StructurePiece', 'World generation'),
    ('StructureStart', 'World generation'), ('StructureFeature', 'World generation'),
    ('LargeFeature', 'World generation'), ('Biome', 'World generation'), ('BiomeSource', 'World generation'),
    ('ChunkSource', 'Main loop and levels'), ('Tag', 'Saves, NBT and IO'), ('InputStream', 'Saves, NBT and IO'),
    ('OutputStream', 'Saves, NBT and IO'), ('AbstractContainerMenu', 'Menus and containers'),
    ('Slot', 'Menus and containers'), ('Container', 'Menus and containers'), ('Recipy', 'Crafting'),
    ('EntityRenderer', 'Rendering'), ('Model', 'Rendering'), ('Particle', 'Rendering'),
    ('Screen', 'Screens and UI'), ('UIScene', 'Screens and UI'), ('UIControl', 'Screens and UI'),
    ('UIComponent', 'Screens and UI'), ('Entity', 'Entities'), ('Dimension', 'Dimensions'),
    ('Achievement', 'Stats and achievements'), ('Stat', 'Stats and achievements'),
    ('Texture', 'Rendering'), ('TexturePack', 'Rendering'),
]
# Name patterns for classes without a telling base.
NAMES = [
    (r'^(Minecraft|MinecraftServer|Timer|Options|GameRenderer|ServerLevel|MultiPlayerLevel|ServerChunkCache|MultiPlayerChunkCache)$', 'Main loop and levels'),
    (r'Packet', 'Networking'),
    (r'Level|^LevelChunk$|ChunkCache|^Explosion$|^TileEventData$|^TilePos$|^MobCategory$|Weather', 'Main loop and levels'), (r'Connection|Socket|Network|Server|Listener|Pending|Rcon', 'Networking'),
    (r'Goal|Navigation|PathFinder|^Path|Sensing|Control$|Pathfinder|RandomPos', 'Mob AI'),
    (r'Renderer|Tesselator|Frustum|Culler|Font|Texture|Particle|Camera|Lighting|MemoryTracker|Tesselat|Chunk$|DirtyChunk', 'Rendering'),
    (r'Recip', 'Crafting'), (r'Menu|Slot|Container|Inventory', 'Menus and containers'),
    (r'Enchant', 'Enchanting'), (r'Potion|MobEffect', 'Effects'),
    (r'Tile$|Tiles$', 'Blocks'), (r'Item$|Items$', 'Items'),
    (r'Feature|Layer|Biome|Noise|Synth|Generator|Decorator|Structure|Piece|Canyon|Cave|Village|Stronghold|Mineshaft|Fortress|Temple', 'World generation'),
    (r'Tag$|Stream|Nbt|NBT|Io$|File|Region|Zone|Compression|compression|Archive|Save|Storage', 'Saves, NBT and IO'),
    (r'Chunk|Tick|Light|Spawner|Portal', 'Main loop and levels'),
    (r'Dimension', 'Dimensions'), (r'Achievement|Stat', 'Stats and achievements'),
    (r'Screen|Gui|Button|Scroll|Toast|Popup|EditBox|UI', 'Screens and UI'),
    (r'Sound|Music|Audio', 'Audio'),
    (r'Player|GameMode|Abilities|FoodData|Food', 'Player and game modes'),
    (r'Tutorial|Hint|Task$|Constraint', 'Tutorial'),
]
ORDER = ['Main loop and levels', 'Player and game modes', 'Entities', 'Mob AI', 'Blocks', 'Block entities',
         'Items', 'Crafting', 'Menus and containers', 'Enchanting', 'Effects', 'World generation',
         'Dimensions', 'Saves, NBT and IO', 'Rendering', 'Audio', 'Screens and UI', 'Tutorial',
         'Stats and achievements', 'Commands', 'Networking', 'Other']
SKIP_DIRS = {'Xbox', 'PS3', 'PSVita', 'Orbis', 'Durango', 'Windows64', 'PS3Media', 'PSVitaMedia',
             'OrbisMedia', 'DurangoMedia', 'Windows64Media'}


def source_classes():
    classes = {}
    for path in sorted(SOURCE.rglob('*.cpp')):
        rel = path.relative_to(SOURCE)
        if SKIP_DIRS & set(rel.parts) or path.stem.lower() in ('stdafx', 'system', 'main'):
            continue
        classes.setdefault(path.stem, rel)
    return classes


def base_classes():
    bases = {}
    pattern = re.compile(r'\bclass\s+(\w+)\s*(?:final\s*)?:\s*(?:public|protected|private)?\s*(\w+)')
    for path in SOURCE.rglob('*.h'):
        if SKIP_DIRS & set(path.relative_to(SOURCE).parts):
            continue
        for m in pattern.finditer(path.read_text(errors='replace')):
            bases.setdefault(m[1], m[2])
    return bases


def subsystem(name, bases):
    chain, seen = [], set()
    while name and name not in seen:
        seen.add(name)
        chain.append(name)
        name = bases.get(name)
    for cls in chain[1:]:
        for base, group in BASES:
            if cls == base:
                return group
    for cls in chain:
        for pattern, group in NAMES:
            if re.search(pattern, cls):
                return group
    return 'Other'


def port_text():
    names, text = set(), []
    for folder in PORT_DIRS:
        for path in folder.rglob('*'):
            if path.suffix not in ('.cpp', '.h', '.inc', '.py') or 'tutorial' in path.parts:
                continue
            if path.name == 'coverage_map.py':
                continue
            names.add(path.stem)
            text.append(path.read_text(errors='replace'))
    return names, '\n'.join(text)


def main():
    classes = source_classes()
    bases = base_classes()
    files, text = port_text()
    words = {}
    for w in re.findall(r'\b[A-Z]\w+', text):
        words[w] = words.get(w, 0) + 1
    rows = {}
    for name, rel in classes.items():
        if name in IMPORTED:
            status = 'ported'
        elif name in files:
            status = 'ported'
        elif words.get(name):
            status = 'partial'
        else:
            status = 'missing'
        rows.setdefault(subsystem(name, bases), []).append((name, str(rel), status))

    out = ['# Source coverage', '',
           'Generated by `tools/coverage_map.py` from `source_full/` (platform folders left out).',
           '**ported**: the port has a file for the class (the tutorial is imported verbatim);',
           '**partial**: the port names the class — a reimplemented piece or a cited rule — without',
           'a file of its own; **missing**: nothing refers to it. Partial is a lead, not a verdict;',
           'see `docs/PORT_STATUS.md` for what each piece covers.', '',
           '| Subsystem | Classes | Ported | Partial | Missing |', '|---|---:|---:|---:|---:|']
    total = [0, 0, 0, 0]
    for group in ORDER:
        entries = rows.get(group, [])
        if not entries:
            continue
        counts = [len(entries)] + [sum(1 for e in entries if e[2] == s) for s in ('ported', 'partial', 'missing')]
        total = [a + b for a, b in zip(total, counts)]
        out.append(f'| {group} | {counts[0]} | {counts[1]} | {counts[2]} | {counts[3]} |')
    out.append(f'| **All** | **{total[0]}** | **{total[1]}** | **{total[2]}** | **{total[3]}** |')
    for group in ORDER:
        entries = sorted(rows.get(group, []))
        if not entries:
            continue
        out += ['', f'## {group}', '']
        for status in ('ported', 'partial', 'missing'):
            names = [e[0] for e in entries if e[2] == status]
            if names:
                out.append(f'**{status.capitalize()} ({len(names)}):** ' + ', '.join(names))
                out.append('')
        if out[-1] == '':
            out.pop()
    result = '\n'.join(out) + '\n'
    if '--check' in sys.argv:
        if not OUT.exists() or OUT.read_text() != result:
            sys.exit('docs/COVERAGE.md is out of date: run tools/coverage_map.py')
        print('docs/COVERAGE.md is current')
        return
    OUT.write_text(result)
    print(f'wrote {OUT.relative_to(ROOT)}: {total[0]} classes, {total[1]} ported, {total[2]} partial, {total[3]} missing')


if __name__ == '__main__':
    main()
