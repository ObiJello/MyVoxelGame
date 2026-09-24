# Terrain library persistence

The terrain library (`src/my_terrain_library/`) saves and reloads partly
generated chunks the way Minecraft does: every chunk it unloads is written
first, and a later request reads it back with everything it had. A world
therefore grows without seams across unloads and server restarts, and the
library's memory stays flat however far players explore.

## How Minecraft does it

`ChunkMap.processUnloads` waits for a holder's save-sync future (every
generation task that touched the chunk has finished), then
`scheduleUnload -> save(chunk)` writes it — a proto chunk at any status as
well as a finished one — and drops the holder. `SerializableChunkData`
carries blocks, biomes, heightmaps, status, structure starts and references,
pending block entities, generated entities and post-processing positions.
Loading goes through the loading pyramid, whose `STRUCTURE_STARTS` step
(`loadStructureStarts`) re-registers the saved starts.

## What the port does

### Who writes what

- **Proto chunks** (status before FULL) are the library's. `ChunkMap::save`
  writes them, on unload and at shutdown (`saveAllChunks(true)`), with MC's
  rules: only when unsaved (`tryMarkSaved`), never over a position whose saved
  data could not be read, never an EMPTY chunk without a valid start.
- **FULL chunks** are the game's. The game converts them and saves them in its
  own serializer; the library never writes one. It reads them back as
  neighbours whose status satisfies every dependency.
- **A FULL chunk's blocks are released once the game has it.** MC keeps one
  copy (the holder's LevelChunk is the world's); ours is converted, so the
  library's copy was a second one — about 47,000 of them, ~40 KB each, with
  50 players spread out. After a conversion the game kept, the chunk becomes
  a candidate (`MyTerrainGenerator::NoteHandedOff`), and
  `ChunkMap::releaseHandedOffChunks` (a slice per tick, from `TickLibrary`)
  calls `ProtoChunk::releaseBlockData` once it is FULL, no task holds it, the
  embedder is not reading it, and all 8 neighbours are past SPAWN — the last
  step that reads a neighbour's blocks (FEATURES, which writes them, is done
  for every neighbour of a FULL chunk). Status, structure starts and
  references stay: starts are read 8 chunks away. Sections become empty ones,
  so a read that should never happen sees air, and converting a released
  chunk is refused with an error. Only with storage: without it the game
  regenerates a chunk it dropped from this copy.
- **The server thread only copies.** As MC's `save` (`copyOf` on the main
  thread, `write` on the background executor), `SerializableChunkData::copyOf`
  takes each section's palette and packed ids as they are, the heightmaps,
  block-entity text and entities (~5 µs a chunk); `write()` does the
  first-seen repack, biome naming and block-entity parsing on the I/O thread.
  Packing in `copyOf` cost 0.37 ms a chunk on the server thread, and a forced
  unload batch of 2,500 made 1-4.7 s ticks.
- **Unloads are budgeted as MC's.** `processUnloads` unloads while the tick has
  time, and past that only the excess of holders READY to unload (no task
  refs, save-sync done — MC's `unloadQueue`) over 2,000, counted down per
  unload. Counting every pending holder forced far more per tick.
- **One writer per region file.** The library writes through the game's
  `AnvilChunkIo` (`Game::Anvil::LibraryChunkStorage`, a
  `ChunkStorageBackend`), under the same mutex as the game's saves and loads.
  A proto never replaces a FULL chunk: `AnvilChunkIo::WriteProtoChunkNbt`
  checks the saved Status under the lock, and a position with a game save
  queued is skipped. Reads see the game's pending evictions first.

### The chunk format (`SerializableChunkData`)

MC 26.3's layout, written in the game's DataVersion (4764): before 5006 a
block state is `{Name, Properties}` (26.3: `{id, properties}`), before 5013
TERRAIN is saved as `"minecraft:carvers"`. Reads accept both. Palettes are
packed exactly as `PalettedContainer.pack` does (first-seen order; blocks use
at least 4 bits on disk, biomes `ceillog2`), and palette entries resolve
through the block table, not the block registry's id map.

Engine extensions, which vanilla ignores:

- `obeycraft:structure_spawn_areas` — the structure spawn areas recorded at
  FEATURES (`IChunk::StructureSpawnArea`).
- `obeycraft:finalize_spawn` on a generated entity the engine still has to
  finalize.
- `structures.obeycraft:starts` — see below.

The generator schedules no ticks and makes no UpgradeData, blending or
below-zero retrogen data, so those fields are never written.

### Structure starts

Each start is saved with MC's layout — `{id, ChunkX, ChunkZ, references,
Children:[{id, BB, O, GD, ...piece state}]}` — but under
`structures.obeycraft:starts`, with MC's own `starts` left empty. This port's
pieces are not MC's 56 piece classes and cannot carry every field vanilla's
piece loaders require, so vanilla must not try to parse them.

Loading does what MC does for ocean monuments
(`OceanMonumentStructure.regeneratePiecesAfterLoad`) for every structure:
`loadStructureStarts` regenerates the starts from the seed (they depend only
on seed, position and biome source), then `StructureSerialization::
restoreStarts` hands back what the seed cannot reproduce:

- the reference count;
- each piece's bounding box (pieces that settle onto the terrain move it);
- each piece's placement state, through `StructurePieceBehavior::saveState /
  loadState`, under MC's tag names: `HPos` (swamp hut, desert pyramid, jungle
  temple), `hasPlacedChest0..3`, `placedMainChest` / `placedHiddenChest` /
  `placedTrap1` / `placedTrap2`, shipwreck `TPX/TPY/TPZ` + `height_adjusted`,
  ocean ruin `TPX/TPY/TPZ`, mineshaft corridor `hps`, fortress `Mob` and
  `Chest`, stronghold `Chest` and `Mob`, Aether large aercloud `Positions`.
  Every other behavior was audited as stateless.

A saved start the seed no longer produces, or whose piece list no longer
matches, is reported and left as regenerated (a world saved by an older
generator).

### Loading

Chunks read from disk go through the loading pyramid. `ChunkGenerationTask`
skips a layer only once the holder has *completed* that step
(`GenerationChunkHolder::hasCompletedStep`) — a chunk read at FEATURES has not
yet run its loading steps, `loadStructureStarts` among them.

### FULL chunks carry their starts too

MC saves a finished chunk's starts and references with it, and a start's
pieces can still be placing into neighbours after its own chunk is FULL. The
library encodes the chunk's `structures` compound when it reaches FULL
(`ChunkStatusTasks::full`, on the worldgen lane, so no piece placement runs
concurrently); `ConvertLibChunk` hands it to the game chunk
(`Chunk::structuresNbt`); the game's serializer writes it back verbatim and
keeps it from the file on load (`NbtScan::FindRootTag`). When the library
reads that FULL chunk as a neighbour, its starts are restored like a proto's.

### Worlds without storage

A read-only world (or `OBEY_NO_LIB_DISK=1`) has nowhere to save, so
`processUnloads` keeps the old rule there: FULL and pre-TERRAIN holders are
freed, protos from TERRAIN up to FULL stay resident because they may hold a
neighbour's decoration.

## Testing

The C++ parity harness has a restart round trip:

```bash
B=terrain/build/async_chunk_test
$B --radius 3 --center X Z --phases 0-5 --dump-full --dump-block-entities --output plain.txt
$B --radius 3 --center X Z --phases 0-5 --storage DIR --save-all --request-limit 20 --output a.txt
$B --radius 3 --center X Z --phases 0-5 --dump-full --dump-block-entities --storage DIR --output b.txt
python3 terrain/tests/parity/compare_parity.py plain.txt b.txt   # must be 0
```

Run 1 generates the first N chunks of the raster order and saves every proto;
run 2 reopens the storage and requests all chunks in the same order. Run 2
must match an uninterrupted run block for block, block entities included.
Passing (2026-09-23) across shipwreck, jungle temple, igloo, warm ocean ruin,
plains village, mineshaft, stronghold, monument, ancient city, trial
chambers, outpost, forest camp, nether fortress, bastion and end city, at
several split points and with run 1 stopped at TERRAIN.
`OBEY_NO_START_RESTORE=1` on run 2 skips handing the saved piece state back
(A/B): an igloo split then differs by 41 blocks, with it by none.

## Known differences from Minecraft

- A start's pieces placing into a neighbour after the start's own chunk was
  saved change the start in memory only; MC has the same gap (placement does
  not mark the start's chunk unsaved).
- Vanilla opening a world mid-generation sees the protos but none of their
  pending starts (they are under `obeycraft:starts`).

## Related code

- `src/my_terrain_library/src/world/level/chunk/storage/SerializableChunkData.cpp`
- `src/my_terrain_library/src/levelgen/structure/StructureSerialization.cpp`
- `src/my_terrain_library/src/server/level/ChunkMap.cpp`: `save`, `saveAllChunks`, `processUnloads`, `scheduleChunkLoad`
- `src/my_terrain_library/include/world/chunk/status/ChunkStatusTasks.h`: `loadStructureStarts`
- `src/server/world/storage/anvil/LibraryChunkStorage.cpp`, `AnvilChunkStorage.cpp` (`WriteProtoChunkNbt`), `NbtScan.cpp`, `ChunkSerializer.cpp` (`structures`)
- `src/server/world/ChunkProvider.cpp` (wiring), `MyTerrainGenerator.cpp` (shutdown save)
- MC reference: `minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/level/chunk/storage/SerializableChunkData.java`, `server/level/ChunkMap.java`
