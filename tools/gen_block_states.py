#!/usr/bin/env python3
"""Generate every block's BlockState property set, exactly as vanilla declares it.

WHY THIS EXISTS
---------------
The engine used to hand-write its property tables in `BlockRegistry.cpp`'s
`InitBlockStates`, keyed off a `StateKind` enum classified from model names.
That approach has three problems that no amount of care fixes:

1. It only covers the families someone got round to writing. 701 of the 1086
   blocks this engine defines carry properties in vanilla; the hand-written
   table knew about a couple of dozen.

2. The value ORDER was guessed, and the guesses are wrong. MC's `facing` is
   north,south,west,east — not compass order. `half` is top,bottom — not
   bottom,top. A slab's `type` is top,bottom,double. The order decides the
   numeric state index, so a wrong order is a silently different state space.

3. Property IDENTITY is (name, value-set), not name. `type` means three
   different things (slab top/bottom/double, chest single/left/right, piston
   normal/sticky), and so do `age` (eight distinct ranges), `half`, `shape`,
   `mode`, `axis`, `level`, `distance`, and the four side properties. A
   string-keyed table cannot tell them apart. There are 120 distinct
   properties across 13 reused names.

So we read the property sets from data instead of writing them.

WHERE THE DATA COMES FROM
-------------------------
`tools/protocol-gen/node_modules/minecraft-data/.../blocks.json`, already
vendored for the protocol generator. Per block it gives `minStateId`,
`maxStateId`, `defaultState`, and a `states[]` array of
`{name, type, num_values, values[]}`.

Two properties of that data make it directly usable:

* `states[]` is already in MC's sorted-by-NAME order, which is the order
  `StateDefinition` uses (`ImmutableSortedMap.copyOf`) and therefore the order
  the state index is a mixed-radix number over — alphabetically-last property
  varying fastest.
* `values[]` is in MC's own `getPossibleValues()` order, which is what
  `Property.getInternalIndex` returns an index into.

Both are asserted below against `maxStateId - minStateId + 1` rather than
trusted.

THE DEFAULT STATE IS NOT INDEX 0
--------------------------------
This is the fact that breaks the engine's old storage assumption, so it is
carried explicitly as `defaultIndex`. MC's `StateDefinition.any()` takes the
FIRST value of every property, and `BooleanProperty` lists `true` before
`false` — so `any()` is usually a nonsense state (waterlogged, powered, lit
all true) and every block then calls `registerDefaultState` to move off it.
567 of the 1086 blocks here have a default that is not index 0; `oak_stairs`
defaults to index 11 of 80, `acacia_fence` to 31 of 32.

BLOCKS THAT POSTDATE THE VENDORED DATA
--------------------------------------
65 rows in BlockDefs.inc are newer than this copy of minecraft-data — the
1.21.9 Copper Age set, the shelves, the oxidised lightning rods.
`minecraft_code_26.1-snapshot-1/decompiled_net/` IS that newer version, so the property sets
are in the repo; they just are not in `blocks.json`.

`Blocks.java` shows what class registers each one, and six of the eight
families reuse a class that an older block already uses:

    *copper_bars    -> IronBarsBlock          (same set as iron_bars)
    *copper_chain   -> ChainBlock             (chain)
    iron_chain      -> ChainBlock             (chain)
    *copper_chest   -> CopperChestBlock       (chest)
    *copper_lantern -> LanternBlock           (lantern)
    copper_torch    -> TorchBlock             (torch)
    copper_wall_torch -> WallTorchBlock       (wall_torch)
    *lightning_rod  -> LightningRodBlock      (lightning_rod)

Those are handled as ALIASES onto the older slug's upstream row, so their
value orders and — critically — their default indices come from the same
verified data as everything else rather than from arithmetic done by hand.

Only two families have no older analogue and are written out explicitly:
ShelfBlock (`builder.add(FACING, POWERED, SIDE_CHAIN_PART, WATERLOGGED)`,
ShelfBlock.java:86) and CopperGolemStatueBlock (`FACING, POSE, WATERLOGGED`,
CopperGolemStatueBlock.java:66). Even for those the DEFAULT INDEX is computed
below from the declared default values, not written down — hand-computing a
mixed-radix index is exactly the kind of silent error this file exists to
avoid.

An engine slug that resolves in NEITHER source is a hard failure. Emitting it
with no properties would be a silent degradation of precisely the kind this
codebase has already been bitten by, and a new block added to BlockDefs.inc
should stop the generator until someone says what its states are.

Output: src/common/world/block/GeneratedBlockStates.{hpp,cpp}
Run:    python3 tools/gen_block_states.py
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MCDATA = os.path.join(
    ROOT, "tools", "protocol-gen", "node_modules", "minecraft-data",
    "minecraft-data", "data", "pc", "1.21.6", "blocks.json")
BLOCKDEFS = os.path.join(ROOT, "src", "common", "world", "block", "BlockDefs.inc")
BLOCKS_HPP = os.path.join(ROOT, "src", "common", "world", "block", "Blocks.hpp")
OUT_DIR = os.path.join(ROOT, "src", "common", "world", "block")

KIND = {"bool": 0, "int": 1, "enum": 2}

# ── Blocks newer than the vendored blocks.json ──────────────────────────────
# See the module docstring. Aliases borrow an older slug's upstream row because
# MC registers them with the same block class, so the property set, the value
# orders and the default are identical by construction.
ALIAS_SUFFIX = [
    # (suffix or exact name, slug whose upstream row to copy)
    ("copper_bars",    "iron_bars"),
    ("copper_chain",   "chain"),
    ("copper_chest",   "chest"),
    ("copper_lantern", "lantern"),
    ("lightning_rod",  "lightning_rod"),
]
ALIAS_EXACT = {
    "iron_chain":        "chain",
    "copper_torch":      "torch",
    "copper_wall_torch": "wall_torch",
    # engine blocks (redstone_plus): the zero-delay torch shares the redstone torch's states
    "blue_redstone_torch":      "redstone_torch",
    "blue_redstone_wall_torch": "redstone_wall_torch",
    # 26.2/26.3 blocks (2026-09-05) — the upstream data is 1.21.6, so every
    # one of them copies the older block MC registers with the same class.
    "cinnabar": "stone", "chiseled_cinnabar": "stone", "polished_cinnabar": "stone",
    "cinnabar_bricks": "stone_bricks",
    "sulfur": "stone", "chiseled_sulfur": "stone", "polished_sulfur": "stone",
    "sulfur_bricks": "stone_bricks",
    "golden_dandelion": "dandelion", "potted_golden_dandelion": "potted_dandelion",
    "red_shrub": "dead_bush",
    "straw_bed": "red_bed",                    # AbstractBedBlock: facing/part/occupied
    "sulfur_spike": "pointed_dripstone",       # SpeleothemBlock: tip dir/thickness/waterlogged
    "poplar_planks": "oak_planks", "poplar_log": "oak_log", "poplar_wood": "oak_wood",
    "stripped_poplar_log": "stripped_oak_log", "stripped_poplar_wood": "stripped_oak_wood",
    "poplar_sapling": "oak_sapling", "potted_poplar_sapling": "potted_oak_sapling",
    "orange_poplar_leaves": "oak_leaves", "red_poplar_leaves": "oak_leaves",
    "yellow_poplar_leaves": "oak_leaves",
    # The Hush (engine dimension, 2026-09-21). Each property set MUST equal
    # the terrain library's block class for the same id (Blocks.cpp
    # bootstrap) or generated chunks unpack to air.
    "hushstone": "stone", "polished_hushstone": "stone",
    "hushstone_bricks": "stone_bricks",
    "sculk_loam": "dirt",
    "echo_ore": "lapis_ore",
    "resonant_crystal": "amethyst_block",
    "whisperwood_planks": "oak_planks",
    "hush_moss": "moss_block",
    "resonance_bloom": "dandelion",            # BushBlock: no properties
    "whisperwood_log": "oak_log",              # RotatedPillarBlock: axis x/y/z
    "lantern_leaves": "oak_leaves",            # LeavesBlock: distance/persistent/waterlogged
    "hush_portal": "nether_portal",            # HORIZONTAL_AXIS: axis x/z
    # The Hush, second drop (2026-09-22). The whisperwood stairs/slab/fence/
    # fence_gate/door/trapdoor and the hushstone stairs/slabs/wall resolve
    # through ALIAS_SUFFIX below (oak_* / cobblestone_wall); these are the
    # ones no suffix covers.
    "stripped_whisperwood_log": "stripped_oak_log",   # RotatedPillarBlock: axis
    "whisperwood_sapling": "oak_sapling",             # SaplingBlock: stage 0/1
    "cracked_hushstone_bricks": "stone",
    "chiseled_hushstone_bricks": "stone",
    "hush_grass": "short_grass",                      # BushBlock: no properties
    "resonant_cluster": "amethyst_cluster",           # facing (6) + waterlogged
    "resonite_ore": "lapis_ore",
    "resonite_block": "iron_block",
    "echo_lantern": "lantern",                        # hanging + waterlogged
    "echo_core": "stone",
    # The Choir Hall puzzle (2026-09-22): the chime has redstone_lamp's single
    # `lit` (SingleBoolBlockImpl LIT in Blocks.cpp); the altar is a plain cube.
    "resonant_chime": "redstone_lamp",
    "choir_altar": "stone",
    # The tools of the deep (2026-09-22): the echo heart is a beacon-class
    # cube (no properties). hanging_whisperfruit is in EXPLICIT (age 0..2 and
    # nothing else — no vanilla block has exactly that set).
    "echo_heart": "beacon",
    # The Hush lighthouse (2026-09-22): the lamp is a beacon-class cube with a
    # block entity (createSimpleBlock in Blocks.cpp: no properties).
    "hush_lighthouse_lamp": "beacon",
    # Aurelith, the Lantern City (2026-09-22). The choirstone stairs/slabs/
    # wall resolve through ALIAS_SUFFIX (oak_stairs / oak_slab /
    # cobblestone_wall); these are the rest. Each class is the one Blocks.cpp
    # registers for the same id.
    "choirstone": "stone", "polished_choirstone": "stone",
    "choirstone_bricks": "stone_bricks",
    "cracked_choirstone_bricks": "stone", "chiseled_choirstone": "stone",
    "choirstone_tiles": "stone",
    "choirstone_pillar": "quartz_pillar",               # RotatedPillarBlock: axis
    "stave_stone": "stone",
    "nightglass": "tinted_glass",                       # TintedGlassBlock: no properties
    "resonite_grate": "copper_grate",                   # WaterloggedTransparentBlock: waterlogged
    "cyan_lumen_panel": "sea_lantern", "violet_lumen_panel": "sea_lantern",
    "amber_lumen_panel": "sea_lantern",
    "lumen_strip": "quartz_pillar",                     # RotatedPillarBlock: the band runs along axis
    "crystal_conduit": "chain",                         # ChainBlock: axis + waterlogged
    "choir_lamp": "lantern",                            # LanternBlock: hanging + waterlogged
    # The river Vesper's water: an always-water block with NO properties (its
    # fluid state is a water source unconditionally, bubble_column's rule in
    # gen_waterlogged.py; it never flows, so it has no `level`).
    "resonant_water": "stone",
    "resonance_engine": "beacon",                       # block entity, no properties
    "voice_beacon": "carved_pumpkin",                   # facing n/s/w/e = the voice
    # Aurelith, reawakening the Heart (2026-09-22). The dormant city's dim
    # lights have their lit twins' classes; the chord socket is a
    # stonecutter-class block (HORIZONTAL_FACING only), the pedestal a plain
    # block, the choir cabinet a barrel (FACING 6-way + OPEN).
    "dim_cyan_lumen_panel": "sea_lantern", "dim_violet_lumen_panel": "sea_lantern",
    "dim_amber_lumen_panel": "sea_lantern",
    "dim_lumen_strip": "quartz_pillar",
    "dim_stave_stone": "stone",
    "dim_choir_lamp": "lantern",
    "chord_socket": "stonecutter",
    "voice_pedestal": "stone",
    "choir_cabinet": "barrel",
    # Aurelith's flickering windows (2026-09-22): sea-lantern-class cubes.
    "guttering_amber_window": "sea_lantern",
    "waking_amber_window": "sea_lantern",
    "restless_amber_window": "sea_lantern",
    "guttering_cyan_window": "sea_lantern",
    "waking_cyan_window": "sea_lantern",
    "guttering_violet_window": "sea_lantern",
    "waking_violet_window": "sea_lantern",
    # ── Twilight Forest + The Aether (pass one, docs/mod-ports.md). Same rule
    # as the Hush: the property set of each alias MUST equal the terrain
    # library's class for that id (Blocks.cpp) or generated chunks unpack to
    # air. Each row names the mod class it stands in for; where the mod class
    # carries a property no vanilla block shares, it is DROPPED in pass one:
    #   * The Aether's `double_drops` (AetherBlockStateProperties.DOUBLE_DROPS,
    #     on its grass/dirt/holystone/quicksoil/aerclouds/ores/logs/leaves/
    #     bushes) — the "placed by worldgen, drops twice" marker; pass two.
    #   * TF's twilight_portal `is_one_way` (TFPortalBlock.DISALLOW_RETURN).
    #   * TF's PatchBlock north/east/south/west connection booleans (clover
    #     and moss patch) — no vanilla block has exactly that set.
    #   * TF's torchberry_plant `has_torchberries` (TorchberryPlantBlock) —
    #     the plant is always lit and always drops berries in pass one.
    #   * TF's moonworm `waterlogged` and tf_mangrove_sapling `waterlogged`
    #     (MoonwormBlock, MangroveSaplingBlock implement SimpleWaterloggedBlock).
    # TF logs/stripped logs: RotatedPillarBlock (axis).
    "twilight_oak_log": "oak_log", "canopy_log": "oak_log",
    "tf_mangrove_log": "oak_log", "dark_log": "oak_log",
    "stripped_twilight_oak_log": "stripped_oak_log", "stripped_canopy_log": "stripped_oak_log",
    "stripped_tf_mangrove_log": "stripped_oak_log", "stripped_dark_log": "stripped_oak_log",
    # TintedParticleLeavesBlock / DarkLeavesBlock: LeavesBlock (distance/persistent/waterlogged).
    "twilight_oak_leaves": "oak_leaves", "canopy_leaves": "oak_leaves",
    "tf_mangrove_leaves": "oak_leaves", "dark_leaves": "oak_leaves",
    "twilight_oak_planks": "oak_planks", "canopy_planks": "oak_planks",
    "tf_mangrove_planks": "oak_planks", "dark_planks": "oak_planks",
    # SaplingBlock (stage); MangroveSaplingBlock's waterlogged dropped (above).
    "twilight_oak_sapling": "oak_sapling", "canopy_sapling": "oak_sapling",
    "tf_mangrove_sapling": "oak_sapling", "darkwood_sapling": "oak_sapling",
    # Plain Block subclasses with no properties: HardenedDarkLeavesBlock,
    # Block (root, mazestone family), LiverootBlock, HedgeBlock, AuroraBrickBlock.
    "hardened_dark_leaves": "stone", "root": "stone", "liveroot_block": "stone",
    "hedge": "stone",
    "mazestone": "stone", "mazestone_brick": "stone", "cracked_mazestone": "stone",
    "mossy_mazestone": "stone", "mazestone_mosaic": "stone", "mazestone_border": "stone",
    "aurora_block": "stone",
    # CritterBlock: DirectionalBlock.FACING (6-way, default up) — EndRodBlock's set.
    "firefly": "end_rod", "cicada": "end_rod", "moonworm": "end_rod",
    # MushgloomBlock (MushroomBlock), MayappleBlock / TorchberryPlantBlock (TFPlantBlock): no properties.
    "mushgloom": "dandelion", "mayapple": "dandelion", "torchberry_plant": "dandelion",
    "fiddlehead": "short_grass",                      # FiddleheadBlock (TFPlantBlock, replaceable)
    "clover_patch": "moss_carpet", "moss_patch": "moss_carpet",   # PatchBlock, NESW dropped
    # FallenLeavesBlock: BlockStateProperties.LAYERS (1..8, default 1) and
    # nothing else — exactly SnowLayerBlock's set, so it aliases `snow`.
    "fallen_leaves": "snow",
    "twilight_portal": "stone",                       # TFPortalBlock, is_one_way dropped
    # The Aether. AetherGrassBlock: GrassBlock (snowy) + double_drops dropped.
    "aether_grass_block": "grass_block",
    # AetherDoubleDropBlock / QuicksoilBlock / AercloudBlock / BlueAercloudBlock /
    # IcestoneBlock / AetherDoubleDropsOreBlock / DropExperienceBlock /
    # FloatingBlock / Block: no properties once double_drops is dropped.
    "aether_dirt": "stone", "quicksoil": "stone", "holystone": "stone",
    "mossy_holystone": "stone", "holystone_bricks": "stone",
    "cold_aercloud": "stone", "blue_aercloud": "stone", "golden_aercloud": "stone",
    "icestone": "stone", "ambrosium_ore": "stone", "zanite_ore": "stone",
    "gravitite_ore": "stone",
    # AetherLogBlock: RotatedPillarBlock (axis) + double_drops dropped.
    "skyroot_log": "oak_log", "golden_oak_log": "oak_log",
    "stripped_skyroot_log": "stripped_oak_log",
    # AetherDoubleDropsLeaves / LeavesWithParticlesBlock: LeavesBlock + double_drops dropped.
    "skyroot_leaves": "oak_leaves", "golden_oak_leaves": "oak_leaves",
    "skyroot_planks": "oak_planks",
    "skyroot_sapling": "oak_sapling", "golden_oak_sapling": "oak_sapling",
    "aether_portal": "nether_portal",                 # AetherPortalBlock: HORIZONTAL_AXIS x/z
    "berry_bush": "stone", "berry_bush_stem": "stone",  # AetherBushBlock + double_drops dropped
    "white_flower": "dandelion", "purple_flower": "dandelion",  # AetherFlowerBlock (FlowerBlock)
    # ── The Aether, pass two. The stairs/slab/wall/fence/fence_gate/door/
    # trapdoor/button/pressure_plate rows resolve through ALIAS_SUFFIX below
    # (StairBlock, SlabBlock, WallBlock/IcestoneWallBlock, FenceBlock, …, the
    # same classes vanilla uses); these are the ones no suffix covers.
    # AetherLogBlock (skyroot/golden oak wood) and RotatedPillarBlock
    # (stripped skyroot wood, the dungeon pillar): axis, double_drops dropped.
    "skyroot_wood": "oak_wood", "golden_oak_wood": "oak_wood",
    "stripped_skyroot_wood": "stripped_oak_wood",
    "pillar": "oak_log",
    "pillar_top": "end_rod",                          # FacingPillarBlock: DirectionalBlock.FACING, default up
    # TorchBlock / WallTorchBlock (AetherBlocks.AMBROSIUM_TORCH copies Blocks.TORCH).
    "ambrosium_torch": "torch", "ambrosium_wall_torch": "wall_torch",
    # HalfTransparentBlock (AerogelBlock) / TransparentBlock (QuicksoilGlassBlock)
    # and plain Blocks: no properties.
    "aerogel": "glass", "quicksoil_glass": "glass",
    "ambrosium_block": "stone", "zanite_block": "stone",
    "enchanted_gravitite": "stone",                   # FloatingBlock(powered=true)
    # The dungeon families. Block (carved/sentry/angelic/hellfire, their
    # lights and locked twins), TrappedBlock: no properties. DoorwayBlock's
    # `invisible` and TreasureDoorwayBlock's horizontal `facing` are DROPPED
    # for now (the doorway blockstates ignore facing: one model per block).
    "carved_stone": "stone", "sentry_stone": "stone",
    "angelic_stone": "stone", "light_angelic_stone": "stone",
    "hellfire_stone": "stone", "light_hellfire_stone": "stone",
    "locked_carved_stone": "stone", "locked_sentry_stone": "stone",
    "locked_angelic_stone": "stone", "locked_light_angelic_stone": "stone",
    "locked_hellfire_stone": "stone", "locked_light_hellfire_stone": "stone",
    "trapped_carved_stone": "stone", "trapped_sentry_stone": "stone",
    "trapped_angelic_stone": "stone", "trapped_light_angelic_stone": "stone",
    "trapped_hellfire_stone": "stone", "trapped_light_hellfire_stone": "stone",
    "boss_doorway_carved_stone": "stone", "boss_doorway_sentry_stone": "stone",
    "boss_doorway_angelic_stone": "stone", "boss_doorway_light_angelic_stone": "stone",
    "boss_doorway_hellfire_stone": "stone", "boss_doorway_light_hellfire_stone": "stone",
    "treasure_doorway_carved_stone": "stone", "treasure_doorway_sentry_stone": "stone",
    "treasure_doorway_angelic_stone": "stone", "treasure_doorway_light_angelic_stone": "stone",
    "treasure_doorway_hellfire_stone": "stone", "treasure_doorway_light_hellfire_stone": "stone",
    # ── Twilight Forest, pass two. Castle stairs resolve through ALIAS_SUFFIX.
    # Block (towerwood family — InfestedTowerwoodBlock too, castle bricks and
    # rune bricks, deadrock, the storage blocks): no properties.
    **{s: "stone" for s in (
        "towerwood", "encased_towerwood", "cracked_towerwood", "mossy_towerwood",
        "infested_towerwood", "castle_brick", "worn_castle_brick", "cracked_castle_brick",
        "castle_roof_tile", "mossy_castle_brick", "thick_castle_brick",
        "encased_castle_brick_tile", "bold_castle_brick_tile",
        "pink_castle_rune_brick", "blue_castle_rune_brick", "yellow_castle_rune_brick",
        "violet_castle_rune_brick", "deadrock", "cracked_deadrock", "weathered_deadrock",
        "ironwood_block", "steeleaf_block", "knightmetal_block", "fiery_block")},
    # TrollsteinnBlock's six per-face `lit` booleans (named down/up/north/…)
    # are DROPPED: the block is always its unlit self for now.
    "trollsteinn": "stone",
    # RotatedPillarBlock (axis).
    "encased_castle_brick_pillar": "oak_log", "bold_castle_brick_pillar": "oak_log",
    "cinder_log": "oak_log", "cinder_wood": "oak_wood",
    # ThornsBlock / BurntThornsBlock: ConnectableRotatedPillarBlock's axis +
    # six connection booleans + waterlogged. Axis kept; connections and
    # waterlogged DROPPED (the model is the axis pillar alone).
    "brown_thorns": "oak_log", "green_thorns": "oak_log", "burnt_thorns": "oak_log",
    "huge_water_lily": "lily_pad",                    # HugeWaterLilyBlock (LilyPadBlock)
    "uncrafting_table": "stone_pressure_plate",       # UncraftingTableBlock: POWERED only
}
# Generic family suffixes for the same additions: only consulted when the
# upstream row is missing, so they never override a 1.21.6 block. Order
# matters — the more specific sign/fence forms come first. 26.3's dyed wool
# and concrete stairs and slabs (Blocks.WOOL_STAIRS / WOOL_SLAB /
# CONCRETE_STAIRS / CONCRETE_SLAB — registerStair / registerSlab, so
# StairBlock / SlabBlock) resolve through `_stairs` / `_slab` here.
ALIAS_SUFFIX += [
    ("_wall_hanging_sign", "oak_wall_hanging_sign"),
    ("_hanging_sign",      "oak_hanging_sign"),
    ("_wall_sign",         "oak_wall_sign"),
    ("_sign",              "oak_sign"),
    ("_fence_gate",        "oak_fence_gate"),
    ("_fence",             "oak_fence"),
    ("_door",              "oak_door"),
    ("_trapdoor",          "oak_trapdoor"),
    ("_button",            "oak_button"),
    ("_pressure_plate",    "oak_pressure_plate"),
    ("_slab",              "oak_slab"),
    ("_stairs",            "oak_stairs"),
    ("_wall",              "cobblestone_wall"),
]

# Not blocks: artifacts of the block-list generator. No properties, on purpose.
NOT_A_BLOCK = {"model_name", "set_spawn", "ominous_banner"}

# The two families with no older analogue. Values are MC's getPossibleValues()
# order; `default` is the value the class's registerDefaultState installs. The
# INDEX is computed from these, never written down.
EXPLICIT = {
    # ShelfBlock.java:86 + :66
    "*_shelf": [
        ("facing",      "enum", ["north", "south", "west", "east"],            "north"),
        ("powered",     "bool", ["true", "false"],                             "false"),
        ("side_chain",  "enum", ["unconnected", "right", "center", "left"],    "unconnected"),
        ("waterlogged", "bool", ["true", "false"],                             "false"),
    ],
    # CopperGolemStatueBlock.java:66 + :61
    # 26.3 PotentSulfurBlock: BlockStateProperties.POTENT_SULFUR_STATE, which
    # is EnumProperty.create("potent_sulfur_state", PotentSulfurState.class)
    # (BlockStateProperties.java:246) — the NAME is what saves and the
    # terrain library's sulfur_pool feature carry, so it must be this one.
    "potent_sulfur": [
        ("potent_sulfur_state", "enum", ["dry", "wet", "dormant", "erupting", "continuous"], "dry"),
    ],
    # 26.3 ShelfMushroomBlock: builder.add(FACING, AGE) with AGE_1.
    "shelf_mushroom": [
        ("age",    "int",  ["0", "1"],                          "0"),
        ("facing", "enum", ["north", "south", "west", "east"], "north"),
    ],
    # Engine block (redstone_plus): RedstoneComponents.cpp "DisplayBlock". Three colour bits, the
    # check output, and the two operations (north&west, south&east); see the block comment there.
    "display_block": [
        ("blue",    "bool", ["true", "false"], "false"),
        ("green",   "bool", ["true", "false"], "false"),
        ("op_nw",   "enum", ["none", "set_r", "set_g", "set_b", "clear_r", "clear_g", "clear_b", "show_r", "show_g", "show_b", "check_r", "check_g", "check_b", "latch"], "none"),
        ("op_se",   "enum", ["none", "set_r", "set_g", "set_b", "clear_r", "clear_g", "clear_b", "show_r", "show_g", "show_b", "check_r", "check_g", "check_b", "latch"], "none"),
        ("powered", "bool", ["true", "false"], "false"),
        ("red",     "bool", ["true", "false"], "false"),
    ],
    # Engine block (The Hush): hanging_whisperfruit — cocoa's AGE_2 without
    # its facing (it hangs straight down from lantern leaves). The terrain
    # library registers it as SingleIntBlockImpl(AGE_2, 0) (Blocks.cpp).
    "hanging_whisperfruit": [
        ("age", "int", ["0", "1", "2"], "0"),
    ],
    "*copper_golem_statue": [
        ("copper_golem_pose", "enum", ["standing", "sitting", "running", "star"], "standing"),
        ("facing",            "enum", ["north", "south", "west", "east"],         "north"),
        ("waterlogged",       "bool", ["true", "false"],                          "false"),
    ],
}


# ── Property identity -> MC's own constant name ─────────────────────────────
# 13 property NAMES are reused with different value sets, so a name alone
# cannot identify a property (`type` means three different things). MC solves
# this by giving each identity its own constant in BlockStateProperties.java,
# and those names are transcribed here verbatim so the C++ constant a reader
# sees is the one they would find in the decompiled source.
#
# Keyed on the exact value tuple, never on the value COUNT: MODE_COMPARATOR
# and TEST_BLOCK_MODE both have four values and are different properties.
AMBIGUOUS_CONSTANT = {
    ("facing", ("north", "south", "west", "east")):               "HORIZONTAL_FACING",
    ("facing", ("north", "east", "south", "west", "up", "down")): "FACING",
    ("facing", ("down", "north", "south", "west", "east")):       "FACING_HOPPER",
    ("axis",   ("x", "y", "z")):                                  "AXIS",
    ("axis",   ("x", "z")):                                       "HORIZONTAL_AXIS",
    ("half",   ("top", "bottom")):                                "HALF",
    ("half",   ("upper", "lower")):                               "DOUBLE_BLOCK_HALF",
    ("type",   ("top", "bottom", "double")):                      "SLAB_TYPE",
    ("type",   ("single", "left", "right")):                      "CHEST_TYPE",
    ("type",   ("normal", "sticky")):                             "PISTON_TYPE",
    ("mode",   ("compare", "subtract")):                          "MODE_COMPARATOR",
    ("mode",   ("save", "load", "corner", "data")):               "STRUCTUREBLOCK_MODE",
    ("mode",   ("start", "log", "fail", "accept")):               "TEST_BLOCK_MODE",
    ("shape",  ("straight", "inner_left", "inner_right",
                "outer_left", "outer_right")):                    "STAIRS_SHAPE",
    ("distance", tuple(str(i) for i in range(1, 8))):             "DISTANCE",
    ("distance", tuple(str(i) for i in range(0, 8))):             "STABILITY_DISTANCE",
    ("level",  ("1", "2", "3")):                                  "LEVEL_CAULDRON",
    ("level",  tuple(str(i) for i in range(0, 9))):               "LEVEL_COMPOSTER",
    ("level",  tuple(str(i) for i in range(1, 9))):               "LEVEL_FLOWING",
    ("level",  tuple(str(i) for i in range(0, 16))):              "LEVEL",
}
# The four side properties share one pattern across three value sets.
for _side in ("north", "east", "south", "west"):
    AMBIGUOUS_CONSTANT[(_side, ("true", "false"))] = _side.upper()
    AMBIGUOUS_CONSTANT[(_side, ("none", "low", "tall"))] = _side.upper() + "_WALL"
    AMBIGUOUS_CONSTANT[(_side, ("up", "side", "none"))] = _side.upper() + "_REDSTONE"
# `age` and the rail shapes disambiguate on size alone.
for _hi in (1, 2, 3, 4, 5, 7, 15, 25):
    AMBIGUOUS_CONSTANT[("age", tuple(str(i) for i in range(_hi + 1)))] = f"AGE_{_hi}"
_RAIL = ("north_south", "east_west", "ascending_east", "ascending_west",
         "ascending_north", "ascending_south")
AMBIGUOUS_CONSTANT[("shape", _RAIL)] = "RAIL_SHAPE_STRAIGHT"
AMBIGUOUS_CONSTANT[("shape", _RAIL + ("south_east", "south_west",
                                      "north_west", "north_east"))] = "RAIL_SHAPE"


def constant_name(name, values, ambiguous_names):
    """MC's BlockStateProperties constant for this (name, value-set)."""
    if name not in ambiguous_names:
        return name.upper()
    key = (name, tuple(values))
    if key not in AMBIGUOUS_CONSTANT:
        raise SystemExit(
            f"property {name!r} with values {list(values)} shares its name with another\n"
            f"  property but has no entry in AMBIGUOUS_CONSTANT. Auto-naming it would\n"
            f"  produce two constants a reader cannot tell apart — add its\n"
            f"  BlockStateProperties.java constant name to the map.")
    return AMBIGUOUS_CONSTANT[key]


def explicit_for(slug):
    """The EXPLICIT entry whose pattern this slug matches, or None."""
    for pattern, props in EXPLICIT.items():
        bare = pattern.lstrip("*")
        if slug.endswith(bare):
            return props
    return None


def alias_for(slug):
    """The older slug whose upstream row this one should copy, or None."""
    if slug in ALIAS_EXACT:
        return ALIAS_EXACT[slug]
    for suffix, target in ALIAS_SUFFIX:
        if slug.endswith(suffix):
            return target
    return None


def load_engine_slugs():
    """Column 2 of BlockDefs.inc, in declaration order."""
    src = open(BLOCKDEFS).read()
    return [m.group(2) for m in re.finditer(r'BLOCK_DEF\(\s*(\w+)\s*,\s*"([^"]+)"', src)]


def count_manual_block_ids():
    """Enumerators Blocks.hpp declares BEYOND the BlockDefs.inc include.

    These are the synthetic variants — *SlabTop, *SlabDouble, SnowGrass, the
    double-plant tops, leaf_litter_N — that spend a whole BlockID on one
    property value. They carry no properties, so each is exactly one state, but
    they are still part of the global id space and kBlockStateCount has to
    include them or the palette width is derived from the wrong number.

    They are slated to collapse back into real properties, at which point this
    returns 0 on its own.
    """
    src = open(BLOCKS_HPP).read()
    body = src.split("#undef BLOCK_DEF", 1)[1].split("Count", 1)[0]
    return len(re.findall(r'^\s*([A-Za-z]\w*)\s*,', body, re.M))


def property_values(state):
    """MC's getPossibleValues() for one property, as strings.

    A bool has no `values` array in the data because MC's BooleanProperty is a
    fixed [true, false] — and the order matters, so it is spelled out here
    rather than inferred.
    """
    if state["type"] == "bool":
        return ["true", "false"]
    vals = state.get("values")
    if not vals:
        raise SystemExit(f"property {state['name']!r} has no values array")
    return [str(v) for v in vals]


def build_explicit(intern_property, slug, props):
    """Intern an EXPLICIT property list and COMPUTE its default index.

    The index is derived here rather than written into the table because a
    mixed-radix digit-by-stride sum is the single easiest thing to get wrong by
    hand, and getting it wrong produces a block that renders and behaves as
    some other perfectly valid state of itself.
    """
    names = [p[0] for p in props]
    if names != sorted(names):
        raise SystemExit(f"{slug}: explicit property list {names} is not sorted by name; "
                         "MC's StateDefinition uses an ImmutableSortedMap and the sort "
                         "order IS the state index layout")

    refs, radices, digits = [], [], []
    for name, kind, values, default in props:
        if default not in values:
            raise SystemExit(f"{slug}.{name}: default {default!r} not in {values}")
        refs.append(intern_property(name, KIND[kind], values))
        radices.append(len(values))
        digits.append(values.index(default))

    # Odometer: last property varies fastest (MC's flatMap accumulation order).
    count, default_index, stride = 1, 0, 1
    for r, d in zip(reversed(radices), reversed(digits)):
        default_index += d * stride
        stride *= r
        count *= r
    return refs, count, default_index


def main():
    if not os.path.exists(MCDATA):
        raise SystemExit(f"missing {MCDATA} — run npm install under tools/protocol-gen")

    upstream = {b["name"]: b for b in json.load(open(MCDATA))}
    slugs = load_engine_slugs()

    # ── Dedupe properties by (name, kind, values) — MC's property identity ──
    prop_index = {}          # key -> index into the emitted property table
    prop_rows = []           # (name, kind, values)
    value_pool = []          # flat string pool
    value_begin = {}         # tuple(values) -> offset into value_pool

    def intern_property(name, kind, values):
        key = (name, kind, tuple(values))
        if key in prop_index:
            return prop_index[key]
        vt = tuple(values)
        if vt not in value_begin:
            value_begin[vt] = len(value_pool)
            value_pool.extend(values)
        idx = len(prop_rows)
        prop_index[key] = idx
        prop_rows.append((name, kind, vt))
        return idx

    block_rows = []          # (slug, [propIdx...], defaultIndex, stateCount)
    aliased = []
    explicit = []
    total_states = 0

    for slug in slugs:
        if slug in NOT_A_BLOCK:
            total_states += 1
            continue

        b = upstream.get(slug)
        source = "upstream"

        if b is None:
            target = alias_for(slug)
            if target is not None:
                b = upstream.get(target)
                if b is None:
                    raise SystemExit(f"{slug} aliases {target!r}, which is not upstream either")
                source = f"alias of {target}"
                aliased.append(slug)
            else:
                props = explicit_for(slug)
                if props is None:
                    # Silently emitting a propertyless block here is how a real
                    # state space quietly becomes a wrong one. Stop instead.
                    raise SystemExit(
                        f"{slug}: no upstream row, no alias and no explicit property set.\n"
                        f"  Add it to ALIAS_SUFFIX/ALIAS_EXACT if MC registers it with a class\n"
                        f"  an older block already uses, or to EXPLICIT with its\n"
                        f"  createBlockStateDefinition list from minecraft_code_26.1-snapshot-1/.")
                refs, count, default_index = build_explicit(intern_property, slug, props)
                block_rows.append((slug, refs, default_index, count))
                total_states += count
                explicit.append(slug)
                continue

        states = b.get("states") or []
        if not states:
            total_states += 1
            continue

        refs = []
        count = 1
        for st in states:
            values = property_values(st)
            if len(values) < 2:
                raise SystemExit(f"{slug}.{st['name']} has {len(values)} value(s); "
                                 "MC forbids single-valued properties")
            refs.append(intern_property(st["name"], KIND[st["type"]], values))
            count *= len(values)

        # Self-check: the cartesian product must equal the id range upstream
        # allocated. If this trips, either the value lists or the property list
        # is not what MC enumerated, and every state index would be wrong.
        span = b["maxStateId"] - b["minStateId"] + 1
        if count != span:
            raise SystemExit(f"{slug}: product of value counts {count} != "
                             f"upstream state span {span} ({source})")

        default_index = b["defaultState"] - b["minStateId"]
        if not (0 <= default_index < count):
            raise SystemExit(f"{slug}: default index {default_index} out of range {count}")

        block_rows.append((slug, refs, default_index, count))
        total_states += count

    import collections
    name_counts = collections.Counter(n for n, _k, _v in prop_rows)
    ambiguous = {n for n, c in name_counts.items() if c > 1}
    const_names = [constant_name(n, v, ambiguous) for n, _k, v in prop_rows]
    dupes = [c for c, n in collections.Counter(const_names).items() if n > 1]
    if dupes:
        raise SystemExit(f"two properties resolved to the same constant name: {dupes}")

    manual = count_manual_block_ids()
    total_states += manual
    write_header(prop_rows, const_names, total_states, len(block_rows))
    write_source(prop_rows, value_pool, value_begin, block_rows)

    stateful = len(block_rows)
    print(f"gen_block_states: {len(slugs)} blocks, {stateful} with properties, "
          f"{total_states} states total, {len(prop_rows)} distinct properties, "
          f"{len(value_pool)} pooled value strings")
    nondefault = sum(1 for _, _, d, _ in block_rows if d != 0)
    print(f"                  {nondefault} blocks whose default state is not index 0")
    aliased_rows = sum(1 for s_, _, _, _ in block_rows if s_ in set(aliased))
    print(f"                  {manual} synthetic BlockIDs from Blocks.hpp "
          f"(1 state each, folded into the total)")
    print(f"                  supplement: {len(aliased)} aliased "
          f"({aliased_rows} with properties), {len(explicit)} explicit, "
          f"{len(NOT_A_BLOCK)} non-blocks skipped")


HEADER = '''// GENERATED by tools/gen_block_states.py — DO NOT EDIT BY HAND.
//
// Every block's BlockState property set, taken from vanilla rather than
// guessed. See the script header for why this is generated: MC's property
// VALUE ORDER decides the state index, and MC's property IDENTITY is
// (name, value-set) rather than name — `type` alone means three different
// things.
//
// Layout mirrors the other generated tables here (kBlockShapeTable,
// kWaterloggable): flat arrays with (begin, count) windows, keyed on the
// vanilla registry slug, which is column 2 of BlockDefs.inc.
//
// Properties are DEDUPED across blocks, exactly as vanilla shares one
// `BlockStateProperties.FACING` object between every block that uses it —
// which is what makes identity comparison valid.
//
// A block absent from kBlockStates has no properties and exactly one state.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Game {

    // MC's three Property subclasses.
    enum class GeneratedPropertyKind : uint8_t { Bool = 0, Int = 1, Enum = 2 };

    struct GeneratedPropertyRow {
        const char* name;
        uint8_t     kind;         // GeneratedPropertyKind
        uint16_t    valueBegin;   // window into kPropertyValues
        uint16_t    valueCount;   // MC getPossibleValues().size(), always >= 2
    };

    struct GeneratedBlockStateRow {
        const char* slug;
        uint16_t    propBegin;    // window into kBlockPropertyRefs
        uint16_t    propCount;
        // Index of the block's default state within its OWN state list. NOT
        // always 0 — MC's any() takes the first value of every property and
        // BooleanProperty lists true first, so most blocks register a default
        // somewhere else entirely. 567 of these are non-zero.
        uint16_t    defaultIndex;
        // Product of the property value counts. Carried so the loader can
        // assert its own arithmetic against what vanilla allocated.
        uint16_t    stateCount;
    };

    // Value-name pool, shared by properties with identical value lists.
    extern const char* const            kPropertyValues[];
    extern const size_t                 kPropertyValueCount;

    // The 118 distinct (name, kind, values) properties.
    extern const GeneratedPropertyRow   kProperties[];
    extern const size_t                 kPropertyCount;

    // Flat property-index windows, one run per block, in MC's sorted-by-name
    // order — which is the order the state index is a mixed-radix number over,
    // with the LAST property varying fastest.
    extern const uint16_t               kBlockPropertyRefs[];
    extern const size_t                 kBlockPropertyRefCount;

    extern const GeneratedBlockStateRow kBlockStates[];
    extern const size_t                 kBlockStateRowCount;

__PROPERTY_IDS__

__TOTALS__

} // namespace Game
'''


def write_header(prop_rows, const_names, total_states, stateful_blocks):
    bits = 0
    while (1 << bits) < total_states:
        bits += 1

    ids = ['    // MC BlockStateProperties constant names, verbatim. A property is',
           '    // identified by (name, value-set), not by name — `type` alone is three',
           '    // different properties — so the 13 reused names carry MC\'s own',
           '    // disambiguating constant (SLAB_TYPE / CHEST_TYPE / PISTON_TYPE, …).',
           '    enum class PropertyId : uint16_t {']
    for i, (c, (name, _kind, values)) in enumerate(zip(const_names, prop_rows)):
        preview = ",".join(values[:3]) + ("…" if len(values) > 3 else "")
        ids.append(f'        {c} = {i},'.ljust(42) + f'// "{name}" = {preview}')
    ids.append(f'        Count = {len(const_names)}')
    ids.append('    };')

    totals = [
        '    // Total distinct block states across every block this engine defines —',
        '    // the size of the global state id space, and MC\'s',
        '    // Block.BLOCK_STATE_REGISTRY.size(). Generated, never a literal: the',
        '    // palette width is derived from it and must move when it does.',
        f'    inline constexpr uint32_t kBlockStateCount = {total_states};',
        f'    inline constexpr int      kBlockStateBits  = {bits};',
        f'    inline constexpr size_t   kStatefulBlockCount = {stateful_blocks};',
        '',
        '    static_assert(kBlockStateCount <= (1u << kBlockStateBits),',
        '                  "state id space does not fit the generated palette width");',
        '    static_assert(kBlockStateCount > (1u << (kBlockStateBits - 1)),',
        '                  "generated palette width is wider than the state space needs");',
    ]

    body = HEADER.replace("__PROPERTY_IDS__", "\n".join(ids))
    body = body.replace("__TOTALS__", "\n".join(totals))
    with open(os.path.join(OUT_DIR, "GeneratedBlockStates.hpp"), "w") as f:
        f.write(body)


def write_source(prop_rows, value_pool, value_begin, block_rows):
    out = ['// GENERATED by tools/gen_block_states.py — DO NOT EDIT BY HAND.',
           '#include "GeneratedBlockStates.hpp"',
           '',
           'namespace Game {',
           '']

    out.append('    const char* const kPropertyValues[] = {')
    for i in range(0, len(value_pool), 8):
        out.append('        ' + ' '.join(f'"{v}",' for v in value_pool[i:i + 8]))
    out.append('    };')
    out.append(f'    const size_t kPropertyValueCount = {len(value_pool)};')
    out.append('')

    kind_name = {0: "Bool", 1: "Int ", 2: "Enum"}
    out.append('    const GeneratedPropertyRow kProperties[] = {')
    for name, kind, values in prop_rows:
        begin = value_begin[values]
        preview = ",".join(values[:4]) + ("…" if len(values) > 4 else "")
        out.append(f'        {{ "{name}", {kind}, {begin}, {len(values)} }},'
                   f'  // {kind_name[kind]} {preview}')
    out.append('    };')
    out.append(f'    const size_t kPropertyCount = {len(prop_rows)};')
    out.append('')

    refs_flat = []
    windows = {}
    for slug, refs, _, _ in block_rows:
        windows[slug] = len(refs_flat)
        refs_flat.extend(refs)

    out.append('    const uint16_t kBlockPropertyRefs[] = {')
    for i in range(0, len(refs_flat), 16):
        out.append('        ' + ' '.join(f'{v},' for v in refs_flat[i:i + 16]))
    out.append('    };')
    out.append(f'    const size_t kBlockPropertyRefCount = {len(refs_flat)};')
    out.append('')

    out.append('    const GeneratedBlockStateRow kBlockStates[] = {')
    for slug, refs, default_index, count in block_rows:
        out.append(f'        {{ "{slug}", {windows[slug]}, {len(refs)}, '
                   f'{default_index}, {count} }},')
    out.append('    };')
    out.append(f'    const size_t kBlockStateRowCount = {len(block_rows)};')
    out.append('')
    out.append('} // namespace Game')
    out.append('')

    with open(os.path.join(OUT_DIR, "GeneratedBlockStates.cpp"), "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
