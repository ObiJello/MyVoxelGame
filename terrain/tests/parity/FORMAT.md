# Canonical Parity Dump Format

Both the Java harness (`MinecraftAsyncChunkTest`) and the C++ harness
(`async_chunk_test` / `CppChunkGeneratorTest.cpp`) emit this format
byte-identically. `compare_parity.py` accepts only this format.

## Layout

```
# <header lines - ignored by the comparator>
C,<chunkX>,<chunkZ>
S,<structure_id>,<refs>,<minX>,<minY>,<minZ>,<maxX>,<maxY>,<maxZ>
P,<structure_id>,<piece_type_id>,<rotation>,<genDepth>,<minX>,<minY>,<minZ>,<maxX>,<maxY>,<maxZ>,<detail>
...
R,<structure_id>,<cx>;<cz>[,<cx>;<cz>...]
...
B,<x>,<y>,<z>,<full_state>
...
Q,<qx>,<qy>,<qz>,<biome_name>
...
H,<type>,<x>,<z>,<height>
...
C,<next chunkX>,<next chunkZ>
...
```

## Section presence by phase spec

Which record kinds appear depends on the run's phase spec (both sides must
agree; the rules are part of this contract):

- `S`/`P`: emitted iff structures are enabled (phase specs starting at 0, and
  `all`).
- `R`: emitted iff structures are enabled AND the target status is
  STRUCTURE_REFERENCES or later (i.e. not for `0-1`).
- `B`/`Q`: emitted iff the target status is BIOMES or later.
- `H`: emitted iff the target status is NOISE or later (heightmaps are a
  NOISE-phase product; before that they are unprimed).
- `E`: emitted iff `--dump-block-entities` is passed AND the target status is
  FEATURES or later.

Structures-off specs (`3-*`) therefore produce exactly the historical format —
pre-existing goldens remain valid byte-for-byte.

## Rules

- **`C` section markers**: one per chunk. Chunks are ordered by `(chunkX, chunkZ)`
  lexicographic ascending (matches C++ `std::map<std::pair<int,int>>` order).
- **`S` structure-start lines**: one per structure start stored in THIS chunk
  (`ChunkAccess.getAllStarts()`; only valid starts are ever stored), sorted by
  `structure_id` (bytewise ascending). `refs` is `StructureStart.getReferences()`
  (incremented by neighbor chunks' STRUCTURE_REFERENCES step — deterministic for
  a fixed run configuration, but dependent on the run's mode/radius; compare
  like runs with like, exactly as for decoration). Bounding box is
  `StructureStart.getBoundingBox()` = the piece-encapsulating box passed
  through `Structure.adjustBoundingBox()` (inflated by 12 on every axis when
  terrain_adaptation != none), inclusive block coordinates.
- **`P` piece lines**: immediately follow their `S` line, one per piece **in
  piece-list order** (the build order; `StructureStart.placeInChunk` iterates
  this order, so it is parity-load-bearing — the comparator must not re-sort).
  - `piece_type_id`: registry id of `StructurePiece.getType()` from
    `BuiltInRegistries.STRUCTURE_PIECE` (e.g. `minecraft:mscorridor`,
    `minecraft:jigsaw`).
  - `rotation`: `StructurePiece.getRotation()` enum constant name
    (`NONE`, `CLOCKWISE_90`, `CLOCKWISE_180`, `COUNTERCLOCKWISE_90`) or `-`
    when null.
  - `genDepth`: `StructurePiece.getGenDepth()`.
  - Bounding box: the piece box, inclusive block coordinates.
  - `detail` (last field; may contain any characters except comma/newline):
    - `TemplateStructurePiece`: the template name string (e.g. `igloo/top`).
    - `PoolElementStructurePiece`:
      `<element_type_id>:<template_id_or_->#<groundLevelDelta>` followed by
      `#J:` and a `|`-separated junction list `x;sourceGroundY;z;deltaY` in
      junction-list order when the piece has junctions.
      `template_id_or_-` is the pool element's template location for
      single/legacy elements, `-` otherwise.
    - all other pieces: `-`.
- **`R` structure-reference lines**: one per structure with references INTO
  this chunk (`ChunkAccess.getAllReferences()`, non-empty sets only), sorted by
  `structure_id`. The value list is the referencing start chunks as `cx;cz`
  pairs sorted numerically by `(cx, cz)`.
- **`B` block lines**: local `x`,`z` in `0..15`, absolute `y` in `-64..319`.
  Iteration order: `y` outer, then `z`, then `x` (all ascending).
  Plain `minecraft:air` is **omitted** (implicit); `minecraft:cave_air` and
  `minecraft:void_air` **are emitted** (carver parity depends on cave_air).
  The comparator treats an absent position as `minecraft:air`.
- **`<full_state>`**: registry name, then if the state has properties a
  `[name=value,...]` suffix with properties **sorted alphabetically by property
  name**, values in their serialized (lowercase) form. Examples:
  - `minecraft:stone`
  - `minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]`
  - Ground truth: Java `StateDefinition` stores properties in an
    `ImmutableSortedMap`, so `StateHolder.toString()` is already alphabetical.
- **`Q` biome lines**: absolute quart coordinates (block >> 2). Full column:
  `qx` in `chunkQuartX..+3`, `qy` in `-16..79`, `qz` in `chunkQuartZ..+3`.
  Iteration order: `qx` outer, then `qy`, then `qz` (matches the historical
  Java single-mode dump). Name is the registry name (`minecraft:plains`).
- **`H` heightmap lines**: `type` is `WS` (WORLD_SURFACE_WG) or `OF`
  (OCEAN_FLOOR_WG). Local `x`,`z` in `0..15`, iteration `type` outer, then
  `z`, then `x`. `height` is `ChunkAccess.getHeight()` semantics (highest
  set block Y, `minY-1` when column empty).
- **`E` block-entity lines** (`E,<x>,<y>,<z>,<canonical_nbt>`): local `x`,`z` in
  `0..15`, absolute `y`. One per block entity in the chunk
  (`ChunkAccess.getBlockEntityNbtForSaving`), ordered `y` outer, then `z`,
  then `x` (all ascending — same as `B`). Emission is gated behind
  `--dump-block-entities` on both harnesses (BOTH emit as of B8, 2026-08-13 —
  all 14 structure gate regions verified 0-diff including E), and requires
  target status FEATURES or later. Worldgen-pending block entities (a
  BE-capable block placed without an explicit block entity, e.g. sculk
  sensors placed by the spreader) serialize as `{id:"DUMMY"}`, exactly like
  Java's pending tags.

  `canonical_nbt` is a single-line deterministic serialization defined HERE
  (a spec implemented identically twice — NOT "whatever SNBT prints"):
  - Compound: `{key:value,...}`, keys sorted bytewise ascending. A key is
    emitted raw if it matches `[A-Za-z0-9_.+-]+`, else double-quoted with `\\`
    and `\"` escapes. The top-level compound drops keys `x`, `y`, `z`
    (redundant with the line position); everything else, including `id`, stays.
  - Numerics carry type suffixes: byte `<n>b`, short `<n>s`, int `<n>`
    (no suffix), long `<n>l` — decimal, `-` for negatives, no leading zeros.
  - **Float/double are IEEE-754 bit patterns in lowercase hex**: float
    `f0x<8 hex digits>`, double `d0x<16 hex digits>` (zero-padded). Textual
    shortest-round-trip formatting differs between Java and C++ standard
    libraries; bit patterns cannot.
  - String: double-quoted, escaping only `\\` and `\"`; raw UTF-8 otherwise.
  - List: `[v1,v2,...]`; empty list `[]`.
  - Arrays: `[B;1b,2b,...]`, `[I;1,2,...]`, `[L;1l,2l,...]` (empty: `[B;]`).
- Lines starting with `#` and blank lines are ignored by the comparator.

## Comparator

`compare_parity.py <java_dump> <cpp_dump>`:
- exit 0: identical; exit 1: mismatches; exit 2: structural error
  (missing chunk sections, unparseable line, chunk-set mismatch).
- Mismatch classes per block type: `missing` (Java has, C++ air),
  `extra` (C++ has, Java air), `wrong-block`, `wrong-properties`
  (same block, different property string); plus `biome`, `heightmap`,
  `structure-layout` (S/P lines compared as an ordered sequence — order
  divergence is a real failure), `structure-refs`, `block-entity`.
- `--first N`: print the first N mismatch coordinates per (chunk, class).
- `--summary-json <file>`: machine-readable results for acceptance gates.
