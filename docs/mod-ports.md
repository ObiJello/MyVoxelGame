# Twilight Forest and Aether ports

Two Java mods ported into the engine as native dimensions, from their real sources
(cloned under `mods_reference/`, git-ignored): **Twilight Forest 4.9** (NeoForge, MC 26.1)
and **The Aether 1.5.10** (NeoForge, MC 1.21.1). Both ship their generated data packs
under `src/generated/resources`, so biomes, features, noise settings, loot tables and
recipes are exact JSON, not guesswork; behaviour comes from the Java.

## Licences (what may be copied)

| | Code | Assets (textures, models, sounds) |
|---|---|---|
| Twilight Forest | LGPL-3.0 | CC BY-NC-SA 4.0 — textures may be shipped in this free, non-commercial game with attribution (`assets/ATTRIBUTION.md`) |
| The Aether | LGPL-3.0 | **All rights reserved** — nothing under `assets/aether` may be copied; every Aether texture and model here is made in-house (`tools/gen_aether_textures.py`), the way the Hush's were |

Ported logic keeps a comment naming the Java class it mirrors, as the vanilla ports do.

## Dimensions (done)

| | id | registry name | y range | time | scale | save folder |
|---|---|---|---|---|---|---|
| Twilight Forest | 3 | `twilightforest:twilight_forest` | -32..256 (logical 288) | fixed dusk 13000 | 1 : 8 (0.125) | `dimensions/twilightforest/twilight_forest` |
| The Aether | 4 | `aether:the_aether` | 0..256 | day cycle | 1 : 1 | `dimensions/aether/the_aether` |

Both have sky light, no ceiling, no nether portals, and stay out of the dimension-stack
ring. `/dimension twilight|aether` works from the start; until each generator lands the
level generates Overworld terrain (the generator warns on the unknown key).

**Beds** (`Game::DimensionBedRule`, `BedBlock.cpp`). Twilight Forest uses the mod's
`BED_RULE` (`TFDimensionGenerator`: can_sleep NEVER, can_set_spawn ALWAYS, no explosion, no
error message), so a dyed bed sets your spawn and nothing else happens. The Aether's
`the_aether.json` has `bed_works: true`, which gives the Overworld rule: sleep at night,
spawn always set. In both, the straw bed is DESTROY_ON_LEAVE, the attribute default.
Gaps: the Aether's eternal-day refusal (`DimensionHooks.isEternalDay` → NOT_POSSIBLE_NOW)
is not modelled because the server runs no Aether time, and the night skip counts only
Overworld sleepers (`IntegratedServer` SleepStatus), so an Aether sleeper lies in bed
without skipping the night.

## Portals

- **Twilight Forest**: a 2×2 pool of water ringed by natural blocks (grass, dirt, flowers,
  the mod's portal-decoration tag), lit by throwing a diamond into it (`TFPortalBlock`,
  the item-entity check). The pool becomes `twilight_portal` blocks; standing in one
  teleports (`TFTeleporter`) and builds a matching pool portal at the destination. Not a
  frame family — a new pool-portal mechanic beside `PortalFamily`.
- **The Aether**: a glowstone frame (nether-portal shape) lit with a water bucket
  (`AetherPortalShape`, `AetherPortalBlock`), portal block `aether_portal`,
  `AetherPortalForcer` finds or builds the far portal. Falling out of the Aether below its
  floor drops you into the Overworld sky. A third `PortalFamily` (glowstone → aether_portal,
  ignition = water bucket, far dimension Aether).
- **`/gamerule immersive_portals`** is the nether's switch only. The Aether portal is
  always `aether_portal` blocks (a glowstone rectangle, as in the mod;
  `Portals::FamilyIsImmersive`) and the Twilight pool is always `twilight_portal` blocks,
  whichever way the rule is set; flipping it never touches either. An Aether portal an
  older build lit as a see-through surface is converted to blocks (both ends) before the
  first tick. Details in `docs/immersive-portals.md`.

## Terrain

- **Aether** — `skylands.json`: holystone default block, no sea, 3D `old_blended_noise`
  squeezed with two Y gradients into floating islands between y 8 and 128; four biomes
  (skyroot meadow / forest / grove / woodland) on a temperature × humidity multi-noise;
  surface rule aether grass over aether dirt. Ported as `ModTerrainSettings::aether()` and
  the `aether:aether` material rule (`ModMaterialRules.cpp`) in the terrain library.
- **Twilight Forest** — `twilight_noise_gen.json`: stone/water, sea level 0, terrain from
  a per-biome `TerrainColumn` grid (`biome_terrain_data/biome_grid.json`: depth, scale,
  weight per biome) fed through TF's own density functions, and a biome layout produced
  by a legacy layer stack (`biome_layer_stack/random_forest_biomes.json`: random forest
  biomes, key biomes with companions, zoom, stabilize, thornlands border, streams).
  Ported as a `TwilightBiomeSource` (layer stack) + `ModTerrainSettings::twilight()`
  (TF's two density functions are float samplers in `TwilightDensityFunctions`).
  Landmarks (hollow hills, hedge maze, lich tower, …) are TF structure sets on their
  landmark grid, data-driven where the library's loaders allow.

## Pass one — landed 2026-09-22 (Release green)

- **Blocks** (68): TF twilight oak / canopy / tf_mangrove / dark wood sets with saplings, hardened dark leaves, root, liveroot, hedge, firefly / cicada / moonworm, mushgloom, fiddlehead, mayapple, clover and moss patches, torchberries, fallen leaves, the mazestone set, aurora block, twilight portal. Aether grass and dirt (grass spreads), quicksoil (friction 1.1), holystone set, three aerclouds (no fall damage), icestone, ambrosium / zanite / gravitite ores, skyroot and golden oak sets, aether portal, berry bush and stem, white and purple flowers. Items: torchberries, liveroot, ambrosium shard, zanite gemstone, blue berry.
- **Portals**: Aether = third PortalFamily (glowstone + water bucket, platform fallback, fall out below y 0 → Overworld sky). TF = diamond thrown into a flower-ringed pool (`TwilightPortalShape`, `TwilightTeleporter` with TF's placement order and return pool).
- **Terrain**: Aether skylands router, 4 biomes, aerclouds, quicksoil shelves, ores, skyroot/golden oak (exact placers). TF layer-stack biome source, TerrainColumn density functions, surface rule, TF caves carver, glacier blanket, 22 biomes, trees (canopy branching placer, spheroid foliage, mangrove roots, darkwood), flora, lakes, legacy ores. TF is told apart from the Overworld by a dimension tag on NoiseGeneratorSettings.
- **Mobs** (11): deer, boar, bighorn sheep, tiny bird, kobold, redcap; phyg, flying cow, sheepuff, cockatrice, zephyr — mod meshes parsed from the mods' Java model classes (`MOD_MODELS` in gen_entity_models.py), mod setupAnim ported.
- **Assets**: TF textures copied with attribution (`tools/copy_twilight_assets.py`, `assets/ATTRIBUTION.md`); Aether art in-house (`tools/gen_aether_textures.py`).

Known pass-one limits: players do not sink into or get launched by aerclouds (client physics has no entityInside); TF's canopy/aurora colourisers are baked textures; TF sky renderer, dark-forest canopy blanket, landmarks, hollow/giant trees; Aether crystal islands, dungeons; cockatrice needles; riding, shearing and variants.

## Aether worldgen, pass two (not yet built)

- **Features**: every biome now lists all 21 of the mod's features in their JSON step
  positions (FeatureSorter indices = the mod's): `water_lake` (`AetherLakeFeature`,
  aether-grass rim), `water_spring`, `holiday_tree` (`HolidayFoliagePlacer` +
  `HolidayTreeDecorator`, gated by `HolidayFilter`: December/January only, the mod's
  config default), `crystal_island` (`CrystalIslandFeature` over the crystal tree:
  `CrystalTreeTrunkPlacer`, `CrystalFoliagePlacer`). `aether:dungeon_blacklist_filter`
  is on every placement that has it (no feature inside a dungeon's box).
- **Structures** (`levelgen/structure/AetherStructures.cpp`): large aerclouds, bronze
  dungeon (graph builder, rooms, boss room, end tunnel, surface ruins), silver dungeon
  (cloud bed, temple shell, 3x3x3 room grid), gold dungeon (island, stubs, gumdrop caves,
  tunnel, golden oaks). Data in `data/aether/worldgen/{structure,structure_set,processor_list}`
  (datagen output, LGPL); `StructureInfo::modJson` carries the type's codec fields. The
  templates in `data/aether/structure/{bronze,silver,gold}_dungeon/` are **our own designs**
  from `tools/gen_aether_structures.py` — the mod's `src/main/resources` NBTs are All Rights
  Reserved and are not copied. The generator keeps each template's id, size, anchor rows and
  DATA markers ("Chest", "Treasure Chest") that the piece code reads; boss spots in the piece
  code match our layouts. Re-run it (`--dump --check-blocks`) after changing a design.
  Aether structures sort before `minecraft:*`, so only the Aether decorates with them in
  its per-step structure list — vanilla dimensions keep vanilla structure seeds.
- **Block names**: `levelgen/AetherBlocks` resolves `aether:<x>` to the engine slug or a
  logged stand-in, for templates (every palette entry of an `aether:` template, lenient on
  properties) and processor lists (namespaced list ids; `aether:double_drops`,
  `boss_room`, `no_replace`, `surface_rule`, `vertical_gradient`).
- **Loot**: `data/aether/loot_table/chests/dungeon/**`, item ids mapped `aether:x` ->
  `minecraft:x`; `aether:config_enabled` resolved at the mod's defaults (golden feather
  entry dropped, valkyrie cape kept). Items the engine lacks drop out of the roll.
- **Bosses**: not spawned — each boss room logs `PENDING BOSS <id> at x y z`. Treasure
  chests carry the reward loot table but are not locked; mimics are plain empty chests.

## Pass two — creatures (2026-09-22, not yet built)

- **Every biome-spawner mob** of both mods: TF squirrel, raven, dwarf rabbit (3 variants), penguin, king spider, hostile/mist/winter wolf, mosquito swarm, skeleton druid, yeti; Aether moa (3 types), aerbunny, aerwhale, blue/golden swet, whirlwind, evil whirlwind, aechor plant. Plus TF landmark hostiles (hedge/swarm spider, wraith, fire/slime/pinch beetle, helmet crab, troll, towerwood borer, maze slime, minotaur, redcap sapper, block-and-chain goblin, upper/lower goblin knight) and Aether dungeon mobs (mimic, sentry, valkyrie, fire minion).
- **Aether mob categories**: the Aether's four appended MobCategory values (surface/darkness/sky monster, aerwhale) are real categories with their own caps (`MobCategory.hpp`, after Misc); `IsMonsterCategory` answers "hostile"; `gen_mob_spawns.py` maps their biome keys.
- **Pass-one gaps**: tiny bird variants, bighorn and sheepuff shearing + fleece colours (sheepuff puffing), saddles on phyg / flying cow / moa (equip, render, drop, save), cockatrice poison needles.
- **Layout**: mob classes in `TwilightMobs` / `TwilightCreatures` / `TwilightHostiles` / `AetherMobs` / `AetherCreatures`, each file owning its factory (`MakeTwilightMob`, `MakeTwilightHostile`, `MakeAetherMob`); renderers in the `ModMobRender.hpp` modules (`TwilightCreatureRender`, `TwilightHostileRender`, `AetherCreatureRender`), which MobRenderer calls at fixed points; saved fields via `Mob::SaveModNbt` / `LoadModNbt` (`ModMobNbt.hpp`). Aether creature art: `tools/gen_aether_entity_textures.py`.

Known pass-two limits: no player riding in the engine (saddles do nothing yet; swets cannot swallow a player, a yeti throws a caught player at once, pinch beetles only clamp mobs); several mod particles are vanilla stand-ins (no Splash/snow ParticleKinds (Flame exists now, for the monster spawner)); morphs into mod mobs draw no mod layers; hedge-maze daylight spawns; sounds.

## Pass two — Twilight Forest world generation (written 2026-09-22, not yet built)

- **Placed features are data-driven**: `TwilightPlacements` builds every placed feature from `data/twilightforest/worldgen/placed_feature/**.json` (copied from the mod), resolving the `feature` id in `TwilightFeatureRegistry` (every configured feature registers under its JSON id). TF modifiers ported: `no_structure` (AvoidLandmarkModifier with the structures' `decoration_clearance`), `chunk_centerer`, `chunk_blanketing`. `setupTwilightBiomes` is generated from the biome JSON, so feature order and seeds follow the mod's.
- **Features**: `TwilightTreeFeatures` (mega canopy, mega oaks, large winter tree, hollow log/stump, fallen logs, wood/plant/troll/vanilla roots, snow under trees), `TwilightDecorFeatures` + `TwilightTemplateFeatures` (berry/oreberry bushes, lily pads, webs, thorns, vines, fire jet/smoker, lampposts, foundation, monolith, stalagmites, mushglooms, druid hut, wells, graveyard, grove ruins, stone circle), `TwilightSpikes` (BlockSpikeFeature).
- **Blocks**: `levelgen/TwilightBlocks` resolves every TF name — the engine slug first, else a logged stand-in — for code and for the template loader (TemplateEngine accepts `twilightforest:` palette names; TF templates in `data/twilightforest/structure/` never throw).
- **Dark-forest canopy blanket** in `ChunkGenerator.cpp` (before the glacier, as in the mod's registry order).
- **Landmark grid**: `TwilightLandmarks` (LegacyLandmarkPlacements), `twilightforest:landmark_grid` / `avoid_landmark_grid` placements in the structure-set loader, TF structures and sets loaded from `data/twilightforest/worldgen/`. Biome HolderSets that name a single mod biome resolve as that biome. `/locate structure` finds landmarks the mod's way (`WorldUtil.findNearestMapLandmark`, 100 regions around the player; `LocateFinder.cpp`); a structure this build does not generate yet answers "not generated by this version" rather than searching.
- **Structures** (`levelgen/structure/TwilightStructures` dispatcher + `levelgen/structure/twilight/*`): hollow hills (3 sizes), hedge maze, hollow tree, naga courtyard (no boss), quest grove, lich tower (no lich). Custom terrain (CustomDensitySource) is added to the beardifier. Chest loot: `tools/copy_twilight_loot.py` copies the mod's chest tables to `data/twilightforest/loot_table/` with items mapped to ours.

Gaps: TF structures not ported (dark tower, knight stronghold, labyrinth, hydra lair, yeti cave, aurora palace, troll cave, final castle, giant house, mushroom tower, camp, fallen trunk) log once and generate nothing, so the canopy's dark-tower hole never opens yet; no TF piece-code worldgen entities (quest ram, ravens, wraiths — the template pipeline exists, the piece code does not call it yet); per-piece beard kind (NeoForge PieceBeardifierModifier) uses the structure's.

## Structure mobs (2026-09-22, not yet built)

- **Spawners are live**: the lich tower's room/central spawners, the hedge maze's and the hollow hills' spawner payloads (SpawnData, SpawnPotentials, range overrides) load into `SpawnerBlockEntity` (MC BaseSpawner) and spawn; mod mobs this build lacks keep the library's vanilla stand-in ids.
- **spawn_overrides apply** (both mods' structure JSONs: TF landmarks and the Aether dungeons carry empty lists for every category, so natural spawning stops inside them, as in the mods).
- **TF controlled spawns** (EntityEvents.gatherPotentialSpawns on NeoForge's PotentialSpawns): each landmark's `controlled_spawns` monster list for the spawn index of the pieces at the spawn position (hollow hills: TFStructureComponent's 0 inside HollowHillStructure.canSpawnMob's ellipsoid; lich tower: yard 0, interior 1, foyer 2 = empty, boss room / roof / fence deny), plus its ambient/water lists. Not tracked: TFStructureStart.isConquered, so a landmark keeps spawning after its boss.
- **Aether**: nothing placed yet — its dungeon mobs come from block behaviour this port does not have (TrappedBlock.stepOn → sentry / valkyrie / fire minion; the chest mimic, which the loader stands in as a plain chest) and the boss-room template entities (slider, valkyrie queen, sun spirit) wait on the bosses; the library still only reports those spots (`markPendingBoss`). Our own Aether templates (`gen_aether_structures.py`) carry no entities for the same reason.

## Passes

1. Dimensions (done), both portals, both terrains and biomes, the core block sets
   (TF: twilight/canopy/mangrove/darkwood woods, mazestone, fireflies; Aether: holystone,
   aerclouds, quicksoil, skyroot/golden oak, icestone, the three ores), trees and
   flora, a first mob set, one landmark each.
2. Remaining blocks and items, tool/armor sets, more mobs, more structures.
3. Bosses and their arenas, progression items, dungeons.

Each pass ends with a green Release build and an in-game check by the user.
