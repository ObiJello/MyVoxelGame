# Redstone

A port of Minecraft's redstone (26.3-pre-2 decompile, `minecraft_code_26.3-pre-2/`),
built on the same update machinery vanilla uses so that farms and contraptions
behave identically. Everything is server-side; the client only renders.

## Update machinery (the part that decides timing)

- `World::SetBlock(pos, state, flags, updateLimit)` is `Level.setBlock` layered
  over `LevelChunk.setBlockState`, step for step: write → old block entity's
  `PreRemoveSideEffects` → `affectNeighborsAfterRemoval` → `onPlace` (on EVERY
  write, the callback guards on block change where vanilla does) → new block
  entity → `sendBlockUpdated` (flag 2) → `updateNeighborsAt` +
  `updateNeighbourForOutputSignal` (flag 1) → the three shape walks (unless
  flag 16). `World::UpdateFlags` carries MC's `Block.UPDATE_*` values.
- `CollectingNeighborUpdater` (`common/world/level/NeighborUpdater`) is
  vanilla's queue: a fan-out is interrupted by the work its first neighbour
  books, and `UPDATE_ORDER` (west, east, down, up, north, south) is preserved.
  Both `neighborChanged` and `updateShape` go through it.
- `Block` hooks: `updateShape` (read-only, the old `neighborChanged`),
  `neighborChanged` (writable), `getSignal`/`getDirectSignal`,
  `isSignalSource`, `hasAnalogOutputSignal`/`getAnalogOutputSignal`,
  `triggerEvent`, `affectNeighborsAfterRemoval`,
  `updateIndirectNeighbourShapes`, `anyInside` (items), `onProjectileHit`.
- `RedstoneSignal.hpp` is `SignalGetter`; `IsRedstoneConductor` reads the
  generated `redstoneConductor` column (Blocks.java's never/always) over the
  collision-full-cube default.
- Block events (`World::BlockEvent` → `ProcessBlockEvents`) drain after random
  ticks, as `ServerLevel.runBlockEvents` does. `IsHandlingTick` spans block
  ticks through block events.
- Generated per block from Blocks.java (`tools/gen_block_hardness.py`):
  `pushReaction`, `redstoneConductor`, `instrument`.

## Components

`RedstoneWire` (default evaluator; the experimental one is a feature flag off
by default and is not ported), `RedstoneComponents` (torches with burnout,
repeater, comparator + block entity, observer, lamp, redstone block, lever,
buttons, pressure plates, note block, target, daylight detector + ticker
entity, redstone ore, copper bulb, TNT, doors/trapdoors/gates, tripwire +
hook), `Rails` (RailState shaping, powered rail signal chains, detector rail
hooks), `piston/` (base, head, moving block, structure resolver, moving
block entity with entity pushing), `RedstoneContainers` + `HopperBlockEntity`
+ `DispenserBlockEntity` + `DispenseItemBehavior` (hopper transfer, dispenser
and dropper, comparator readings of every container and of cake, cauldron,
composter, end portal frame, respawn anchor, jukebox).

## Simulation distance beyond 32

Scheduled ticks run only in block-ticking chunks, and which chunks those are
comes from the player's `PLAYER_SIMULATION` ticket (`ChunkTicketManager`,
`ChunkLevel`). MC's level scale (entity ticking 31, block ticking 32, full
33) puts that ticket at `31 - simulationDistance`, which is why vanilla's
option stops at 32. This engine shifts the three rungs up by
`ChunkLevel::SIMULATION_HEADROOM` (97) so a simulation distance up to
`kMaxSimulationDistance` (128) still maps to a level >= 0; every consumer
names the constants, so nothing else changes.

Chunks only load through a session's chunk loaders, and a tracked chunk is
also sent to the client, so a ring wider than the view distance is loaded by
a `ChunkLoader::Source::Simulation` loader (`IntegratedServer::
ComputeChunkLoaders`): its chunks count for `PlayerSession::KeepsLoaded`
(unloading, load cancellation) but not for `IsWatching` (sending, block
change broadcasts, entity tracking). The setting runs to 128 in the debug
Render Controls panel and the video options; the cap on the server is
`IntegratedServerConfig::maxSimulationDistance`. Cost is what it looks like:
64 chunks keeps ~16k chunks resident and ticking, 128 keeps ~66k.

## The redstone_plus rule

`/gamerule redstone_plus true` (level.dat `obeycraft.redstone_plus`, per
world, off by default) frees redstone from two physical limits; everything
else stays vanilla, so a vanilla contraption behaves the same except that
its signals reach further.

- **No range decay.** A dust network carries the strongest signal fed into
  it to every cell. There is no 15-block limit, a wire needs no repeaters to
  go further, a whole lane switches in one evaluation instead of one
  repeater delay per 15 blocks, and comparator arithmetic (a signal
  strength as a value) survives any distance.
- **No torch burn-out.** `RECENT_TOGGLES` is not fed while the rule is on.
- **The blue redstone torch** (`blue_redstone_torch`, `blue_redstone_wall_torch`;
  engine blocks, offered in the creative tab only while the rule is on, and
  a plain redstone torch when it is off). It flips with no delay, inside the
  neighbour update that changed its input, so a chain of gates settles
  within the tick like combinational logic. Each torch may flip
  `kMaxInstantFlipsPerTick` (8) times a game tick; past that it falls back
  to the scheduled flip, so a loop of blue torches becomes a tick-rate clock
  rather than a hang. Same shape, hardness, placement and drops as the red
  torch (the generators alias it); the textures are the red torch with the
  glow hue-rotated to blue.

Why it needs its own evaluator (`RedstoneWire.cpp`,
`UpdatePowerStrengthPlus`): vanilla's recursive evaluator relies on decay to
turn a network off — the wire that lost its source is strictly stronger
than its neighbours, so its target drops. Without decay it would see a
neighbour at its own strength and keep it. So the rule evaluates the way MC's
`ExperimentalRedstoneWireEvaluator` does: the whole connected network in
one pass (gathered over the geometric neighbourhood, the fixpoint walked
along `getIncomingWireSignal`'s real feeding edges, which are not quite
symmetric), and a wire ignores `neighborChanged` from a wire, so a network
is evaluated once per event rather than once per cell. Changed wires get
vanilla's neighbour updates (the cell and its six neighbours). A change of
the rule takes effect as circuits next update; nothing is re-evaluated
eagerly.

Two more things the rule does, both found the hard way with the Snake
machine (2026-09-11):

- **Deferred re-checks.** With zero-delay gates a network settles inside one
  neighbour-update cascade, and the order of that cascade is implementation
  detail (this engine, the tick sim and a hypothetical vanilla would all
  differ). A repeater that re-checks its input mid-cascade can see a
  transient, and a scheduled repeater always turns on. So under the rule the
  delayed components (red torches, repeaters, comparators, lamps) do not
  decide inside the cascade: `RedstoneComponents.cpp` lists them and
  `RedstoneFlushDeferredChecks` re-checks them against the settled state
  before and after `World::ProcessBlockUpdates`. Their tick lands when it
  would have; the glitch is gone. Dust and blue torches stay immediate.
- **Border settle on load.** Nothing re-evaluates redstone when a chunk
  loads, and a dust network that crosses into a chunk still streaming in is
  cut at the border; without decay the far half keeps its saved strength
  for good (the Snake clock latched after one step). When a chunk finishes
  loading, `IntegratedServer::SettleRedstoneBorders` gives every wire or
  component on its borders, and the cell across, one neighbour update —
  redstone only, and only when the neighbour chunk is loaded.

- **Ticking waits for the neighbours.** In vanilla a chunk block-ticks only
  once the status pyramid has delivered its neighbours as FULL. Here a
  ticket says "ticking" the moment it is placed, so `World`'s tick check and
  the deferred re-checks now also require the 3×3 around the chunk to be
  resident (a component next to a hole reads air across the border; a
  zero-delay gate flipped during load and stepped a counter before the
  machine was started). Cheap: only chunks with a tick due are asked.

Dev trace: `OBEY_RS_TRACE="x,y,z;..."` with `OBEY_RS_TRACE_OUT=<file>`
appends every `SetBlock` on those cells as `gameTime x y z slug on`, for
lining the engine's event order up against `play2.py --rec`.

The generator in `tools/redstone_computer` has the matching switch
(`build.set_plus`, `Machine3(plus=True)`, `write_snake_world.py --plus`):
no transport repeaters on lanes, rows or screen lines, no decay in the tick
sim, and the rule written into the world's level.dat.

### The display block

`display_block` (`BlockDefs.inc`; `DisplayBlock` in `RedstoneComponents.cpp`;
offered in the creative menu only while the rule is on) is an addressable
colour pixel with its own memory, so a display is a grid of these plus one
dust line per row and one per column, instead of a latch and taps per pixel.

Properties: `red`, `green`, `blue` (the stored colour; white when all off),
`powered` (the read-back output) and two operations, `op_nw` for the
north&west line pair and `op_se` for south&east, each one of `none`,
`set_r/g/b`, `clear_r/g/b`, `show_r/g/b`, `check_r/g/b`, `latch`:

| op        | while both lines of the pair are powered                   |
|-----------|-------------------------------------------------------------|
| `set_X`   | the bit turns on (and stays)                                |
| `clear_X` | the bit turns off                                           |
| `show_X`  | the bit follows the pair (a live, unstored colour)          |
| `check_X` | if the bit is on, the pixel's top block powers what sits on it (strong, upward) |
| `latch`   | while the column face (the clock) is powered, copy every bit from the display group across the row face (a frame-buffer pixel) |

A connected group of display blocks (face neighbours) is one pixel: the
bits are shared, so a pillar shows its colour along its height and a chain
of display blocks carries a pixel from its lines to a wall elsewhere. An
operation block reads its row lines on its own north and south faces and
its column lines on the west and east faces of the block two above it
(when that is a display block), so rows and columns cross two levels
apart; otherwise it reads its own faces. A `latch` block's row face (north
for `op_nw`, south for `op_se`) is its data face: the group never joins the
neighbour across it, so a buffer stays a separate pixel from the group
that feeds it and only samples while clocked — a display that shows
complete frames whatever order the logic behind it updates in. Dust
powered under any block of the group clears every bit; on a check hit every block with no display
block above it powers what sits on it. The stored bits are read from the
group's lowest block (y, then z, then x) before the operations apply. A
face reads any adjacent dust that carries power, whatever the dust's shape
(vanilla dust only powers what it points at), plus anything else that
signals toward it. Evaluated like the lamp: deferred to the end of the
cascade under the rule; a group is capped at 4096 blocks. A comparator
reads the colour bits as 0..7. An empty hand cycles `op_nw`; sneaking
cycles `op_se`.

### Redstone warm-up

While a level's kept chunks (forced or indexed) are still arriving,
redstone is frozen: no border settles, no deferred re-checks, no
scheduled block ticks (`ServerLevel::redstoneWarmup`, `World::
SetRedstoneFrozen`). A machine spanning dozens of chunks would otherwise
evaluate gates against chunks not yet in — a dust net cut at a border
reads as off — and instant torches and latches remember that transient
(the Snake machine's clock gate opened during loading and the game died
before the player touched anything). Once every kept chunk is resident
the borders that arrived meanwhile are settled together and the level
runs; the log says `[RedstonePlus] warm-up ...`. Later loads settle at
arrival as before.

### Emissive blocks

`Block::emissive` (the display block) marks a block's face-map records
(`TerrainVertex::kFaceMapEmissive`, word 1 bit 16); the terrain fragment
shaders skip the day/night sky dim for those fragments, so a screen reads
the same at night. This engine has no block light, so nothing around an
emissive block is lit by it.

## Force-loaded chunks and the redstone index

A simulation distance loads and generates every chunk in its radius, whether
or not anything in it ever ticks. Two ways to keep only what matters loaded
(`src/server/level/ChunkKeeper.hpp`):

- **`/forceload`** — vanilla's command and vanilla's file
  (`<dimension>/data/chunks.dat`, `data.Forced` as packed chunk positions).
  Forced chunks carry a `Forced` ticket at `ChunkLevel::ENTITY_TICKING`,
  MC's level 31, so the chunk and its eight neighbours block-tick and the
  ring beyond is fully loaded. `add`/`remove` take column positions in
  blocks (relative `~` allowed), at most 256 chunks a command; `remove all`
  and `query [<x> <z>]` as in MC. `write_snake_world.py` writes the machine's
  chunks into this file, so the Snake world opens with exactly its chunks
  resident.
- **`/gamerule redstone_chunks`** (engine rule, level.dat
  `obeycraft.redstone_chunks`) — every saved chunk that holds a redstone
  component (`IsRedstoneComponent`) is kept the same way, halo included, so a
  far contraption runs without a ring over the land between. The index lives
  in `<dimension>/data/obeycraft_redstone_chunks.dat`; it is updated from
  the chunk save path (`ChunkCache::SetSavedObserver`) and built once for an
  existing world by a background scan of its region files' palettes.

Both differ from a player ring in one way that matters here: tickets in this
engine tick chunks but do not load them (loading is watch-set driven), so
`ChunkKeeper::Service` requests the loads a forced ticket would have caused
in MC, and `UnloadUnwatchedChunks` treats a kept chunk like a watched one.
Kept chunks tick whenever the world is open, player or no player, which is
what vanilla's forced chunks do too.

## Deliberate deviations and gaps

- Piston replication is vanilla's: block events reach the client
  (`BlockEntityActionS2C` carries the block id), `ClientBlockAccess::BlockEvent`
  runs the block's `triggerEvent` under a local-write scope, and
  `ClientChunkManager::TickBlockEntities` ticks the client's own moving cells,
  which push the local player through `ILevelWrite::MoveLocalPlayerByPiston`.
  Entities are moved directly (`Entity::Move`) on the server.
- `MovingPiston` has a full-cube collision for its two ticks instead of the
  carried block's shape.
- No sound system: `PlaySound` logs. Note blocks and pistons name the vanilla
  events for when one lands.
- Not ported: minecarts (detector rails never press; powered rails do power),
  sculk sensors, lectern/bell/jukebox/trapped-chest signals, crafter,
  lightning rods, item frames in comparator inputs, dispenser behaviours for
  boats/minecarts/armour/shears/shulker boxes/bottles/honeycomb/heads.
- Daylight detector sky light is the heightmap's open-sky test (no light
  engine); the sun-angle curve is vanilla's.
