# Known structure-instance regions (seed 12345 unless noted)

Gate regions for the structure-parity batches. Each is a `--radius 2 --center
X Z --phases 0-2` paired run; compare with S/P/R filtered to the implemented
subset (see the FILTER awk in the session logs / run gates by hand).

B6 block gates re-run the same regions at `--phases 0-7` UNFILTERED (both
harnesses `--dump-full`, C++ adds `--partial-structures`). All 13 hardcoded +
template family-variants gated at 0-7 as of 2026-08-13 (buried_treasure,
swamp_hut, desert_pyramid, jungle_pyramid, mineshaft, mineshaft_mesa, igloo,
shipwreck, ocean_ruin_cold, ruined_portal, stronghold x2, monument, mansion).
ALWAYS `make clean` before gating - incremental builds miss header changes.

| structure | center chunk | notes |
|---|---|---|
| desert_pyramid | -238 -313 | also -224,-313 and -320,-304 |
| swamp_hut | -126 -106 | |
| jungle_pyramid | -147 65 | also -114,71 |
| buried_treasure | 47 25 | also 111,32; 116,-30; 9,186; 9,202 |
| mineshaft x2 | r8 @ 0 0 | archived: java_s12345_r8_c0_0_p0-2.txt.gz |
| mineshaft (seed 0) | r8 @ 0 0 | archived: java_s0_r8_c0_0_p0-2.txt.gz |
| mineshaft_mesa | -168 110 | badlands |
| stronghold | -105 124 | ring 0; 115 pieces |
| stronghold | 110 23 | ring 2; 163 pieces |
| monument | 15 -157 | deep ocean |
| igloo (lab, 6 pieces) | 20 72 | snowy |
| igloo (top only) | 36 41 | snowy |
| shipwreck (ocean) | -8 3 | |
| ocean_ruin_cold (small) | 7 4 | |
| ocean_ruin_warm (small) | -132 146 | warm ocean |
| ruined_portal (underground) | r8 @ 0 0 | chunk (1,0), in archived target |
| mansion (499 pieces) | -437 -674 | dark forest, far region |
| village_savanna (163 pieces, jigsaw) | -396 -354 | |
| pillager_outpost (7 pieces) | -394 -77 | exclusion zone live |
| ancient_city (84 pieces) | -784 -402 | city_anchor start jigsaw; empty-template quirk |
| trail_ruins | -799 -300 | |
| village_plains | -799 -471 | also -799,160; savanna -798,211; plains -797,-117 |
| trial_chambers (274 pieces) | 0 14 | aliases + uniform height; more PS candidates in r64 trace |
| ocean_ruin_warm LARGE+cluster | -117 160 | 42x35 bbox; drowned markers buried in stone |
| ocean_ruin_cold LARGE+cluster | 3 26 | 37x42 bbox |
| shipwreck_beached | 5 156 | beach biome; bbox (80,90,2496)-(88,98,2523) |
| igloo lab re-gate | 20 72 | 6 pieces, marker-clip verified |
| control (no structures) | 0 0 (r2) | zero spurious starts |

B7 COMPLETE (2026-08-13): ALL overworld structures at full 0-7 block parity
(jigsaw families incl. ancient_city/trail_ruins/trial_chambers + all B6
families + large/cluster ocean ruins + beached shipwreck). The C++ 0-* guard
is lifted (--partial-structures is a no-op). Key late fixes: batch-2 block
sturdiness rules (candle etc. - the ancient city sculk cloud), block_rot
rottable_blocks gate, jigsaw final_state partial-spec parsing (default+
overrides), data markers clipped to chunkBB (Java filterBlocks semantics),
ocean ruin "drowned" marker block writes (AIR above sea level, WATER below).

As of 2026-08-13 ALL overworld structures are implemented, so gates compare
UNFILTERED (no awk whitelist needed); the r8@0,0 archived target matches
unfiltered, including trial_chambers R-refs to (0,14).

TODO: gate an ocean_ruin LARGE+cluster instance (p=0.3*0.9; none found yet)
and a beached shipwreck (beach biome).

Find more instances: `./structure_search.sh --structure <id> --radius N
--center X Z`; biome-constrained ones via `build/biome_search --find <biome>`
first (desert/jungle/swamp/badlands/deep_ocean patches).

## Part C dimension gate regions (seed 12345, r2, phases 3-7 unless noted)

| Region | Center | Dimension | Covers |
|---|---|---|---|
| origin | 0 0 | nether | nether_wastes + crimson_forest (fungi, weeping vines, vegetation, springs, fire, glowstone, mushrooms, ores, debris) |
| warped forest | 40 -40 | nether | warped fungi, twisting vines, sprouts, warped vegetation |
| soul sand valley | -120 -120 | nether | basalt pillars, crimson roots patch, soul sand ore |
| basalt deltas | -40 25 | nether | delta, basalt columns, blobs, spring_delta, double springs, deltas ores; SURFACE basalt[axis=y] |
| nether biome census | -80 80 / 120 40 / 160 -80 | nether | crimson-rich alternates |
| origin (main island) | 0 0 | end | end spikes (obsidian pillars, cages, crystal bedrock+fire) |
| spawn platform | 6 0 | end | END_PLATFORM fixed at (100,49,0) |
| outer islands mixed | 80 0 | end | all 4 outer biomes incl. end_island_decorated |
| highlands | 100 30 | end | chorus plants + end_gateway_return |

Nether goldens: p3-6 r2 (origin/-40,25/s999@-10,1), p3-5 r4 origin, p3-7 r2
(all four regions above). Probe new regions fast: C++ `--phases 3-3 --dump-full`
+ Q-line census (seconds per candidate; biomes are at parity).

## Part D nether/end structure gate regions (phases 0-7, all PERFECT 2026-08-20/21)

| Region | Seed | Center (r) | Dimension | Covers |
|---|---|---|---|---|
| fortress | 12345 | -42 1 (r2) | nether | fortress pieces (bridges, crossings, stairs, rooms) |
| fortress + spawner | 12345 | -42 2 (r3) | nether | + blaze-spawner throne (E line, `_be` golden) |
| bastion | 12345 | -21 8 (r2) | nether | bastion_remnant jigsaw; E: bastion_bridge + bastion_other chests |
| portal + fossil | 12345 | -34 15 (r2) | nether | ruined_portal_nether (Blackstone processor) + nether_fossil (dried ghast); E: ruined_portal chest |
| end city | 12345 | 62 24 (r2) | end | end_city template graph; E: end_city_treasure chests ×2 |
| fresh fortress | -292632757340340250 | -17 6 (r3) | nether | held-out fortress + blaze spawner E, determinism |
| fresh fortress chests | -292632757340340250 | -21 9 (r2) | nether | nether_bridge chest E lines ×3 (corridor-turn needsChest pieces) |
| fresh end city | 1993066572193685600 | -77 4 (r2) | end | held-out end city + treasure chest E ×2, determinism |
| fresh nether 3-7 | 1993066572193685600 | -1 -36 (r2) | nether | held-out features spot |
| fresh end 3-7 | -292632757340340250 | -107 -66 (r2) | end | held-out outer-island features spot |

All promoted to goldens (`_be` tags carry E lines). Bastion S bbox for the
fresh nether seed also verified at chunk (-37,4) via C++ 0-1 probe.
Fortress chests are RARE: layout rolls needsChest on corridor-turn pieces;
seed 12345's fortress at -42,1 places none anywhere — scan C++-side with
`--dump-block-entities` + awk C/E-line join to find a chest window first.
