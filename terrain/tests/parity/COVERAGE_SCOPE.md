# What "100% parity" means (and does not mean)

Last updated: 2026-08-21.

## Verified scope

The parity claim, backed by `run_regression.sh` (128-golden net across three
dimensions), `run_acceptance.sh` (5 seeds × 3 r10 regions), quadrant regions,
`run_spot_check.sh` (fresh random seeds), and per-dimension fresh-Java
structure gates:

- **Dimensions**: Overworld, Nether, and End.
- **Phases 0-7**: STRUCTURE_STARTS, STRUCTURE_REFERENCES, BIOMES, NOISE,
  SURFACE, CARVERS, FEATURES — with `generateStructures=true` (0-7 tags) and
  the structureless 3-7 configuration both locked by goldens.
- **Structures**: all 34 overworld structures plus fortress, bastion_remnant,
  nether_fossil, ruined_portal_nether, and end_city — placement
  (`--trace-placements`), layout (S/P/R record kinds at 0-2), Beardifier
  terrain adaptation, full block placement, and block entities.
- **Block entities** (`E` record kind, canonical NBT per `FORMAT.md`):
  chest LootTable + drawn LootTableSeed (village/shipwreck/fortress
  nether_bridge/bastion/ruined_portal/end_city_treasure and the rest),
  mob spawners (incl. blaze), brushable blocks, decorated pots.
- **Granularity** (`FORMAT.md`): every non-air block with full sorted property
  list; all biome quarts per chunk; WORLD_SURFACE_WG + OCEAN_FLOOR_WG
  heightmaps; S/P/R structure records; E block-entity records.
- **Determinism**: C++ double-generation byte-identity on every regression
  combo. Spawn pregeneration is disabled on both sides (vanilla itself is
  nondeterministic there — parallel FEATURES steps of adjacent chunks race).
- **Coverage**: every overworld biome appears in at least one golden region
  (`coverage_report.py` gates this); all 5 nether biomes and all 5 end
  biome regions (main island, highlands, midlands, small islands, barrens)
  are exercised by dimension goldens, each biome's configured-feature list
  RNG-for-RNG.

## NOT covered (opt-in future track)

| Gap | Status | Rough effort to close |
|---|---|---|
| **Entities** | Witch/cat/villager/shulker spawns, item frames (elytra), minecart chests: entity creation draws no worldgen RNG on the compared streams; block parity unaffected. Not dumped. | Days-weeks: entity NBT dump + comparator. |
| **Lighting / spawn / full** (phases 8-11) | Never run on either side. | Days-weeks: dump format + comparator extension, light-engine parity. |
| **Live heightmaps** (WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING*) | Only the frozen `_WG` variants compared. | Days: dump + comparator extension. |
| **Fluid/block tick lists, POI** | Not dumped (template fluid ticks are scheduled but dump-invisible at phase ≤7). | Days. |
| **Unit-level parity tests** (RNG/noise/density groups) | Region compares are the effective net. | Optional. |

## Regression workflow (for any code change)

1. `./run_regression.sh --tier 0` — seconds, inner loop (22 goldens incl.
   dimension + structure combos).
2. `./run_regression.sh --tier 1` — minutes (87 goldens).
3. `./run_regression.sh --tier all --perf` — full gate: 128 goldens incl.
   19 r10 regions, determinism, perf tolerance vs `perf/baseline.json`.
4. After a whole change category: `./run_spot_check.sh --count 2 --phases 0-7`
   (fresh Java, new random seeds) plus per-dimension fresh gates via
   `run_full_parity.sh --dimension nether|end` on fresh seeds.
5. Final: `./run_acceptance.sh --fresh-java` (held-out, multi-hour).

Goldens are frozen Java output (jar sha in `goldens/MANIFEST.json`); they are
sufficient for validating C++-only changes. Fresh-Java runs (spot checks,
`--fresh-java` acceptance, the Part D structure gates) guard against the
golden set itself being stale or lucky. Structure gate regions are archived
in `targets/REGIONS.md`.

## World types (2026-08-21)

The four MC world presets are implemented and gated at byte parity vs fresh
Java (seed 12345, r2 @ origin; goldens carry `_wt<type>[-<preset|biome>]`
tags):

- **flat**: default settings 3-7; classic_flat and overworld presets 0-7
  (villages, strongholds, ruined portals, lakes + full biome decoration on
  the overworld preset); a custom "<layers>;<biome>" string (cherry_grove)
  3-7. Custom-layer goldens are NOT promotable (irreversible `-x<hash>` tag)
  — gate them ad hoc with `--flat-layers`.
- **amplified** and **large_biomes**: 3-7 and 0-7 (router-only variants of
  the overworld — `NoiseRouterData::overworld(largeBiomes, amplified)`).
- **single_biome_surface**: plains 3-7, desert 0-7 (desert pyramids etc. on
  a FixedBiomeSource world).

Harness flags: `--world-type`, `--flat-preset`, `--flat-layers`,
`--single-biome` (both harnesses + run_full_parity.sh). The bugs these gates
caught (anchor literalism, SlabBlock.canPlaceLiquid, level-vs-generator
height accessors) are recorded in the project memory; the rule they teach:
transcribe VerticalAnchor kinds literally and check every heightAccessor-vs-
chunkGenerator call site against the decompiled source.
