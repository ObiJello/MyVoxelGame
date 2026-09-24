# Minecraft 26.3 worldgen + chunk saving port

Status (2026-09-23): **phases 1-4 done, including the float density engine.**
The Java parity harness runs 26.3-pre-2; the library uses 26.3's 10
statuses, 26.3's float density engine and datapack-driven worldgen (noise,
density functions, noise settings and material rules decoded from the
vanilla JSON), 26.3 terrain algorithms (material rules, ore veins in the
rules, carving mask), both new biomes, and 26.3 features and structures.
Parity at seed 12345 is exact everywhere it is measured: a 31-area overworld
sweep over every biome, the sulfur, dappled, desert, badlands, mushroom and
warm areas, 6 Nether and 2 End areas, and a structures-on sweep of 16
structure areas (block entities included). Every router and aquifer density
function of all 7 noise settings matches Java bit for bit
(`run_density_parity.sh`). The library saves and reloads unfinished chunks as
MC does (phases 2-4; `docs/terrain-library-persistence.md`), verified by a
restart round trip across the structure areas.

Goal: bring the terrain library (`src/my_terrain_library/`) from its current
parity target (26.1-snapshot-1, DataVersion 4764) to Minecraft 26.3, and port
Minecraft's chunk saving so the library saves and frees every chunk holder as
`ChunkMap.processUnloads` does. Order agreed: statuses/terrain → saving →
structures → unload. Reference source: `minecraft_code_26.3-pre-2/` (26.3-pre-2, world
version 5018). Everything below was checked against that source.

## Phase 1 — 26.3 statuses and terrain

### Statuses (verified)

`ChunkStatus.java:111-120`: empty, structure_starts, structure_references,
biomes, **terrain**, features, initialize_light, light, spawn, full (10; ours
has 12 with noise/surface/carvers). TERRAIN is the first FINAL-heightmap
status. Pyramid shape is unchanged: TERRAIN takes CARVERS' place.

| | Generation pyramid requirements (`ChunkPyramid.java:30-50`) |
|---|---|
| TERRAIN | buildTerrain; (STRUCTURE_STARTS, 8), (BIOMES, 1); write radius 0 |
| FEATURES | generateFeatures; (STRUCTURE_STARTS, 8), (TERRAIN, 1); write radius 1 |

Accumulated radii for FULL: empty/structure_starts 11, references/biomes 3,
terrain 2, features/initialize_light 1, light/spawn/full 0. MAX_LEVEL stays 44;
level 35 becomes TERRAIN.

Old saves: `MergeTerrainChunkStatusFix` (DataVersion 5013) maps `carvers` →
`terrain`, and `noise`/`surface` → `biomes` with block_states, Heightmaps and
blending_data removed. Our worlds are stamped 4764, so a TERRAIN proto is
written as `minecraft:carvers` until the stamp moves past 5013; on read,
`carvers`/`terrain` → TERRAIN, `noise`/`surface` → BIOMES with blocks dropped.

### Terrain algorithm changes (verified in `NoiseBasedChunkGenerator.buildTerrain`)

1. One step, async: NoiseChunk created locally, doFill → buildSurface →
   carvers → close. No NoiseChunk or carving mask on the ProtoChunk.
2. doFill samples finalDensity for the whole volume (`sampleVolume`) and uses
   only the aquifer — **ore veins moved into the material rules**
   (`OreVeinRule`, `OverworldMaterialRules.java:142-156`; bedrock now wins).
3. Surface ("material") rules apply to every solid block, not only the default
   block (`MaterialSystem.java:142-176`).
4. Carvers only mark a local `CarvingMask`; one `applyCarvingMask` pass writes
   blocks (per column run: grass tracking, `#uncarvable` = bedrock only,
   `aquifer.computeSubstance(...,0.0)`, topMaterial under carved grass).
   Nether: carved cells above lava become air, not cave_air. Carvers are data
   records (`Carvers.java`), carver biome sampled uncached at the source
   chunk's quart (minX>>2, 0, minZ>>2).
5. Biomes: climate sampler + `createResolverForChunk`, no NoiseChunk.
6. Heightmaps (MOTION_BLOCKING…) primed at the end of TERRAIN, not at FEATURES.
7. Also: fix `ChunkStep.cpp:40` — it sets the status to the target every
   time; MC only raises it (`ChunkStep.java:62-67`).

### New content since 26.1-snapshot-1 (verified: `Biomes.java` in both trees)

26.3 adds two biomes and nothing else to the biome registry:
`dappled_forest` (poplar trees) and `sulfur_caves` (an underground biome with
its own material-rule bands, `OverworldMaterialRules.java:99-139`). They bring:

- the poplar wood family (log, wood, leaves, sapling, planks, leaf litter,
  shelf, signs, …) and the sulfur/cinnabar block families in the library's
  block registry (the game already has them in `BlockDefs.inc`);
- their placement in `OverworldBiomeBuilder`, their `OverworldBiomes` /
  `BiomeDefaultFeatures` settings, and the vegetation features
  (`VegetationFeatures`, `VegetationPlacements`);
- the **abandoned camp** structure (`AbandonedCampStructurePools`,
  `Structures`, `StructureSets`) with its templates and loot tables;
- game side: two new BiomeRegistry ids (append-only), colours, and mob spawn
  lists (`tools/gen_biomes.py`, `tools/gen_mob_spawns.py`).

Done in the order the parity harness shows them: biome placement first (it
changes which biome every quart gets), then material rules, features,
structure.

All of these are now ported one-to-one rather than approximated (ore veins
through the JSON `ore_vein` rules, the volume biome sampler, DensityVolume
sampling) and verified exact by the sweeps.

### Method

`terrain/tests/parity/` already compares Java vs C++ chunk dumps
(`run_full_parity.sh`, goldens, `compare_parity.py`). Steps: retarget the Java
harness classpath to 26.3 and update it for the TERRAIN status; regenerate
goldens; port until the regression, acceptance and spot checks are clean in
all three dimensions.

Consequence: newly generated terrain will differ from terrain generated before
the port, so existing worlds get seams at the old/new border.

## Phases 2-4 — chunk saving, structure starts, unloading (done)

Built as described in `docs/terrain-library-persistence.md`:

- **I/O.** The library reads and writes the world's region files through the
  game's `AnvilChunkIo` (`LibraryChunkStorage`, a `ChunkStorageBackend`
  behind the library's `IOWorker`) instead of its own region handle. A read
  that fails to decode is remembered and never written over.
- **Chunk format.** `SerializableChunkData` carries MC's proto field set —
  status (per DataVersion), sections packed as `PalettedContainer.pack`,
  heightmaps (missing ones primed), PostProcessing, block entities, generated
  entities, structures — plus two engine keys (structure spawn areas, entity
  finalize flag).
- **Structure starts.** Saved with MC's start/piece layout under
  `structures.obeycraft:starts`; loading regenerates each start from the seed
  (MC's monument rule, for all) and hands back reference counts, piece boxes
  and every stateful piece's placement state under MC's tag names. FULL
  chunks carry their starts through the game's save too.
- **Unloading.** `processUnloads` saves each holder once no task works on it
  (`isReadyForSaving` + generation refs, MC's save-sync) and frees it at any
  status; `saveAllChunks(true)` at shutdown. The kept edge band remains only
  for worlds without storage.
- **Loading pyramid.** Found on the way: the layer skip in
  `ChunkGenerationTask::scheduleChunkInLayer` trusted the persisted status,
  so a chunk read at FEATURES never ran its loading steps
  (`loadStructureStarts`); it now waits for the step to have completed.

## Phase 1 results and what remains

Fixed on the way (each was a real divergence from vanilla, found with the
harness): engine structures filed under `minecraft:` (the Hush, Aurelith)
took vanilla structures' step indices and reseeded every vanilla structure's
placement (`ChunkGenerator::applyBiomeDecoration`, now filed after the vanilla
registry); plains trees used the 2% bee oak instead of 26.3's 5% ones, and
`FANCY_OAK_BEES` / `SUPER_BIRCH_BEES` had 5% hives instead of always; bamboo
was solid-render; the forest rock and template bushes tested the narrowed
26.3 `#dirt`; sculk cursors lacked 26.3's 12-block worldgen radius; sculk and
ore tag lists were hard-coded 26.1 sets. New Nether features
(RandomNeighborSpread, SteppedColumnCluster, SingleBlockPillar,
ProjectedRandomPatchySquare), the 26.3 desert well (template overlay with
archaeology loot), sculk patch sequences, and the abandoned camp (18
structures) are in. The structures-on sweep then found: 26.3's
`BlockRotProcessor` tests the processed state (a jigsaw's final_state rots),
not the template's; copper chests (every stage) and the crafter draw a
`LootTableSeed` like any randomizable container; beds have no block entity
since 26.3 while copper chests, shelves, copper golem statues and potent
sulfur do; campfires from templates save their items and cooking times.

Tools: `--block-trace <file>` on the Java harness (every block a decoration
feature sets, same line shape as the C++ harness's `--block-trace`) and
`/tmp`-style diffing of the two traces find the first divergent write.

## The float density engine (done)

26.3 computes density in float through compiled samplers. The port lives in
`levelgen/density/` (namespace `minecraft::levelgen::density`) and replaced
the double-precision engine outright:

- **Functions and samplers.** Every `DensityFunction` compiles to a
  `DensitySampler` with a volume path (`sampleVolume`) and a point path
  (`sampleValue`); the two sum in different orders, and each caller uses the
  one Java uses. `DensityFunctionCompiler` inlines registry references,
  applies the SliceUniformAxes rewrite and the cache rule, and dedups
  samplers by structural equality. `SamplerContext` holds the per-use caches
  (a cached volume answers later point queries) and a buffer arena;
  `RandomState` lends each NoiseChunk a buffer pool.
- **Noise.** `synth/`: 26.3 `NormalNoise` (base_octave / base_amplitude /
  octave_count / amplitude_modifiers, with `createParity` for the pre-26.3
  firstOctave+amplitudes form), Perlin, simplex, smeared Perlin, BlendedNoise.
- **Data.** `WorldgenRegistries` decodes `worldgen/noise` and
  `worldgen/density_function` with DensityFunction.CODEC's rules (a number is
  a constant, a string a registry reference, an object dispatches on "type")
  and constructs each class directly, as the codecs do. `TerrainSettings` is
  NoiseGeneratorSettings decoded from `worldgen/noise_settings`.
  `levelgen/material/` is the 26.3 material system, its rules decoded from
  `worldgen/material_rule` and `material_condition`.
- **Terrain.** `NoiseChunk` (volume, caching samplers, aquifer), `Aquifer`,
  `Beardifier` (a sampler behind the context's beardifier field), the
  climate sampler (six bound samplers; the chunk biome fill samples whole
  volumes, `MultiNoiseBiomeSource::createResolverForChunk`),
  `NoiseSpawnFinder`, and `NoiseBasedChunkGenerator::buildTerrain` (fill,
  material rules, carvers on one NoiseChunk).
- **The engine's dimensions** build their settings in code
  (`ModTerrainSettings`): the Hush is the Overworld's settings under its own
  block, sea level and material rules; the Aether and the Twilight Forest
  keep their mods' routers, with the pre-26.3 conventions carried over (the
  beardifier appended to the final density, every flat/2d/once cache as
  26.3's plain cache, interpolation at the noise cell size). The Twilight
  Forest's two custom density functions are float samplers.

Found on the way: the biome RTree must be built as 26.3 builds it — 19
children per node, Java's stable sort re-run per dimension, the cheapest
split's buckets kept as they were at that point. Its search is approximate
at ties, so a 6-child or differently ordered tree picks another biome at
the odd quart. Structures run 26.3's column pre-checks
(`GenerationContext::couldStructureExistInColumn` and its terrain-column /
chunk-centre forms, in jigsaw placement, single-piece structures,
onTopOfChunkCenter, end cities and mansions) through one caching climate
sampler per `createStructures` call, as Java does — the column volumes land in
that context's caches, which later point lookups read.

Tools: `terrain/tests/parity/run_density_parity.sh [seed]` samples every
router and aquifer function of every vanilla noise_settings entry in Java
(the vanilla datapack loaded as a world load does) and in C++, as uncached
points, cached chunk and quart volumes, and cached points, and diffs the
float bits.

Game side: `dappled_forest` and `sulfur_caves` have BiomeRegistry ids
(appended), colours, water fog and spawn lists; 26.3's 64 wool/concrete
slabs and stairs are game blocks, and potent sulfur has its five states, its
block entity (nausea, the dormant/erupting geyser clock and launch) and its
particles.
