# F3 debug screen — reference

A port of Minecraft 26.3's debug screen (`DebugScreenOverlay`, the
`client/gui/components/debug` entries, `KeyboardHandler`'s F3 chords,
`DebugOptionsScreen`, `GameModeSwitcherScreen`, the `client/renderer/debug`
renderers). Code lives in `src/client/renderer/gui/debug/` and
`src/client/renderer/debug/{Gizmos,DebugRenderer}.*`. Design notes are in the
"F3 debug screen" section of CLAUDE.md; this file is the user-facing list.

Every key below is rebindable in Options → Controls under the **Debug**
category (ids `key.debug.*`, saved to options.txt). The chord MODIFIER
(`key.debug.modifier`) defaults to **Right Alt** here, not vanilla's F3: on a
Mac keyboard F3 sits on the Fn/Globe layer and macOS eats Fn+C (Control
Center), Fn+H (Show Desktop), Fn+A, Fn+N, Fn+M, Fn+D, Fn+F, Fn+E and Fn+Q
before the game sees them. So "F3+X" in the table means **hold Right Alt, tap
X** (rebind the modifier back to F3 if you prefer vanilla). Plain F3
(`key.debug.overlay`) still toggles the overlay. A key that fired a chord
does NOT also do its normal action for that press (Right Alt+T does not open
chat). When the modifier and the overlay key are the SAME key, tapping it
toggles the overlay only if no chord fired while it was held (vanilla's
rule); with the defaults they differ, so F3 toggles on press.

## Keys

| Key | Does | Feedback |
|---|---|---|
| F3 | Toggle the debug overlay | — |
| F3+A | Reload all chunks (remesh every section) | "Reloading all chunks" |
| F3+B | Toggle entity hitboxes (`entity_hitboxes` entry) | "Hitboxes: shown/hidden" |
| F3+C | Copy `/execute in <dim> run tp @s x y z yaw pitch` to the clipboard | "Copied location to clipboard" |
| F3+C held 10 s | Deliberate crash (warns every second first) | "F3 + C is held down…", "Crashing in N…" |
| F3+D | Clear chat + chat history | — |
| F3+G | Toggle chunk borders (`chunk_borders` entry) | "Chunk borders: shown/hidden" |
| F3+H | Toggle advanced item tooltips (registry name + component count) | "Advanced tooltips: shown/hidden" |
| F3+I | Copy `/setblock x y z <block[state]>` for the targeted block, or `/summon <entity> x y z` for the targeted mob/player | "Copied client-side block/entity data to clipboard" |
| F3+N | Spectator ↔ previous game mode (via `/gamemode`) | — |
| F3+F4 | Game-mode switcher: each F4 tap moves the selection, releasing F3 switches. Mouse works too | — |
| F3+F6 | Debug Options screen (below). F3+F6 again closes it | — |
| F3+P | Toggle "pause on lost focus" (off by default here; on = unfocused 500 ms → pause menu) | "Pause on lost focus: enabled/disabled" |
| F3+S | Dump the block atlas to `screenshots/debug/atlas_blocks.png` | "Saved dynamic textures to …" |
| F3+T | Reload resource packs (full resource reload) | "Reloaded resource packs" |
| F3+L | Start/stop a 10-second frame-timing profile → `debug/profiling/profile-<time>.csv` | "Profiling started for 10 seconds…", "Profiling ended. Saved results to …" |
| F3+V | Client version info (version, data version, pack formats, renderer) in chat | "Client version info:" |
| F3+X | Improved transparency — reports that this renderer has no OIT pass | "Improved transparency: not available…" |
| F3+K | Toggle the ImGui debug panels (engine-specific, was Shift+`+D) | "Debug panels: shown/hidden" |
| F3+1 | Profiler pie chart (frame phase timers). While it is up and F3 is NOT held: digits 1-9 descend into a slice, 0 goes back up | — |
| F3+2 | FPS chart (left) + server tick chart (right, integrated server only) | — |
| F3+3 | Ping chart (right) + bandwidth chart (left, remote servers only) | — |
| F3+4 | Lightmap texture (bottom right, 64×64) | — |
| F3+Esc | Pause the world without opening the pause menu (Esc resumes) | — |

Only one of the F3+2 / F3+3 / F3+4 views is shown at a time (as in vanilla).
The overlay's own footer lists them: "Debug charts: [F3+1] Profiler hidden; …"
and "To edit: press [F3+F6]".

## Debug Options screen (F3+F6)

Every entry has a status: **Off**, **In Overlay** (shown while F3 is up),
**Always** (shown even with the overlay off). Search box filters by id.
Footer: **Default profile**, **Performance profile** (vanilla's two presets),
**GUI Scale** (Unchanged / Auto / 1..4 — draws the overlay smaller than the rest
of the GUI) and **Done**. Choices persist to `<gamedir>/debug-profile.json`
in vanilla's format. Entries marked grey are hidden by the "Reduced Debug
Info" option.

### Text entries

| Id | Line(s) |
|---|---|
| `game_version` | "MyVoxelGame 0.1.N (0.1.N/vanilla)" |
| `fps` | "N fps T: <limit> (fifo/immediate) @<Hz>" |
| `tps` | "Integrated server @ <ms>/<target> ms, tx, rx" (or "…server" when remote); shows frozen / stepping / sprinting |
| `memory` | Mem % of RAM, allocation rate (resident growth), peak |
| `detailed_memory` | process resident / peak / virtual, GPU buffers / textures / peak |
| `system_specs` | build (compiler, config), CPU (count × brand), display size + GPU vendor, GPU name (+iGPU/dGPU), backend + driver |
| `looking_at_block_state` | "Targeted Block: x, y, z", registry name, properties (booleans green/red) — 20 block reach |
| `looking_at_block_tags` | `#minecraft:…` tags of that block (from data/minecraft/tags/block) |
| `looking_at_fluid_state` | "Targeted Fluid", `minecraft:water` / `flowing_water` / `lava` / `empty`, falling/level |
| `looking_at_fluid_tags` | fluid tags |
| `looking_at_entity` | "Targeted Entity" + `minecraft:<type>` under the crosshair |
| `looking_at_entity_tags` | entity-type tags |
| `chunk_render_stats` | "C: rendered/total (s) D: dist, pC: pending meshes, aB: free upload permits" |
| `chunk_generation_stats` | "NoiseRouter T/V/C/E/D/W/PV/AS/N" + "Biome builder PV/C/E/T/H" at the feet (integrated server) |
| `entity_render_stats` | "E: rendered/total, SD: simulation distance" |
| `particle_render_stats` | "P: n" |
| `chunk_source_stats` | "Chunks[C] W: …" client, "Chunks[S] W: …" server |
| `player_position` | XYZ, Block, Chunk (with region file), Facing (yaw/pitch), dimension + FC |
| `player_section_position` | "Section-relative: x y z" |
| `player_speed` | blocks/tick |
| `coordinates` | "x y z" — whole-number block position only (not vanilla; set to Always for a bare coordinate readout) |
| `light_levels` | "Client Light: n (sky, block)" — block light is always 0 (no light engine) |
| `heightmap` | "CH S M" from the client column, "SH S O M ML" from the server chunk |
| `biome` | "Biome: minecraft:…" |
| `local_difficulty` | "Local Difficulty: x // y" (uses the chunk's inhabited time and the moon phase) |
| `day_count` | "Day #n" |
| `entity_spawn_counts` | "SC: chunks, MO/C/AM/AX/UWC/WC/WA/MI: counts" |
| `sound_mood`, `sound_cache` | always 0 (no sound engine) |
| `post_effects` | nothing (no post effects) |
| `gpu_utilization` | "GPU: n%" (GPU frame time / CPU frame time; only measured while enabled) |
| `simple_performance_impactors` | clouds / biome blend, filtering, terrain rendering mode |

### Renderer entries (world-space, drawn over the world)

| Id | Draws |
|---|---|
| `entity_hitboxes` (F3+B) | white box, feet point, red eye slab, blue view arrow for mobs, players, items, orbs, falling blocks; yellow seat box for riders |
| `chunk_borders` (F3+G) | vanilla's yellow/cyan grid on the player's chunk, red corner posts, blue section box |
| `3d_crosshair` | RGB axes 1 block ahead, replacing the 2D crosshair (on by default with the overlay) |
| `chunk_section_paths` | lines from each visible section toward the direction the occlusion BFS entered it, coloured by distance |
| `chunk_section_visibility` | red lines between face pairs a section cannot see through, yellow fill when any are blocked |
| `chunk_section_octree` | numbered boxes of the visible sections (no octree exists; green = "close") |
| `visualize_water_levels` | green fills to each water block's height + its amount |
| `visualize_heightmap` | coloured tiles per heightmap type over ±2 chunks (refreshed every second) |
| `visualize_collision_boxes` | white boxes of every collision shape within 6 blocks |
| `visualize_entity_supporting_blocks` | red (you) / green (others) highlight on the block each entity stands on |
| `visualize_sky_light_levels` | sky light value text per block within 10 blocks (plus "n/a" per section: no light engine data) |
| `visualize_block_light_levels` | nothing to show (no block light) |
| `visualize_solid_faces` | red fill on every sturdy face within 6 blocks |
| `visualize_chunks_on_server` | server chunk-status map in the screen centre + "Client/Server" labels above every chunk within 12 |
| `visualize_sky_light_sections` | nothing (no light sections) |

## Commands added alongside

- `/setblock <x> <y> <z> <block[state]> [destroy|keep|replace]` — vanilla
  syntax (`~`/`^` coordinates, `minecraft:oak_stairs[facing=east]`). What
  F3+I copies runs unchanged.

## Files

- `debug-profile.json` — entry statuses (next to options.txt)
- `screenshots/debug/` — F3+S atlas dumps
- `debug/profiling/profile-*.csv` — F3+L recordings
