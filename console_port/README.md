# Minecraft console desktop port

An independent C++ desktop port **in progress**, using the user-provided archive at
`/Users/obey/Downloads/minecraft console`. The current executable plays local
**survival and creative** worlds, but it is **not yet a complete port of the
console game** (see `docs/PORT_STATUS.md`).
The archive itself is already C++; the main job is replacing its platform services
and incrementally bringing the original game systems across.

## Run on this Mac

**User requirement:** do not open the client or show previews until the entire port
is complete. Commands below are retained for later use. Current validation uses
the nonvisual test executables.

Double-click `run.command`, or run it from Terminal. It builds the client and stores
its saves exclusively in this folder's `saves/` directory.

```sh
./console_port/run.command
```

Manual build from the project root:

```sh
cmake -S console_port -B console_port/build -DCMAKE_BUILD_TYPE=Release
cmake --build console_port/build -j 6
./console_port/build/minecraft_console --data-dir ./console_port/saves
```

This standalone CMake project does not configure or link MyVoxelGame. It only reuses
the existing vendored **GLFW, GLAD, GLM, and stb** libraries from `../ext`.
No existing terrain, game logic, renderer implementation, menus, shaders, or game
assets are used. CMake, Python 3 (reference extraction), system zlib, and a C++20 compiler are
required; no dependency downloads are needed. Native Apple Silicon macOS with
AppleClang and OpenGL 3.2 is the tested platform; other hosts are unverified.

## What is here

* `original/Minecraft.World/`: untouched math/noise, biome-layer, cave/ravine,
  packed-data, material, placement-feature, memory/primitive-stream, NBT, world-metadata
  player-ability and compressed tile/metadata/light source modules. `original/reference-only/` retains
  broader source files whose full classes are not yet compiled.
* `ported/`: compiled original modules with desktop adaptations. Density/surface
  methods from RandomLevelSource are a separate stage; its full simulation,
  structure, and postprocessing class remains unfinished.
* `platform/`: explicit platform and generation interfaces. These adapters are
  separate from the pending full original Level, Biome, and Tile class ports.
* `src/`: desktop window/input, renderer, UI, host world, collision/raycasting,
  saves, generation interfaces, and chunk storage. A partial original natural
  feature pass now decorates newly generated terrain with lakes, dungeons, ores,
  trees, plants and cold-biome snow/ice; structures and the full source
  post-process sequence remain unfinished. Scheduled water/lava flow and a first
  spawner-backed and bounded natural enemy and animal spawning run on the world clock.
  Zombie, skeleton, spider, creeper, cow, pig, sheep and chicken models and skins are
  rendered, including sheep fleece dye. The full Mob AI, combat and survival
  systems remain unfinished.
* `assets/`: textures, font, button art, and panorama copied from the console archive.
* `docs/source_manifest.json`: original paths and SHA-256 hashes of every import.

The front end is the PS3 `Common/UI/UIScene_*` menus, ported as data in
`src/ConsoleMenus.cpp` (the PS3, single-player, full-version and offline branches of
each scene's constructor and input handler): the main menu (Play Game, Leaderboards,
Help & Options, Minecraft Store), the Start Game list, Create New World and More
Options, Help & Options (Change Skin, How To Play, Controls, Settings, Credits), the
five Settings pages and Reset to Defaults, the pause menu and the death menu. Every
label, description, tooltip and message box is the original text from `strings.resx`
(`ported/ConsoleStrings.cpp`), How To Play shows the 22 original pages with their
button images, and Credits rolls `UIScene_Credits`' PS3 list. There is no PlayStation
Network, so the store says it has no offers and Leaderboards and Change Skin explain
that they are unavailable. The Iggy SWF movies that drew these screens are not in the
supplied files, so panels, positions and fonts are an approximation built from the
supplied button, panel, logo and panorama art. The PS3 loose `MenuTitle.png`
contains an Xbox subtitle; the renderer samples only its common Minecraft wordmark
and draws a PlayStation subtitle with the supplied font.

## Play

Choose **Play Game** for the Start Game list: **Create New World**, **Play Tutorial**,
the port's **Classic Tutorial World** (the archived tutorial save for free
exploration) and your saves. **Create New World** has World Name and Seed fields
(select one and type; Enter finishes), the Survival/Creative toggle, Difficulty and
**More Options** (Superflat World and the other host options; the online options are
disabled offline). Creative asks for confirmation first, as on the console. Leave the
seed blank for the original biome-balanced random search; the search screen can be
cancelled. The console hashes text seeds and numeric zero, so `0` resolves to seed 48.
Numeric overflow is rejected. Select a saved world to load it. Each new world gets its own
`worlds/world-<unique-id>/world.inner`; creating a world preserves earlier worlds.
The old root `world.inner` (or legacy `world.mcp`) is also listed automatically.

| Action | Keyboard / mouse | Mapped gamepad |
|---|---|---|
| Move / look | WASD / mouse | Left / right stick |
| Jump / swim / fly up | Space | Cross |
| Sprint | Left Ctrl | — |
| Toggle flight (creative) | Double-tap Space, or F | Double-tap Cross |
| Sneak / fly down | Left Shift | R3 |
| Mine / place, use, eat | Left / right mouse (hold) | R2 / L2 |
| Select hotbar slot | 1–9 / wheel | L1 / R1 |
| Inventory / building blocks | E | Triangle |
| Crafting (survival) | C, or use a crafting table | Square |
| Drop item / stack | Q / Ctrl+Q | Circle |
| Pause | Esc | Start |
| Menus: move / select / back | Arrows / Enter / Esc, or point and click (right click: back) | D-pad or left stick / Cross / Circle |
| Screenshot | F2 | — |

The gamepad column is controller layout 1. Help & Options → Controls chooses between
the three PS3 layouts (`DefineActions`' `MAP_STYLE_0`–`2`), shows the controller
labels for the highlighted layout, and sets Invert Look and Southpaw; the game reads
buttons through that layout, as the console's `InputManager` does. Gamepads go through
GLFW; physical controller testing is still needed.

Help & Options → Settings writes the profile (`GAME_SETTINGS`, saved to
`settings.dat` with the tutorial progress). In use: in-game sensitivity, Autosave
(off, or every 15–120 minutes; the world is also saved on exit and from the pause
menu), Hints (tutorial popups), In-Game Tooltips, Display HUD and Interface Opacity.
Music and sound volume, gamma, clouds, bedrock fog, difficulty, hand, death messages,
gamertags, splitscreen and UI size are stored but have no effect yet. Focus loss
pauses play. Saves use the PS3 inner archive, region and chunk formats
in `world.inner`, with validated reads and synced atomic replacement on macOS.
The loader accepts supported creative worlds with all central chunks present;
complete PS3 save importing, Sony packaging and simulation remain unfinished.
Unknown archive data, unloaded chunks and other dimensions survive edits.

Older 96/128-high `world.mcp` prototype saves migrate on load into 256-high columns;
the old file remains untouched and the next save writes `world.inner`. The player's
position, health, food, air and experience are saved and restored. Direct executable launches without `--data-dir` use
`~/Library/Application Support/MinecraftConsolePort`.

## Verification

Linux (tested in a cloud container with Clang 18 / GCC 13) needs the X11 and GL
development headers for GLFW (`libxrandr-dev libxinerama-dev libxcursor-dev
libxi-dev libgl-dev`) and `-DGLFW_BUILD_WAYLAND=OFF` unless `wayland-scanner` is
installed. The hidden-window smoke test runs under `xvfb-run`.

```sh
python3 console_port/tools/verify_import.py
ctest --test-dir console_port/build --output-on-failure --timeout 60
./console_port/build/minecraft_console --smoke-test ./console_port/screenshots/smoke
```

The smoke command now creates a hidden window, but is not being run during this
phase. Existing images are from the earlier prototype and do not depict the current
terrain implementation.

The source parity test compiles the untouched modules separately and compares exact
hexadecimal floating-point and integer output across five seeds, bounded random
rejection cases, Gaussian values, negative coordinates, scalar noise, octave noise,
and both 2D and 3D noise regions. Additional comparisons cover biome graphs,
density/surface/carver chunk hashes, and tree/ore/sediment/mushroom/well block output, metadata, heightmaps and remaining
random state. Original light propagation, chunk skylight updates, plant rules and
lake placement have additional comparisons and direct behavioral checks. Primitive
streams, NBT and world metadata also have source comparisons and independent checks
for malformed data, ownership, legacy defaults and mode transitions. Compressed
chunk storage, legacy NBT and version-8 records, RLE/DEFLATE, file tables and region allocation are
verified as libraries. PS3WorldStorage connects them using original dimension names
and the level.dat Data wrapper, preserving unknown save data. It handles the inner
archive and preserves the existing chunk format and container versions. A storage
bridge converts full-height flat chunks to/from both original compressed sections,
preserves opaque NBT and requests light rebuilding when saved heights need repair.
Native file APIs validate and atomically replace inner archives using a synced
sibling staging file. Tests cover failed replacement and concurrent reads/writes.
The original scheduled-update record and ServerLevel queue have verified identity,
ordering, duplicate handling, instant/forced scheduling, processing limits and
chunk extraction. The port also repairs stale saved delays after clock changes and
keeps callback-driven queue mutations safe. An explicit TileTicks codec now snapshots
and restores pending updates through both console chunk formats, with atomic batch
loading and original relative-delay semantics. Real block tick handlers and live
simulation integration still require porting.
Original Vec3/AABB math and block-ray hit construction now compile separately,
with checked collision boundaries and independent thread-local temporary pools.
Player/entity movement and entity-hit handling remain pending.
Client block selection now uses the original Level ray traversal and base Tile
shape intersection, including original face IDs for adjacent block placement.
The adapter still covers only the client's current cube blocks and static fluids;
the complete shape registry remains pending. This integration was tested nonvisually.
Original FoodData and FoodConstants now implement the console hunger/exhaustion,
healing/starvation and saved timer rules through explicit player/item hooks. The
player's host-option, trust, flight and invulnerability guards are also ported.
Tests compare 720 hunger histories and 6,144 player permission cases against the
archive, including healing interruptions and saved-state reloads. The full player,
food-item registry, damage handling and survival loop remain pending.
PlayerExperience also retains the original XP formulas, level spending, death
reward cap and saved progress. Its 360 source histories cover threshold rounding
and reloads; independent tests cover the largest award, score wrapping, malformed
saves and transactional overflow rejection. Orb pickup, death dispatch and the
inventory/enchantment callers still require the full player/entity port.
The playable renderer now uploads the original 16×16 console lightmap and uses
stored sky/block light coordinates in the actual terrain mesh. Caves and enclosed
rooms darken, nearby emission illuminates walls, and source removal updates them.
Original block-light flicker advances at 20 ticks per second while playing.
The CPU formulas also support dimension ramps, lightning and night vision, though
those effect/dimension systems are not yet connected to the client. Water and lava now use the original weighted corner heights and liquid light
sampling, including shallow fluid metadata and gap-free stacked columns. Original
flow vectors now orient the top textures, and side UVs follow surface heights using
the original still/flow atlas regions. The original water/lava frame strips now animate at their console tick durations,
including still lava’s reverse sequence. Accepted block edits now run the original
lava/water cooling rule: source lava becomes obsidian and shallow lava becomes
cobblestone when water touches its sides or top. Cooled blocks update lighting and
survive native saves. Fizz effects, fluid spreading, weather progression, ambient
occlusion and complete sky rendering remain pending. World time now advances during play and survives save/load. Original
sun/moon textures, phase ordering, biome sky colours and grass/foliage/water tints
are connected. Original basic cloud textures, motion and weather tint replace the
placeholder rectangles; volumetric clouds and full fog rendering remain incomplete.
The shared shader sources are tested on macOS in a windowless CGL context with
pixel readback; this creates no client window, screenshots or previews.
Sony packaging, whole-save migrations and live simulation integration remain
pending. The client now uses the inner-save storage and full-height chunk bridge.
Nonvisual tests cover full-height edits, cross-chunk lighting, migration, unchanged
light preservation, failed-save retries and transactional load rejection. All 33 material property sets are compared with the original classes. The archived reference alone uses `-fwrapv` to retain
its expected signed wrapping semantics. The actual port uses defined arithmetic.

The core test covers deterministic world creation, collision and spawn, ray reach
and adjacent placement cells, edits, save replacement, and corruption/truncation
rejection. The window smoke test walks every front-end menu scene (How To Play,
Controls, each Settings page, Credits, message boxes), creates a world through Create
New World, captures menu, world, inventory and tutorial images, performs an
edit/save/reload round trip, exits through the death menu, starts the tutorial from
the Start Game list, and checks for OpenGL errors. `console_menu_core` checks the menu
scenes against the source (buttons, focus, settings written, message boxes) and
`console_tutorial_core` drives the tutorial's lessons, hints and constraints. The smoke test uses its
own data directory and does not touch normal saves.

```sh
cmake -S console_port -B console_port/build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCONSOLE_BUILD_CLIENT=OFF -DCONSOLE_SANITIZERS=ON
cmake --build console_port/build-sanitize -j 6
ctest --test-dir console_port/build-sanitize --output-on-failure --timeout 60
```

On the development host, AddressSanitizer stalled during runtime initialization
before the tests printed output, including outside the sandbox. Its checks are
therefore unverified. A separate undefined-behavior-only build can be run with
`-DCONSOLE_SANITIZER_SET=undefined` and `-B console_port/build-ubsan`.

## Remaining port work

The host streams a movable 128 × 128 × 256 region generated through the original
biome, density, surface, cave, and ravine stages. It no longer uses the temporary
hills-and-trees algorithm. The generator preserves the console's 54-chunk finite-world
setting. Players can explore the full 864 × 864-block extent; outgoing edits are
retained in the native save archive and restored on return. Newly generated visible
chunks receive a deterministic source-derived pass for lakes, ores, shore deposits,
trees, biome foliage, cold-biome snow/ice and biome-specific wells and emerald ore.
Tutorial schematics run after biome decoration and before the shifted snow/ice pass.
Full worlds are **not reproduced yet**: the larger structure generator, dungeons,
live liquid spread and the remaining postprocessing order are pending. The client now uses the 256-high chunk store with metadata and original
light propagation, including a two-chunk halo around the visible region. The renderer
now uses those light values; complete block metadata and ambient occlusion remain
pending.

The original Nether density, surface and cave stages and the End island density and
surface stages now compile through `DimensionChunkGenerator`. Their outputs match
384 separately compiled source samples, including finite Nether walls and End void
falloff. Dimension decoration, fortresses, portals/transitions and live client
integration remain pending; these libraries have only been tested nonvisually.

Crops, saplings, grass, leaves, cactus, sugar cane, vines and the rest of the randomly
ticking tiles now grow and decay with the original rules, the weather cycles, and
hoes, seeds, carrots, potatoes, nether wart and bone meal work (`docs/TICK_MAP.md`). Other unfinished systems include per-chunk mesh uploads, ambient
occlusion, scheduled ticks for tiles other than liquids, redstone, survival, mobs/entities, inventories and recipes, audio/music,
animation, first-person hand, split screen, networking, PSN/store services, and legacy
saves. `docs/COVERAGE.md` lists every source class and whether the port has it;
`docs/TICK_MAP.md` follows the game tick and gives the order of the remaining work. Existing 96/128-high
prototype `.mcp` saves expand on load without overwriting the old file. See `docs/PORT_STATUS.md` for exact boundaries.

The imported source and assets retain their original provenance. This folder does
not grant a new license to them or claim affiliation with the original publishers.


Terrain rendering now reads original metadata-based textures for logs, planks,
leaves, wool and sandstone, including horizontal log ends and sandstone undersides.
The selectors are extracted with `tools/extract_block_textures.py`. This remains
an incomplete port; complete block rendering and item interactions are
still pending.

Placed logs now follow the original console player-heading/height rule. Original
cube-face UV arithmetic also orients bark along horizontal logs. Rejected placement
restores displaced water depth. These paths have nonvisual placement and mesh tests;
full block interaction and rendering remain unfinished.


Chunk streaming prepares a bounded batch of missing chunks each game update. The visible
8×8-chunk window moves around the player, with a two-chunk lighting halo. Resident
chunk storage is bounded to 144 chunks plus the incoming batch; explored chunks are
kept in the finite-world save archive. New chunks use the saved seed and generator
type. Natural features are prepared one visible chunk per update from cached raw
terrain; saved and player-edited chunks are never regenerated for this pass.
The partial pass now includes the original lake feature before ore and biome
decoration and enclosed cave spring placement afterward. Cold biomes also freeze
exposed water and accumulate the source's initial snow layer. Desert wells and
Extreme Hills emerald ore run after the shared biome decorator. Natural plants use cutout mesh geometry and their imported terrain
atlas tiles; this does not yet include every source block shape or feature.
Returning to unloaded areas restores block metadata and opaque save fields.
Outgoing chunk archival and both lighting passes now advance over multiple updates.
Terrain meshing builds eight sections per update and uploads four GPU sections per
update while the previous mesh stays visible. New-world creation and loading run
off the UI thread; automatic saves wait until streaming completes. These changes reduce the long transition stalls;
interactive timing still needs a play-test. Player-position persistence and original
entity simulation are not part of this streaming adapter.


The supplied PS3 tutorial package now decodes through a native GRF/schematic loader.
All 19 supplied schematic files decode, and the 17 structures embedded in
Tutorial.pck match the original XML placement layout. Block metadata and translated
entity records are preserved. The package is available for creative exploration;
scripted lessons, entities and survival progression still need their original
runtime systems. PS3 content uses EdgeZLib raw DEFLATE/RLE; the LZX decoder still
reads earlier generic tutorial saves. The general LZX dependency is documented in
third_party/libmspack/README.md with its source and license.

### The console tutorial (September 24)

**Play Game → Play Tutorial** creates a new Tutorial World and runs the original
`FullTutorial` (`Common/Tutorial/*`, imported unchanged into `ported/tutorial/` by
`tools/import_tutorial.py`): survival mode with the level rules' health, hunger and
steak, the map in slot 9, every lesson, hint and reminder popup with its PS3 button
images (or the keyboard key when you last used the keyboard), the input and area
constraints that hold you in place during a lesson, the frozen time of day, the
crafting, furnace, brewing, inventory and creative menus' own lessons, and the music
disc collection goal. Completed lessons are remembered in the profile, so a second
tutorial skips them; turning Hints off in Settings hides the hint popups. Leaving the
tutorial area ends it and turns mob spawning on. A saved tutorial world loads as an
ordinary world, as on the console.

### Tutorial exploration (September 13)

**Play Game → Tutorial World** opens the original tutorial package for creative
exploration. It uses the original spawn and 17 schematic placements, streams
surrounding chunks and keeps edits in a separate save. Wooden doors open with
right-click/L2; ladders support climbing and Shift holds your height. Stairs and
slabs use original shapes, textures and picking. F enables flight.

The original guided lessons, complete item/container behavior, entities, survival gameplay and
some specialized block rendering/behavior remain unfinished. This is not a full
console port. Targeted nonvisual tests pass; no visible client was
launched, so interactive/visual QA has not been performed.

### Tutorial chests (September 14)

New Tutorial Worlds now include the original Overworld chest loot. Right-click/L2
opens a chest; select a slot with arrows/D-pad or the mouse. Enter/Cross moves the
whole stack, H/Square moves half (rounded up), and R/Triangle moves one item.
Right-clicking a slot also moves half. Compatible stacks merge up to their original
item limits before empty slots are used. Esc/Circle closes the screen.
Enchantments and music-disc tags survive transfers and saves. Chests use the
original closed model and item icons.

Adjacent chests now open as one 54-slot inventory and use the original large
chest model. Carried-item use and guided lessons remain unfinished. Previously saved tutorial chunks are not refilled;
create a new Tutorial World to get the newly integrated original chest actions.

### Stack transfers, fences and gates (September 22)

Container transfers use original per-item limits and compare complete retained item
NBT, so tagged or damaged items do not merge incorrectly. Whole, half and one-item
moves work in both single and double chests and survive save/reload.

Oak fences, Nether brick fences and fence gates use the original collision and
TileRenderer cuboids. Fences connect to compatible neighbors, remain 1.5 blocks high
for collision, and gates clear the passage when opened with right-click/L2. Open and
closed gate pieces remain pickable. The source creative catalog puts fence gates
in Building Blocks and both fences in Miscellaneous; choosing one fills a free
hotbar slot, or replaces the selected slot if full.
Placed gates use the original
player-facing direction and support rule.

Targeted tutorial, container, transfer and fence tests pass under Debug/UBSan, and
all 531 tracked source/asset imports verify. See `docs/PLAYTEST.md` for the
interactive test route. The full suite should be run after each integration batch.
