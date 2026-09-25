# Console port status — September 24, 2026

The full port is **in progress**. The user requires no visible client, screenshots,
or previews until everything is ported. No visible client has been launched since
that instruction. Changes stay inside `console_port/`; only general vendored
libraries are reused from the parent project.

The original Minecraft.World directory has 1,572 files. Imported implementation
modules now compile alongside density/surface, tile-rule, and lighting subsets
extracted from RandomLevelSource, tile classes, Level and LevelChunk. The import
inventory is not a completion percentage.

## Compiled and checked

| Boundary | Implementation and verification |
|---|---|
| Math/noise | Random, ImprovedNoise, PerlinNoise, Synth, Mth; exact sampled integer/float comparison against separate original compilation |
| Biome graph | All layers in the original newbiome umbrella, IntCache and LevelType; normal/large/legacy comparisons across seeds and signed coordinates |
| Terrain | Original prepareHeights, getHeights, buildSurfaces; original noise allocation order and finite-world falloff |
| Carvers | LargeFeature, LargeCaveFeature, CanyonFeature; original cave/ravine stage order, sampled chunk hashes |
| Nether terrain | Original HellRandomLevelSource density/surfaces and LargeHellCaveFeature; lava sea, bedrock floor/ceiling, finite walls, gravel/soul sand and console nether-wart surface placement |
| End terrain | Original TheEndLevelRandomLevelSource density/surfaces; central island, radial falloff and vertical density shaping |
| Dimension terrain API | Independent Nether/End generation stages with original biome IDs, bounded coordinates and serialized access; 384 exact source output comparisons plus order/concurrency, boundary and block-property checks |
| Host integration | World::generate calls original base stages and a partial natural-feature pass; moving 8×8 chunk window uses cached raw terrain for deterministic decoration across the 54×54 world |
| Packed data | Original DataLayer indexing and nibble layout; metadata/light-array tests, including corrected setAll |
| Chunk storage | New host storage for original 16×16×256 columns, packed data/light arrays, biome fields and light-blocking generation heightmap for registered tiles |
| Materials | Original Material, MaterialColor and all five material subclasses; all 33 registered material property sets compared against the archive |
| Direction tables | Original Facing and Direction modules replace temporary direction constants |
| Plant placement | FlowerFeature (flowers and both mushrooms), TallGrassFeature, DeadBushFeature, ReedsFeature, CactusFeature, WaterlilyFeature, PumpkinFeature, VinesFeature; 384 source comparisons and independent soil/water/light/attachment checks |
| Placement rules | Extracted original Bush, Mushroom, ReedTile, CactusTile, WaterlilyTile, DeadBushTile, PumpkinTile, VineTile and base Tile methods, plus Level::isTopSolidBlocking |
| Lighting | Original uncached CPU brightening/darkening queue, overlap recovery, finite bounds and forced updates; original chunk skylight initialization, height changes, gap checks and initial emission scan adapted to flat storage |
| Lakes | LakeFeature and original water-freezing rules; water/lava carving with live lighting, existing-liquid rejection, source-water/neighbor/light freezing checks |
| Placement | Feature, TreeFeature (including jungle vines/cocoa), BirchFeature, PineFeature, SpruceFeature, OreFeature, SwampTreeFeature, MegaTreeFeature, GroundBushFeature, HugeMushroomFeature, ClayFeature, SandFeature, DesertWellFeature; 420 block/data/heightmap/random-state comparisons across grass, obstructions, sand, water, mycelium, high terrain and negative chunk boundaries |
| Memory streams | Original byte-array input/output and buffered output; source byte comparisons plus growth, overlap, bounds, EOF, flush and ownership checks |
| Primitive streams | Original data input/output and numeric containers; big-endian integers, IEEE floating-point bits and modified UTF; platform PlayerUID serialization is excluded and remains pending |
| NBT | Original tag IDs 0–11, named tags, arrays, lists, compounds and NbtIo wire layout; source comparisons, malformed/truncated data, Unicode, copy/equality and ownership tests |
| World metadata | Original LevelData, LevelSettings (including GameType) and Abilities; save fields, legacy defaults, generator selection, PS3 world limits, mode transitions, movement abilities and binary round trips |
| Compressed tiles | Original CPU CompressedTileStorage palette packing (uniform, 1/2/4/8-bit), mutations, regions, compression, copies and wire format; exact source samples plus exhaustive-coordinate, malformed-data and concurrent-access checks |
| Sparse storage | Original SparseDataStorage and SparseLightStorage plane packing, zero/full-bright sentinels, nibble reorder, region access and compression; source byte comparisons and validation/concurrency checks |
| Chunk records | Version-8 header and eight-section order extracted from OldChunkStorage/LevelChunk; complete tile/data/light payloads, height/biome bytes, original population-flag migration, and retained entity/tick/unknown NBT |
| Legacy chunk records | Original NBT fields and concatenated 128-high section layout; 128/256-high loads, current-height rewrites, population migration, unresolved-biome defaults, source writer comparison and complete-coordinate checks |
| Save archive | Original inner FileHeader table and contiguous file layout; fixed-width UTF-16 names, PS3/other-platform header endianness, legacy table versions, replacement/removal and malformed-data checks |
| Compression | Original RLE loop and source comparisons; PS3 EdgeZLib size prefix plus raw DEFLATE, wrapped DEFLATE for supported other platforms, bounded decoding and independent legacy fixture |
| Region files | Original 4 KiB sectors, offset/timestamp tables, eight-byte chunk headers and first-fit allocation; 100 exact original writer comparisons, PS3 field checks, short-sector handling and transactional writes |
| PS3 world storage | Connected legacy and version-8 archive, region, compression, chunk record and level metadata libraries; extracted original dimension prefixes/initial file order, Data-root wrapper, signed coordinate routing, preservation of unported data |
| Storage bridge | Full-height flat chunks convert to/from original compressed sections; complete-coordinate/nibble checks, heightmap transpose and repair, retained opaque NBT, all-dimension legacy/current save round trips |
| Native persistence | POSIX inner-archive reads and atomic replacement with unique sibling staging files; content fsync and reported directory-sync status, failed-write preservation, malformed-file rejection and concurrent reader/writer checks |
| Client chunk/save integration | Streaming 128×128×256 window backed by ChunkStorage, original metadata/light operations and PS3 inner saves; transactional load, prototype migration and unloaded/other-dimension data preservation |
| Scheduled update records | Original TickNextTickData identity, hash and due-time/insertion ordering; 640 original record samples, duplicate identity checks, extreme times/hashes and concurrent construction |
| Scheduled update queue | Original ServerLevel scheduling, duplicate suppression, instant/forced insertion, 1,000-record processing limit, chunk extraction and clock adjustment through explicit world hooks; independently extracted source comparisons and callback/overflow/exception checks |
| Saved block updates | Original TileTicks compound list and relative delays; 12 original NBT writer comparisons, atomic queue loads, coherent snapshots, duplicate handling and legacy/current archive reload/dispatch tests in all three dimensions |
| Collision geometry | Original Vec3 and AABB arithmetic and block HitResult construction; 384 vector/box scenarios against a separate source build, six ray faces, collision boundaries, corner ties, ring-pool reuse and thread isolation |
| Block selection | Original Level::clip traversal and base Tile::clip shape intersection; 240 source comparison scenarios with signed coordinates, liquid/solid filtering and metadata-dependent shape fixtures; client mining/placement now calls this traversal |
| Hunger | Original FoodData and FoodConstants through explicit player/item hooks; 720 source state histories, saturation/exhaustion thresholds, 80-tick healing/starvation, host hunger behavior and transactional NBT restoration |
| Player food permissions | Original player exhaustion and eating guards plus flight/hunger/invulnerability permission predicates; 6,144 source comparisons cover all 2,048 host/player/ability/authority combinations at three food levels |
| Player experience | Original XP costs, float progress, award/cap/score, level withdrawal, death reward and XpP/XpLevel/XpTotal fields; 360 source histories plus boundary, overflow and transactional save/award checks |
| Console render lighting | Original 16×16 GameRenderer lightmap, dimension ramps, sky darkening, night vision, flicker and propagated packed light lookup; 324 lightmap histories and 1,152 sampler scenarios match independent source builds |
| Playable lighting integration | Actual terrain mesh carries stored sky/block light into shared GL shaders; sealed-room/source-removal/build-ceiling/halo checks plus windowless macOS GPU pixel checks for terrain lighting and unlit UI |
| Liquid surface integration | Original TileRenderer weighted corner heights and LiquidTile height/light override drive water/lava meshes; 864 source corner scenarios, liquid-light source comparisons and playable mesh checks for shallow/stacked fluid and full lava emission |



Placement routines are compiled and verified individually. A source-derived subset
of BiomeDecorator/postProcess now runs on new visible chunks, including lakes,
dungeons, ores, shore deposits, trees, foliage, springs and snow/ice. Its skipped
structures, fluid ticks and detached daylight approximation mean it is **not** a
complete or exact post-process port.

## Compatibility changes and intentional fixes

- Random uses a monotonic timer and explicit unsigned wrapping arithmetic.
- Layer seed mixing uses modulo 2^64; nextRandom reinterprets the state as signed
  before the original shift/modulo. Negative zoom shifts use 64-bit multiplication.
- IntCache replaces Windows TLS with owned C++ thread-local storage, retaining its
  allocation/reuse algorithm. The reference uses a small native TLS adapter.
- LevelType initializes its replacement flag and checks empty registry slots before
  dereferencing them. Its std namespace import was removed to avoid std::byte clashes.
- Mth's original sine table is initialized once with synchronization/static storage.
  Table values are unchanged; generation workers no longer race initialization.
- LargeFeature seed-coordinate products use unsigned wrapping arithmetic.
- Material and MaterialColor startup is synchronized and idempotent; unused color
  slots are zero-initialized. The original registry objects keep process lifetime.
  MinecraftColours.h retains the complete original eMinecraftColour enum, extracted
  from the preserved App_enums.h.
- Generation heightmaps skip zero-light-block tiles, following LevelChunk. Leaves,
  water, ice, lava, webs and slabs retain their original opacity overrides. A uint16
  host height retains 256 instead of the original byte overflow at the build ceiling.
- DataLayer copying is disabled because it owns a raw buffer. Original setAll used
  AND between nibbles, producing zero for valid light values. The port uses OR and
  masks both nibbles. This intentional correction has an independent test; parity
  is not claimed for the broken original method.
- Density noise arrays use the owning doubleArray adapter. Timing counters are
  omitted. The reference extractor preserves numeric bodies and uses raw arrays
  so original explicit deletes remain valid.
- Nether and End terrain methods are extracted from their preserved source files.
  Noise objects retain the original constructor order but use unique ownership;
  temporary double arrays use the port's owned buffers. The reference retains the
  original raw arrays and explicit deletes and separately compiles the original
  noise/carver modules. Nether recursive cave RNGs use scoped ownership. Numeric
  loops, surface coordinate order, random consumption, finite wall conditions and
  End island falloff remain unchanged. The Nether size is ceil(worldChunks/hellScale).
  DimensionChunkGenerator validates settings/coordinates and serializes access to
  mutable noise/RNG state. It assigns the original fixed Nether/End biome IDs.
  Nether wart's generation material, opacity and ID are registered from Tile and
  NetherStalkTile; its survival, growth and full simulation class remain pending.
- Original lighting cache mode is explicitly disabled. Native mutex scope replaces
  Windows critical-section calls and releases correctly on exceptions. The numeric
  propagation algorithm and queue ordering are preserved. Reference builds use the
  same uncached mode with mechanically extracted numeric bodies.
- Chunk-light adapters expose original 128-high section get/set operations over flat
  storage. The port fixes the original heightmap scan omitting y=255 and byte-height
  truncation, and extends the initial emission scan to both sections. The reference
  retains its lower-section-only scan. Independent checks cover y=127/128/200/255,
  cross-chunk falloff, opaque obstructions, ceiling dimensions, save/reload/rebuild
  and source removal; parity is not claimed for the corrected initialization.
  The generation caller now applies the original LevelChunk::tick ceiling guard
  before running sky-gap checks. Minimum chunk height also retains 256.
- Carver reference casts qualify the original byte type to avoid std::byte ambiguity.
- Memory streams validate ranges, handle empty-capacity growth and overlapping
  buffers, disable accidental ownership copies, and fix BufferedOutputStream's
  mismatched delete. Explicit flush forwards to the downstream stream.
- Primitive streams use unsigned bit assembly and bit_cast, correct written-byte
  counts, and distinguish EOF from valid 0xff. Modified UTF supports native wide
  characters via UTF-16 units, including non-BMP pairs; malformed input throws.
  Float infinity and Double::isInfinite are corrected. Child streams remain borrowed
  unless deleteChildStream is explicitly used; that operation is now repeat-safe.
- NBT arrays constructed from existing buffers remain borrowed. Loaded/copied arrays
  are owned by their tags; getByteArray/getIntArray return borrowed views. Future
  original loaders that transfer buffers must use takeByteArray/takeIntArray, and
  must not delete a borrowed view. Compound removal destroys its child; take detaches
  it. These are explicit ownership changes from the archived callers' conventions.
- NBT replaces values without leaks, preserves existing contents on failed loads,
  validates collection sizes/types, and rejects cycles or aliased child ownership.
  Parsing has a 512-scope nesting limit, 256 MiB allocation budget and 1,048,576-node
  budget. Int-array equality compares the full payload and list equality checks
  ordered values. List specializations share TagList storage with checked access;
  getList returns TagList*. Debug printing uses wide stream references.
- NbtIo retains the supplied console source's uncompressed payload despite the
  inherited compress/readCompressed method names. compress returns the valid byte
  count instead of the original backing-array capacity padding. Failed decompress
  never deletes the caller's input buffer. Non-compound roots retain the original
  null result; malformed tags throw.
- LevelData initializes its fields, validates required pointers, and uses the host
  epoch clock for LastPlayed. LevelType and GameType registration is idempotent and
  synchronized. SavePlatform exposes the explicit cheats option read by setGameType;
  connection to the original host-options UI remains pending. Original PS3 dimension
  macros are preserved in ConsoleWorldLimits.h from the hashed ChunkSource.h.
- CompressedTileStorage retains the original CPU packing loops and deferred-free
  queues. Recursive mutex guards replace Windows locks and cover CPU readers;
  signed 64-bit mask shifts become unsigned. Heap allocation replaces console
  physical pages and zeroes alignment padding in both port and reference samples.
  No SPU or GPU reader behavior is claimed. Binary reads validate lengths, offsets,
  overlapping palettes and reserved tile 255, then replace storage transactionally.
  The original wire order is big-endian byte count and little-endian palette indices.
  Empty default sections remain a zero-length sentinel until written. ChunkStorage
  retains flat arrays and now connects to these codecs through ChunkStorageCodec.
- Sparse storage uses a full-width pointer/count state and recursive CPU locks in
  place of 48-bit pointer packing and console RCU operations. Raw pointer views and
  state updates are private; no lock-free renderer interface is claimed. Original
  uniform-zero/full-bright sentinels and paired-Y region rounding are retained.
  Reads validate plane counts and reject invalid or aliased indices before replacing
  storage. Native allocators report failure and queued buffers are reclaimed.
- ChunkRecord preserves the version-8 binary header and section order, with entity,
  tile-entity, tick and unknown NBT retained rather than instantiated. The original
  all-neighbors population-flag repair is retained; LastUpdate keeps all 64 bits
  instead of the archived loader's int truncation. Other binary chunk versions
  are rejected; the older NBT chunk family has its own codec. Raw byte heightmaps retain their
  original layout. This is a record codec, used by the new inner-save storage library but not the
  complete simulation loader. The client now saves through this record codec.

- FileHeader uses explicit fixed-width wire fields instead of native wchar_t/struct
  dumps. Reads validate file-table bounds, duplicate names and contiguous offsets.
  V1 table lengths are bytes and omit timestamps; V2–V8 table lengths count entries.
  Rewrites preserve the original container version until payload conversion exists.
  ConsoleSaveArchive owns its entries and payloads, with a 64 MiB PS3 capacity bound.
  Changing table endianness does not convert opaque region or player payloads.
- Compression keeps the original RLE run encoding but replaces the fixed 100 KiB
  scratch buffer and unsafe empty-input loop. Native zlib replaces PS3 SPU deflate:
  PS3 chunks carry a big-endian RLE-size prefix followed by raw DEFLATE. Decoding
  checks exact sizes, trailing bytes and allocation limits. No byte-identical SPU
  compression claim is made. Xbox 360 LZX remains explicitly unsupported.
- ConsoleRegionFile retains the original first-fit reuse order, freed-sector zeroing,
  same-size overwrite padding and extra sector at an exact payload boundary. New
  files reserve both headers before allocating chunk sectors, fixing the original
  lazy-write underallocation. Reads pad a partial final sector only when all declared
  payload bytes are present; overlapping sectors and truncated payloads throw.
  Writes stage compression and allocation before replacing live state. Region
  reference comparisons start from an existing valid two-sector header; the lazy
  first-write fix has independent checks. Reference compression is shared only to
  isolate the original sector allocator and byte writer.
- PS3WorldStorage connects the inner save codecs and offers explicit readFile and
  writeFile operations. It retains the original DIM-1 and DIM1/ naming difference and
  twelve initial region-entry order, and writes level metadata under the Data root.
  Known metadata updates preserve unknown fields, embedded Player tags and unrelated
  archive entries. Chunk coordinates must match their region slot. The original archive version selects legacy NBT or version-8 binary chunks,
  independently of the last-written version. Existing container versions are
  preserved; this does not perform a whole-save version conversion. Reads do not create missing region entries. Asynchronous save queues,
  profiles, map ownership and Sony save packaging/signing are
  still pending. The client now uses this library for `world.inner`; it does not instantiate entities.

- ChunkStorageCodec splits/joins full-height columns into the original two-section
  layout, including all packed metadata and light nibbles. It transposes the host
  heightmap into the original wire order, preserves biome order and deep-copies
  opaque record fields. Capture requires paused mutation and a current heightmap;
  malformed layers and unregistered generation blocks are rejected. Restore
  recomputes uint16 heights and explicitly requests relighting for stale/wrapped
  saved heights. It does not run light initialization or instantiate entities.
- NativeSaveFile validates regular files and the inner archive's 12-byte minimum
  and 64 MiB capacity. Writes stage in a unique, private sibling file, sync contents,
  then atomically rename; precommit failures remove staging files and retain the
  previous save. Directory sync is reported separately after the rename commits.
  Parent directories must exist. Concurrent readers see complete old or new files;
  concurrent writers use distinct staging files, with the last rename winning.
  This POSIX backend persists the inner archive only, without Sony packaging or
  client integration. Tests use disposable fixtures, never user save files.
- TickNextTickData retains position/tile equality independent of due time, and
  orders equal due times by its construction sequence. Hash arithmetic explicitly
  wraps at 32 bits. A typed, null-safe equality argument replaces the unchecked
  void-pointer cast. Its process-wide sequence is atomic and rejects exhaustion;
  copies retain their original sequence. The reference compiles the untouched
  original with signed wrapping.
- ScheduledTickQueue transfers ServerLevel's two-index queue to explicit host
  methods for time, loaded-neighborhood checks, tile lookup and real tick dispatch.
  Original eight-block halo checks, first-duplicate-wins behavior, time/sequence
  traversal, instant ticks, forced load insertion, 1,000-record cap and missing-halo
  discard behavior are retained. Source comparisons include callbacks inserting
  earlier and later records. The native queue uses recursive scoped locks, checked
  deadline arithmetic, 64-bit chunk boundaries and transactional insertion/removal.
  Clock changes rebuild both indices; the original changed only the ordered index,
  leaving saved delays stale in the identity set. Full-range clock shifts are
  checked before committing. Callback chunk extraction cannot leave a dangling
  iterator; recursive draining or clock adjustment during dispatch is rejected.
  Callback exceptions release the lock and preserve the remaining queue, while the
  current tick remains consumed as in the original. Real Tile::tick implementations
  and simulation integration are still pending.
  Test host callbacks are fixtures, not implemented block simulation.
- ScheduledTickCodec writes the original i/x/y/z/t integer fields and loads relative
  delays through forced queue insertion. Snapshots capture world time and chunk
  ticks under one lock; saving leaves the queue intact. Loads validate the complete
  list before a batch commit, retain existing/first-duplicate precedence, and bypass
  loaded-neighbor checks without executing blocks. Equal-time insertion follows
  serialized list order, as in the source; the original hash-set save order does
  not preserve pre-save construction order. Invalid field types, missing fields,
  foreign-chunk coordinates, invalid IDs and overflowing deadlines are rejected.
  Writes reject delays outside the signed 32-bit save field instead of silently
  truncating them, and preserve the previous NBT on failure. Raw ChunkRecord reads
  still preserve ticks until this explicit loader is invoked. Actual simulation
  chunk loading and real block callbacks remain pending.
- Geometry retains original numeric formulas, rotation conventions, clipping
  tolerances, strict/inclusive bounds and face tie ordering. Native thread-local
  pools replace Windows TLS, initialize lazily, release safely and fix AABB's
  mismatched array delete. UseDefaultThreadStorage now creates isolated per-thread
  storage rather than sharing mutable pools. The original 1,024-entry ring reuse
  and no-op reset/clear behavior are retained: temporary vectors/boxes and hit
  positions are borrowed, and persistent callers must copy or own permanent values.
  Vec3 diagnostic formatting uses a local buffer. The reference retains original
  pool/arithmetic code with a TLS adapter. Only block-hit construction is ported;
  Entity-based HitResult construction/distance APIs are explicitly unavailable until
  Entity is ported. This library is not yet the client's movement/collision system.
- BlockRaycaster extracts the original Level traversal, including its 201-step
  limit, start-cell query, mutable start endpoint and axis tie order. Base Tile
  clipping uses explicit local-coordinate shape providers in place of shape TLS;
  the arithmetic and Facing values are unchanged. The reference separately
  compiles original vector/box/math code and the extracted traversal. Native calls
  reject null/nonfinite/out-of-range endpoints and unregistered tiles before unsafe
  access. The client's World::raycast now uses this path and maps the returned face
  to the adjacent placement cell. Its current adapter supports the existing full-cube
  block set and skips static fluids for normal picking. Metadata/slab/non-colliding
  shape comparisons use fixtures; the complete original Tile shape registry, special
  block overrides and entity targeting remain pending. Movement remains the existing
  host implementation. No visible client was launched for this integration.

- The client now stores its active 64 chunks in full-height ChunkStorage and maps
  local x/z to original coordinates with a -64 offset. Editing prepares a two-chunk
  light halo from saved chunks or original base terrain and invokes original
  no-update tile/light operations. Complete block callbacks remain pending.
  Valid loaded light survives an unchanged save; stale/wrapped heights trigger a
  rebuild. `world.inner` atomically replaces the previous inner archive, preserving
  unloaded chunks, other dimensions and unknown NBT/archive entries. Loading stages
  all changes before replacing the live world. Only supported creative blocks,
  generators, original world size and a complete central region are accepted.
  Earlier 96/128-high prototype files migrate without overwriting their originals.
  Player/entity/tick data is retained, but not instantiated or executed; world time
  now advances during play; scheduled world simulation remains pending. No preview was run.

- FoodData retains the original strict exhaustion threshold, single drop per tick,
  80-tick timer, difficulty-dependent starvation and console hunger-ignore branch.
  Healing does not add the exhaustion cost used by newer versions. The hunger-ignore
  branch retains its timer during interruptions and spends one food when healing;
  existing exhaustion still drains, so the original Player guards are also ported.
  FoodPlayerAccess and FoodItemAccess replace only direct entity/item calls and
  require actual healing, starvation damage and nutrition implementations from their
  caller. PlayerFoodRules preserves the original host/privilege combinations,
  creative protection, client/server guard and TU6 untrusted-player exhaustion rule.
  The original invulnerability privilege uses HostCanBeInvisible; this naming is
  retained. Live creative-player food state and ticks are now connected; food
  items, movement exhaustion, and the survival loop remain pending. Reference
  fixtures record calls and are never linked into the client.
  Native food mutations reject invalid ranges and nonfinite floats, and large
  nutrition uses a wide intermediate to avoid signed overflow. NBT loads preserve
  original absent-field defaults and last-food behavior, but validate tag types,
  finite values and the saved timer before replacing state. Opaque parent tags
  survive food writes. These checks intentionally reject corrupt food data that
  the original accepted unchecked.

- PlayerExperience extracts the original Player XP operations and three save fields.
  The 15/30-level cost changes, float arithmetic, total-XP cap, whole-award score,
  level-only withdrawal and 100-point death reward cap are retained. Batch awards
  retain original rounding: 121 XP at level 29 leaves a tiny positive remainder
  after reaching level 31, unlike two exact separate threshold awards. Score wraps
  through explicit unsigned arithmetic; reward multiplication uses a wider type.
  Native entry points reject negative awards/withdrawals and malformed saved state.
  Maximum level is bounded so the next-level cost fits the original signed integer.
  Awards stage all state until level transitions succeed. Missing XP tags retain
  original zero defaults; score is not stored in these three original XP fields.
  The creative player now loads and saves these fields through the native world
  archive. Mob death and furnace-result orbs now dispatch XP to the live player;
  enchanting/anvil callers and full player loading remain pending. These methods
  alone do not simulate a complete player.

- ConsoleLightmap transfers the original GameRenderer CPU color loop, the normal
  and Nether brightness ramps, Dimension celestial angle, Level sky darkening,
  night-vision duration and two-channel block-light flicker. RGBA channels are
  explicit bytes instead of PS3-native packed integers. Reference output uses the
  original PS3 packing with unsigned shifts to avoid host undefined behavior.
  Eight explicit random samples preserve the flicker expression and decay order;
  the client supplies them through a renderer-owned Random, independent of terrain
  generation. Input ranges are checked before state changes or float-to-int casts.
  The original hard-coded zero gamma remains unchanged.
- RenderLightSampler transfers Level::getBrightnessPropagate and getLightColor,
  including five-neighbor sampling, ceiling/surrounding light and emission floors.
  Explicit access hooks supply chunk data and registered tile propagation flags.
  Air keeps propagate[0] false because the original registry skips its null Tile.
  Host direct storage queries beyond build height return zero instead of indexing
  wrapped compressed sections. Full shapes remain pending; the liquid-specific packed-light override is now
  connected to water/lava mesh sampling. The client prepares its two-chunk lighting halo before mesh sampling;
  rendering a loaded world therefore runs the existing full light rebuild, while
  an immediate load/save without mesh construction still preserves valid light.
- TerrainMesh extracts CPU mesh construction from Renderer and reads a contiguous
  block snapshot to avoid millions of chunk-map lookups per rebuild. The current
  cube faces use original directional shades (1, .5, .6, .6, .8, .8), packed light
  coordinates and lightmap texel centers. The renderer uploads the lightmap on a
  separate texture unit and keeps UI/placeholder sky drawing unlit. The actual
  shared shader sources compile/link and render through a windowless CGL context;
  numeric pixel readback checks light-channel placement and UI bypass. No window,
  image file or preview is created. World time advances during play; weather stays at its saved value until
  weather simulation is ported. Night vision, lightning and dimension overrides
  are verified library inputs, not active player effects in the client. Original
  AO, complete fluid meshes, full sky/fog and complete TileRenderer remain pending.

- LiquidSurface extracts LiquidTile::getHeight/getLightColor and
  TileRenderer::getWaterHeight. Source/falling fluid metadata retains its weighted
  corner contribution, neighboring open/solid cells affect the surface and liquid
  above makes the corner full-height. Native callers reject invalid nibbles,
  coordinate overflow and all-solid empty sampling instead of producing NaN.
  The actual water and lava meshes use four independent heights, original top/bottom
  offsets and directional shading. Internal same-liquid faces are culled and
  stacked columns meet without the former fixed .12-block gap. Liquid tops sample
  the source cell; bottoms/sides sample their neighbors, then independently combine
  sky/block light with the cell above. Lava tops retain full emitted light.
  Existing host tints and simplified side geometry remain;
  exact full Tile face rules and liquid simulation are still pending.
  Targeted storage, lighting/source-parity and offscreen shader tests pass in both
  Release and UBSan after this change; the entire suite was not unnecessarily rerun.

- LiquidFlow extracts the original neighboring-depth, open-drop and falling-wall
  current calculations, including the ice exception and still-water slope sentinel.
  Original TileRenderer top rotation and StitchedTexture interpolation use the
  original PreStitchedTextureMap still/flow atlas regions. The playable terrain
  mesh uses these top UVs and height-dependent side UVs for both water and lava.
  640 source scenarios compare exact vector, slope and UV bits; independent tests
  cover walls, drops, ice, malformed inputs and actual mesh integration. Release
  and UBSan lighting/source-parity and windowless shader checks pass. The client
  builds; no visible client was launched. These currents read existing metadata;
  liquid spreading, entity pushing and full face rules remain
  pending.

- TextureAnimation adapts StitchedTexture::cycleFrames with owned frame schedules.
  The four original TitleUpdate liquid PNG strips and timing files are imported
  unchanged. TextureManager's vertical square-frame layout is used for atlas
  uploads: water has 32 frames, still lava 20, and flowing lava 16. Still water
  holds each frame for two ticks, still lava reverses after frame 19, and flowing
  lava holds each frame for three ticks. Empty flowing-water timing uses the
  original next-frame fallback, starting from the loaded frame-zero state.
  The client updates only the affected atlas rectangles at its existing 20 Hz
  visual tick while playing; no mesh rebuild is required. Independent tests cover
  8,000 animation ticks, duplicate-upload suppression, static frames, invalid
  schedules and actual strip decoding/dimensions. The client builds and targeted
  animation, lightmap/source-parity and windowless shader tests pass in Release
  and UBSan. No visible client or preview was launched. Other animated textures,
  resource-pack reloads, mipmapped atlas updates and fluid simulation remain pending.

- LiquidReaction extracts LiquidTile::updateLiquid into an explicit replacement
  and fizz result. Water on the four sides or above converts lava metadata zero
  to obsidian (49), and metadata one through four to cobblestone (4). Water below
  does not cool lava; deeper/falling lava retains its tile but requests fizz.
  Accepted client edits evaluate the placed cell and six neighbors in original
  Level notification order. Raw storage edits/load remain no-update operations.
  Obsidian now has client collision, original atlas tile 37 and native save support.
  Tests cover all 2,048 material/contact/metadata combinations, cross-chunk cooling,
  light removal, metadata reset and save/load. The client builds; targeted contact,
  storage, lightmap/source-parity and windowless shader checks pass in Release and
  UBSan. Fizz locations are returned for future effect dispatch; sound and particles
  are not yet rendered. This is the cooling callback only, not general Tile updates
  or flowing-fluid simulation.

- Visible front-end work now follows the original LoadOrJoinMenu/CreateWorldMenu
  separation: Play Game lists named local saves and opens a separate creation
  screen with world-name and seed fields. Four-save pagination supports larger
  libraries. New worlds use separate uniquely allocated directories; existing root
  saves remain listed and legacy .mcp migration remains available. Names are stored
  in original LevelData metadata; invalid archives remain listed as unreadable.
  This is a host UI implementation of the source flow, not a completed port of the
  console Scaleform layout, online join, thumbnails, or game-mode configuration.
  Tests verify isolated save allocation, name persistence/order, selected paths,
  malformed-name rejection and corrupt-save isolation. Release/UBSan targeted
  storage, cooling and rendering checks pass; the visible client was not launched.
  Original Tutorial.pck and tutorialDiff exist in the supplied archive. The tutorial
  difference loader, broader block behaviors and tutorial scripting remain
  pending (rule decompression and schematic world loading are now implemented); no generated substitute is presented as the original tutorial world.
  Terrain generation is unchanged in this batch (original base terrain/carvers,
  central 8-by-8 chunks at that time; the later streaming batch extends exploration).

- ConsoleDlcPack ports the version-3 DLCManager table/blob layout using owned
  buffers and bounded reads instead of native casts and borrowed pointers. It reads
  both endian variants, the original variable UTF-16 struct tails and per-entry
  parameter tables. Tests read the actual Tutorial.pck language/rule entries and
  cover malformed tables, names, truncated payloads and trailing bytes. The package
  is decoded, not instantiated as a playable world. Its GameRules.grf uses LZX/RLE;
  decompression and schematic world placement are now implemented; live rule execution remains pending.
  The archive also contains original BuildOnly/Tutorial/GameRules.xml in UTF-16:
  seed 1227750481513469519, spawn (97,72,-106), plus schematic/structure rules.
  These source definitions can support a later data adapter without substituting
  a generated mock tutorial. Streaming now permits visiting that coordinate, but
  the subsequent tutorial adapter instantiates its schematics in streamed chunks.

- CelestialMesh adapts original LevelRenderer sun/moon quads, rotation, dimensions,
  rain opacity and eight-phase moon UV ordering. The original sun and moon atlas
  assets render before terrain with additive blending, no depth writes and no fog.
  The shared shader preserves faint sky alpha separately from terrain cutout.
  SkyColour extracts Level sky brightness/weather/lightning arithmetic and the
  original colours.xml palette. Client sky and provisional distance fog now follow
  the camera biome and time. World time advances at 20 ticks/second during play,
  pauses outside play, and persists in LevelData. This does not implement sleeping,
  weather progression, scheduled simulation, stars, sunrise gradients or full fog;
  the volumetric cloud path remains pending. CPU checks cover colours, celestial geometry,
  rain and phase boundaries; windowless GPU readback checks faint-alpha handling.

- BiomeTint extracts the original nine-neighbor integer channel average and uses
  original Grass/Foliage/Water palettes. Spruce and birch retain their metadata-based
  colour overrides. Terrain tops/leaves/water now use these colours; ordinary water
  retains its texture colour and alpha instead of an extra blue/opacity multiplier.
  Biome samples include the saved/generated halo across client and chunk edges.
  Fancy grass-side overlays and complete foliage render shapes remain pending.

- Superflat base generation ports FlatLevelSource::prepareHeights (bedrock, two dirt
  layers, grass, then air) and Dimension's fixed plains biome. Create World now
  offers Default/Superflat; generator metadata, neighboring flat chunks, spawn and
  save reload are connected. Layer/biome checks and actual client save/halo tests
  pass. Flat villages, structure postprocessing and full console world extent are
  still pending. Default terrain decoration is unchanged by this addition.

- Basic clouds now use LevelRenderer's 32-block patches, 512-block coverage,
  2048-block texture repeat, .03-block-per-tick drift, 128.33 world height and .8
  vertex opacity with the original cloud PNG. Level cloud tint arithmetic supplies
  day/night, rain and thunder colour. Negative-coordinate wrapping and UV drift are
  tested without displaying a client. This replaces the former scattered white
  rectangles; advanced volumetric cloud geometry and options integration remain
  pending. Tests also decode the original sun, moon and cloud assets.

- ConsoleSeed adapts CreateWorldMenu numeric/text seed handling: nonzero decimal
  integers are used directly, while text and numeric zero use the original signed
  32-bit Java UTF-16 hash. Native guards reject overflowing numbers, lone minus and
  input longer than the original 60-character keyboard limit. The host keyboard
  currently accepts printable ASCII; the numeric/hash API handles UTF-16 units.
  Empty input starts original BiomeSource seed selection over a 200x200 raw-biome
  area. Fractions, critical biome thresholds, the 15-percent ocean cap, variant
  maxima and nine-type requirement are extracted from the original source.
  One candidate is checked per UI update, preserving the original Random stream
  while allowing cancellation between candidates. No save is allocated until a
  candidate is accepted. A known search history reaches seed -5138114651764625079
  on attempt 84 from entropy 1234; tests cover this full history, parser boundaries,
  strict thresholds and original variant merging. Client builds and targeted
  Release/UBSan checks pass without opening a visible client.

- Legacy chunk loading copies borrowed tag arrays into compressed storage and
  releases them through normal tag ownership, replacing the original manual deletes.
  The Blocks array selects 128 or 256 height; packed data/light arrays must match,
  and heights/biomes must contain 256 bytes. Missing biomes use the original 255
  sentinel. Missing upper sections retain air, zero metadata/block light and full
  skylight. LastUpdate keeps all 64 bits. Both population-flag migrations are retained.
  Writes use the original current-height array layout and Level wrapper, preserving
  unknown inner and outer tags. Arrays are concatenated 128-high sections, not
  interleaved 256-high columns. Malformed coordinates, array sizes and field types
  fail before a record is returned. Entity/tick NBT remains opaque. Legacy End resets,
  old map/profile migrations and live simulation loading are still pending.


## Limits of source comparisons

Reference builds use preserved, hashed originals or mechanically extracted function
bodies. Reference and port share explicit generation-only Level/Biome/block-access
interfaces. This validates transferred algorithms, not the entire original engine
or PS3 SPU implementation.

BiomeRegistry contains original generation IDs, depth/scale/climate and terrain
materials, not the full simulation/decorator Biome class. Tile exposes only properties
needed by current generation fixtures; the complete registry remains pending.
Unknown generation tile properties are rejected instead of defaulting to air.
GenerationRegion requires prepared neighbors and throws on missing chunks. Live
neighbor notifications deliberately throw rather than silently pretending to work;
generation features use doUpdate=false. The explicit-type HugeMushroomFeature
constructor uses live updates and is not supported by this generation adapter.
The original thread-local instaTick flag is carried through SandFeature. Scheduled
queue behavior is separately ported, but its connection to this generation adapter,
onPlace callbacks and falling-block simulation remain pending.
Prepared generation regions now update chunk skylight and run original light checks
after edits when the required neighbors are present. Explicit no-light-check writes
invalidate lighting. Plant brightness queries reject missing light preparation or
an incomplete generated lighting halo.
These comparisons validate placement routines with the adapter, including stable
sand/gravel fixtures; they do not validate original LevelChunk callback side effects.

ChunkStorage tracks the top light-blocking tile for the supported generation registry.
The complete tile registry and full simulation LevelChunk lifecycle still need
porting. Original record serialization is available through the storage bridge,
including full-height light restoration and explicit rebuild requests. The lighting cache,
PS3/SPU paths and complete chunk lifecycle remain pending. The current terrain
renderer now samples the stored light through the original console lightmap.
ChunkStorage is now the client's backing store. NBT and LevelData are connected to
its inner-archive saves; this is not a completed PS3 save importer. Original chunk/map
loaders still need their array ownership transfers adapted before they can use the
new tag ownership contract. The complete Player and save-container services are not
implied by the Abilities and metadata port.

Nether and End base terrain are now separate compiled generation libraries. Neither
is connected to the client or dimension transitions. Nether fortresses, springs,
fire/glowstone decoration and the original postProcess sequence remain pending;
End podiums, obsidian spikes/crystals, dragon spawning and biome decoration remain
pending. Live setTile callbacks and fluid ticks must be ported before their features
can execute with the original behavior. Passing base terrain comparisons does not
establish complete dimension generation or visual parity.

## Next source boundaries

1. Finish the complete original Tile registry, lighting semantics and metadata
   integration. Port remaining features and the exact BiomeDecorator and
   RandomLevelSource::postProcess order, including structures, fluid ticks and
   source random consumption. Extend the finite-world streaming adapter to
   original simulation/entity activation and per-chunk rendering.
2. Port simulation Level/LevelChunk, block registration, scheduled updates,
   entities/player state, inventories, recipes and survival interactions.
3. Extend the connected PS3 inner-save storage to the full simulation loader. Port
   PlayerUID, legacy map/profile migrations, file/resource streams, save scheduling,
   map/player loading and Sony packaging. The client now writes `world.inner` and
   migrates earlier prototype `.mcp` files without overwriting them.
4. Connect original rendering, lighting, meshes, animation, HUD, crafting and tutorial
   logic. Current menus are scaffolding, not the Iggy UI. Find actual packaged PS3
   title/controller artwork before claiming visual parity.
5. Replace platform audio, profiles, split screen, network and storage services.
   Sony/4J runtime dependencies and online services require working replacements.

## Verification

The earlier 76-test nonvisual suite passed in Release after the tutorial world,
stair and slab integration. The current decoration batch has targeted Release
coverage for generation, streaming and tutorial world loading; a consolidated
full-suite result is pending. Tutorial loading, schematic storage, client storage,
lighting and stair checks also passed under Debug/UBSan before this batch. See the
September 13 entry below for scope and remaining limitations. The preceding streaming test passed under UBSan (63.79 seconds),
using a 180-second harness timeout for its full traversal. Coverage includes source samples, biome query
seams/concurrency, chunk order/concurrency/finite edges, carvers, packed data, original
feature output, all material properties, host collision and save behavior (including
96/128-high prototype save migration), binary streams, NBT, world metadata, compressed
chunk storage, region allocation, archive layouts, legacy chunk rewrites, PS3 inner-save integration,
the Nether/End base terrain stages, flat/compressed storage integration, full-height
relighting, atomic native persistence, scheduled-update ordering/queue/NBT behavior
and original collision geometry/ray traversal with client selection integration.
Client storage tests cover 256-high edits, metadata, lighting across chunks, native
save reload, retained unloaded/dimension/opaque data, failed-save retry, FIFO
rejection and transactional rejection of unsupported central blocks or game modes.
Hunger, food saves, original player food permissions and experience state also pass
their behavioral and source comparisons. Console lightmap/propagated sampling, real
terrain mesh lighting and offscreen GPU shader checks also pass. All 438 source/asset imports pass
their recorded SHA-256 checks.

AddressSanitizer builds but its runtime stalls after logging
`AddressSanitizer: libc interceptors initialized`, including outside the former
sandbox. This remains an unresolved validation limitation.

The new shared shaders also pass numeric checks in a windowless OpenGL context.
Earlier window checks passed for the initial prototype. None have been rerun or
shown after the no-preview instruction. Existing screenshots are historical and do
not depict current terrain. Controller hardware, complete-game testing and final
visual comparison remain outstanding.


### Metadata-dependent terrain textures (September 11)

The original TreeTile, WoodTile, SandStoneTile, ClothTile and LeafTile texture
selection bodies now run through a reproducible adapter. Atlas locations are
extracted from the original PreStitchedTextureMap registrations. Terrain mesh
construction passes stored metadata into these selectors, preserving all four
wood species, log end faces for three axes and all-bark logs, spruce/jungle leaf
textures, sixteen wool colours and three sandstone variants. Chiseled and smooth
sandstone use the original top texture underneath. Invalid plank/sandstone types
retain the source fallback; leaf decay flags do not change species.

This ports texture selection, not the complete TileRenderer: fancy grass overlays and the full creative item catalogue remain outstanding.
Directional bark UV rotation and placement orientation were added in the next batch. The renderer currently uses fancy leaf textures.
The new test covers 480 metadata/face combinations and checks vertex UVs from the
actual terrain mesh for five stored variants. No visible client or preview was
opened.

Texture, liquid reaction and native client storage tests pass in Release and UBSan.

All 69 nonvisual tests pass in Release and UBSan after the metadata texture changes.


### Original log placement and cube face UVs (September 11)

The client now calls the original PistonBaseTile::getNewFacing rule used by
TreeTile::setPlacedBy. The adapter maps native feet coordinates and radian camera
heading to original player coordinates and yaw. Close-range above/below tests,
rounded compass quadrants and species-preserving log metadata follow the original
bodies. Rejected colliding placements restore replaced water metadata as well as
its block ID. This remains a bridge into the host creative placement flow; the
complete original BlockItem/Player interaction pipeline is not yet ported.

Six original TileRenderer face routines now supply normalized full-cube UVs.
The original tesselateTreeInWorld rotation flags orient bark along each log axis.
Original quad vertices are mapped by position to native mesh corners; the existing
atlas inset remains. Cube top/bottom orientation now follows the original source.
This adapter deliberately covers full cubes and the non-AO vertex branch, not
partial block shapes, full ambient occlusion, or a complete TileRenderer port.

Behavioral tests cover compass rotations, vertical thresholds, species preservation,
placement collision rollback, UV corner coverage, bark direction on all three
axes and actual terrain vertex UVs. Both new adapters reproduce from their source
imports. No client window or preview was opened.


### Playable finite-world chunk streaming (September 11)

The fixed client boundary is replaced by a moving resident window. Player motion
requests incoming chunks before reaching its edge; each update restores or generates
at most two missing chunks while the current window stays playable. Once ready,
the window shifts without rebasing player/world coordinates, lighting is rebuilt
with a two-chunk halo, and the renderer uploads the new region mesh. Signed floor
division now maps negative client positions to the original chunk coordinates.
Collision, block picking/placement, metadata, biome tints, sky colour, liquid mesh
access and snapshot indexing all use the active origin. The original 54×54-chunk
world is fully reachable (864×864 blocks), with collision retained at its outer edge.

ServerChunkCache::create/load/save was inspected and imported as the lifecycle
reference: restore from storage before invoking the original generator, and preserve
chunk state before unloading. The desktop's two-chunk-per-update scheduling and
bounded moving window are native platform adaptations, not a claim that the whole
console server cache or entity lifecycle has been ported. Each newly generated chunk
runs the existing original biome/density/surface/carver pipeline with saved seed,
world size and generator type. Superflat continues using the original flat layers.

Outgoing chunks are captured in PS3WorldStorage before eviction, including original
opaque NBT fields and edits made while a new window is preparing. Overlapping chunks
stay resident. Disk save retains both active and evicted chunks, unknown data and
other dimensions. Uncommitted incoming chunks need not be saved because they can be
restored/generated again. Unsupported incoming saved blocks reject the transition
without replacing the active window. Loaded chunk storage remains at most 144 chunks,
plus a bounded incoming batch; the compressed archive grows with explored terrain.

Nonvisual tests cross the former boundary, visit both signed coordinate directions
and all finite edges, return to edits after eviction, save during preparation and
far from spawn, reload distant edits, preserve opaque/other-dimension data, sample
cross-chunk lighting and check actual streamed mesh positions. A full streamed chunk
is compared cell-for-cell with the original generator. Original entity ticking,
player-position persistence, decoration/structures and tutorial content remain
pending. Lighting/whole-region mesh rebuilds can still stall a transition frame;
background generation and per-chunk GPU updates are further performance work.

Streaming validation: 71/71 Release tests passed; the streaming traversal, native
storage and block texture checks pass with UBSan halting on the first error.
All 306 source/asset manifest hashes match. No visible client was launched.


### Original tutorial package and structure data (September 12)

ConsoleLzx now decodes the framed XMem LZX streams in the supplied tutorial assets,
using the original 128-KiB window and a vendored, unmodified libmspack LZX decoder
(commit 55d501976171397ccd5d5a7a1ca7da065b1d9a06; LGPL-2.1, source/license and hashes
in third_party/libmspack). The wrapper validates frame sizes and output capacity;
the existing original console RLE adapter restores final block/data bytes.

ConsoleSchematic ports the version-1/version-2 schematic layout, supports raw, RLE
and LZX/RLE payloads, retains original entity NBT, and copies unrotated block/data
columns into intersecting 16×16×256 chunks. All 19 loose supplied schematics decode,
including both castle sections. Original x/z/y ordering and packed metadata are
preserved across negative chunk coordinates and the 128-height boundary. This is
the block/data phase: callers must rebuild heightmaps and lighting afterward.

ConsoleGameRuleFile reads original GRF versions 0/1, the string table, embedded
schematics, attributes and recursive rule nodes. The actual Tutorial.pck contains
17 embedded schematics and two root definitions (MapOptions and LevelRules). Its
compressed payload includes 35,896 zero padding bytes after the parsed rule tree;
the reader permits zero padding but rejects nonzero tails. Root/child/string/file
counts, recursion, compression sizes and truncation are bounded. Definitions are
decoded, not yet executed as live tutorial objectives.

TutorialSchematics now loads directly from Tutorial.pck. Its 17 active placements
match the supplied GameRules.xml in order, names and coordinates, including original
even-coordinate normalization. The generated XML layout serves as a reproducible
reference. Seed 1227750481513469519 and spawn (97,72,-106) are checked, including
the castle data at the spawn location. Chunk placement leaves unrelated terrain
untouched. The nonvisual console_tutorial_inspect tool inventories block IDs in the
active structures; docs/tutorial_block_counts.csv records this prerequisite set.

Schematic entity records are deep-copied and translated to world coordinates, with
original chunk ownership rules and the entity-only 0.01 epsilon. Chest/sign/block
entity payloads remain intact; hanging entity TileX/Y/Z and entity Pos are shifted.
Actual supplied records are assigned exactly once across their schematic chunks.
This preserves records for future instantiation; it does not implement live mobs,
painting placement effects, chest menus, inventories, or entity ticking.

The tutorial is NOT yet playable. This batch does not enable a misleading menu
entry or substitute an approximate world. Remaining integration includes the full
required block rendering/collision/interaction registry, entity/container creation,
forced features and structure actions, tutorial objectives and text. Tutorial
creation/spawn/save identity is now connected to streamed chunks (September 13). No game window or
preview was opened.


### Tutorial world storage, stairs and slabs (September 13)

`World::generateTutorial` loads the original package and all 17 active schematic
placements over original seed 1227750481513469519 terrain. Its original feet spawn
(97,72,-106) maps to client coordinates (161,72,-42), with a movable window centered
on that region. Original null-collision plants no longer block the spawn flower.
The tile property registry now retains all 53 previously missing active tutorial
IDs, plus loose-schematic snow and three additional original stair registrations.
These properties cover material, opacity and emission, not complete tile behavior.

Tutorial schematics are applied before heightmap/light initialization on newly
generated chunks. Native archive records retain block/entity NBT; saved records
prevent reapplication over edits after eviction. The native bridge archive entry
`console_port.tutorial.pck` preserves the original package for self-contained
reloads; this marker is an adapter extension, not an original PS3 save field.
Tests compare the complete original castle spawn column and exercise distant
streaming, bounded residency, edited metadata and disk reload. The loader still
uses the client's creative game mode; original survival/tutorial rules are pending.

`tools/extract_stair_shape.py` extracts StairTile's original base, corner, lock and
upside-down box algorithms plus HalfSlabTile's height selection. The native player
collision path and terrain mesh consume these boxes. All ten stair registrations
use original base material/species textures; stone slabs use their eight source
texture variants and double-slab top-only flag. The existing extracted TileRenderer
face routines now accept box bounds, preserving clipped rather than stretched UVs.
Inset surfaces remain visible beside full neighboring blocks. Stair picking keeps
original StairTile's six-octant rule (including its distinction from corner
collision), with owned temporary hit results; slab picking uses the selected half.

Nonvisual fixtures cover orientations, inversion, mixed-material corners, lock
neighbors, player contacts, empty-space picking, actual mesh vertices/UVs, inset
face culling and stair/slab metadata surviving tutorial eviction and save/reload.
All 415 manifest entries match both their copied bytes and supplied source SHA-256.
No visible client or preview was opened.

The subsequent entry below enables creative exploration through the menu. Fences,
plants and remaining specialized renderers/collisions, original stepping and full movement physics,
container and entity instantiation, the 22 chest/one spawner structure actions,
forced features, tutorialDiff, lesson progression and original HUD/menu integration
remain incomplete. Passing these tests establishes these data and shape paths,
not full-game or visual parity.


### Tutorial exploration entry, doors and ladders (September 13)

Play Game now includes **Tutorial World**, which allocates a separate local save,
loads the actual tutorial package, positions the player at the original spawn and
converts UpdatePlayerRule's 81.59-degree heading to the native camera convention.
The original terrain and schematic chunks can be explored in the current creative
client with walking, jumping, flight, mining, placement, streaming and save/reload.
The entry identifies this as creative exploration; it is not the scripted tutorial.
World-list pagination was adjusted to account for the extra entry.

DoorShape extracts DoorTile's original composite metadata, three-pixel bounds,
open/hinge orientation and upper/lower/flipped texture selection. Both halves use
the same state for mesh, collision and picking. Right-click/L2 consumes door use;
wooden doors toggle the lower open bit from either half, while iron doors retain
the original hand-use restriction. Held use does not repeatedly toggle the door.
Creative mining removes the matching other half. Open state and hinge survive
chunk eviction and native save/reload. Redstone activation, sound events, support
updates and survival drops are still pending.

LadderShape extracts LadderTile's two-pixel collision boxes, TileRenderer's offset
ladder quads and Mob's ladder velocity clamp. The native player adapter recognizes
ladder/vine feet blocks, limits falling and lateral speed, supports sneak holding,
and climbs on horizontal collision. Original per-tick velocity constants are
converted to seconds for the existing native movement loop. This is an integration
adapter; it does not replace the remaining native movement/gravity with full Mob
physics. Vine rendering and attachment behavior remain unfinished.

Nonvisual checks cover all door material/direction/open/hinge combinations, both
halves' texture choices, collision and picking through open passages, mining both
halves, ladder bounds/quads/velocity limits and actual tutorial save persistence.
The client builds and the complete 75-test Release suite passes. The affected
shape and tutorial integration tests pass under Debug/UBSan. All 418 original
imports pass SHA-256 verification; generated shape adapters reproduce exactly.
No visible client or preview was launched, so interactive and visual QA remain
unperformed under the user's current restriction.

Remaining tutorial work includes usable chests/inventories, original entities,
22 placed chest actions and the spawner action, scripted lessons, forced features,
tutorialDiff, remaining specialized block rendering/behavior and original survival
HUD/gameplay. The map's creative exploration path is available; the full console
game and original guided tutorial are not complete.

The exploration batch also imports SmoothStoneBrickTile's original four texture
selectors. Castle stone brick is now pickable/editable and its metadata selects
normal, mossy, cracked or chiseled atlas entries in the actual terrain mesh.


### Tutorial chest contents and native container interaction (September 14)

The runtime tutorial rule adapter now reads all 22 PlaceContainer actions from the
actual package: 21 Overworld chests and one Nether chest. Only the Overworld actions
are applied to this client's active dimension. Coordinates remain exact (they do
not use the schematic even-coordinate alignment). Chest block/facing and tile NBT
replace preexisting schematic chest records at those action locations. Source item
slot order, counts, auxiliary values, 4jdata markers, ordinary enchantments and
StoredEnchantments on books are retained. All twelve tagged discs are represented
in the definitions; one remains in the unconnected Nether. This is the supplied
orientation-zero/zero-local-offset action subset, not a general structure executor.
Spawner actions and the rest of the structure/rule executor remain pending.

Right-click/L2 opens a chest interface. Arrow keys/D-pad or the mouse select slots;
Enter/Cross moves a whole stack to the first empty slot in a 36-slot carried
inventory, or returns a carried stack to the chest. Esc/Circle closes it. Full
inventories and empty-source retries leave item contents unchanged. Copying whole
NBT records preserves unknown tags, enchantments and collection markers. This is a
native container adapter: stack merging/splitting, drag operations, double-chest
combination, player hotbar/equipment and item-use mechanics are not ported yet.
The carried inventory is separate from the existing nine-block creative palette.

Carried item NBT is persisted in the native archive extension
`console_port.inventory.dat`; chest contents remain in original chunk TileEntities.
Both are saved together by the existing transactional native save path. Loading
validates the carried slot list before swapping world state. Chest edits retain
chunk context during streaming. Mining a chest removes its stored contents so
putting a chest block back cannot resurrect the original loot. Creative destruction
does not spawn item drops. Opaque blocks above either adjacent chest half block
opening; cat obstruction, support behavior, redstone and full tile-entity ticking
remain separate work. Earlier tutorial saves are not refilled or overwritten;
create a new Tutorial World to receive the newly integrated placement actions.

The original closed single ChestModel now supplies lock, body and lid geometry.
Cube/Polygon face/UV conventions and ChestRenderer facing transforms are adapted
to the shared CPU mesh; the lid retains the source polygon offset to avoid fighting
with the body. Chest collision bounds use the extracted ChestTile neighbor rules.
The texture comes from the supplied archive's Common/res/1_2_2/item/chest.png under
PS4_GAME, with its precise path/hash in the manifest; byte identity to the packed
PS3 texture is not claimed. Animated lids and the large-chest model are pending.

The container screen uses the untouched PS3 Media/items.png and icon registrations
extracted from Item, RecordingItem registrations and DyePowderItem selectors.
English captions are a native fallback derived from source description identifiers;
full localization and metadata-specific icons beyond the added dyes remain pending.

Tests cover the package's 22 actions and dimension split, twelve disc markers,
books, actual placed loot, source/destination conservation, unknown NBT, blocked
lids, full inventories, save/reload, mining, model bounds/facing and icon indices.
The 76-test Release suite and affected Debug/UBSan checks pass. All 435 imports
match their original/copy SHA-256 values; generated icon and collision adapters
reproduce exactly. No visible client or screenshot was opened. Scripted tutorial
lessons, usable carried items, entity simulation and complete visual parity remain
unfinished; these checks do not establish full console-game completion.


### Combined double chests (September 14)

Adjacent chest halves now expose a single 54-slot container. ChestTile's original
west/north-first ordering and CompoundContainer's slot routing are preserved, so
opening either half shows the same slot order. Each half retains its own 27-slot
NBT record, including when the pair crosses a chunk boundary. Whole-stack deposits
search the first half before the second. New empty chest blocks initialize a
source-format chest record on first deposit. Illegal clusters larger than a pair
are rejected by the native interaction adapter.

The screen adapts between three and six chest rows with consistent mouse, arrow
key and D-pad selection. Blocking either lid prevents opening the pair. Mining one
half discards only that half's contents and leaves the survivor as a single chest.
The native renderer now uses LargeChestModel's 30-pixel boxes, 128x64 UV layout and
ChestRenderer's direction-specific offsets, drawing only from the north/west half.
It retains separate body/lid passes and the source polygon offset. The large chest
texture is imported from the same archived Common/res/1_2_2 resource family as the
single chest texture; packed PS3 texture identity is not claimed. Closed poses are
implemented; lid animation remains pending.

Nonvisual tests cover both opening sides, second-half transfers, a full first half,
blocked neighboring lids, a pair spanning two chunk records, streaming eviction,
save/reload, removal of one half, first deposit into a fresh chest and actual large
mesh output with all four directional transforms. The full 76-test Release suite
passes, with affected container/tutorial checks under UBSan. All 438 imported files
match their recorded source and copied hashes. No visible client was opened.

Item use, stack merging/splitting, hotbar/equipment integration, guided tutorial
lessons and the remaining game systems are still unfinished.

### Stack transfers and fence family (September 22)

Single and combined chests now move whole, half and single-item quantities in both
directions. The native adapter applies item-specific 64/16/1 limits extracted from
the original registrations, merges compatible stacks before empty slots, and compares
the complete retained item NBT so damage, enchantments and custom tags stay distinct.
Transfers are transactional and persist in the existing chunk/player extension data.

FenceTile and FenceGateTile collision bounds and TileRenderer cuboids now drive the
playable mesh. Oak and Nether fences connect to matching fences, gates and full solid
neighbors. Gate open state changes collision, rendering and selection consistently.
The creative inventory exposes these blocks, replacing the active hotbar slot on
selection; gate placement retains the original four-way player heading and solid
support check. Gates in tutorial structures can be opened with the normal use input.

The client builds and all 78 Release tests pass. Targeted tutorial-world, container,
stack-transfer and fence tests pass under Debug/UBSan. The import verifier
matches all 531 recorded source and asset copies. No visible client was launched.

### PS3 tutorial package provenance (September 22)

The playable tutorial now embeds the supplied PS3Media/Media/Tutorial.pck, which
uses PS3 EdgeZLib raw DEFLATE followed by the original byte-run decoding for its
GRF and schematic entries. The parser accepts the package's one zero alignment
byte while still rejecting malformed or oversized streams. Earlier saves with the
generic LZX tutorial package remain readable through the existing decoder.

The PS3 and generic packages contain identical rule trees, all 17 placement
coordinates, and voxel/metadata content for every embedded schematic. A regression
test also compares the PS3 package's structures and NBT records with the supplied
loose schematic assets. This package correction alone does not alter the castle or
surrounding terrain geometry. The missing original terrain post-process and biome
decoration remain a visible tutorial-world fidelity gap.

### Partial natural decoration in new worlds (September 23)

New Default and Tutorial worlds now apply the ported ore, shore-deposit, tree and
biome-foliage subset before tutorial schematics. Each target chunk is decorated
against untouched generator output from a five-by-five neighborhood. This makes
the result independent of exploration route, prior decoration and player edits in
neighboring chunks. The result alone is copied into the incoming window; imported
and player-edited saved chunks are never redecorated. New, undecorated halo chunks carry an explicit save
marker until they first become visible, so save/reload does not skip their feature
pass or overwrite existing player work.

Streaming generates raw neighbors and decorates at most one visible chunk per
update. Incoming decoration is staged until the window commits, leaving the active
light region intact for edits during loading. The two lighting passes then run in
bounded batches. Release tests cover route-independent signed coordinates, both
finite-world edges, edits during decoration, tutorial structure overlap, save/return
and native storage. A nonvisual local profile observed a 13 ms maximum streaming
call and 5 ms edit during decoration; interactive frame timing remains untested.

This is a partial source-derived feature pass, not exact original terrain
post-processing. The larger structure generator and some source random
consumption remain missing; detached plant checks use approximate daylight until
the final world lighting pass. Previously saved chunks retain their existing terrain.

The native LakeFeature now runs in the original water/lava positions ahead of
biome decoration, and SpringFeature places enclosed cave sources after foliage.
The source's immediate flowing-liquid tick remains pending. Flowing and still liquid IDs both survive archive validation and
share terrain-mesh seams. Naturally generated grass, flowers, reeds, mushrooms,
saplings, vines and waterlilies use cutout geometry, while cactus and cocoa render
inset shapes. Their original atlas slots and metadata survive save/reload.
The client builds and all 80 nonvisual Release tests pass after this addition;
no visible client was launched. The imported manifest now verifies 535 files.

Cold biomes now run the original shifted post-process snow/ice scan after biome
decoration. The generation adapter ports the source rain-height and snow
eligibility checks. New snow layers render at their two-pixel starting height,
can be picked, and use the original no-collision rule until a pile reaches
half-block height. Frozen water and snow persist through the block whitelist.
Desert and Desert Hills also run their rare well feature, while both Extreme
Hills biomes place source-count emerald ore with its native atlas tile.
Tutorial schematics now run at the source post-process point between biome
decoration and snow/ice. Lighting setup no longer reapplies them afterward;
their chest and spawner records remain attached to the decorated chunks.
The larger structure generator, structure overlap rules and final tutorial
visual parity still need work.

RandomLevelSource now makes the original eight dungeon attempts after lakes and
before biome decoration. The MonsterRoomFeature port checks stone floor/ceiling
and cave openings, carves the room, builds cobble/mossy floor and walls, places up
to two loot chests, and records its Skeleton/Zombie/Spider spawner. Chest loot
uses the source item rolls, including enchantment ID and level for enchanted
books. Generated chest and spawner NBT is attached only where the finished chunk
still contains the corresponding block, and survives the native save and
streaming path. The live spawner and entity subset is described below; full
creature simulation remains unfinished.

### Scheduled fluids and first entity activation (September 23)

The visible world now advances flowing water and lava on the 20 Hz world clock.
The detached LiquidTileDynamic pass follows the source depth, falling, source
renewal, lava slowdown, and four-direction slope rules. Water waits five ticks;
overworld lava waits thirty. Neighbor edits wake still liquid, while the existing
water/lava reaction pass handles cooling. Work is limited to 64 due fluid updates
per client tick; remaining work stays queued. New visible chunks activate only
their own dynamic liquid cells. Native `TileTicks` records preserve pending fluid
deadlines through save and reload, while other tile tick records remain opaque.
Generated spring source blocks can now enter the active flow queue when their
chunk becomes visible. Exact generation-time immediate ticks and every block's
neighbor callback remain incomplete.
The renderer now completes a mesh snapshot after at most two restarts under
continuous fluid edits, then begins a fresh pass; water movement cannot starve
the visible mesh indefinitely.

Loaded MobSpawner tile records now advance only when a player is within the
source 16-block radius. Their saved delay, entity ID, spawn count, local cap and
global cap feed a first live entity list. Spawned entities receive basic gravity,
distance despawning and native per-chunk `Entities` NBT persistence, including
streaming eviction and reload. The later visible-mob section describes the
native-record activation and rendering added after this first simulation pass.

### Visible source mobs and bounded natural spawning (September 23)

The renderer now builds the original `HumanoidModel`, `SkeletonModel`,
`SpiderModel`, `CreeperModel`, `QuadrupedModel`, `CowModel`, `PigModel`, `SheepModel`,
`SheepFurModel` and `ChickenModel` boxes with `Cube` face UVs from 64×32 skins. The skins
come from the supplied console asset tree and are recorded by SHA-256 in the
source manifest. The same lightmap and fog shader as terrain shades live mobs.
The source model's mirrored limbs, spider leg poses, cow horns/udder, pig snout,
chicken wings and separately tinted sheep fleece are included; walking limbs
respond to entity motion. Model boxes, UV ranges, heights, body yaw, lightmap
coordinates and model counts have nonvisual tests.

Loaded native `Zombie`, `Skeleton`, `Spider`, `Creeper`, `Cow`, `Pig`, `Sheep` and `Chicken` NBT records now appear in the
live entity list, including on first creation of the tutorial world. Native
records keep their complete original NBT when the desktop save is written;
active native mobs now update position, motion, rotation, health and hurt timers
without discarding unrelated archive fields.
Generated and spawner-created mobs use the existing compact per-chunk NBT path.
Native sheep retain their sheared state and source dye index for rendering.

A bounded adaptation of `MobSpawner::tick` chooses positions in loaded interior
chunks, checks floor clearance, the source 24-block player and spawn exclusions,
and `Monster`'s dark-light tests. It currently chooses the four rendered
enemy types and caps hostile natural spawns at 30. A separate 40-tick friendly
pass uses `Animal`'s grass and daylight-brightness condition and the source's
common sheep/pig/cow weights plus chickens; sheep draw source color odds.
Basic horizontal steering, bounded random strolling and
substepped collision keep those mobs moving without walking through solid
walls or falling through a one-block floor. Source target acquisition excludes
the currently invulnerable creative player. Day/night,
native-NBT preservation, sheep dye/fleece, wall collision and open-space movement are checked.
Biome-specific species selection, exact console caps, complete navigation,
projectile attacks, creeper fuses/explosions, loot drops, passive behavior, natural
aquatic spawning, sounds and effects remain to be
ported. This remains a partial playable integration, not full game parity.

### Tutorial hanging artwork (September 23)

The imported tutorial package contains 11 paintings and 28 item frames. Native
`Painting` and `ItemFrame` NBT now activates into the resident world and leaves
the original records intact on save. The source `HangingEntity` tile position,
direction and legacy `Dir` migration set their placement. Painting geometry
uses all 26 `Painting::Motive` dimensions and atlas offsets, the original
six-face 16-pixel segment layout and the supplied console `art/kz.png`.
Each segment samples light from its source wall position, as in
`PaintingRenderer::setBrightness`.
Item frames use the source 12-pixel wood rim, inset back, rotation field and
saved item ID/damage to draw displayed icons from the imported atlases.
Nonvisual tests cover motif geometry, UVs, placement directions, item rotation,
lighting, tutorial NBT activation and eviction without duplication. The full
`ItemFrameRenderer::drawItem` path still needs porting: in particular, framed
blocks use flat icons here, and framed maps, compass animation and item-model
depth are pending. Full tutorial visual parity is still incomplete.

### Tutorial model tiles (September 23)

The tutorial package's five `Skull` tile entities now select the source
skeleton, wither skeleton, zombie, player and creeper skins. The source
`SkeletonHeadModel` cube, 0.1-pixel expansion, wall placements and saved
sixteenth-turn rotations are built into the world mesh. Two ender chests use
the original `ChestModel` boxes, facing metadata and imported ender-chest
texture instead of an opaque terrain cube. The relevant textures are recorded
in the import manifest.

The package also has three tile entities named `Cauldron`; inspection of
their block IDs shows that these are brewing stands (block 117). The source
`TileRenderer` stem, three base pieces and double-sided bottle arms now render
at those locations. Furnace face textures and cauldron (block 118) rim,
interior walls, floor and water-level surface use source atlas slots. Cauldron
and brewing-stand collision now use the original component boxes. Model and
tutorial tests check all five skull types, both ender chests, brewing-stand
meshes, cauldron water height and partial collision. Brewing UI/functions,
skull interactions,
and other block renderers remain pending.

Streaming now reactivates entities from all records entering the visible
window, including records that were already resident in the old lighting
halo. Source record indices prevent duplicate mobs and artwork. A native-save
reload followed by a return to the tutorial's art area verifies that the
skulls and all visible hanging decorations reappear.

The enchanting-table base now uses the original top/side/bottom texture slots,
12-pixel height, collision and picking shape. Its floating book uses the
supplied console `item/book.png` and all seven source `BookModel` parts,
including the page face masks that prevent coplanar flicker. A bounded
per-table tick applies the source proximity opening, yaw tracking and page
motion rules. Mesh/model and source-position tests run without opening the
client. Dynamic book state and page timing are an adaptation of the source
tile entity, not yet proven frame-for-frame identical across world reloads.

### Personal ender chest inventory (September 23)

The source `PlayerEnderChestContainer` uses one 27-slot `EnderItems` list per
player. Both tutorial ender chests now open the existing container screen and
read/write the same personal inventory. Whole, half and single-item transfers
reuse the source-format item records and preserve unknown stack NBT. A solid
block above an ender chest prevents opening. The player's `EnderItems` list is
saved in the port's player-inventory entry and restored transactionally;
placement uses the source horizontal facing rule and creates an `EnderChest`
tile-entity record, which mining removes. Nonvisual tests cover sharing between
both tutorial chests, blocked lids, placement facing, native tile records and
save/reload. Lid opening animation, sounds and server/network behavior remain
pending.

### Furnace cooking and creative supplies (September 23)

The three-slot `FurnaceTileEntity` now runs its source 200-tick cook cycle in
resident world chunks. The port includes all `FurnaceRecipes` mappings, fuel
intervals, charcoal tracking, lava-bucket remainder, output-capacity checks,
and the lit/unlit block swap with the source's level-13 light emission. Native
`Furnace` tile records retain `Items`, `BurnTime`, `CookTime`, and
`CharcoalUsed`. A port-only duration field keeps the progress bar correct if a
save is loaded after the last fuel item was consumed. Placement creates the
tile record and mining removes it. The client opens a furnace screen with
input, fuel, result, progress bars, inventory transfer, mouse, keyboard, and
gamepad controls. The creative inventory now has a smelting-items category so
freshly generated worlds can provide the recipe inputs and fuels; its items
enter the carried inventory rather than the building-block hotbar.

Nonvisual tests cover recipe and fuel values, exact cook timing, blocked
output, bucket and charcoal behavior, native save/reload mid-cook, player
transfers, creative supplies, and tile removal. Furnace sound, particles,
the original full creative tab layout remain pending. Taking a furnace result
now emits source-sized XP orbs with its output-item experience value.
The test also checks both furnace tile entities in the imported tutorial
schematics after generation and chunk streaming.

The held creative slot now accepts item IDs as well as blocks. Its smelting
category includes water buckets and glass bottles, which activate the source
`CauldronTile::use` water rules: a bucket fills to level three, and a bottle
adds a water potion to carried inventory while removing one level even in
creative mode. Cauldrons can be placed from the building palette. Tests cover
empty cauldrons, filling, bottle output, and native water-level persistence.
Rain filling, leather dye washing, and dropped potion entities remain pending.

### Brewing stands and potion formulas (September 23)

The source `BrewingStandTileEntity` uses the native tile ID `Cauldron` under
block 117. All three tutorial records now open a four-slot brewing screen;
new stands create the same tile record, and mining removes it. The port runs
the 400-tick brew cycle, twelve simplified source ingredient formulas,
source potion damage-bit normalization, cancellation checks, and ingredient
consumption. Potion and bottle slots hold one item, and the stand's metadata
tracks its three occupied bottle positions for the existing model renderer.
The creative inventory has a third category with the source brewing items.

Nonvisual tests verify tutorial tile placement after streaming, potion formula
values, a water-to-awkward-to-speed chain, slot limits, native save/reload in
mid-brew, chunk eviction, metadata, and mining. Thrown potions, brewing
particles/sound, statistics, and the complete original inventory layout remain
pending.

### PS3 container artwork and potion layers (September 24)

The client now loads the original PS3 nine-slice panel tiles, brewing stand
illustration, brewing arrow/bubbles, furnace arrow/flame, and their XUI scene
references. Normal item slots use the source `IconHolder` sprite; brewing item
slots leave the stand artwork visible beneath their focus highlight. Furnace and
brewing slot positions, titles, progress art, player
inventory rows, and separate hotbar follow the 1280×720 source scenes at the
client's 640×360 UI scale. The single chest/ender-chest layout follows the
source container scene; large chests use the panel tiles while retaining their
current slot layout. Mouse hit regions follow the same coordinates. Transferring
a potion or bottle now requests a mesh refresh when the brewing stand's occupied
bottle bits change, so its model responds immediately.

`PotionItem` now draws the source atlas's tinted contents under its drinkable
or splash outline. The simplified source's effect colours and fallback base
colour come from the imported `colours.xml`; a focused test covers the values.
Asset hashes and the client build pass, as does the 89-test nonvisual suite.
The hidden OpenGL smoke test also draws the furnace, brewing, and chest screens
without errors. The title menu's button size and positions now follow
`xuiscene_main.xui` while preserving the currently implemented actions. No
visible client inspection has been done, so pixel-level parity, the full
creative inventory and large-chest XUI layouts, and other source menu scenes
remain pending.

### Drinkable potion effects (September 24)

The client now evaluates the eleven simplified `PotionBrewing` effect formulas
from the supplied console source, including redstone duration, glowstone
amplifier, and splash duration adjustment. Drinking a potion takes the source
32 world ticks. The active effects use the source stronger/longer stacking rule
and tick at 20 Hz. Speed and slowness alter walking speed; night vision drives
the existing source-derived lightmap; regeneration, poison, and instant
healing/harming update player health. Creative damage invulnerability is
preserved. Taking a brewed potion from a stand equips it in the current
quickbar slot so the result is usable in this client's current inventory flow.

Player `Health` and `ActiveEffects` now use the source NBT field names in the
native save's player-inventory entry. The focused test covers potion values,
effect rules, drinking, ticking, and native save/reload. All 90 nonvisual tests
and 579 import hashes pass. Splash throwing, potion particle/sound behavior,
full survival health/combat, and the original quickbar/inventory semantics are
still unported. This does not establish full-game or visual parity.

### Source-backed quickbar (September 24)

The client now reads selected items and the nine quickbar icons from the same
player inventory slots 0–8 used by chest, furnace, brewing, and native save
transfers, matching `Inventory::getSelected` in the supplied source. Creative
selection puts one item in a free quickbar slot or merges with a compatible
stack; holding Shift requests the original full-stack quick action. When the
bar is full, the selected slot is replaced. Taking a brewed potion selects its
new inventory slot, or swaps it into the selected quickbar slot if the bottle
was placed in the larger backpack. Placement now uses the selected stack's
damage value, so creative variants are retained.

The fourth inventory-menu category displays all 36 real carried slots and
lets the player select a quickbar slot or swap a backpack item into the active
slot. This gives tutorial loot and smelted/brewed items a direct path to use
without visiting a container. The category still lacks the original cursor
pickup/drag flow and equipment slots.

The container test verifies empty source slots, stack limits, slot swaps,
replacement, invalid requests, and save/reload. All 90 tests, 582 imported
source/asset hashes, and the hidden
OpenGL smoke test pass. The complete console creative catalog, exact controller
inventory interactions, equipment, survival inventory use, and item pickup
still remain to be ported.

### Original creative catalog entries (September 24)

The eight PS3 creative tabs now expose all 423 active `IUIScene_CreativeMenu`
entries, in source order, on 50-slot pages. The extraction reads original
`Tile` and `Item` numeric IDs, metadata constants, and potion masks; a parity
test regenerates the C++ table from the archived source. The separate ninth
tab retains the 36-slot player inventory view. The source's five skull icons
are now resolved from the item atlas, and block icon rendering receives each
catalog entry's damage value.

The source `TilePlanterItem` mapping is present for string, sugar cane, cake,
repeaters, brewing stands, cauldrons, and flower pots. Brewing-stand and
cauldron items from Miscellaneous can place their currently supported blocks.
Other mapped item blocks still await tile simulation, and much of the wider
catalog still lacks exact item behavior and rendering. The menu's tab
art, tooltips, and controller cursor interaction also remain incomplete.

### Creative item names (September 24)

The creative menu and carried-item labels now use the archived English
`strings.resx` with the original `Tile` and `Item` description registrations.
All 259 distinct catalog IDs have source-backed base names. Variant lookups
cover the source wood, leaves, saplings, sandstone, slabs, wool, carpet,
stone-brick, quartz, wall, dye, skull, charcoal, and spawn-egg rules. Egg names
combine the source `EntityIO` name ID with `MonsterPlacerItem`'s label template.
The potion path still composes strength, duration, and splash labels separately.
`extract_item_names.py --check` verifies the generated C++ names against those
references. This improves labels only; it does not make all listed items usable
or visually faithful.

Spawn eggs now also draw both original item-atlas layers with `EntityIO`'s
per-creature color pair from the imported PS3 `colours.xml`. The extractor and
parity check cover all 21 source creative egg variants. Right-clicking a block
with an egg can spawn all 21 creature types in this catalog. Their simulated
entities persist through native saves. Exact source entity AI, mob attacks,
projectiles, spawn-limit rules, drops, and animations remain incomplete.

Zombie Pigman uses the source ZombieModel layout and original skin. Cave Spider
uses the source SpiderModel and its 0.7 model scale. Silverfish uses the source's
seven-segment body and three shell layers with staggered animation. Villager
uses its original 64x64 skin and nine ModelPart cubes. Spawned Villagers select
one of the five source professions and retain it in native saves; the renderer
uses the matching title-update skin. The title-update Zombie skin also replaces
the older fallback asset. Their
textures are imported from the supplied console content. The desktop movement
and pathfinding code remains an approximation of the original mob behavior.
The simulated entities now collide using the source constructor widths and
heights rather than the player's box, including the narrow Silverfish and
short Cave Spider. Projectile attacks, most jump rules, climbing, and many
species-specific goals are still missing.

The creative client now targets the closest visible mob before block mining.
Its ray respects intervening blocks, and source sword/tool damage, strength and
weakness effects, the 20-tick hurt cooldown, excess-damage rule, knockback,
health NBT, and 20-tick death removal run for simulated and imported mobs.
Imported records retain unrelated fields while updating active movement and
health; death animations now survive a save and defeated source records are
omitted after the twentieth death tick. A red hurt pass supplies hit feedback.
Enchantments, item durability, item drops, and
full survival rules remain unported.

Source mob melee damage values, difficulty scaling, the player's 20-tick hurt
window, vertical-overlap/reach checks, the Monster attack timer, and line-of-sight
gating are now represented separately. `Level::getNearestAttackablePlayer`
excludes invulnerable creative players, so mobs neither chase nor attack in the
currently supported creative client. Survival loading is still deliberately
blocked until its inventory, damage, death, and progression paths are complete.
Spider leap, Cave Spider poison at other difficulty settings, projectiles,
neutral-mob anger, and damage mitigation remain incomplete.

The live player state now owns the ported `FoodData` and `PlayerExperience`
objects. World ticks run the source 80-tick hunger healing/starvation timer,
and native saves carry `foodLevel`, saturation, exhaustion, food timer, `XpP`,
`XpLevel`, and `XpTotal`. Loading these fields is transactional; old saves
without them retain source defaults, and unrelated player NBT survives edits.
Creative invulnerability still blocks starvation damage. Food consumption,
exhaustion from movement and combat, and survival UI are not yet wired.

The original XP orb atlas now renders billboards with source frame thresholds,
color pulse, and a boosted light channel. Player kills release source-sized
rewards after the 20-tick death animation; imported `XPOrb` records and newly
spawned orbs move, follow nearby players, respect the two-tick pickup delay,
expire after five minutes of active ticking, and survive native saves. Furnace
result collection also produces source-valued orbs, including fractional
stochastic rounding. Orb movement and player acquisition are still bounded
approximations; breeding, fishing, bottles, enchanting, and other XP sources
remain pending.

The source OzelotModel adds its head, body, two-piece tail, and four legs. The
renderer selects among the original wild, black, red, and Siamese skins using
the saved `CatType`; egg-spawned Ocelots begin in the wild state. Taming and
full cat AI are pending.

WolfModel's head details, body, tail, and four legs now render with the
original wild, angry, or tame skin. Tame wolves use the original translucent
collar layer tinted from the source Sheep color table. Their sitting pose,
saved owner, anger, and collar color are read from native mob records;
creative eggs spawn wild wolves. Taming and wolf goals remain incomplete.

The seven remaining creative egg creatures now use their original skins and
model-part layouts: Slime, Ghast, Enderman, Blaze, Magma Cube (`LavaSlime`),
Squid, and Mooshroom (`MushroomCow`). Slime sizes use the source power-of-two
selection and NBT `Size` field. Mooshrooms use the red cow skin and three red
mushroom crosses. Ghasts use their 4.5-scale source render transform, fixed-seed
tentacle lengths, and obstacle-checked flight targets. Blaze and Magma Cube
use full-bright lighting; Enderman eyes have the source's additive second pass.
Blaze now follows ground gravity with slow falling and target-height adjustment.
Squid movement and tentacles use a water pulse; Slime and Magma Cube use source
jump-delay ranges and different jump strengths. These remain bounded
approximations: projectile attacks, many goals, and exact renderer transforms
are pending. Mooshroom mushroom transforms still need precise source parity.

### Review fixes and survival mode (September 24)

A full review pass (three independent reviewers plus Linux builds under Clang,
GCC, AddressSanitizer and UBSan) found and fixed:

- **Liquids:** turning still water/lava back into flowing liquid reset its depth
  to 0, so every flowing cell became a new source and water flooded without
  limit. The depth is now kept, as `LiquidTileStatic::setDynamic` does.
- **Furnaces** lost their facing every time they lit or went out (`FurnaceTile::setLit`
  keeps the data).
- **Entity persistence:** mobs spawned during play were deleted, unsaved, when the
  streaming window moved away; killed or collected entities loaded from a save
  came back after a window move; saving dropped unloaded simulated entities in
  halo chunks; a restarted eviction pass (or a save during eviction) lost the
  orbs and fluid ticks the first pass had archived. Entities now stay frozen in
  resident halo chunks, activated save entries are flagged, and re-captures
  start from the archived record.
- **Crashes:** the villager librarian texture was never loaded; paintings, item
  frames and enchanting tables outside the window threw from `renderLight`; any
  per-frame exception closed the game without saving.
- **Assets:** six item icons (brick, clay, reeds, brewing stand, flower pot,
  quartz) were read from the terrain atlas; the extractor now reads only the
  item atlas. The item-frame rim uses birch planks (console `Tile::wood`).
- **Compatibility:** stale bytes after the `level.dat` NBT root and
  `originalVersion` 0 are accepted, as the original loader accepts them.
- **Portability/tests:** a missing `<cstdint>` broke the Linux build, and the
  experience-orb test kept a pointer into a destroyed temporary.

Sanitizer notes: on Linux, AddressSanitizer runs. Remaining ASan/UBSan reports
are in the untouched *original* reference builds used for parity comparison
(for example `BufferedOutputStream`'s mismatched delete and leaks in the
original NBT/LevelData code), which the port already documents fixing.

**Survival mode** is now playable (Create New World → Game Mode):

| Boundary | Source and verification |
|---|---|
| Tile destroy times/materials | `tools/extract_survival_tiles.py` extracts all 147 registrations from `Tile::staticCtor`, including stair/wall inheritance and `setIndestructible`. 30 tiles whose class source is absent from the subset use a flagged material fallback. |
| Mining | `Tile::getDestroyProgress`, `Player::getDestroySpeed/canDestroy`, `Inventory::canDestroy`, `Item::Tier`, `DiggerItem`, `PickaxeItem`, `ShovelItem`, `HatchetItem`, `WeaponItem`, `ShearsItem`; the console's removal of the airborne penalty is kept. Client progress follows `MultiPlayerGameMode` start/continue/destroyDelay with the destroy-stage crack. |
| Drops and tool wear | `Tile::playerDestroy/spawnResources`, the present subclasses' `getResource/getResourceCount/getSpawnResourcesAuxValue`, shears/leaf/crop/stem/door/slab rules, `ChestTile::onRemove` container drops and `mineBlock`/`hurtEnemy` durability. Stone, ore, gravel, clay, glowstone, glass, ice, bookshelf, web, snow, huge mushroom, mycelium, sign and wood-slab drops are reconstructed (sources absent). |
| Dropped items | ItemEntity pop/throw velocities, bob, pickup box, lifetime and NBT schema; physics constants follow Java (`ItemEntity.cpp` absent). Saved with chunks. |
| Damage and death | `Mob::hurt` window, `causeFallDamage`, air supply/drowning, suffocation, void, `Player::die` inventory/XP drops; lava/fire ticks follow Java `Entity` (absent). Death screen and respawn. |
| Food | All `FoodItem`/`BowlFoodItem`/`GoldenAppleItem` registrations, eat duration, effects and probabilities; walk/sprint/swim/jump/attack/mine exhaustion. |
| Crafting | Superseded, see "Full source imported" below: all 222 original recipes are now extracted. |
| Persistence | Game type (survival/creative; adventure still rejected), player position, air and fire now save and restore. |

`tests/survival_tests.cpp` covers destroy rates for every tier/material path,
harvest rules, drops, tool wear, food, meshes, and a full world flow (mine,
drop, pick up, place, fall, drown, eat, craft, die, respawn, save/reload).
`tests/world_regression_tests.cpp` reproduces the liquid, furnace and entity
bugs (it fails on the previous code). The hidden-window smoke test also renders
the survival HUD, dropped items, crack overlay, crafting and death screens with
no GL errors. Armour, enchantment effects on mining, sleeping, hunger-bar
shaking, item-stack merging on the ground and the survival tutorial lessons
remain unported.

### Same seed, same world on every platform

Building with GCC showed that caves differed from the Clang build for the same
seed. The original `LargeCaveFeature` passes `random.nextLong()` and
`random.nextFloat()` in one argument list; C++ leaves their order unspecified
(Clang draws the seed first, GCC/MSVC the float), so macOS and Windows builds
carved different caves. The source-parity test could not see it because the
reference is built by the same compiler. All generation code that draws two
random values in one argument list or arithmetic expression (carvers and the
plant/cactus/reed/vine scatter features) is now sequenced in Java's
left-to-right order, and the reference extraction applies the same sequencing.
Clang output is byte-for-byte unchanged, so existing macOS worlds keep their
terrain.

Clang also fuses `a*b+c` into FMA instructions on Apple Silicon, which changed
noise results relative to x86 (verified on x86 with `-mfma`). The build now uses
`-ffp-contract=off`, matching Java's unfused arithmetic. On Apple Silicon this
can change the least significant bits of noise; any resulting block differences
at the border between old and newly generated chunks of an existing Apple
Silicon world are expected to be rare.

`console_*_golden` tests pin SHA-256 hashes of the noise, biome, terrain,
feature, plant, lake and dimension samples, so any compiler or platform that
would build a different world for the same seed fails the suite.

### Full source imported (`source_full/`)

The complete archive sources are now committed under `source_full/`
(`Minecraft.World` 1,560 files, `Minecraft.Client` 2,020). All 603 previously
imported files are byte-identical to it. Everything the survival batch had to
reconstruct is now taken from the source:

- **Crafting:** `tools/extract_recipes.py` compiles the untouched `Recipes.cpp`
  registration code and all seven recipe tables against generated ID stubs and
  dumps `Recipes::getRecipeIngredientsArray()`, the data the console crafting menu
  uses. `src/CraftingRecipes.cpp` now holds all **222** original recipes in menu
  order with their `Recipy::eGroupType` tabs, 2×2/3×3 type and 3×3 layout;
  `console_recipe_extraction` re-runs the extraction and fails if they drift.
  Crafting follows `IUIScene_CraftingMenu`: ingredients are removed one at a
  time from the first matching slot, water/lava/milk buckets leave an empty
  bucket, and the result is added afterwards (dropped when there is no room).
  The menu has the console's seven group tabs (Tab / L1 R1). The original reads
  varargs characters with `va_arg(vl,wchar_t)`, which is undefined behaviour that
  GCC turns into a trap; the extractor reads the promoted `int` instead.
- **Tile materials:** all 147 tile classes now resolve from source; the fallback
  table is gone and every previously reconstructed material was confirmed.
- **Drops:** checked against StoneTile, OreTile, RedStoneOreTile, GravelTile,
  ClayTile, LightGemTile, GlassTile, IceTile, BookshelfTile, MobSpawnerTile,
  WebTile, SnowTile, HugeMushroomTile, MycelTile, SignTile, CakeTile and
  WoodSlabTile. Fixed: ores, redstone ore and spawners now drop experience
  (`popExperience`); redstone ore and glowstone make the extra
  `getResourceCountForLootBonus` random draw (glowstone clamps to 1–4); the XP
  draw happens whether or not the caller wants the amount; locked chests drop
  themselves; ice melts into flowing water.
- **Game modes:** `ServerPlayerGameMode::destroyBlock` wears the tool before
  `playerDestroy` (a tool that breaks no longer gets its special drop).
  `MultiPlayerGameMode` timing: instant tiles break on the first tick without a
  delay, the 5-tick delay follows only completed timed breaks, and the crack is
  stage `(int)(progress*10)-1`. Reach follows `getPickRange`/`GameRenderer::pick`:
  blocks 4.5 (creative 5), entities 3 (creative 6).
- **Dropped items:** `ItemEntity` stacks now merge with matching neighbours, and
  the lava fling runs only on a block crossing or every 25 ticks, as in the source.
- **HUD:** `Gui.cpp` layout (XP bar 8px, hearts/food 18px and air 28px above the
  hotbar), heart shake at low health, Regeneration ripple, damage blink with
  `lastHealth`, food shake without saturation, and the outlined level number,
  which also shows in creative.

### Console tutorial, strings and front-end menus (September 24)

- **Strings:** `tools/extract_strings.py` turns the PS3 `strings.resx` into
  `ported/ConsoleStrings.cpp` (every `IDS_*` in `strings.h` order);
  `tools/extract_descriptions.py` adds the `Tile`/`Item` description and use
  strings. The renderer maps their UTF-8 onto the code page 437 font.
- **Tutorial:** all of `Common/Tutorial` except `TutorialMode`/`FullTutorialMode`
  is imported by `tools/import_tutorial.py` and compiled unchanged against
  `ported/tutorial/TutorialHost.h`, which stands in for the engine classes it
  touches (renamed so they do not clash with the port's own `Level`, `Entity`,
  …) and forwards to `src/TutorialSession` / `TutorialEngine`. `FullTutorialMode`
  itself is `src/TutorialSession.cpp`. The game raises the events
  `Minecraft::tick`, `LocalPlayer`, `MultiPlayerGameMode`, the container menus and
  `ClientConnection` raise on the console, applies input and area constraints,
  freezes the time of day at 8000, restores health/hunger/steak on respawn until
  the food lesson is done, and saves the completion bits and music disc flags in
  the profile. Two changes to the imported code, both undefined behaviour that
  sanitizers caught: `TutorialHint` gets the virtual destructor it lacks (hints
  are deleted through the base class; the two derived destructors the source
  declares but never defines are supplied by the host), and
  `m_bSceneIsSplitscreen`, which `Tutorial::tick` reads but only the Xbox build
  sets, starts false. Allocations the source never frees are listed in
  `tests/lsan_tutorial.supp` for sanitizer runs.
- **Menus:** `src/ConsoleMenus` is the `UIScene_*` front end as data — controls,
  focus rules (disabled options skipped, Controls previews the focused layout),
  checkboxes committed on Back as `handleInput(ACTION_MENU_CANCEL)` does, sliders
  written as they move, the Reset to Defaults and exit/save message boxes with the
  source's button order, How To Play paging, the PS3 credits list
  (`tools/extract_credits.py`) and tooltips. `console_menu_core` tests it.
- **Controller layouts:** `DefineActions`' three PS3 layouts are generated into
  `ported/tutorial/generated/JoypadMap.inc`; the game reads the pad through the
  selected layout and southpaw, the tutorial and the Controls menu show that
  layout's button images, and the in-game tooltips follow `Minecraft::tick`
  (swim up, crafting, inventory, and what Use and Action do to the target).
- **Not exact:** the Iggy movies (`MediaPS3.arc`) are not supplied, so menu
  geometry, fonts and the controller picture are approximations; audio,
  difficulty, gamma, clouds and bedrock fog settings are stored but unused.

### Random tile ticks and weather (September 25)

- `tools/extract_tile_properties.py` writes `ported/TileProperties.cpp`: every tile
  `Tile::staticCtor` registers, with its class, material, solid render, light block,
  light emission, ticking flag and cube shape, worked out from the registration chain
  and the constructor chain (conditions on constructor arguments evaluated). It
  agrees with every hand-transcribed light and solid table the port had. LeafTile's
  constructor reads `allowSame` before setting it (uninitialised in the source); the
  table keeps leaves non-solid, as the port always has.
- `tools/extract_tile_ticks.py` copies the tick, survival and growth methods of the
  ticking tiles unchanged into `ported/tick/TileTickRules.cpp`; they compile against
  `ported/tick/TileTickHost.h`, whose `Level` is the live world (`src/WorldTiles.cpp`).
  `Sapling::growTree` is transcribed because it constructs the tree features; the host
  runs the original features (without the update flag; the level relights as it
  writes).
- `World::tickTiles` follows `ServerLevel::tickTiles`: the tiles chosen on the
  previous tick, the chunk rings out to 9 around the player, the mood-sound, lightning,
  freeze/snow/rain and `checkLight` draws, then the update thread's selection (80
  samples per chunk from a copy of `randValue`, edge clipping, 100 grass and 100 lava
  at most, 256 in all). The source iterates an unordered set of chunks; the port uses
  the insertion order.
- `World::tickWeather` is `Level::tickWeather` with `prepareWeather` on the first tick;
  `rainLevel()`/`thunderLevel()` now ease in and out.
- Farming: `HoeItem`, `SeedItem`, `SeedFoodItem` and `DyePowderItem::useOn` are
  extracted the same way (with `ItemInstance`/`Player` stand-ins) and run from right
  click (`World::useItemOn`, after containers and doors, as `ServerPlayerGameMode::
  useItemOn` does). Farmland (15/16 high, wet/dry top), crops, carrots, potatoes and
  nether wart (`tesselateRowTexture`, four planes a sixteenth low) and pumpkin/melon
  stems (`tesselateStemTexture`/`tesselateStemDirTexture`, coloured by
  `StemTile::getColor`, whose `a&0xFF - b&0xFF` precedence is kept) now render in
  normal worlds, and these tiles are allowed by `World::set`. The in-game tooltips
  show Till, Plant and Grow.
- `console_tile_tick_core` drives each rule on a map-backed level and runs 1,200
  world ticks on a generated world (about 0.9 ms a tick; almost all block changes are
  the existing lava settling).

### Tile updates (September 25)

- The tick `Level` is now in level coordinates and has the source's update
  semantics: the host stores (`setTileAndDataNoUpdate`, running the old tile's
  `onRemove` and the new tile's `onPlace` as `LevelChunk::setTileAndData` does), and
  `Level::setTile`/`setTileAndData`/`setData`, `tileUpdated`, `updateNeighborsAt` and
  `neighborChanged` (with `noNeighborUpdate`) are the source's. `World::set`/`setData`
  stay raw storage; `World::setTileAndUpdate`/`setDataAndUpdate` are the update-aware
  edits, used for breaking, placing (after the collision check), doors, fence gates
  and melting ice. `World::updateLiquidNeighbors` is gone.
- Scheduled ticks: `World` keeps one `ScheduledTickQueue` for every tile (level
  coordinates, the source's 1,000-a-tick limit), replacing the fluid-only map. A tick
  runs only for tiles whose class the port has (`sim::tickPorted`); a chunk's saved
  `TileTicks` for other tiles (redstone, pistons) stay in the record untouched and
  are written back. Ticks in the streaming halo wait (re-queued 20 ticks on) until
  the window reaches them. Flowing liquids with no saved tick are still scheduled when
  their chunk becomes visible.
- Liquids are now the source: `LiquidTile`, `LiquidTileDynamic` and `LiquidTileStatic`
  (flow, slope search, spread, the lava/water reaction, still/flowing switching, the
  `chunkSourceXZSize` edge check) are extracted with the other tile methods;
  `src/FlowingFluidTick.*` (the port's own step) is removed. A generated world's
  flowing water settles in the first few hundred ticks and then stays still.
- Newly extracted: `HeavyTile` (with a `FallingTile` entity: `FallingTile::tick`,
  vertical clipping as `Entity::move`, landing, dropping, `MAX_FALLING_TILE` 20),
  `FireTile` (init/flammability, tick, spread, burn-out, placement), lava's fire
  spread (`LiquidTileStatic::tick`), `TorchTile`, `DoorTile::neighborChanged`,
  `LadderTile`, `SignTile`, `WoolCarpetTile`, `CakeTile`, `FlowerPotTile`,
  `TopSnowTile`, and the `neighborChanged` of `Bush`, `CactusTile`, `ReedTile`,
  `FarmTile`, `CocoaTile` and `VineTile`; `FlintAndSteelItem::useOn`. A broken door's
  upper half drops nothing; the lower half drops the door when its neighbour update
  removes it (`DoorTile::getResource`).
- Fire renders with `TileRenderer::tesselateFireInWorld`, extracted unchanged into
  `ported/TileRender.cpp` (`tools/extract_tile_render.py`). The console's terrain atlas
  holds only a placeholder in fire's slot (15,1); the animated `fire_0`/`fire_1` strips
  and their frame timing files are imported from `res/TitleUpdate/res/textures/blocks`
  into `assets/animations`, like the liquids', and animate into that slot in
  `PreStitchedTextureMap` order (fire_1 last, so it shows). Falling blocks
  draw as their tile's cube (`FallingTileRenderer`).
- Not yet: redstone, pistons, TNT explosions, nether portals, the Fire Spreads host
  option (always on) and saving a block mid-fall.
- `console_tile_tick_core` covers falling sand, torches, doors, fire placement and
  netherrack, still water waking, and through `World`: sand landing, a door's single
  drop, a torch falling with its block, flint and steel, and a pending tick surviving
  save and load. `console_block_texture_core` covers the fire shapes and the falling
  block cube; `console_tile_render_extraction` checks the extraction. 1,200 world ticks
  take about 0.7 ms a tick.
- Torches: `TileRenderer::tesselateTorchInWorld`/`tesselateTorch` join fire in
  `ported/TileRender.cpp` (the extractor is now `tools/extract_tile_render.py`), with
  the torch and redstone torch atlas slots; torches are allowed in generated worlds.
  Placing a torch or ladder uses the clicked face (`Level::mayPlace`,
  `getPlacedOnFaceDataValue`), so it goes on the wall you click and not in mid-air.

### Redstone (September 25)

- Extracted with the tile methods: `RedStoneDustTile` (power strength, corners,
  connections), `NotGateTile` (redstone torches, with the burnout history kept per
  world rather than per `Level*`, since the port makes a `Level` per call),
  `LeverTile`, `ButtonTile` (stone, and wood pressed by arrows), `PressurePlateTile`
  (wood: any entity; stone: mobs and players), `DiodeTile` (repeaters: delay, facing
  from the player, right click cycles the delay), `RedlightTile` (lamps),
  `TrapDoorTile`, `FenceGateTile` and `DoorTile::use`, plus `Tile::setShape` and
  `FenceTile::isFence`. `TilePos` keeps the source's hash in unsigned arithmetic (the
  source's signed multiply overflows).
- Right click runs the tile's own `use` (`World::useBlock`/`usable`, for `TestUse`
  tiles), replacing the port's door and gate code: a gate now opens away from the
  player as in `FenceGateTile::use`. Placement checks `Level::mayPlace` against the
  clicked face and takes `getPlacedOnFaceDataValue` for torches, ladders, dust, levers,
  buttons, plates, repeaters and trapdoors, and repeaters run `setPlacedBy`. Breaking
  calls `Tile::destroy`. Each tick, the player, mobs, dropped items and XP orbs run
  `Tile::entityInside` for the tiles they stand in (`Entity::checkInsideTiles`), so
  plates are pressed. Redstone items place dust (`RedStoneItem`).
- The in-game Use tooltip follows `Minecraft::tick`: chests Open; workbenches,
  furnaces, brewing stands, enchanting tables, anvils, wooden doors, levers, buttons,
  trapdoors and gates Use (the port showed Open for all of them).
- Lamps use their atlas slots; dust, levers, buttons, plates, repeaters and
  trapdoors still draw as cubes until their `TileRenderer` shapes are ported.
- `console_tile_tick_core` drives a lever through dust to a lamp (signal strength
  falling by one per dust, the lamp's four-tick delay), a redstone torch inverting,
  a repeater's delay and delay cycling, a pressure plate under the player, and doors.
