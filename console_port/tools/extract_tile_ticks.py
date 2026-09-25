#!/usr/bin/env python3
"""Extract the original random tile tick rules: ported/tick/TileTickRules.cpp.

Also the useOn of the farming items (hoes, seeds, bone meal, cocoa beans).

Copies each listed method unchanged from source_full/Minecraft.World into one
file that compiles against ported/tick/TileTickHost.h (namespace console::sim),
and writes the Tile::*_Id constants to ported/tick/TileIds.inc.

Run with --check to fail when the generated files differ from the source.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'source_full/Minecraft.World'
OUT = ROOT / 'ported/tick'
FILES = {p.name.lower(): p for p in SRC.iterdir()}

# (class, method) in the order they are written. A method may carry its full
# parameter list where the class overloads it.
METHODS = [
    ('Tile', 'mayPlace(Level *level, int x, int y, int z)'),
    ('Bush', 'mayPlaceOn'), ('Bush', 'tick'), ('Bush', 'checkAlive'), ('Bush', 'canSurvive'),
    ('DeadBushTile', 'mayPlaceOn'),
    ('WaterlilyTile', 'mayPlaceOn'), ('WaterlilyTile', 'canSurvive'),
    ('Sapling', 'tick'), ('Sapling', 'isSapling'),
    ('CropTile', 'mayPlaceOn'), ('CropTile', 'tick'), ('CropTile', 'getGrowthSpeed'),
    ('StemTile', 'mayPlaceOn'), ('StemTile', 'tick'), ('StemTile', 'getGrowthSpeed'),
    ('Mushroom', 'tick'), ('Mushroom', 'mayPlaceOn'), ('Mushroom', 'canSurvive'),
    ('NetherStalkTile', 'mayPlaceOn'), ('NetherStalkTile', 'canSurvive'), ('NetherStalkTile', 'tick'),
    ('GrassTile', 'tick'), ('GrassTile', 'shouldTileTick'),
    ('MycelTile', 'tick'),
    ('FarmTile', 'tick'), ('FarmTile', 'isUnderCrops'), ('FarmTile', 'isNearWater'),
    ('ReedTile', 'tick'), ('ReedTile', 'mayPlace'), ('ReedTile', 'canSurvive'), ('ReedTile', 'shouldTileTick'),
    ('CactusTile', 'tick'), ('CactusTile', 'canSurvive'), ('CactusTile', 'shouldTileTick'),
    ('DirectionalTile', 'getDirection'),
    ('CocoaTile', 'canSurvive'), ('CocoaTile', 'tick'), ('CocoaTile', 'getAge'),
    ('TreeTile', 'onRemove'), ('TreeTile', 'getWoodType'),
    ('LeafTile', 'onRemove'), ('LeafTile', 'tick'), ('LeafTile', 'die'), ('LeafTile', 'shouldTileTick'),
    ('VineTile', 'tick'), ('VineTile', 'isAcceptableNeighbor'),
    ('IceTile', 'tick'), ('IceTile', 'shouldTileTick'),
    ('TopSnowTile', 'tick'), ('TopSnowTile', 'shouldTileTick'),
    ('SnowTile', 'tick'), ('SnowTile', 'shouldTileTick'),
    ('RedStoneOreTile', 'tick'), ('RedStoneOreTile', 'shouldTileTick'),
    ('CauldronTile', 'handleRain'),
    ('CropTile', 'growCropsToMax'), ('StemTile', 'growCropsToMax'),
    ('CocoaTile', 'getPlacedOnFaceDataValue'),
    ('HoeItem', 'useOn'), ('SeedItem', 'useOn'), ('SeedFoodItem', 'useOn'), ('DyePowderItem', 'useOn'),
    ('FlintAndSteelItem', 'useOn'),
    # Tile updates: Level's neighbour and signal queries, then the tiles that
    # react to their neighbours, schedule ticks or burn.
    ('Level', 'updateNeighborsAt'), ('Level', 'neighborChanged'),
    ('Level', 'getDirectSignal'), ('Level', 'hasDirectSignal'), ('Level', 'getSignal'), ('Level', 'hasNeighborSignal'),
    ('Level', 'isTopSolidBlocking'), ('Level', 'isSolidBlockingTileInLoadedChunk'), ('Level', 'mayPlace'),
    ('Bush', 'neighborChanged'), ('CactusTile', 'neighborChanged'), ('CocoaTile', 'neighborChanged'),
    ('ReedTile', 'neighborChanged'), ('ReedTile', 'checkAlive'), ('FarmTile', 'neighborChanged'),
    ('VineTile', 'neighborChanged'), ('VineTile', 'updateSurvival'),
    ('TopSnowTile', 'mayPlace'), ('TopSnowTile', 'neighborChanged'), ('TopSnowTile', 'checkCanSurvive'),
    ('WoolCarpetTile', 'neighborChanged'), ('WoolCarpetTile', 'checkCanSurvive'),
    ('WoolCarpetTile', 'canSurvive'), ('WoolCarpetTile', 'mayPlace'),
    ('CakeTile', 'neighborChanged'), ('CakeTile', 'canSurvive'), ('CakeTile', 'mayPlace'),
    ('FlowerPotTile', 'neighborChanged'), ('SignTile', 'neighborChanged'),
    ('LadderTile', 'mayPlace'), ('LadderTile', 'getPlacedOnFaceDataValue'), ('LadderTile', 'neighborChanged'),
    ('TorchTile', 'isConnection'), ('TorchTile', 'mayPlace'), ('TorchTile', 'getPlacedOnFaceDataValue'),
    ('TorchTile', 'tick'), ('TorchTile', 'onPlace'), ('TorchTile', 'neighborChanged'),
    ('TorchTile', 'checkCanSurvive'), ('TorchTile', 'shouldTileTick'),
    ('DoorTile', 'neighborChanged'), ('DoorTile', 'setOpen'), ('DoorTile', 'getCompositeData'),
    ('HeavyTile', 'onPlace'), ('HeavyTile', 'neighborChanged'), ('HeavyTile', 'tick'), ('HeavyTile', 'checkSlide'),
    ('HeavyTile', 'falling'), ('HeavyTile', 'getTickDelay'), ('HeavyTile', 'isFree'), ('HeavyTile', 'onLand'),
    ('FireTile', 'init'), ('FireTile', 'setFlammable'), ('FireTile', 'getTickDelay'), ('FireTile', 'tick'),
    ('FireTile', 'checkBurnOut'), ('FireTile', 'isValidFireLocation'), ('FireTile', 'getFireOdds'),
    ('FireTile', 'canBurn'), ('FireTile', 'getFlammability'), ('FireTile', 'mayPlace'),
    ('FireTile', 'neighborChanged'), ('FireTile', 'onPlace'), ('FireTile', 'isFlammable'),
    ('LiquidTileStatic', 'tick'), ('LiquidTileStatic', 'isFlammable'),
    # Liquids: the flow itself, the lava/water reaction and the static/dynamic switch.
    ('LiquidTile', 'getDepth'), ('LiquidTile', 'getTickDelay'), ('LiquidTile', 'onPlace'),
    ('LiquidTile', 'neighborChanged'), ('LiquidTile', 'updateLiquid'), ('LiquidTile', 'fizz'),
    ('LiquidTileStatic', 'neighborChanged'), ('LiquidTileStatic', 'setDynamic'),
    ('LiquidTileDynamic', 'setStatic'), ('LiquidTileDynamic', 'iterativeTick'), ('LiquidTileDynamic', 'tick'),
    ('LiquidTileDynamic', 'mainTick'), ('LiquidTileDynamic', 'trySpreadTo'), ('LiquidTileDynamic', 'getSlopeDistance'),
    ('LiquidTileDynamic', 'getSpread'), ('LiquidTileDynamic', 'isWaterBlocking'), ('LiquidTileDynamic', 'getHighest'),
    ('LiquidTileDynamic', 'canSpreadTo'), ('LiquidTileDynamic', 'onPlace'),
    # Redstone: dust, torches, levers, buttons, pressure plates, repeaters,
    # lamps, and the doors, trapdoors and gates they open.
    ('Tile', 'setShape'), ('FenceTile', 'isFence'),
    ('RedStoneDustTile', 'mayPlace'), ('RedStoneDustTile', 'updatePowerStrength(Level *level, int x, int y, int z)'),
    ('RedStoneDustTile', 'updatePowerStrength(Level *level, int x, int y, int z, int xFrom'),
    ('RedStoneDustTile', 'checkCornerChangeAt'), ('RedStoneDustTile', 'onPlace'), ('RedStoneDustTile', 'onRemove'),
    ('RedStoneDustTile', 'checkTarget'), ('RedStoneDustTile', 'neighborChanged'), ('RedStoneDustTile', 'getDirectSignal'),
    ('RedStoneDustTile', 'getSignal'), ('RedStoneDustTile', 'isSignalSource'), ('RedStoneDustTile', 'shouldConnectTo'),
    ('RedStoneDustTile', 'shouldReceivePowerFrom'),
    ('NotGateTile', 'removeLevelReferences'), ('NotGateTile', 'isToggledTooFrequently'), ('NotGateTile', 'getTickDelay'),
    ('NotGateTile', 'onPlace'), ('NotGateTile', 'onRemove'), ('NotGateTile', 'getSignal'), ('NotGateTile', 'hasNeighborSignal'),
    ('NotGateTile', 'tick'), ('NotGateTile', 'neighborChanged'), ('NotGateTile', 'getDirectSignal'), ('NotGateTile', 'isSignalSource'),
    ('LeverTile', 'mayPlace(Level *level, int x, int y, int z, int face)'), ('LeverTile', 'mayPlace(Level *level, int x, int y, int z)'),
    ('LeverTile', 'getPlacedOnFaceDataValue'), ('LeverTile', 'getLeverFacing'), ('LeverTile', 'neighborChanged'),
    ('LeverTile', 'checkCanSurvive'), ('LeverTile', 'updateShape'), ('LeverTile', 'attack'), ('LeverTile', 'TestUse'),
    ('LeverTile', 'use'), ('LeverTile', 'onRemove'), ('LeverTile', 'getSignal'), ('LeverTile', 'getDirectSignal'),
    ('LeverTile', 'isSignalSource'),
    ('ButtonTile', 'getTickDelay'), ('ButtonTile', 'mayPlace(Level *level, int x, int y, int z, int face)'),
    ('ButtonTile', 'mayPlace(Level *level, int x, int y, int z)'), ('ButtonTile', 'getPlacedOnFaceDataValue'),
    ('ButtonTile', 'findFace'), ('ButtonTile', 'neighborChanged'), ('ButtonTile', 'checkCanSurvive'),
    ('ButtonTile', 'updateShape(LevelSource'), ('ButtonTile', 'updateShape(int data)'), ('ButtonTile', 'attack'),
    ('ButtonTile', 'TestUse'), ('ButtonTile', 'use'), ('ButtonTile', 'onRemove'), ('ButtonTile', 'getSignal'),
    ('ButtonTile', 'getDirectSignal'), ('ButtonTile', 'isSignalSource'), ('ButtonTile', 'tick'), ('ButtonTile', 'entityInside'),
    ('ButtonTile', 'checkPressed'), ('ButtonTile', 'updateNeighbours'), ('ButtonTile', 'shouldTileTick'),
    ('PressurePlateTile', 'getTickDelay'), ('PressurePlateTile', 'mayPlace'), ('PressurePlateTile', 'neighborChanged'),
    ('PressurePlateTile', 'tick'), ('PressurePlateTile', 'entityInside'), ('PressurePlateTile', 'checkPressed'),
    ('PressurePlateTile', 'onRemove'), ('PressurePlateTile', 'updateShape'), ('PressurePlateTile', 'getSignal'),
    ('PressurePlateTile', 'getDirectSignal'), ('PressurePlateTile', 'isSignalSource'), ('PressurePlateTile', 'shouldTileTick'),
    ('DiodeTile', 'mayPlace'), ('DiodeTile', 'canSurvive'), ('DiodeTile', 'tick'), ('DiodeTile', 'getDirectSignal'),
    ('DiodeTile', 'getSignal'), ('DiodeTile', 'neighborChanged'), ('DiodeTile', 'getSourceSignal'), ('DiodeTile', 'TestUse'),
    ('DiodeTile', 'use'), ('DiodeTile', 'isSignalSource'), ('DiodeTile', 'setPlacedBy'), ('DiodeTile', 'onPlace'),
    ('DiodeTile', 'destroy'),
    ('RedlightTile', 'onPlace'), ('RedlightTile', 'neighborChanged'), ('RedlightTile', 'tick'),
    ('TrapDoorTile', 'updateShape'), ('TrapDoorTile', 'setShape'), ('TrapDoorTile', 'attack'), ('TrapDoorTile', 'TestUse'),
    ('TrapDoorTile', 'use'), ('TrapDoorTile', 'setOpen'), ('TrapDoorTile', 'neighborChanged'), ('TrapDoorTile', 'getDir'),
    ('TrapDoorTile', 'getPlacedOnFaceDataValue'), ('TrapDoorTile', 'mayPlace'), ('TrapDoorTile', 'isOpen'),
    ('TrapDoorTile', 'attachesTo'),
    ('FenceGateTile', 'mayPlace'), ('FenceGateTile', 'setPlacedBy'), ('FenceGateTile', 'use'),
    ('FenceGateTile', 'neighborChanged'), ('FenceGateTile', 'isOpen'),
    ('DoorTile', 'TestUse'), ('DoorTile', 'use'),
]
# Classes whose header constants (static const int / static bool) are written
# to TileConstants.inc as TILE_CONSTANTS_<Class> for the stand-in classes.
CONSTANT_CLASSES = ['FireTile', 'HeavyTile', 'TopSnowTile', 'DoorTile', 'TntTile', 'StairTile', 'HalfSlabTile',
                    'NotGateTile', 'DiodeTile', 'TrapDoorTile', 'FenceGateTile']
# The class's source file where it differs from the class name.
SOURCE_FILE = {'WaterlilyTile': 'WaterLilyTile.cpp'}


def source(cls):
    path = FILES[SOURCE_FILE.get(cls, cls + '.cpp').lower()]
    return path.read_text(encoding='utf-8-sig', errors='strict').replace('\r\n', '\n')


def method(cls, name):
    text = source(cls)
    # Definitions start a line; qualified calls inside bodies are indented.
    pattern = r'^\S[^\n]*?\b' + re.escape(f'{cls}::{name}') + ('' if '(' in name else r'\s*\(')
    matches = [m for m in re.finditer(pattern, text, re.M)]
    if len(matches) != 1:
        raise SystemExit(f'{cls}::{name}: {len(matches)} definitions')
    start = text.rindex('\n', 0, matches[0].start()) + 1
    opening = text.index('{', matches[0].end())
    depth, end = 0, opening
    while True:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
        if depth == 0:
            return text[start:end]


def expected():
    out = ['// Generated by tools/extract_tile_ticks.py: original tile tick methods,',
           '// copied unchanged from source_full/Minecraft.World. Do not edit by hand.',
           '#include "TileTickHost.h"', '', 'namespace console::sim {', '']
    for cls, name in METHODS:
        out.append(f'// {SOURCE_FILE.get(cls, cls + ".cpp")}')
        out.append(method(cls, name))
        out.append('')
    out.append('}')
    rules = '\n'.join(out) + '\n'
    tile_h = re.sub(r'//[^\n]*', '', FILES['tile.h'].read_text(encoding='utf-8-sig'))
    ids = ''.join(f'static const int {m[1]}_Id = {m[2]};\n'
                  for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', tile_h))
    constants = []
    for cls in CONSTANT_CLASSES:
        header = re.sub(r'//[^\n]*', '', FILES[(cls + '.h').lower()].read_text(encoding='utf-8-sig'))
        body = re.sub(r'//[^\n]*', '', source(cls))
        lines = []
        for m in re.finditer(r'static const int (\w+)(\s*=\s*([^;]+))?;', header):
            value = m[3]
            if value is None:
                d = re.search(r'const int ' + cls + r'::' + m[1] + r'\s*=\s*([^;]+);', body)
                if not d:
                    raise SystemExit(f'{cls}::{m[1]} has no value')
                value = d[1]
            lines.append(f'static const int {m[1]} = {value.strip()};')
        constants.append(f'#define TILE_CONSTANTS_{cls} \\\n    ' + ' \\\n    '.join(lines) + '\n')
    return {'TileTickRules.cpp': rules, 'TileIds.inc': ids, 'TileConstants.inc': ''.join(constants)}


def main():
    stale = []
    for name, text in expected().items():
        target = OUT / name
        if '--check' in sys.argv:
            if not target.exists() or target.read_text() != text:
                stale.append(name)
        else:
            target.write_text(text)
    if stale:
        sys.exit('ported/tick is out of date (' + ', '.join(stale) + '): run tools/extract_tile_ticks.py')
    print('ported/tick matches the source' if '--check' in sys.argv else 'extracted ported/tick')


if __name__ == '__main__':
    main()
