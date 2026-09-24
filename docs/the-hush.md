# The Hush

A fourth dimension behind the ancient city's reinforced-deepslate frame. The deep dark
is about silence and the warden hunts by sound; the Hush is where the sculk came from — a
soundless world under a fixed midnight, lit only by what grows there.

This document is the design of record. Engine seams (dimension id, portal families, fixed
time, generator hand-off) are in `engineering-notes.md`; this file is about content and
progression.

## Progression

| Stage | Where | What you do | What you unlock |
|---|---|---|---|
| 0 | Overworld deep dark | Loot an ancient city for **echo shards** | The key |
| 1 | Ancient city frame | Use an echo shard on the reinforced-deepslate frame | The Hush (a return frame is built where you land) |
| 2 | Hush surface | Whisperwood, hushstone, **echo ore** (renewable shards), resonance blooms; hushlings watch you; **echo wraiths** hunt you; the sweeping beams of the **lighthouses** mark where to go | Hushstone / whisperwood building sets, echo lanterns, more portals, a lighthouse lamp of your own |
| 3 | Underground | The **Crystal Caverns**: resonant clusters on every wall, **resonite ore** | Raw resonite → resonite ingots |
| 4 | Resonite tier | Resonite tools (one step above diamond) and the block of resonite | Mining the vaults' heart |
| 5 | Echo Vaults | Break the **echo core** at the vault's heart; the **Silent Warden** wakes | The **resonant heart** |
| 6 | Capstone | Resonant heart + resonite + crystal | The **echo blade** |

Nothing in the Hush is gated by luck: shards are renewable from echo ore, resonite is a
normal deep ore, vaults are common (spacing 24), and every vault has a core.

The Hush portal is always a vanilla block portal — a reinforced-deepslate rectangle (up to
21×21 inside; the ancient city frame is 20×6) lit by an echo shard fills with `hush_portal`
blocks, crossed by standing in them. `/gamerule immersive_portals` is the nether's switch
only (`Portals::FamilyIsImmersive`) and never touches it; a Hush portal an older build lit
as a see-through surface is converted to blocks (both ends) before the first tick
(`docs/immersive-portals.md`).

## World

Open sky, perpetual midnight (fixed time 18000), sea level 50, full -64..320 height,
Overworld terrain shapes with Hush surface rules. Coordinate scale 1:1 with the Overworld.

**Beds** follow the Hush's own BedRule (`Game::DimensionBedRule`, `BedBlock.cpp`): never
sleep, never set the respawn point, never explode (unlike the Nether and End). Using a bed
shows one of a few action-bar lines (`HushBedRefusal`, e.g. "The silence is listening. You
cannot rest here.") with a quiet sculk-sensor click, after MC's range/obstruction checks.

### Biomes

- **Hush Meadows** — rolling sculk-loam plains under cyan stars. Hush grass, resonance
  blooms, hush moss, the odd whisperwood, now and then a small crystal outcrop. Hushlings
  graze here.
- **Whisperwood Forest** — dense whisperwood whose lantern leaves glow; the floor is moss
  and blooms. Echo wraiths drift between the trunks.
- **Resonant Barrens** — bare hushstone and polished outcrops, crystal fields, the
  richest echo ore. Skeletons and wraiths.
- **Crystal Caverns** (underground, the Hush's dripstone-caves band) — caves lined with
  resonant clusters, resonite ore, still pools. Motes drift in the air. Echo wraiths nest
  here; bats too.
- **Sunken Choir** — flooded lowland: the Overworld's coast and shelf shapes drowned by
  the Hush's sea level, a few blocks of teal water over sculk and sculk-loam lake beds
  (the surface noise splits the bed). Broken hushstone-brick fragments stand in the
  water (pillars, wall stubs, arches with their keystone gone, rubble), kelp and sea
  pickles grow between them, hush grass on the dry banks. The Hush leviathan swims here.
- **Hollow Deep** — chasm country: inland, rugged, in the peaks-and-valleys weirdness
  band. Its ravines come from the Hush deep canyon (below); whisperwood suspension bridges
  span them, hanging resonant-crystal clusters from every overhang and chasm ceiling,
  tall crystal formations on the rims, which are thin sculk loam over hushstone. Crystal
  golems and echo mimics.
- **Aurora Steppe** — high, cold, open plateau (temperature 0): hush-moss ground streaked
  with bare sculk loam, scattered tufts of hush grass, the odd polished-hushstone boulder,
  scattered resonance blooms, a tall crystal formation now and then as a landmark. The sky
  shows the aurora. Lumen moths drift over it.

Climate table (`MultiNoiseBiomeSource::buildHushParameters`) is a partition of RANGES,
like `OverworldBiomeBuilder` — surface biomes at depth 0 and 1, caverns at depth 0.2..0.9.
Exact points lose to the caverns' full-range entry (distance 0 on every axis but depth), which
turned most of the surface into treeless Crystal Caverns until 2026-09-22. Continentalness
< -0.19 = sunken choir; inland, |weirdness| > 0.5 = hollow deep (at every temperature);
in the middle weirdness band temperature < -0.3 = aurora steppe, and the rest splits by
humidity: forest > 0.1, barrens < -0.1 when temperature > 0, meadows otherwise. First appearances = `getHushBiomeKeys()` order (meadows, forest,
barrens, caverns, sunken choir, hollow deep, aurora steppe) — load-bearing for feature seeds.

**Biome size.** `MultiNoiseBiomeSource::sampleHushClimate` reads temperature at 3x and
humidity at 1.5x the block x/z. The Overworld router's temperature and vegetation noises feed
only biome choice (final density never reads them), so this shrinks the climate-driven biomes
without changing terrain; continentalness, erosion, depth and weirdness stay at the real
position, so the sunken choir stays on the flooded ocean band (98–100 % of c < -0.19 has
its surface under the Hush sea level of 50) and the hollow deep stays on its chasm band.
Measured offline (library router and table at surface depth, 8192×8192 blocks every 16
blocks, seeds 1, 42, 12345, 987654321, -7777, 31337):

| Biome | Share, vanilla-scale climate | Share now (min–max) | Typical walk inside | Mean patch |
|---|---|---|---|---|
| Sunken Choir | 28 % | 28 % (20–33) | ocean, ~1 km | 510 |
| Hush Meadows | 33 % | 21 % (18–23) | ~330 blocks | 280 |
| Whisperwood Forest | 18 % | 17 % (15–18) | ~400 | 290 |
| Resonant Barrens | 1.4 % (0.2–3.3) | 12 % (11–13) | ~390 | 250 |
| Aurora Steppe | 12 % (0–24, none in seed 42) | 11.6 % (8.5–16) | ~520 | 405 |
| Hollow Deep | 7 % | 11 % (9–13) | ~200 (a strip) | 150 |

Walking over land you cross into another biome every ~370 blocks (335–407 by seed; it was
~800, 575–1053, at the vanilla climate scale). "Typical walk" is the length-weighted
straight-line run inside the biome; "mean patch" the mean connected area as an equivalent
diameter, in blocks. The aurora steppe used to own every weirdness below its temperature
cutoff (T < -0.35), so nothing cut it: its walk was ~800 and its mean patch 556, twice the
other climate biomes. The hollow deep now takes the peaks-and-valleys band there too (the
chasms follow the rugged band at every temperature) and the aurora cutoff moved to -0.3 to
keep its share. The sunken choir stays the largest: it is the ocean band, and oceans
connect. Changing the table or the scales re-rolls
the biome layout of new chunks only; saved chunks keep their biomes.

**Hush deep canyon** (`ChunkGenerator.cpp`, Hush carver branch): vanilla CANYON with
probability 0.08, y 28..72, yScale 4, thickness trapezoid(3, 11, 3), distance factor
0.85..1, horizontal radius factor 0.9..1.15 — horizontal radius up to ~12.5 and
roughly twice vanilla's depth. It is carver index 3 (the three vanilla carvers keep
their seeds) and runs for source chunks whose biome is the Hollow Deep either at
vanilla's y = 0 sample or at y = 64 (the Hush's y = 0 is usually the caverns band).

Features (`HushFeatures` / `HushPlacements`, wired in `BiomeFeatureRegistry::setup*`):

| Biome | Step | Placement |
|---|---|---|
| Hush Meadows | vegetal | `CRYSTAL_FORMATIONS_MEADOWS` (`CRYSTAL_FORMATION_SMALL`, rarity 16) first, then the blooms, grass, moss and sparse whisperwood |
| Resonant Barrens | vegetal | `CRYSTAL_FORMATIONS_BARRENS` (`CRYSTAL_FORMATION_FIELD`, rarity 2), `CRYSTAL_SHARDS_BARRENS` (`CRYSTAL_FORMATION_SMALL`, count 1), `RESONANT_CLUSTER_SURFACE` |
| Sunken Choir | local modifications | `SUNKEN_RUINS` (Hush `SunkenRuinFeature`, rarity 2, ocean floor, water only) |
| | vegetal | `KELP_SUNKEN_CHOIR` (vanilla kelp, noise count 40), `SEA_PICKLE_SUNKEN_CHOIR` (count 12, rarity 8), `PATCH_HUSH_GRASS_SHORE` |
| Hollow Deep | local modifications | `ROPE_BRIDGES` (Hush `RopeBridgeFeature`: 3-wide suspension bridge, 8 rim searches per chunk) |
| | underground decoration | `RESONANT_STALACTITES` (the hanging crystal formation; 32 tries, scan up to a ceiling; no biome filter so the chasm walls in the caverns band get them) |
| | vegetal | `CRYSTAL_FORMATIONS_RIM` (`CRYSTAL_FORMATION_TALL`, rarity 4), `PATCH_HUSH_GRASS_RIM` |
| Aurora Steppe | local modifications | `HUSHSTONE_BOULDERS` (FOREST_ROCK in polished hushstone, rarity 3) |
| | vegetal | `CRYSTAL_FORMATIONS_STEPPE` (`CRYSTAL_FORMATION_TALL`, rarity 8), `PATCH_HUSH_GRASS_STEPPE` (noise count 3/5), `PATCH_RESONANCE_BLOOM_STEPPE` (rarity 12) |

Hush grass is scattered, not a carpet: `PATCH_HUSH_GRASS` is the vanilla grass patch at
16 tries (vanilla 32), placed 2/4 a chunk in the meadows (noise count), 3/5 on the steppe,
1 in the forest, on the rims and on the shore.

Every one also carries its echo ore (`ORE_ECHO`; the barrens' `ORE_ECHO_DENSE`) then
`ORE_RESONITE` (that order everywhere: the feature sorter rejects a cycle). Below ~13 blocks
under any surface biome the column is the Crystal Caverns, so an ore behind a biome filter
runs only where the caverns also carry it; `ORE_ECHO_DENSE` is the barrens' own, so it has
no biome filter (it runs in every chunk whose 3x3 neighbourhood holds barrens): filtered,
~80 % of its veins were refused.

A rope bridge (`RopeBridgeFeature`) is a whisperwood suspension bridge. It needs a rim
within 6 blocks of its origin and a gap 3–14 wide, with the far rim at the same height within
15 blocks: the one-chunk feature write margin, since the bridge writes from one block behind
the near rim to one past the far one, and ±3 across. The gap must hold air, never water, with
5+ blocks of air under the centre line. Before anything is written, the whole volume is
checked: the 3-wide deck and the headroom over it, both hand-rope lines, and both towers on
solid rim ground. A flooded or partly blocked chasm (an overhang, a tree, a wall reaching into
the deck anywhere but at its ends) is refused, and the next direction is tried.
- **Deck:** 3 wide. It sags in a catenary (cosh) curve, deepest at mid-span (0.15 blocks
  per block of span, so about 2 blocks over 14), built in half-steps: no step is more than
  a slab, so you walk across without jumping. A whole level is a whisperwood-plank centre
  run between stripped-log edge boards. A half level is a bottom slab over a top slab, one
  block thick, across all three rows. Edge boards are sometimes missing (8 %) or sunk half
  a block (6 %), never in the centre row and never at a tie.
- **Towers:** one on each rim. Two 4-high whisperwood-log posts at ±2 and a log cross-beam
  over the deck, with an echo lantern hanging under the beam (2 blocks of headroom under
  it). A footing of hushstone bricks is set into the rim around the posts, and the landing
  is planks between stripped-log boards.
- **Hand-ropes:** whisperwood fences at ±2, running from the post tops down in a catenary
  to a rail one block over the deck at mid-span. They are stacked wherever the rope steps
  down, so the line stays unbroken, and their connections are set along the span (worldgen
  runs no shape updates).
- **Under the deck:** stripped-log cross-ties across ±2 about every third column, iron-chain
  hangers from each tie end up to the rope, and sometimes a short chain dangling under a
  tie.

**Crystal formations** (`ResonantCrystalFormationFeature`, `ResonantCrystalFormation.cpp`;
presets in `HushFeatures::bootstrap`). A main shard at the origin and satellites fanned
evenly around it (with jitter), each leaning away from the centre. A shard is a voxelised
tapered prism along its leaning axis: 1–5 wide at the base (1, 2x2, 3x3, rounded 4x4 or
5x5), narrowing to 80 % over its first 55 %, then closing to a one-block point with a
`resonant_cluster` on the tip pointing along it (up, or sideways past 45 degrees). A
leaning shard is a face-connected staircase, never cubes touching at an edge. Floor
formations stand on a mound: a raised core disc and a ragged dressed ring of calcite,
polished hushstone and hushstone. Then clusters bud on the shards' sides and on the
ground around, and a few 2–3 block shards lie fallen just outside the mound.

| Preset | Main shard | Satellites | Mound | Where |
|---|---|---|---|---|
| `CRYSTAL_FORMATION_SMALL` | 4–7 long, 1–2 wide, lean 0–12 | 2–3, 2–4 long, 1 wide, lean 25–50 | core 1, ring 2 | meadows accent, barrens scatter |
| `CRYSTAL_FORMATION_FIELD` | 6–10 long, 2–3 wide; 20 %: 11–15 long, 3–4 wide | 3–6 (+1), 3–7 long, 1–2 wide, lean 20–50 | core 3, ring 5 | barrens outcrops |
| `CRYSTAL_FORMATION_TALL` | 10–15 long, 3–4 wide, lean 2–10; 35 %: 15–19 long, 5 wide | 3–5 (+1), 4–9 long, 1–3 wide, lean 15–40 | core 3, ring 5 | steppe landmarks, deep rims |
| `RESONANT_STALACTITE` (hanging) | 4–8 long, 2 wide; 15 %: 8–11 long, 3 wide | 1–3, 2–5 long, 1 wide, lean 20–45 | none (clusters on the ceiling) | chasm ceilings and overhangs |

With a landmark main shard the satellites are 25 % longer. The rules the feature keeps:
- It stands only on hushstone, polished hushstone, sculk loam or hush moss, or hangs from
  those or echo/resonite ore. The four neighbouring columns must find footing within one
  block of the origin's level, so slopes, ledges and chasm lips are refused.
- Crystal goes only into air or hush grass, never water. The mound re-dresses only those
  ground blocks, and only under a free cell.
- Low overhangs are filled down to their footing (at most 3 blocks), so nothing floats.
- Every cluster has a solid, non-cluster block behind it.
- Nothing lands more than 13 blocks from the origin in x or z (clusters 14), inside the
  one-chunk write margin.
- It draws only from the placement's random.

Crystal golems still find their resonant crystal. The formations run first in each biome's
VEGETAL list, before the grass and blooms. Adding them (and dropping the old spire columns)
shifted the Hush's VEGETAL feature indices, and with them the feature seeds of every
vegetal feature in newly generated chunks. Saved chunks keep what they have.

### Blocks

Stone family: `hushstone`, `polished_hushstone`, `hushstone_bricks`,
`cracked_hushstone_bricks`, `chiseled_hushstone_bricks`, plus stairs and slabs for
hushstone, polished hushstone and hushstone bricks, and the hushstone brick wall.

Wood family: `whisperwood_log`, `stripped_whisperwood_log`, `whisperwood_planks`,
`whisperwood_stairs`, `whisperwood_slab`, `whisperwood_fence_gate`, `whisperwood_door`,
`whisperwood_trapdoor`, `whisperwood_fence`, `lantern_leaves` (glowing, untinted),
`whisperwood_sapling` (grows in the dark — the only tree that does; MC saplings need light 9).

Ground and flora: `sculk_loam`, `hush_moss`, `hush_grass`, `resonance_bloom` (glowing
flower).

Crystal and ore: `resonant_crystal` (glowing cube, the crystal formations), `resonant_cluster` (wall
cluster, caverns), `echo_ore` → echo shards, `resonite_ore` → raw resonite,
`resonite_block`.

Special: `echo_lantern` (hanging or standing light, crafted from a resonant crystal and
echo shards), `echo_core` (the vault heart; only a resonite pickaxe breaks it; breaking
it wakes the Silent Warden), `hush_portal`, `resonant_chime` and `choir_altar` (the Choir
Hall puzzle, below; both glow, the chime brighter while it sounds), `hush_lighthouse_lamp`
(a glowing Fresnel lens between a resonite base and cap, with a block entity whose renderer
sweeps the lighthouse beams — below; beacon-class, 3.0 hardness, no tool, drops itself;
found in lighthouses and, rarely, their chests; motes drift in to settle on it).

Aurelith (the city, below) has its own material language, `tools/gen_aurelith_textures.py`
and `tools/gen_aurelith_block_data.py` (textures, models, blockstates, loot, recipes,
tags; both `--check`):

- **Choirstone** — pale, cool, polished stone ("hushstone sung smooth"): `choirstone`,
  `polished_choirstone`, `choirstone_bricks`, `cracked_choirstone_bricks`,
  `chiseled_choirstone` (Stave rings), `choirstone_tiles`, `choirstone_pillar` (axis),
  stairs and slabs for polished / bricks / tiles, `choirstone_brick_wall`.
- **Stave stone** — dark stone carved with glowing Stave glyphs (emissive, a slow shimmer).
- **Nightglass** — smoked violet-teal glass, translucent, drops itself (tinted glass).
- **Resonite grate** — a copper-grate-class cutout cube in resonite.
- **Lumen panels** (`cyan_`, `violet_`, `amber_lumen_panel`) — glowing panels in a thin
  resonite frame, breathing slowly; **lumen strip** — a light band in a dark casing that
  runs along its axis (a vertical line of light up a tower, a band along a cornice), light
  flowing along it. All emissive, sea-lantern class (0.3, no tool).
- **Crystal conduit** — a chain-class pipe (axis, waterlogged): a glowing crystal core in
  resonite collars. **Choir lamp** — a lantern-class streetlight (standing or hanging): a
  crystal globe in a resonite cage. Both emissive.
- **Resonant water** — the river Vesper's glowing violet water: an always-water block
  (bubble column's rule) drawn by the fluid renderer with its own sprites, glow and
  underwater fog; water in every rule (`docs/fluids.md`, "Resonant water").
- **Resonance engine** — the Heart's core (beacon class, block entity 40): its hanging
  crystal rings are `AurelithHeartRenderer`'s. Found only in Aurelith (no recipe).
- **Voice beacon** — a gate tower's lens (block entity 41, `facing` n/e/s/w picks the
  voice): `VoiceBeaconRenderer` stands a 256-block sky beam on it (`Render::VolumetricBeam`)
  in the voice's colour — Soprano pale cyan-white, Alto cyan, Tenor violet, Bass indigo.

Emissive now reaches every face of an emissive block, not only full-cube faces the greedy
mesher merges: a vertex flag (`TerrainVertex` alpha 0xFE, `docs/engineering-notes.md`) lets
echo lanterns, resonant clusters, choir lamps, conduits and resonant water glow at night.

#### The lighthouse lamp

`HushLighthouseRenderer` (`client/renderer/blockentity/`) draws it off the lamp's block
entity (`HushLighthouseLampBlockEntity`, type `hush_lighthouse_lamp`, id 25 — it carries only
the lamp's guide, below), the way MC's `BeaconRenderer` hangs off the beacon's.

- **Beams** — two opposed beams leaning 3° down, each a cone lying on its side, 60 blocks
  long from just outside the lens: an outer glow of six planes round the beam's axis
  (teal, widening from 0.3 to 6.6 blocks half-width) and a core of three (pale cyan-white,
  0.1 to 1.5), alpha fading in over the first two blocks and out along the length. The
  shared block-entity shader, alpha-blended, depth-tested with no depth write, both faces,
  emissive and fogged (MC `RenderTypes.beaconBeam(translucent)`); past 96 blocks the beams
  widen with distance (MC `beamRadiusScale`). A glow billboard sits in front of the lens and
  flares to three times its size when a beam points at you.
- **Sweep** — a pure function of the level's game time (the Hush's day time is fixed, its
  game time runs) and a seed from the lamp's position (MC `Mth.getSeed`), so every player
  sees the same sweep and no two lamps run in step. Each lamp turns one way (seeded) a
  quarter turn per stroke, a stroke every 7.5–10.5 s (a full turn in 30–42 s). A stroke is
  a pause, the move eased in and out (a sigmoid of seeded steepness), a pause; six strokes
  in ten stall part-way (for 5–15 % of the stroke), and most stalls shudder back 1–2°
  before they catch. The light breathes, shimmers at ~7 Hz and, in about one half-second
  in thirty, gutters and recovers.
- **It notices you** — per viewer: within 40 blocks (fading out by 56, and not at the
  tower's foot), a beam coming round to you slows over 24° of its sweep, stops on you for
  16° of it (about a second and a half), dips to your eyes (down to 50° below level),
  steadies its light and quivers, then swings off faster to make the time up. A C1,
  never-reversing warp of the sweep round your bearing, blended in by distance, so the beam
  never jumps. Portal views draw the plain sweep.
- **It guides** — within 600 blocks of an Aurelith, a lamp now and then points the way. The
  server looks the city up once per lamp (`server/level/LighthouseGuide`: the Aurelith
  random-spread cells whose potential start chunk could be in reach, the structure's own
  generation run for each — biome, level site, jigsaw — and the start piece's centre taken
  as the Heart; cached per cell for the session, at most one lamp answered per tick, queued
  as lamp chunks arrive). The answer lives in the lamp's block entity (`GuideChecked`,
  `GuideX`, `GuideZ` in the chunk NBT; sent with the chunk and on change), so it is found
  once per world. In one turn of every four to six (seeded per lamp), the first stroke
  that carries a beam across the bearing to the Heart splits its move there: the beam
  eases onto the city, holds ~3.6 s with its light steadied and a touch brighter and the
  beam levelled toward the horizon, then eases on and still ends its stroke on time — a
  continuous pure function of game time, the lamp and its target, identical for every
  player; the noticing warp runs on top.
- **Off-screen** — MC `shouldRenderOffScreen`: the block-entity dispatcher draws the lamp
  while its section is culled, and walks the loaded chunks within 72 blocks for lamps no
  visible section brought in, so the beam still crosses the sky in front of you when the
  tower is behind you. Seen from any distance while its section is visible.

### Items

`raw_resonite`, `resonite_ingot`, `resonite_sword` / `_pickaxe` / `_axe` / `_shovel` /
`_hoe` (tier above diamond: speed 9, +1 damage, 1800 durability where durability
exists), `resonant_heart` (boss drop), `echo_blade` (capstone sword: 10 damage, and a hit
silences — Slowness II for 3 s on any mob; players carry no status effects in this
engine). Resonite is its own mining tier, one above netherite: it mines everything
netherite does, and `echo_core` yields to nothing else. `choir_heart` (the Choir Mother's
drop; stack 1, epic). Aurelith's quest: `soprano_voice_key`, `alto_voice_key`,
`tenor_voice_key`, `bass_voice_key` (rare, stack 1) and `held_note` (epic, stack 1; the
reward — see "Reawakening the Heart" under Aurelith).

### Tools of the deep

Items `tuning_fork`, `echo_compass`, `cloak_of_silence`, `resonance_bow`,
`recall_chime`, `whisperfruit`; blocks `hanging_whisperfruit`, `echo_heart`.
Behaviour: `ItemBehaviors.cpp` (thin shells) → the common bridge
`common/world/level/HushItems.hpp` → `server/items/HushItems.cpp`; blocks in
`world/block/HushBlocks`. Server→client effects ride one packet,
`HushSignalS2C` (0x72, a kind byte: ping / burst / compass), drawn by
`client/world/HushSignalState`.

- **Tuning fork** — use it on a resonant crystal or cluster: a resonance ping.
  For 6 s the striker sees, through walls (always-on-top gizmo cuboids), every
  ore within 24 blocks of the crystal (`*_ore` + ancient debris; echo ore paler
  cyan, capped at the nearest 512), every mob within 24 (red), and the nearest
  Echo Vault / Warden's Tomb within 64 (violet) — marked at its `/locate` point
  on the surface, where the dig starts, not at a modelled entrance. A ring of
  motes spreads from the crystal. Cooldown 5 s.
- **Echo compass** — its needle points at the nearest Echo Vault. The server
  searches (`FindNearestStructure`, 8 rings) every 2 s only when the holder has
  walked 48+ blocks from the last search, caches it per player and sends the
  target when it changes; the client runs its own `CompassAngleState` wobble
  (`Item.cpp`, frames `echo_compass_00..31`). Outside the Hush it spins.
- **Cloak of silence** — chest-slot wearable (EQUIPPABLE, no armour). The
  warden's (and the Silent Warden's) sniff sensor and the echo wraith's hunt
  goal skip its wearer (`HushItems::IsSoundCloaked`, through
  `EntityLevel::GetChestItemId`). A warden you hit, or one already angry,
  keeps its anger; a wraith you hit still retaliates.
- **Resonance bow** — MC's `BowItem` draw/release (the vanilla bow now shares
  it: this engine had no bow). Its `ResonanceArrow` pierces one mob and bursts
  on every impact: 2 damage (magic) and knockback to everything within 2
  blocks but the shooter, and a ring of motes. Spawned and saved as a plain
  `arrow`.
- **Recall chime** — hold 2.25 s (45 ticks, goat-horn pose; the length of its
  note: `block.bell.resonate` at pitch 1.6 rings out after 1.9–2.2 s across its
  variants): back to the landing of the last hush gate crossed, in either
  direction (recorded on every real hush-portal crossing, vanilla and
  immersive; saved as `obey_hush_gate` in the player data; `/dim` and `/tp`
  cross no gate), cross-dimension allowed, nudged up to 2 blocks out of the
  portal along the arrival facing. Not used up. Cooldown 60 s, spent only by a
  recall that happens. Every refusal says why on the action bar: no gate
  remembered yet, still ringing (seconds left), let go before the note rang
  out. The teleport runs from `HushItems::TickPlayer`, right after the
  player's own tick in which the hold ended.
- **Whisperfruit** — `hanging_whisperfruit` (age 0..2, no facing) hangs under
  lantern leaves: whisperwood trees carry it (`AttachedToLeavesDecorator`, 0.08,
  age randomized), it grows by random tick with no light gate, takes bone
  meal, falls when its leaf goes, and a ripe one is picked by right-click (1–2
  fruit, back to age 0). The fruit (4 hunger, saturation modifier 0.3, Night
  Vision 60 s) plants it under a lantern leaf. The block's slug differs from
  the item's on purpose: one slug for a block and a pure item would be
  ambiguous to recipes and loot.
- **Echo heart** — a glowing cube. Its own scheduled tick every 10 ticks
  (booked on place, re-booked by a random tick if lost; saved with the chunk)
  gives players within 16 blocks Regeneration I and Resistance I (ambient,
  220 ticks, refreshed every 80 like a beacon), throws echo wraiths and echo
  mimics out of that radius, and renews its entry in a registry the natural
  spawner asks: no monster-category mob spawns naturally within 16 blocks. A
  column of teal motes rises from it (client animateTick).

### Mobs

- **Hushling** (creature) — a small six-legged sculk creature, black with cyan eyes.
  Skittish: wanders, freezes when you come close, flees when you come closer. Spawns in
  packs of 2–4 on the meadows and in the forest. Drops sculk, rarely an echo shard.
- **Echo Wraith** (monster) — a translucent teal spirit that flies and charges; the
  Hush's answer to the vex (it respects walls — the mover has no ghost mode). 14 HP, 4 damage. Spawns everywhere in the Hush,
  thickest in the caverns and forest. Drops echo shards.
- **Silent Warden** (boss) — the warden that never left. Wakes when a vault's echo core is
  broken; 300 HP, sonic boom, boss bar. Drops the resonant heart, once per vault.
- **Lumen Moth** (ambient) — a hand-sized violet moth with a glowing abdomen and eyespot
  wings. Drifts to the nearest light — any emissive block, a torch or lantern, a player
  holding one — and circles it. Swarms of 2–6 at the surface of every surface biome. Drops
  nothing.
- **Crystal Golem** (creature, neutral) — an iron-golem-sized hushstone guardian grown
  through with resonant crystal; 80 HP, slow, a 12-damage swing that throws you. Guards
  crystal fields (only spawns within 8 blocks of resonant crystal; Resonant Barrens,
  Aurora Steppe) and stays near where it spawned. Turns hostile when hit or when a player
  breaks a resonant crystal or cluster within 16 blocks; its crystals whiten while angry.
  Drops raw resonite and resonant crystal.
- **Hush Leviathan** (creature) — a 6×3-block sky whale with a crystal-studded spine, drifting
  on long curving paths 22+ blocks above the ground. Passive; rare (a natural spawn is refused
  within 96 blocks of another). 60 HP, drops nothing.
- **Echo Mimic** (monster) — a teal after-image of a player: a faint shimmer until you are
  within 6 blocks, it replays its target exactly two seconds late — the same path (jumps and
  drops included), the same look (yaw and pitch; its body turns by a player's rule), the
  crouch, and the arm swings (attacks and digging). Only when it catches up to striking range
  does it look at the real you, raise its arms and strike; fall back out of range and it
  resumes the echo. Knocked off the trail (or blocked from it, or left behind by a teleport)
  it walks back on. Light is its weakness — double damage within 4 blocks of a light, and it
  burns beside one. Hollow Deep and Crystal Caverns. Drops an echo shard, sometimes.
  (`EchoMimic` in `HushCreatures.hpp`: a per-tick trail of position, look, ground, crouch
  and swing, bridged across ticks the network brought no move; the replay step is applied
  in `Travel`, after the look and move controls.)
- **The Choir Mother** (boss) — a floating sculk matriarch, veiled, horned and trailing
  tendrils; 400 HP, purple boss bar (shared with the Silent Warden's: each player sees the
  nearest boss). Summoned by solving the Choir Hall's puzzle. Three phases by health:
  (1) conducts — summons 2–3 echo wraiths every 10 s (at most six near her); (2) adds sonic
  rings — an expanding ring of motes along the floor that hits for 6 and throws you outward
  unless you jump it; (3) adds the silence field — Darkness and Slowness II within 12 blocks. Drops the choir heart, 6–10
  echo shards and resonant crystal.
- **The Unsung** (boss, `the_unsung`, `TheUnsung.hpp`) — Aurelith's twist: the Undersong in
  the only shape it ever heard, a conductor's. ~3.9 blocks tall: indigo robes flaring into
  hanging sculk plates and tendrils, a pale blank mask split by a crack of light, a hollow
  crystal ring for a throat, a dark-crystal baton, and four voice shards (Soprano, Alto,
  Tenor, Bass colours) orbiting it. Rises out of the Heart's dais when a city is woken (the
  quest controller calls `SetArena(heart, rotation)`; 3 s untouchable while it rises). 500 HP,
  armour 10, white notched boss bar "The Unsung", immune to the stillness, never despawns.
  Phases by health, each change a 2 s untouchable tell (it rears up and roars):
  (1) *The False Chord* — hovers after you; a baton sweep with a 0.7 s raised-arm wind-up
  (8 damage, 10 / 12 in later phases); discord notes (shulker-bullet flight, dodgeable and
  destructible; 4 damage, Slowness II + Weakness I for 3 s instead of levitation); a choir of
  echo wraiths (2 per call, at most 4 near it). It **steals a voice**: one statue of the Four
  Voices goes dark (its throat crystal turns to sculk), a tether of that voice's colour runs
  to it and it takes 15 % damage — stand at that statue's foot (4.5 blocks) for 3 s to sing it
  back (two players: twice as fast; action-bar progress), which relights the statue and
  staggers it for 3 s (150 % damage taken). (2) *The Undersong* — adds sonic rings on the plaza
  floor (jump: 6 magic, a shove) and the **Heart's pulse** every 30 s: a 3 s charge (motes
  climb the Heart's column, a rising note), then a pulse — if the Unsung is within 9 blocks
  of the dais it takes 25, is staggered 5 s and any stolen voice returns; players by the dais
  get Regeneration II. (3) *The Last Rest* — every ~35 s it flies to the dais and channels for
  8 s, drawing a dark stream out of the rings: 40 damage breaks it (3 s stagger); a finished
  channel heals it 60 and lays the Silence (Darkness + Slowness, 6 s, 32 blocks). Death is an
  80-tick dissolve — it rises while the four voices stream back to their statues. Drops 8–14
  echo shards, 2–4 resonite ingots, 3–5 resonant crystal, 200 xp; the Held Note is the city's
  reward (given once by the quest, not the loot table). Without an arena (egg, `/summon`) it
  adopts the nearest resonance engine within 32 blocks (the full fight, voices named as if the
  city were unrotated), else fights at its spawn point without the statue and Heart mechanics.
- Existing: endermen everywhere, skeletons on the barrens, bats underground.

#### The Choir Hall puzzle

Right-click the `choir_altar` with an echo shard: the altar takes the shard and sings a
random sequence of 4–6 notes by lighting the ring's `resonant_chime`s one at a time.
Strike the chimes (right-click) in the same order within 5 s + 2 s per note. Right, and the
shard is spent and the Choir Mother rises three blocks above the altar; a wrong note or the
timeout resets the song and returns the shard. The ring is every chime within 12 blocks
horizontally and 4 vertically of the altar, ordered clockwise from north — structures just
place chimes round an altar (at least three).

### Structures

- **Echo Vaults** — sunken hushstone-brick halls under the surface with a sinkhole shaft,
  degraded walls, loot chests (`chests/echo_vaults`), and the echo core at the heart.
- **Whisper Shrines** — small surface ruins: a ring of hushstone-brick pillars around an
  echo lantern and a chest (`chests/whisper_shrine`).
- **Choir Hall** — a grand hall under a ribbed dome (25×18×25): eight polished piers, choir
  stalls along the walls, a rose window, echo lanterns on chains from the vault. At the
  centre a dais with the `choir_altar`, and a ring of six `resonant_chime` blocks on the floor
  around it — the Choir Mother's note puzzle. Two chests (`chests/choir_hall`). Aurora Steppe
  and Hush Meadows, rare (spacing 40, separation 16).
- **Hush Lighthouses** (`minecraft:hush_lighthouse`) — weathered, tapering hushstone-brick
  towers, 32 tall on an 11×11 footing, octagonal in plan. At the foot the keeper's room
  (radius 4): a chest (`chests/hush_lighthouse`: echo shards, glow ink, paper, arrows, raw
  resonite, resonant crystal, a spyglass, a sculk sensor, now and then a lighthouse lamp),
  bookshelves, a lectern, a chair, a hanging echo lantern, cobwebs, three cyan-glass
  windows. A ladder climbs from it up a shaft through thick lower masonry, past a brick-slab
  ledge where the tower steps in (radius 3), glass slits and two sculk-sensor "listening"
  windows, to a gallery on upside-down-stair corbels (echo lanterns hung under the diagonal
  ones) with a whisperwood-fence rail. The lantern room is a 5×5 ring of cyan stained glass
  on a chiseled sill between four polished mullions — the ladder arrives through its west
  side, a door opens east onto the gallery — with the `hush_lighthouse_lamp` on a chiseled
  pedestal at its centre (y 25). Brick-stair eaves, a resonant-crystal finial and an
  iron-chain spike on top. Sculk grows in the walls and as veins up the outside, thinning
  with height; the vaults' degradation processor cracks a fifth of the bricks per placement.
  Every surface biome, a rare landmark (spacing 44, separation 16, salt 73920417 — a quarter of the first pass's density); on the Sunken Choir's
  shallows `beard_thin` builds ground up under its footing. Placement is a plain
  random spread: there is no coastal bias. They replaced the Listening Posts (2026-09-22);
  posts already generated stay as blocks.
- **The Warden's Tomb** — the Silent Warden's arena, buried (top 2 below the surface; a
  ladder shaft on a jigsaw reaches up through the ground): an entrance hall, a corridor
  framed in reinforced deepslate, then a dome 25 across and 12 high with reinforced-deepslate
  ribs, a wide open floor ringed in deepslate tiles, soul lanterns and chained echo
  lanterns, and the `echo_core` on a two-step dais at the centre. Behind the arena, reachable
  only through it, the treasure room: three chests (`chests/wardens_tomb`, with a second
  pool that sometimes adds music disc 5 or an enchanted golden apple) round a
  resonite-block plinth. Resonant Barrens and Hollow Deep, rare (spacing 36, separation 12).
- **Sunken Library** — a half-flooded library set four blocks under the water line in the
  Sunken Choir: the lower hall is flooded (submerged shelves, two broken shelf rows, kelp,
  sea pickles, two waterlogged chests), a whisperwood gallery with a fence rail and three
  lecterns runs round it above the water, with a dry chest; the door is at gallery level
  with its threshold one block above the water line; the ceiling has fallen through in three
  places. Chests: `chests/sunken_library` (paper, books, echo shards, disc fragments, glow
  ink, an enchanted book, and one lore book per chest). Spacing 24, separation 8.

  The lore books are real written books (`written_book`, `set_book_cover` +
  `set_written_book_pages` in the loot table), all signed by **Wren of the Listeners**, a
  scribe of the library, and readable in the hand or on a lectern (see
  `docs/hush-lore.md` for the canon they follow):
  - *Hymnal of the Sunken Choir* — the hymns sung over the Shallows: for the lighting of
    lanterns, the crossing of water, the Four Voices, and those who rest.
  - *On the Silent Warden* — why the Wardens hunt a false note, and how the oldest of them
    was set beneath the echo cores during the Souring and never came back up.
  - *The First Listening* — the Choir's origin story: a note sung into amethyst that came
    back brighter, the ancient cities, and the gate that opened onto the Hush.
  - *Resonance: A Treatise* — a tuner's primer: sound is a shape, light is song made
    visible, echo shards are notes frozen, resonite, and the Heart's Undying Chord.
  - *Litany of the Chimes* — the Choir Hall rite (the altar's song, answered chime by
    chime), with a later warning that something else now answers it.
  - *Where the Sculk Began* — written by Wren after the Great Rest: the Heart silenced,
    the Last Procession, and the listening sculk growing into the silence.

  To read them without finding a library, `/loot give @s loot minecraft:chests/sunken_library`
  rolls the chest's table straight into the inventory.

- **Aurelith, the Lantern City** (`minecraft:aurelith`) — the Choir's capital, abandoned
  mid-life with its lights still burning (lore: `docs/hush-lore.md`). A walled octagonal
  city ~206 blocks across on one levelled platform, generated by `tools/gen_aurelith.py`:
  - **Streets.** Two 13-wide avenues (Canticle Way north–south, Held Note Avenue east–west)
    paved in choirstone tiles with flush cyan lumen kerb lines, choir lamps every 8 blocks
    and glowing whisperwoods in the outer lanes; a grid of 7-wide streets; every building
    has street doors, a switchback stair to its roof and a railed roof terrace.
  - **The Plaza of the Held Note** (centre): concentric paving with cyan, violet and Stave
    rings and eight lumen spokes pointing in; a colonnade of 52 pillars whose lintel frames
    each avenue; a half-step dais with the **resonance engine** on a crystal pedestal —
    the silenced Heart, four crystal rings turning slowly and unevenly above it (they
    slow and a pulse runs round them when you come close); the Four Voices as statues on
    the axes, facing the Heart; the Conductor's Podium north of the dais (Ysolde's plaque,
    the Charter on a lectern). The sculk is thickest round the Heart.
  - **The Vault of the Chord**: a whisperwood trapdoor in a ring of Stave stone behind the
    Bass statue (west) opens on a ladder, a lamp-lit passage runs under the plaza to a
    round chamber under the dais — the spring the Heart was raised over, crystal pillars,
    three `chests/aurelith_vault` chests, Ysolde's last words on a lectern.
  - **The river Vesper**: four blocks of glowing violet resonant water two below the
    street, 9 wide, from the Vesper's Spring (a round basin with a crystal fountain, west)
    to the harbour and the Listening Well (a glowing shaft 15 deep, east); violet panels
    in chevrons on the bed, crystal clusters, quay walls with a light line, railed
    promenades, glowing trees; three arched bridges (Canticle Way's grand bridge, two
    humped footbridges). The water sheds no particles (the `HushMist`/`VesperGlint`
    emitters on `resonant_water` and the awakened city's river were removed).
  - **The Vesper Quays**: the harbour basin, two whisperwood piers, mooring posts,
    moored boats, the harbourmaster's house (`chests/aurelith_quays`, the Canticle).
  - **Towers**: the Conductor's Spire (tiers of 19, 15 and 11, a view terrace at S + 70
    with the Conductor's chair and Ysolde's diary, a crystal-crowned needle to S + 95 —
    the view point where the four beams, the river and the Heart line up); the Archive of
    Echoes (a ring tower 68 tall: galleries every 7 blocks round an open atrium, a helical
    ramp of half-steps up the atrium's edge, a crystal column, Stave rings outside, a
    sealed reading cell in the wall); Starward (a stepped ziggurat with grand exterior
    stairs, a ribbed nightglass dome over a star-map floor); crystal needles; a sky bridge
    at S + 42 from the Archive through a needle to the Spire across Canticle Way, and a
    fallen one from Starward whose middle lies in the street below.
  - **The Four Gates**: Soprano (north), Alto (east), Tenor (south), Bass (west) — two
    41-tall towers each (inscription rings, corner light lines, storeys joined by a
    switchback, crowned tops) flanking an arch over the avenue, a walk over the arch, and
    a voice spire carrying the **voice beacon**: four sky beams that find the city from
    far away.
  - **Quarters**: the Hall of Instruments and the Tuners' Works (crescents on the plaza;
    the Works' conduit line runs under grates to the dais), the Stillhouses (terraced
    houses on Stillhouse Row: beds made, tables laid, amber hearth-light in the windows;
    Maren's house with the unsent letter), the Arcade of Echoes (arcaded shops, stalls
    with goods left out, Hesper's lamp stall nine), the Quiet Gardens (three terraces,
    groves, a still pool), and ~70 mid-rises and towers (10–14 storeys near the centre,
    rotundas, setback terraces, domes, crowns, conduit masts).
  - **Life**: some lamps will not settle. Seven flickering-window blocks
    (`guttering_amber_window`, `waking_amber_window`, `restless_amber_window`,
    `guttering_cyan_window`, `waking_cyan_window`, `guttering_violet_window`,
    `waking_violet_window`): a lit window (four panes, a mullion cross, a lamp low in the
    room) animated through lit / guttering / ember / dark frames on an irregular seeded
    .mcmeta schedule with per-frame `time` (the texture animator honours MC's
    `AnimationFrame.time`), each block its own 2–3 minute loop so no two kinds blink
    together. Guttering: mostly lit, a sag now and then, rarely out and coaxed back.
    Waking: mostly dark, a lamp lit for a spell or a glow carried past. Restless (amber):
    as often lit as not. The generator turns ~55 % of the lit facade windows into
    guttering ones (12 % of the amber ones restless instead) and ~4 % of the dark ones into
    waking ones, a whole window at a time, by a hash of the window (`place_flickering_
    windows`); Maren's lamp stays steady. Sea-lantern class, emissive, block light 9
    (waking 4, restless 7 — a steady average; block light does not animate); a nightglass
    over a lumen panel makes two guttering windows, two nightglass three waking ones, a
    panel between two nightglass three restless ones. **The wall's pulse**: a slow swell of
    light travels the Stave band round the wall once every 90 s (a bright head, an
    18-block tail), drawn by the four voice beacons as additive quads just proud of the
    band's exposed faces, each beacon its quarter of the circuit, skipping the gate
    complexes (`AurelithWallPulse`); an awakened city carries a second swell opposite and
    burns brighter.
  - **Interiors** (`furnish_room(kind=...)`, the room kits at the end of
    `gen_aurelith.py`): every door opens onto a room someone left mid-life. A mid-rise
    floor is laid out by use (`interior_kind`: shops and workshops at street level on the
    busy streets, homes above, the Listeners' and conductors' quarters bookish): *home* —
    beds with a nightstand and rug, a kitchen run (smoker, counter, cauldron, barrel), a
    table laid for two, a settle, shelves; *workshop* — crafting / smithing / fletching
    benches, grindstone, stonecutter, racks of crystal and conduit stock; *study* — desks
    with chairs and candles, shelves two high, a map table; *shop* — counters with goods,
    stock shelves, the till. A big floor gets one set per ~45 cells. The Stillhouses have a
    hearth, kitchen and table below and wardrobes above; the gatehouses a guard room
    (bunks, a mess table, stores); the Tuners' Works a test bench of note blocks and chimes;
    the Spire's lobby is the conductors' study; the Archive's galleries have reading desks
    against the shelves (`archive_reading_rooms`); the Arcade's stalls each carry one
    theme of goods — lamps, pottery, crystal, candles, cloth, books, glass, food
    (`stall_goods`) — and never stand in front of a shop door. Every piece goes in as an
    *attempt* that is undone if it would cut a room's stairs and doorways (the generator's
    `keep_clear` cells) off from one another, and nothing is set on a cell that opens
    straight onto the street (a colonnade walk), so the walk-check stays green.
  - **Age**: cracked bricks, sculk blooms along the river and in the streets (veins up the
    walls, sensors listening), moss and grass in the paving, dead lamps, cobwebs, five
    buildings with a broken upper corner and rubble at their feet; the
    `aurelith_weathering` processor varies it per city.
  - **Lore**: 14 written books (`tools/aurelith_books.py`: the Charter, the Heart's
    maintenance log, the Conductor's diary, a child's lesson, the final broadcast, a
    traveller's legend, the Canticle of the Vesper, the lampwright's primer and ledger,
    the making of Wardens, the Four Voices, the observatory's notes, an unsent letter, the
    Undersong) on lecterns across the city and in the district chests; ~40 glowing signs:
    street names, gate inscriptions, plaques (`HERE THE CHORD WAS HELD`, `YSOLDE / LAST
    CONDUCTOR / SHE STAYED.`), a scratched `we will come back` by the Soprano Gate.
  - **Loot** (`chests/aurelith_*`): stillhouse, arcade, archive, quays, tuners,
    instruments, gardens, observatory, gatehouse, spire and vault — each a themed table
    plus a pool that usually adds one of the district's lore books (always, in the vault).
  - **Inhabitants** (`place_inhabitants`, template entities placed once with the city and
    kept): a crystal golem standing watch just inside each of the Four Gates (neutral, home
    16 round its post), two echo mimics in the Archive of Echoes' dark galleries (the second
    and fourth), and four hushlings grazing the Quiet Gardens' lowest terrace. The city is
    otherwise quiet: `aurelith.json`'s `spawn_overrides` gives MONSTER an empty list over the
    whole city (`full`), so echo wraiths do not roam its streets — except the Archive's
    interior (its atrium and galleries), where an `obeycraft:districts` override lets echo
    mimics (1–2) spawn in the dark. The district is written by `aurelith_spawn_overrides` as
    boxes in the Archive's four pieces' own coordinates.
  - **Placement.** Biomes: Hush Meadows, Aurora Steppe, Resonant Barrens
    (`has_structure/aurelith`). `random_spread` spacing 90 chunks, separation 32, salt
    1147031147; the site must be level (the engine's `level_site` extension: a 7x7 height
    sample across ±100 blocks, refused beyond 24 blocks of spread, the platform set at the
    median) — together about 1 % of the Echo Vaults' frequency: one city per ~21 km² of the
    Hush (cities typically 4.5–5 km apart; spacing 52 gave 3 %, and the user asked for 1 %
    — frequency goes with 1/spacing², the site pass rate is unchanged). Measured offline (6 seeds, 32k-block windows):
    42 % of grid cells have a city biome at the site, and the Hush's rolling terrain is
    rarely flat over 200 blocks (median spread 39; 4 % of those cells within 18, 26 % within
    24), so the first placement (spacing 96, spread 18) built a city in 1.7 % of its cells —
    one per ~140 km², 0.15 % of the vaults. With spread 24, 9.8 % of the cells build one.
    `beard_box` carves the ground above the street and builds it up below.
  - **Pieces.** The city is one designed volume cut into 32×32 columns, each an upper
    piece (the street and up) and, where the river, the wall footings or the vault reach
    down, a lower piece; unique jigsaw links make the assembly exact (the whole city takes
    the start's random rotation). ~590k template blocks in 79 pieces.
  - **The outskirts** (`obeycraft:aurelith_outskirts`, `tools/gen_aurelith_outskirts.py`;
    lore in `hush-lore.md`). Four ruined roads leave the gates (Soprano 330 blocks past the
    gate, Alto 290, Tenor 310, Bass 270): eleven wide for the first 24 blocks, then seven,
    choirstone tiles down the middle, polished kerbs; lamp posts every 24 blocks for the
    first 150 (stumps, broken shafts, now and then a whole post with its echo lantern still
    lit); the paving thins from ~92 % kept at the gate to ~20 % far out and gives out over
    the last eighth, cracking, sinking into potholes and growing moss and sculk on the way;
    past the first 60–140 blocks the line meanders ±3. Every stone is set on its own
    column's surface, so a road drapes over hills instead of cutting or floating; it paves
    only natural ground (hushstone, sculk loam, hush moss, sculk, calcite and the like) and
    so drowns where it meets water and never paves over another structure. A **mile-marker**
    every measure (100 blocks; the wall is one measure from the Heart, so the first stone out
    reads II): the stump of a Stave-banded column on the right of the road going out, its
    face cut in the Stave (a stave-stone drum) and the words on a waxed sign
    (`mile_<voice>_<n>`), the shaft fallen away from the road, its crystal cap in the grass.
    **Along the roads** (each dropped per city by chance): farmsteads (a stillhouse plan whose
    roof fell in, a cold hearth, a bed frame, a wild plot of whisper saplings), road watch
    posts (a hushstone tower stump broken on a slant, a ladder to a surviving ledge, the
    crown fallen beside it), roadside shrines (a ring of short pillars round an echo
    lantern — REST AND LISTEN), a colonnade fragment, a cistern of glowing water, and over
    the Soprano road 36 blocks out the **processional arch** of the Last Procession, its
    lintel fallen onto the road. **The Garden of Stones** (off the Bass road, 150 out, 41 to
    the right; 47×47, a path from the road to its gate): a broken hushstone precinct wall,
    the processional way past blank-faced statues to a round chapel of twelve pillars (one
    fallen) round a bier and a Stave ring, the lectern with "The Rite of Rest", rows of
    tombs with their names on plaques and a lamp at each head, cypresses of whisperwood
    along the wall, and the Conductors' mausoleum (the First Conductor, Severin, Ismay, one
    stone with no name cut yet) with the Keepers' chest (`chests/aurelith_necropolis`).
    **The waystation** (off the Alto road, 165 out, 27 to the left; 29×25): a courtyard
    wall, a gate with its lamps and a bell, a signpost, a two-room hostel (the common room
    with the hearth, the laid table and "The Waystation Book"; four bunks and the chest,
    `chests/aurelith_waystation`), an open shelter with troughs and a well of glowing
    water under a lantern post.

    *How it is placed — companion structures (engine extension, `my_terrain_library`
    `levelgen/structure/AurelithOutskirts.{h,cpp}`).* A start is only referenced by chunks
    within 8 chunks of it (MC `ChunkGenerator.createReferences`), so nothing 300 blocks out
    can hang off the city's own start, and raising that radius would change the chunk
    dependency grid for every structure. Instead the structure set
    `obeycraft:aurelith_outskirts` has an **`obeycraft:anchored` placement**: `anchor_set`
    (`minecraft:aurelith`), `anchor_origin` (the Heart's position in the city's start piece,
    16,16) and a list of `slots` — design-frame offsets from the Heart. A chunk is a
    placement chunk when, for a potential start chunk A of the anchor's random_spread grid
    and some rotation r, a slot lands in it (`A + floor(R_r(slot + origin) / 16)`, MC
    `Rotation.rotate` with pivot zero). The structure (type `obeycraft:aurelith_outskirts`)
    then runs the city's own generation for A (its biome check and level-site test —
    "does the city exist here?"), cached per (seed, A), reads the start piece's rotation
    and keeps only the slots whose chunk matches under it, so every piece lands relative to
    the Heart and the roads leave along the gates' real axes. A slot holds road stretches
    (code pieces: a road is split into 64-block stretches, one start each, so no piece is
    beyond the reference radius; they are pool-element pieces with non-rigid projection,
    which the Beardifier leaves alone) and templates (rigid, `beard_thin`, set on the
    median of five surface probes over their footprint, `chance` per city from a hash of
    seed, anchor and piece, `dry` refuses a site under water). 23 starts per city; the
    anchor set must have frequency 1 and no exclusion zone (checked at load). The engine's
    own structures (namespace `obeycraft:`, data under `data/obeycraft/worldgen/`) are
    appended after every vanilla and mod structure of their step in the decoration order,
    so they shift no vanilla, Twilight Forest or Aether feature seed.
    `/locate structure` cannot find a companion start (its placement is neither rings nor
    random spread): find the city and walk out of a gate.
  - **City sound** (`client/sound/AurelithSounds`, events in
    `assets/sound_overlays/obeycraft/aurelith.json`; the city is not a biome, so this runs
    beside the biome ambience and reads the city records in `client/world/AurelithState`):
    - *The Heart's hum*: looping sounds bound to the resonance engine (a low beacon drone,
      heard ~40 blocks out and swelling as you approach). Once the Chord is sung a
      brighter conduit layer swells in (`AurelithState::Voice`) and the pitch climbs with the
      rings' pace; under the Undersong a sour respawn-anchor layer wobbles against it.
    - *The gates' thrum*: a loop on each voice beacon's lens (~56 blocks), pitched by voice
      (Soprano highest, Bass lowest), louder once the city is lit and swelling as the beams
      bend together.
    - *The river*: `resonant_water`'s animateTick sound (`BlockAmbientSounds`): low lapping
      water from surface cells only, now and then a bubble or a glassy note.
    - *The wind*: more than ~52 blocks over the street inside the walls (the Spire's view
      terrace, the Archive's crown), a wind bed that rises with height, and gusts.
    - *The empty choir*: in the streets, every 15–40 s a faint snatch of the F# Chord (two to
      four notes on a flute, glass or a bell) from 6–14 blocks away, drifting as it sings and
      stopping short of home. Awakened, every 10–25 s, sometimes doubled at the octave, and it
      resolves. Silent during the awakening and the fight; the stillness hushes it.
    - *Music*: inside the walls the city's own (`music.aurelith.dormant`: Infinite Amethyst,
      Echo in the Wind, A Familiar Room, Eld Unknown, Wending, Ancestry), silence while the
      Chord is sung, the dragon fight's track against the Unsung (`music.aurelith.unsung`),
      then `music.aurelith.awakened` (Stand Tall, Left to Bloom, Endless, Featherfall,
      Komorebi, Floating Dream). A song that no longer belongs fades out (MusicManager's
      fadePlaying), never cuts.
    - *The quest's cues* (`common/sound/AurelithSoundCues.hpp`): a sounds.json event picks
      one entry per play, so each chord-like cue is a principal event plus layers played in
      the same tick — the Chord is harp and bell on F#3, C#4, A#4 and F#5 over the big bell;
      the discord is the root against a semitone and a tritone, a low bell and cracking glass.
  - **Reawakening the Heart** (the quest; shared facts and numbers in
    `common/world/level/AurelithQuest.hpp`, the server in `server/level/AurelithCities`,
    lore in `hush-lore.md`). A generated city is **dormant**: every Stave band, lumen panel,
    lumen strip and choir lamp is its `dim_*` twin (`dim_stave_stone`,
    `dim_cyan_/violet_/amber_lumen_panel`, `dim_lumen_strip`, `dim_choir_lamp` — same
    properties, block light 2–7 instead of 15, a darker texture; each drops its lit twin).
    - *The four voice keys* (`soprano_/alto_/tenor_/bass_voice_key`, rare, stack 1) are
      placed with the city, one per district, on **voice pedestals** (`voice_pedestal`, a
      block entity holding one item, shown turning over its top; use with an empty hand to
      take it, with an item to set it down; breaking it drops what it holds):
      *Soprano* — the Archive's sealed reading cell (third gallery, north wall): break
      through the shelf wall under Liesl's seal (`SEALED / C.1147 / - L.`, one glowing
      Stave glyph). *Alto* — the Hall of Instruments' street bay: a **choir cabinet**
      (`choir_cabinet`, barrel-like, `open` state) that is tuned shut; four chimes stand
      before it on coloured plinths (violet, amber, Stave, cyan). Strike them in the Alto's
      song — amber, violet, cyan, amber (the hearth, the river, the Chord, the hearth
      again; "The Four Voices" gives it, "The Lampwright's Primer" beside it gives the
      colours) — and it opens and hands out the key; a wrong chime or a pause over 10 s
      starts the song again. Breaking it spills the key. *Tenor* — the tuning mast on the
      Tuners' Works' roof: a ladder up a 14-tall conduit pillar to a railed perch.
      *Bass* — the Vault of the Chord, before the spring.
    - *The Conductor's Podium*: four **chord sockets** (`chord_socket`, a wall-torch-like
      cradle, block entity) on the tier north of the dais, the carved order over them
      (`FOUR VOICES / FROM THE FLOOR / TO THE CROWN`). Use a key on a socket to seat it
      (it rings its voice's note; an empty hand takes it back until the Chord locks it).
      Which socket does not matter; **the order the keys are seated in** does: the Chord
      is raised from the floor to the crown — **Bass, Tenor, Alto, Soprano**. The hints:
      the Podium's carving; Orrin's "Log of the Heart" (C.1141: "floor to crown: the Bass
      first, the Tenor, the Alto, and the Soprano to close it"; C.1140 names the Tenor's
      mast); Aubade's lesson ("the deep one first and the high one LAST"); "The Four
      Voices" ("Deep, bright, warm, high"); Liesl's "Undersong" (the Soprano's key is in
      the cell with her); Ysolde's diary ("the deep one below, the high one with Liesl, the
      others with their guilds").
    - *Discord* (the fourth key in the wrong order): the root against a semitone and a
      tritone, the keys thrown out of their sockets toward the Heart, a dark burst, and the
      plaza's lights within 20 blocks stutter dark and back in a ripple (`StartFlicker`).
    - *The awakening* (timeline in `AurelithQuest.hpp`, ticks from the fourth key): the
      sockets flare one by one (an arpeggio, 12 ticks apart), the Chord is struck (56) and
      the sockets lock; the rings spin up from their dormant pace to six times it over
      6 s (60–180) under a rising cue (`aurelith.awaken_rise`, `_swell`, `_bloom`); the four
      gate beams bend inward along arcs and meet 90 blocks over the Heart, where a pillar of
      light stands up out of it (100–240; `VoiceBeaconRenderer` chains beam cones along a
      quadratic arc); from tick 140 a **wave of light** runs out from the Heart at 0.6
      blocks a tick: every `dim_*` block in the loaded city is swapped for its lit twin
      in rings, batched every 5 ticks (`UpdateClients | KnownShape`, no neighbour updates);
      the Heart bursts into motes (150). Then the Undersong: the rings go sour (jitter, a
      gutter in the lights, a sour hum), the motes turn dark (360), and at 440 **the Unsung**
      rises out of the dais (see Mobs). The city is **contested** until it dies.
    - *Resolved*: the Chord resolves (`aurelith.resolve`), the Held Note is dropped once at
      the Heart's foot, the **Coda** (Ysolde's last book) is set on a lectern on the Podium
      tier, the Podium plaque now reads `YSOLDE / LAST CONDUCTOR / SHE STAYED. / WE CAME
      BACK.` and the dais's `HERE THE CHORD / WAS HELD / C.1 - C.1147 / AND IS AGAIN`. The
      city stays **awakened**: rings at 3–6× pace, a second swell on the wall, brighter
      beams bent into the pillar, the awakened music and
      choir; chunks that load later are lit on arrival; hostile natural spawns stop inside
      the walls (`Aurelith::SuppressesHostileSpawn` in the natural spawner — the octagon
      from the wall's inner face). Lost boss (peaceful, unloaded): raised again the next
      time a player stands in the plaza, so a city is never stranded half-awake.
    - *The Held Note* (`held_note`, epic, stack 1): hold use for 1 s (the goat horn's pose)
      to sound the Chord: every hostile mob within 16 blocks stands still for 6 s, bosses are
      slowed (Slowness II, 4 s), a ring of light spreads from you. 40 s cooldown per player.
    - *State*: per city, keyed by the Heart, saved in `<dim>/data/obeycraft_aurelith.dat`
      (Dormant / Awakening / Contested / Awakened, stage start, Held Note given, Coda set).
      The city's rotation is written into the Heart's block entity at generation
      (`Rotation`), so every landmark is found from the Heart. Clients get `AurelithS2C`
      (0x7D: city state within 640 blocks, forget, bursts) into `client/world/AurelithState`,
      which every renderer and the sound host read.
    - *Commands* (`/aurelith`, op): `status`; `tp <heart|podium|soprano|alto|tenor|bass|vault|
      gate_soprano|gate_alto|gate_tenor|gate_bass>` (nearest city); `keys` (the four keys);
      `sing` (seat the keys in order); `advance` (skip to the next stage: the Unsung, then
      its defeat); `reset` (dormant again, lights dimmed, sockets emptied); `heldnote`.

**Who lives in them** (template entities and spawners written by the generators; placed once,
when the chunk is generated, and saved with the world from then on; pool pieces run
`finalizeSpawn(STRUCTURE)` on them as vanilla villages do):
- *Echo Vaults*: the reliquary room's chest stands on an **echo-wraith spawner** — two
  wraiths a cycle, at most three about, 300–900 ticks between cycles, awake only with a
  player within 12 blocks. Break it, or light the room and fight past it.
- *Whisper Shrines*: two lumen moths circling the lantern.
- *Hush Lighthouse*: an echo mimic lurking at the foot of the dark shaft, facing the
  ladder — the keeper's lantern below the planks weakens it but does not burn it.
- *Sunken Library*: a Hush leviathan drifting some twenty blocks over the water.
- *Warden's Tomb*: nothing new — the Silent Warden still rises only from the echo core —
  and its `spawn_overrides` keep MONSTER spawns out of the tomb's pieces, so the arena
  belongs to the Warden alone.
- *Choir Hall*: unchanged (the Choir Mother is summoned by the puzzle).

All structure templates come from `tools/gen_hush_structures.py` (`--check-blocks` cross-
checks every palette block against `Blocks.cpp`; an unregistered palette block aborts
structure generation, so the check must pass before shipping). The Echo Vaults also appear
in the Hollow Deep and Aurora Steppe, the Whisper Shrines on the Aurora Steppe.

**How often** (measured offline 2026-09-22: every placement cell's candidate chunk in a
16k-block window, 6 seeds, the surface height from the library router, the biome check the
structure itself runs; "cell" = spacing², "built" = the share of cells whose candidate passes
the biome and site checks; salts are all unique):

| Structure | Spacing / separation (chunks) | Biomes | Built | One per | Nearest neighbour |
|---|---|---|---|---|---|
| Whisper Shrine | 20 / 6 | meadows, forest, barrens, steppe | 59 % | 0.17 km² | ~260 blocks |
| Echo Vaults | 24 / 8 | all land | 70 % | 0.21 km² | ~300 |
| Sunken Library | 24 / 8 | sunken choir | 29 % | 0.50 km² | ~340 |
| Hush Lighthouse | 44 / 16 | every surface biome | 99.5 % | 0.50 km² | ~530 |
| Choir Hall | 40 / 16 | steppe, meadows | 34 % | 1.2 km² | ~630 |
| Warden's Tomb | 36 / 12 | barrens, hollow deep | 21 % | 1.6 km² | ~680 |
| Aurelith | 90 / 32 (+ level site) | meadows, steppe, barrens | 9.8 % | 21 km² | ~4.7 km |

The buried two (Echo Vaults, start 12 under the surface; Warden's Tomb, 15 under) check
their biome at the surface: `"biome_at_surface": true` in their structure JSON (engine
extension, `StructureInfo::jigsawBiomeAtSurface`). MC checks the biome at the jigsaw start,
and a dozen blocks under the Hush's surface the depth axis is already in the Crystal Caverns
band on about half the land (depth 0.2..0.9; the surface is ~0, 1/128 per block down), so the
tomb was refused in 65 % of its cells for sitting in the caverns and built in only 5 % (one
per 6.4 km², neighbours ~1.3 km apart), and the vaults lost a third of theirs (42 % built).

**Finding them.** `/locate structure <id>` and `/locate biome <id>` search the dimension you
are in (the Hush's own structure sets and biome source), so run them in the Hush. Ids are
`minecraft:` ones (`minecraft:echo_vaults`, `minecraft:hush_meadows`); the bare name works too,
and tab completion lists every loaded structure, biome and tag. Asked from the wrong
dimension, the "not found" line is followed by where it does generate. The green coordinates
teleport you when clicked: onto the ground above a structure (the Echo Vaults and the Warden's
Tomb are buried below it), and for the Crystal Caverns onto a cave floor inside the biome.

### Atmosphere

Dark-teal sky disc, teal fog, cyan-tinted stars at a fixed angle, no sun, moon or clouds;
terrain shaded at the Overworld's midnight level. Teal motes drift in the air of every
Hush biome (densest underground); the portal sheds teal particles.

- **Auroras** — five slow cyan-to-violet curtain bands over the stars, always faint,
  strong over the Aurora Steppe (blended over a few seconds as you cross into it).
  `AuroraRenderer`, drawn with the sky shader; strength from `HushAtmosphere`.
- **Cavern fog** — teal, much shorter fog in the Crystal Caverns and the Hollow Deep, and
  a weaker version anywhere deep under cover.
- **Water** — underwater fog pulled toward a luminous teal; brightest in the Sunken Choir.
- **The stillness** — every 6–12 minutes, for 8–15 s, every Hush mob but the bosses stops
  dead (AI, navigation and controls paused, momentum dropped); the fog closes in, the
  auroras dim and the sky flattens, then it all lifts. Server-owned per Hush level
  (`HushStillness`, not saved), synced by `HushStillnessS2C`; `/stillness [<seconds> |
  stop | query]` triggers it.

## Recipes

- 4 hushstone → 4 polished; 4 polished → 4 bricks; bricks smelt to cracked; 2 brick slabs
  → chiseled; stairs 6 → 4 and slabs 3 → 6 for each stone.
- whisperwood log → 4 planks; planks → stairs, slab, fence gate, door, trapdoor as oak.
- raw resonite smelts (or blasts) to a resonite ingot; 9 ingots ↔ block of resonite.
- Resonite tools follow the diamond recipes with resonite ingots.
- Echo lantern: resonant crystal in the middle, echo shards on the four sides.
- Echo blade: a resonant heart flanked by two resonant crystals, over a resonite ingot,
  over a stick.
- Tuning fork: `I I` / ` E ` / ` I ` (resonite ingot, echo shard).
- Echo compass: a compass ringed by four echo shards.
- Cloak of silence: `P P` / `MPM` / `MMM` (phantom membrane, hush moss).
- Resonance bow: ` SE` / `R E` / ` SE` (stick, resonite ingot, echo shards as the string).
- Recall chime: ` S ` / `IEI` / `ICI` (stick, resonite ingot, echo shard, resonant crystal).
- Echo heart: `CCC` / `CHC` / `EEE` (resonant crystal, choir heart, echo shard) — the beacon's shape.
- Dyes: resonance bloom → cyan dye; resonant cluster → light blue dye.
- Choirstone: 8 hushstone round an echo shard → 8; polished, bricks, tiles (2×2 each),
  pillar (2 polished stacked), chiseled (2 polished slabs), cracked (smelt the bricks),
  stairs / slabs / wall as stone, and stonecutting between the forms.
- Stave stone: 4 polished choirstone round a glow ink sac → 4. Nightglass: 8 glass round a
  resonite ingot → 8. Resonite grate: 4 ingots in a ring → 4 (or stonecut a block).
- Lumen panels: ` G ` / `GCG` / ` D ` (glass, resonant crystal, cyan / purple / orange dye)
  → 4. Lumen strip: `RCR` → 6. Crystal conduit: resonite / crystal / resonite → 4.
  Choir lamp: ` I ` / `NCN` / ` I ` → 2.
- Voice beacon: `NNN` / `NHN` / `RRR` (nightglass, resonant heart, resonite ingot).

## Deliberate limits

- No item cooldown system: the fork's and chime's cooldowns are server-side
  per player (a refused use says so on the action bar); the hotbar shows no
  cooldown sweep. The client cannot see the gate or the cooldown, so a refused
  chime still plays its hold pose locally (nothing else happens).
- No game-event / vibration system and no simulated sculk sensors or
  shriekers: the cloak hides its wearer from the listeners that exist (the
  warden's sniff, the wraith's hunt); "moves without vibrations" holds
  trivially and is the hook to honour when vibrations land.
- The echo heart's light column is particles, not a beam.
- The lighthouse beams light nothing (no block light engine) and are drawn before the
  translucent terrain pass, so glass or water BEHIND a beam blends over it where they
  overlap.

- Sound: the engine now has MC's sound system (src/client/sound/, src/common/sound/). Hush
  events with no vanilla file map onto vanilla sounds through sounds.json overlays
  (assets/sound_overlays/obeycraft/), so they are re-pitched vanilla, not original audio.
- No block light engine: glowing blocks are full-bright but do not light their
  surroundings.
- Tools and armor carry no enchantments (no ENCHANTMENTS component); enchanted books
  from loot do.
- Armor is not rendered on player bodies, so the Hush ships no armor set.
