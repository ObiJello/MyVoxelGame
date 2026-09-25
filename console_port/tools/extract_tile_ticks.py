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
    # Pistons: the base, the head and the moving piece with its tile entity,
    # and the push reactions pistons consult.
    ('Tile', 'getPistonPushReaction'), ('EntityTile', 'onRemove'),
    ('DoorTile', 'getPistonPushReaction'), ('IceTile', 'getPistonPushReaction'),
    ('PressurePlateTile', 'getPistonPushReaction'), ('BedTile', 'getPistonPushReaction'),
    ('RailTile', 'getPistonPushReaction'),
    ('PistonBaseTile', 'ignoreUpdate()'), ('PistonBaseTile', 'ignoreUpdate(bool set)'), ('PistonBaseTile', 'use'),
    ('PistonBaseTile', 'setPlacedBy'), ('PistonBaseTile', 'neighborChanged'), ('PistonBaseTile', 'onPlace'),
    ('PistonBaseTile', 'checkIfExtend'), ('PistonBaseTile', 'getNeighborSignal'), ('PistonBaseTile', 'triggerEvent'),
    ('PistonBaseTile', 'updateShape(LevelSource'), ('PistonBaseTile', 'getFacing'), ('PistonBaseTile', 'isExtended'),
    ('PistonBaseTile', 'getNewFacing'), ('PistonBaseTile', 'isPushable'), ('PistonBaseTile', 'canPush'),
    ('PistonBaseTile', 'createPush'),
    ('PistonExtensionTile', 'onRemove'), ('PistonExtensionTile', 'mayPlace(Level *level, int x, int y, int z)'),
    ('PistonExtensionTile', 'mayPlace(Level *level, int x, int y, int z, int face)'), ('PistonExtensionTile', 'updateShape'),
    ('PistonExtensionTile', 'neighborChanged'), ('PistonExtensionTile', 'getFacing'),
    ('PistonExtensionTile', 'addAABBs'),
    # TNT and explosions.
    ('Tile', 'getExplosionResistance'), ('Level', 'getSeenPercent'),
    ('TntTile', 'onPlace'), ('TntTile', 'neighborChanged'), ('TntTile', 'wasExploded'), ('TntTile', 'destroy'),
    ('TntTile', 'use'),
    ('Explosion', 'Explosion'), ('Explosion', '~Explosion'), ('Explosion', 'explode'), ('Explosion', 'finalizeExplosion'),
    ('PistonMovingPiece', 'onPlace'), ('PistonMovingPiece', 'onRemove'),
    ('PistonMovingPiece', 'mayPlace(Level *level, int x, int y, int z)'),
    ('PistonMovingPiece', 'mayPlace(Level *level, int x, int y, int z, int face)'), ('PistonMovingPiece', 'use'),
    ('PistonMovingPiece', 'spawnResources'), ('PistonMovingPiece', 'neighborChanged'),
    ('PistonMovingPiece', 'newMovingPieceEntity'), ('PistonMovingPiece', 'getAABB(Level *level, int x, int y, int z)'),
    ('PistonMovingPiece', 'getAABB(Level *level, int x, int y, int z, int tile'), ('PistonMovingPiece', 'updateShape'),
    ('PistonMovingPiece', 'getEntity'),
    ('PistonPieceEntity', 'PistonPieceEntity()'), ('PistonPieceEntity', 'PistonPieceEntity(int id'),
    ('PistonPieceEntity', 'getId'), ('PistonPieceEntity', 'getData'), ('PistonPieceEntity', 'isExtending'),
    ('PistonPieceEntity', 'getFacing'), ('PistonPieceEntity', 'isSourcePiston'), ('PistonPieceEntity', 'getProgress'),
    ('PistonPieceEntity', 'getXOff'), ('PistonPieceEntity', 'getYOff'), ('PistonPieceEntity', 'getZOff'),
    ('PistonPieceEntity', 'moveCollidedEntities'), ('PistonPieceEntity', 'finalTick'), ('PistonPieceEntity', 'tick'),
    # Dispensers: the tile, its tile entity's items, and what it fires.
    ('DispenserTile', 'getTickDelay'), ('DispenserTile', 'onPlace'), ('DispenserTile', 'recalcLockDir'),
    ('DispenserTile', 'TestUse'), ('DispenserTile', 'use'), ('DispenserTile', 'fireArrow'),
    ('DispenserTile', 'neighborChanged'), ('DispenserTile', 'tick'), ('DispenserTile', 'setPlacedBy'),
    ('DispenserTile', 'onRemove'), ('DispenserTile', 'throwItem'), ('DispenserTile', 'dispenseItem'),
    ('DispenserTileEntity', 'DispenserTileEntity()'), ('DispenserTileEntity', '~DispenserTileEntity'),
    ('DispenserTileEntity', 'getContainerSize'), ('DispenserTileEntity', 'getItem'),
    ('DispenserTileEntity', 'removeItem'), ('DispenserTileEntity', 'getRandomSlot'),
    ('DispenserTileEntity', 'setItem'), ('DispenserTileEntity', 'addItem'), ('DispenserTileEntity', 'getMaxStackSize'),
    ('BucketItem', 'emptyBucket'), ('PotionItem', 'isThrowable'),
    ('RailTile', 'isRail(Level *level, int x, int y, int z)'), ('RailTile', 'isRail(int id)'),
]
# Entity and mob methods, written to EntityRules.cpp (same host header).
ENTITY_METHODS = [
    # Damage sources.
    ('DamageSource', 'mobAttack'), ('DamageSource', 'playerAttack'), ('DamageSource', 'arrow'),
    ('DamageSource', 'thrown'), ('DamageSource', 'indirectMagic'), ('DamageSource', 'thorns'),
    ('DamageSource', 'isProjectile'), ('DamageSource', 'setProjectile'), ('DamageSource', 'isBypassArmor'),
    ('DamageSource', 'getFoodExhaustion'), ('DamageSource', 'isBypassInvul'),
    ('DamageSource', 'DamageSource(ChatPacket'), ('DamageSource', 'getDirectEntity'), ('DamageSource', 'getEntity'),
    ('DamageSource', 'bypassArmor'), ('DamageSource', 'bypassInvul'), ('DamageSource', 'setIsFire'),
    ('DamageSource', 'setScalesWithDifficulty'), ('DamageSource', 'scalesWithDifficulty'), ('DamageSource', 'isMagic'),
    ('DamageSource', 'setMagic'), ('DamageSource', 'isFire'), ('DamageSource', 'getMsgId'),
    ('EntityDamageSource', 'EntityDamageSource(ChatPacket'), ('EntityDamageSource', 'getEntity'),
    ('EntityDamageSource', 'scalesWithDifficulty'),
    ('IndirectEntityDamageSource', 'IndirectEntityDamageSource(ChatPacket'),
    ('IndirectEntityDamageSource', 'getDirectEntity'), ('IndirectEntityDamageSource', 'getEntity'),
    # The Level queries entity movement makes, liquids' flow and the light ramp.
    ('Level', 'getCubes'), ('Level', 'containsAnyLiquid'), ('Level', 'containsFireTile'),
    ('Level', 'checkAndHandleWater'), ('Level', 'containsMaterial'), ('Level', 'containsLiquid'),
    ('Level', 'getBrightness(int x, int y, int z)'), ('Level', 'hasChunkAt'), ('Level', 'getTileRenderShape'),
    ('Tile', 'isSolidFace'), ('LiquidTile', 'getHeight'), ('LiquidTile', 'getRenderedDepth'), ('LiquidTile', 'isSolidFace'),
    ('LiquidTile', 'getFlow'), ('LiquidTile', 'handleEntityInside'),
    ('Dimension', 'updateLightRamp'),
    # Entity.
    ('Entity', '_init'), ('Entity', 'Entity(Level'), ('Entity', '~Entity'), ('Entity', 'getEntityData'),
    ('Entity', 'resetPos'), ('Entity', 'remove'), ('Entity', 'setSize'), ('Entity', 'setRot'),
    ('Entity', 'setPos(double'), ('Entity', 'turn'), ('Entity', 'tick'), ('Entity', 'baseTick'),
    ('Entity', 'lavaHurt'), ('Entity', 'setOnFire'), ('Entity', 'clearFire'), ('Entity', 'outOfWorld'),
    ('Entity', 'isFree(float'), ('Entity', 'isFree(double'), ('Entity', 'move'), ('Entity', 'checkInsideTiles'),
    ('Entity', 'playSound'), ('Entity', 'makeStepSound'), ('Entity', 'checkFallDamage'), ('Entity', 'getCollideBox'),
    ('Entity', 'burn'), ('Entity', 'isFireImmune'), ('Entity', 'causeFallDamage'), ('Entity', 'isInWaterOrRain'),
    ('Entity', 'isInWater'), ('Entity', 'updateInWaterState'), ('Entity', 'isUnderLiquid'), ('Entity', 'getHeadHeight'),
    ('Entity', 'isInLava'), ('Entity', 'moveRelative'), ('Entity', 'getBrightness'), ('Entity', 'setLevel'),
    ('Entity', 'absMoveTo'), ('Entity', 'moveTo'), ('Entity', 'distanceTo(shared_ptr'),
    ('Entity', 'distanceToSqr(double'), ('Entity', 'distanceTo(double'), ('Entity', 'distanceToSqr(shared_ptr'),
    ('Entity', 'playerTouch'), ('Entity', 'push(shared_ptr'), ('Entity', 'push(double'), ('Entity', 'markHurt'),
    ('Entity', 'hurt'), ('Entity', 'intersects'), ('Entity', 'isPickable'), ('Entity', 'isPushable'),
    ('Entity', 'isShootable'), ('Entity', 'awardKillScore'), ('Entity', 'spawnAtLocation(int resource, int count)'),
    ('Entity', 'spawnAtLocation(int resource, int count, float'), ('Entity', 'spawnAtLocation(shared_ptr'),
    ('Entity', 'isAlive'), ('Entity', 'isInWall'), ('Entity', 'interact'), ('Entity', 'getCollideAgainstBox'),
    ('Entity', 'handleEntityEvent'), ('Entity', 'animateHurt'), ('Entity', 'isOnFire'), ('Entity', 'isRiding'),
    ('Entity', 'isSneaking'), ('Entity', 'setSneaking'), ('Entity', 'isSprinting'), ('Entity', 'setSprinting'),
    ('Entity', 'isInvisible'), ('Entity', 'setInvisible'), ('Entity', 'getSharedFlag'), ('Entity', 'setSharedFlag'),
    ('Entity', 'getAirSupply'), ('Entity', 'setAirSupply'), ('Entity', 'killed'), ('Entity', 'checkInTile'),
    ('Entity', 'makeStuckInWeb'), ('Entity', 'is'), ('Entity', 'getYHeadRot'), ('Entity', 'setYHeadRot'),
    ('Entity', 'isAttackable'), ('Entity', 'isInvulnerable'), ('Entity', 'copyPosition'),
]
# Static member definitions (one statement each), written before the methods
# of the same output: (class, member, output).
STATICS = [(
    'DamageSource', name, 'EntityRules.cpp') for name in (
    'inFire', 'onFire', 'lava', 'inWall', 'drown', 'starve', 'cactus', 'fall', 'outOfWorld', 'genericSource',
    'explosion', 'controlledExplosion', 'magic', 'dragonbreath', 'wither', 'anvil', 'fallingBlock')]
# Enums copied whole from their headers into .inc files the host includes:
# (header, enum name or None for the whole header, output).
ENUMS = [
    ('Class.h', 'eINSTANCEOF', 'ClassTypes.inc'),
    ('SoundTypes.h', 'eSOUND_TYPE', 'SoundTypes.inc'),
    ('ParticleTypes.h', None, 'ParticleTypes.inc'),
    ('ChatPacket.h', 'EChatPacketMessage', 'ChatMessages.inc'),
]
# Classes whose header constants (static const int / static bool) are written
# to TileConstants.inc as TILE_CONSTANTS_<Class> for the stand-in classes.
CONSTANT_CLASSES = ['FireTile', 'HeavyTile', 'TopSnowTile', 'DoorTile', 'TntTile', 'StairTile', 'HalfSlabTile',
                    'NotGateTile', 'DiodeTile', 'TrapDoorTile', 'FenceGateTile', 'PistonBaseTile',
                    'PistonExtensionTile', 'DispenserTile']
# The class's source file where it differs from the class name.
SOURCE_FILE = {'WaterlilyTile': 'WaterLilyTile.cpp'}


def source(cls):
    path = FILES[SOURCE_FILE.get(cls, cls + '.cpp').lower()]
    data = path.read_bytes()
    try:
        text = data.decode('utf-8-sig')
    except UnicodeDecodeError:
        # A few files carry Windows-1252 text in comments (PotionItem's section sign).
        text = data.decode('cp1252')
    return text.replace('\r\n', '\n')


def method(cls, name):
    text = source(cls)
    # Definitions start a line; qualified calls inside bodies are indented.
    pattern = r'^(?:\S[^\n]*?\b)?' + re.escape(f'{cls}::{name}') + ('' if '(' in name else r'\s*\(')
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


def static(cls, member):
    text = source(cls)
    matches = list(re.finditer(r'^[^\n]*\b' + re.escape(f'{cls}::{member}') + r'\s*=[^;]*;', text, re.M))
    if len(matches) != 1:
        raise SystemExit(f'{cls}::{member}: {len(matches)} static definitions')
    return matches[0][0]


def header_enum(header, name):
    text = FILES[header.lower()].read_text(encoding='utf-8-sig').replace('\r\n', '\n')
    if name is None:
        return text.replace('#pragma once\n', '')
    start = re.search(r'enum ' + re.escape(name) + r'\b', text)
    if not start:
        raise SystemExit(f'{header}: no enum {name}')
    end = text.index('};', start.start()) + 2
    return text[start.start():end] + '\n'


def parent_class(cls):
    header = FILES.get((cls + '.h').lower())
    if not header:
        return None
    m = re.search(r'class\s+' + re.escape(cls) + r'\s*:\s*public\s+(\w+)', header.read_text(encoding='utf-8-sig', errors='replace'))
    return m[1] if m else None


def render_shapes():
    """Tile::getRenderShape for each registered tile class: the class's own
    constant, or its nearest ancestor's (ported/TileProperties.cpp names them)."""
    names = sorted(set(re.findall(r'\{\d+,"(\w+)"', (ROOT / 'ported/TileProperties.cpp').read_text())))
    rows = []
    for name in names:
        cls, shape = name, None
        while cls and shape is None:
            path = FILES.get((cls + '.cpp').lower())
            if path:
                m = re.search(re.escape(cls) + r'::getRenderShape\(\)\s*\{\s*return\s+(?:Tile::)?(SHAPE_\w+)\s*;', source(cls))
                if m:
                    shape = m[1]
            cls = parent_class(cls)
        rows.append(f'CONSOLE_RENDER_SHAPE("{name}", {shape or "SHAPE_BLOCK"})\n')
    return '// Generated by tools/extract_tile_ticks.py: Tile::getRenderShape by class.\n' + ''.join(rows)


def rules_file(methods, output):
    out = ['// Generated by tools/extract_tile_ticks.py: original methods,',
           '// copied unchanged from source_full/Minecraft.World. Do not edit by hand.',
           '#include "TileTickHost.h"', '', 'namespace console::sim {', '']
    for cls, member, target in STATICS:
        if target == output:
            out.append(f'// {SOURCE_FILE.get(cls, cls + ".cpp")}')
            out.append(static(cls, member))
    if any(target == output for _, _, target in STATICS):
        out.append('')
    for cls, name in methods:
        out.append(f'// {SOURCE_FILE.get(cls, cls + ".cpp")}')
        out.append(method(cls, name))
        out.append('')
    out.append('}')
    return '\n'.join(out) + '\n'


def expected():
    rules = rules_file(METHODS, 'TileTickRules.cpp')
    tile_h = re.sub(r'//[^\n]*', '', FILES['tile.h'].read_text(encoding='utf-8-sig'))
    ids = ''.join(f'static const int {m[1]}_Id = {m[2]};\n'
                  for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', tile_h))
    ids += ''.join(f'static const int SHAPE_{m[1]} = {m[2]};\n'
                   for m in re.finditer(r'static const int SHAPE_(\w+)\s*=\s*(-?\d+)', tile_h))
    item_h = re.sub(r'//[^\n]*', '', FILES['item.h'].read_text(encoding='utf-8-sig'))
    item_ids = ''.join(f'static const int {m[1]}_Id = {m[2]};\n'
                       for m in re.finditer(r'static const int (\w+)_Id\s*=\s*(\d+)', item_h))
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
    files = {'RenderShapes.inc': render_shapes(), 'TileTickRules.cpp': rules, 'EntityRules.cpp': rules_file(ENTITY_METHODS, 'EntityRules.cpp'),
             'TileIds.inc': ids, 'ItemIds.inc': item_ids, 'TileConstants.inc': ''.join(constants)}
    for header, name, output in ENUMS:
        files[output] = f'// Generated by tools/extract_tile_ticks.py from {header}.\n' + header_enum(header, name)
    return files


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
