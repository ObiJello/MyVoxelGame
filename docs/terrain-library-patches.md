# Terrain Library Patches

When replacing `src/my_terrain_library/` with a newer version, re-apply these patches.

## Patch 1: Abort flag for clean shutdown

**Problem:** `ServerChunkCache::getChunk()` has two blocking paths (main thread `while` loop and worker thread `future->join()`) that prevent threads from exiting during shutdown, causing the game to hang on close.

**Files to modify:**

### `include/server/level/ServerChunkCache.h`

Add to the **public** section (after `setTaskPoller`):

```cpp
void requestAbort() { m_abort.store(true, std::memory_order_release); }
bool isAbortRequested() const { return m_abort.load(std::memory_order_acquire); }
```

Add to the **private** section (after `m_taskPoller`):

```cpp
std::atomic<bool> m_abort{false};
```

### `src/server/level/ServerChunkCache.cpp`

**Worker thread path** — replace the `future->join()` call with an abort-aware poll:

```cpp
// If not on main thread, dispatch and wait
if (std::this_thread::get_id() != m_mainThreadId) {
    if (m_abort.load(std::memory_order_acquire)) return nullptr;

    auto future = getChunkFuture(x, z, targetStatus, loadOrGenerate);

    // Poll with abort check instead of blocking join()
    while (!future->isDone()) {
        if (m_abort.load(std::memory_order_acquire)) return nullptr;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    auto result = future->getNow(nullptr);
    return result ? result->orElse(nullptr) : nullptr;
}
```

**Main thread path** — add abort check at top of the `while (!future->isDone())` loop:

```cpp
while (!future->isDone()) {
    if (m_abort.load(std::memory_order_acquire)) {
        return nullptr;
    }
    // ... existing code (runDistanceManagerUpdates, taskPoller, yield) ...
}
```

## Patch 2: Tracy profiling zones (TerrainProfiling.h)

**Problem:** chunk generation is the program's dominant cost but runs on the
library's BackgroundExecutor pool, which has no game-side zones — without
these patches it is invisible in a Tracy trace. (This patch was lost once, on
the 2026-08-21 library replacement, because it was never written down: the
build then failed on the missing `util/TerrainProfiling.h` include. Keep this
section current.)

**Files:**

### `include/util/TerrainProfiling.h` (GAME-LOCAL FILE — not in the canonical library)

`rsync --delete` removes it on every replacement. Restore with:

```bash
git checkout HEAD -- src/my_terrain_library/include/util/TerrainProfiling.h
```

Defines `TERRAIN_ZONE_N` / `TERRAIN_THREAD` / `TERRAIN_PLOT` — TracyClient-only
(never include the game's Profiling_Tracy.hpp here: both trees have a
top-level `server/`, so the game's include path could shadow library headers).
Compiles to nothing without TRACY_ENABLE. `TERRAIN_THREAD` is used by the
game's own MyTerrainGenerator.hpp (worker thread naming), so the header must
exist even if the zone patches below are skipped.

### `include/world/chunk/status/ChunkStatusTasks.h`

Add `#include "util/TerrainProfiling.h"` to the include block, then one zone
per stage task:
- `TERRAIN_ZONE_N("Gen.Biomes")` — in generateBiomes, BOTH paths: inside the
  sync fallback branch AND inside the supplyAsync lambda (a zone around the
  supplyAsync call would time the dispatch, not the generation).
- `TERRAIN_ZONE_N("Gen.Noise")` — generateNoise, both paths likewise.
- `TERRAIN_ZONE_N("Gen.Surface" / "Gen.Carvers" / "Gen.Features" /
  "Gen.Spawn" / "Gen.Full")` — first line of the corresponding task bodies.

### `src/levelgen/ChunkGenerator.cpp`

Include, plus in doCreateBiomes: `TERRAIN_ZONE_N("Biomes.NoiseChunkCreate")`
inside the block wrapping getOrCreateNoiseChunk (NoiseChunk construction is
charged here — it is more expensive than the noise itself), and
`TERRAIN_ZONE_N("Biomes.Fill")` inside the block wrapping fillBiomesFromNoise.

### `src/levelgen/NoiseChunk.cpp`

Include, plus construction sub-zones: `NC.Arena` (scoped block around the
m_wrapArena resize), `NC.WrapRouter` (inside the wrappedRouter lambda),
`NC.Aquifer` (inside the aquifer creation block), `NC.FullNoise` (inside the
fullNoiseValue lambda), and after the wrap: `TERRAIN_PLOT("Wrap/Distinct" /
"Wrap/OwnedDensity" / "Wrap/OwnedOther", ...)`. (The old "Wrap/Visits" plot
needed a game-local m_wrapVisits counter in wrap(); dropped 2026-08-21.)

## Structural wrap dedupe (2026-08-29) — a behavioural patch, not a zone

Java's `NoiseChunk.wrapped` is a `HashMap<DensityFunction, DensityFunction>`
keyed by RECORDS, so structurally identical subtrees share one wrapper
(FlatCache / Cache2D / NoiseInterpolator) per chunk. The port keyed by
pointer and `mapAll` always creates fresh nodes, so every NoiseChunk expanded
the router into ~7,000 nodes with 5,095 wrapper entries, all misses, each
duplicated FlatCache eagerly filling its column grid. Measured: 11.2 ms of
the 15.3 ms biome step per chunk.

- `include/levelgen/DensityFunction.h`: `mapAll` is now a non-virtual wrapper
  around `mapAllImpl` (memo hooks on `Visitor`: `lookupMapped` /
  `rememberMapped`), plus `structuralKey(std::string&)` with `keyTag/keyPtr/
  keyDouble/keyFloat/keyInt` helpers. Default key = identity.
- `include/levelgen/DensityFunctions.h`, `include/synth/BlendedNoise.h`:
  every record-like node overrides `structuralKey` (type tag + value fields +
  CHILD POINTERS — children are deduplicated before the parent is wrapped, so
  pointer identity of children is Java's recursive record equality). Splines
  key recursively over locations/derivatives/values/coordinate function.
  Singletons (BlendAlpha/Offset, EndIsland, Beardifier, NoiseChunk's own
  wrappers) keep identity.
- `include/levelgen/NoiseChunk.h`: `wrap()` consults `m_structural` (key ->
  wrapper) after the pointer map; `m_mapMemo` (original -> mapped, and
  mapped -> mapped identity) shared by every WrapVisitor of the chunk.
  `OBEY_NO_STRUCT_DEDUPE=1` switches the structural map off (A/B).
- `src/levelgen/NoiseChunk.cpp`: destructor only destructs ARENA wrappers and
  each exactly once (several keys now share a wrapper; a plain node mapped to
  an equal plain node is owned by m_ownedMappedDensityNodes and must not be
  destructed twice — that was a startup SIGABRT). Plots `Wrap/Structural`,
  `Wrap/Interpolators`.
- `include/world/chunk/status/ChunkStatusTasks.h`: `debugGenHash` —
  `OBEY_GEN_HASH=1` prints `[GenHash] x z hash` of every block state at the
  END OF CARVERS (last step that writes only the centre chunk; decoration is
  order-dependent between neighbours so region files are NOT an oracle).

Verified: dedupe off vs on, 4,263 common chunks, 0 differing; off-vs-off
control also 0. Result: ~133 distinct wrapped structures per chunk instead
of 5,095; Gen.Biomes 15.3 -> 3.75 ms, Gen.Noise 11.8 -> 9.5 ms; fresh
generation 97 -> 138 chunks/s at spawn, 81 -> 123 chunks/s on a far teleport
(rd 32, M4, GL harness).

### Pre-allocation dedupe + parallel stronghold rings (2026-08-29, later)

- `DensityFunction::mapAll` (non-virtual) now computes the node's key with its
  children MAPPED (`keyImpl(out, &visitor)`, memoised) BEFORE calling
  `mapAllImpl`; a hit in the visitor's pre-map returns the existing mapped node
  with no construction. `structuralKey`/`mappedKey` both route through
  `keyImpl`; helpers `keyChild`/`keyNoise`. ONLY for visitors whose
  `memoises()` is true (NoiseChunk's WrapVisitor) — for a non-memoising
  visitor the key pass would re-walk children exponentially (RandomState's
  noise wiring hung world load before this guard).
  Per NoiseChunk: 7,035 -> 200 density nodes, 2,942 -> 551 other objects,
  5,095 -> 93 wrap entries; NC.WrapRouter 2.0 -> 1.4 ms, NC.Aquifer 0.9 -> 0.5,
  getBaseHeight 3.1 -> 1.9 ms (village_plains start 871 -> 488 ms).
- `ChunkGeneratorStructureState::startRingGeneration(executor)`: Java-style
  parallel ring positions (ChunkGeneratorStructureState.java:126-138). Serial
  candidate pre-pass keeps the LegacyRandomSource stream bit-exact (each
  search gets `random.fork()`), searches fan out on the terrain pool,
  `getRingPositionsFor` joins on first use. MyTerrainGenerator calls it right
  after creating the state. `OBEY_SERIAL_RINGS=1` = old path; with
  `OBEY_STRUCT_LOG=1` a `[RingPositions] hash=` line verifies parity
  (identical serial vs parallel). Lane time for rings: 2.17 s -> 0.
- `OBEY_STRUCT_LOG=1` also logs `[StructTry] <structure> (x,z) ms` for every
  structure start over 30 ms; zones `Struct.Try`, `Struct.EnsureRings`,
  `Tmpl.Load*`, `NBCG.BaseHeight`, `Noise.FillSlice`.
Verified: end-of-carvers hash off vs on, 4,263 common, 0 differing.
Result: spawn view (3,725 chunks) fully generated by t=25 s (was t=30).
- `src/levelgen/RandomState.cpp` NoiseWiringVisitor memoises (2026-08-30):
  the wired router is built ONCE as a shared DAG (Java's Holder sharing), so
  every NoiseChunk walks ~100 original nodes instead of ~7,000.
  NC.WrapRouter 1.3 -> 0.23 ms, Gen.Biomes 2.97 -> 1.77 ms, getBaseHeight
  1.75 -> 0.93 ms, village_plains start 482 -> 278 ms; spawn view (3,725)
  generated by t~21 s. Hash oracle off vs on: 4,267 common, 0 differing.

### Decoration / lane constant factors (2026-08-30)
- `WorldGenRegion::getChunk`: per-region cache of resolved ChunkAccess*
  (holder futures mutex + 2 shared_ptr copies per block access before).
- `WorldGenRegionLevel::getBiome`: RegionBiomeSource + BiomeManager built once
  per region (were built per call). `BiomeFeatureRegistry::hasFeature`: O(1)
  per-biome set. `applyBiomeDecoration`: neighbour biomes deduped by pointer
  before the name lookup (was per biome entry per section).
- `TagMatchTest`: last-Block memo (atomic). `BlockState::hasBlockEntity` and
  `isFaceSturdy(direction)`: cached per state (atomics). Tree feature: block
  class flags memoised per Block (dynamic_cast per block before);
  mangrove propagule by Block pointer. Ore loop: build-height bounds hoisted.
  Aquifer: LAVA/WATER default states hoisted.
- Lane scheduling: `GenerationChunkHolder::m_emptyChunk` mirrors the EMPTY
  chunk lock-free for getPersistedStatus(); `scheduleChunkInLayer` returns
  early for holders already at/past the layer's status (future is done and
  successful by construction). Overhead 0.94 -> 0.65 ms/chunk.
Result: Gen.Features 4.3 -> 3.3 ms (far) / 3.0 -> 2.3 ms (spawn); far
teleport view (3,725) complete 21-22 s after the teleport (was >30 s at the
start of 2026-08-29); spawn view by t=19.
Client (`ClientChunkManager::ScheduleMeshBuildsWithSnapshots`): eligibility
pre-pass over the dirty set + 3-frame idle backoff (reset by any dirty
event): 10% -> 3% of the main thread in a loaded view, +12 fps there.
- 2026-08-30 (later): `BulkSectionAccess` caches min/max section Y (two
  virtual calls per candidate block in ore placement before). Noise fill
  profiled after all of the above: ImprovedNoise (Perlin) ~33%, aquifer
  body 15%, interpolation 12% — vanilla's own algorithms, no port waste left.
