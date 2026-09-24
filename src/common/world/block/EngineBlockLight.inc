// File: src/common/world/block/EngineBlockLight.inc
//
// HAND-MAINTAINED light emission for the engine's own blocks — the ones that
// glow more (or less) than the vanilla block their slug aliases to in
// tools/gen_block_light.py (GeneratedBlockLight.inc). Read by
// BlockLightProperties::Init AFTER the generated table, so a row here wins.
//
//   ENGINE_BLOCK_LIGHT("registry_slug", emission 0..15)
//
// Adding a glowing block: add one line here (no generator run needed). Blocks
// flagged `Block::emissive` are ALSO drawn full-bright (MC emissiveRendering:
// their faces ignore the light around them), so a softly glowing block can
// keep its lit look while casting only a little light.
//
// Values follow the vanilla block each is classed as where there is one
// (lantern 15, soul lantern 10, amethyst cluster 5, nether portal 11) and the
// mod's own lightLevel for the Twilight Forest / Aether ports.

// ── The Hush ────────────────────────────────────────────────────────────────
ENGINE_BLOCK_LIGHT("resonant_crystal", 10)        // glowing crystal formations
ENGINE_BLOCK_LIGHT("resonance_bloom", 7)          // glowing flower
ENGINE_BLOCK_LIGHT("lantern_leaves", 8)           // whisperwood canopy glow
ENGINE_BLOCK_LIGHT("hush_portal", 11)             // nether portal class
ENGINE_BLOCK_LIGHT("resonant_cluster", 5)         // amethyst cluster class
ENGINE_BLOCK_LIGHT("echo_lantern", 13)
ENGINE_BLOCK_LIGHT("echo_core", 10)
ENGINE_BLOCK_LIGHT("hanging_whisperfruit", 8)
ENGINE_BLOCK_LIGHT("echo_heart", 12)
ENGINE_BLOCK_LIGHT("hush_lighthouse_lamp", 15)    // beacon class
ENGINE_BLOCK_LIGHT("resonant_chime", 7)
ENGINE_BLOCK_LIGHT("choir_altar", 6)

// ── Aurelith, the Lantern City ──────────────────────────────────────────────
ENGINE_BLOCK_LIGHT("stave_stone", 4)              // glyph shimmer
ENGINE_BLOCK_LIGHT("cyan_lumen_panel", 15)        // sea lantern class
ENGINE_BLOCK_LIGHT("violet_lumen_panel", 15)
ENGINE_BLOCK_LIGHT("amber_lumen_panel", 15)
ENGINE_BLOCK_LIGHT("lumen_strip", 12)
ENGINE_BLOCK_LIGHT("crystal_conduit", 10)
ENGINE_BLOCK_LIGHT("choir_lamp", 15)              // lantern-class streetlight
ENGINE_BLOCK_LIGHT("resonant_water", 6)           // the river Vesper's glow
ENGINE_BLOCK_LIGHT("resonance_engine", 12)
ENGINE_BLOCK_LIGHT("voice_beacon", 15)            // beacon class
// The dormant city's lights (the dim_* twins burn low until the Heart wakes).
ENGINE_BLOCK_LIGHT("dim_cyan_lumen_panel", 6)
ENGINE_BLOCK_LIGHT("dim_violet_lumen_panel", 6)
ENGINE_BLOCK_LIGHT("dim_amber_lumen_panel", 6)
ENGINE_BLOCK_LIGHT("dim_lumen_strip", 5)
ENGINE_BLOCK_LIGHT("dim_stave_stone", 2)
ENGINE_BLOCK_LIGHT("dim_choir_lamp", 7)
// The flickering windows: a lamp behind glass. Block light does not animate,
// so each casts its schedule's AVERAGE: the guttering ones are mostly lit,
// the waking ones mostly dark, the restless one half and half.
ENGINE_BLOCK_LIGHT("guttering_amber_window", 9)
ENGINE_BLOCK_LIGHT("waking_amber_window", 4)
ENGINE_BLOCK_LIGHT("restless_amber_window", 7)
ENGINE_BLOCK_LIGHT("guttering_cyan_window", 9)
ENGINE_BLOCK_LIGHT("waking_cyan_window", 4)
ENGINE_BLOCK_LIGHT("guttering_violet_window", 9)
ENGINE_BLOCK_LIGHT("waking_violet_window", 4)

// ── Twilight Forest (TFBlocks lightLevel) ───────────────────────────────────
ENGINE_BLOCK_LIGHT("mushgloom", 3)
ENGINE_BLOCK_LIGHT("firefly", 15)
ENGINE_BLOCK_LIGHT("moonworm", 14)
ENGINE_BLOCK_LIGHT("torchberry_plant", 7)         // with berries (always, pass one)
ENGINE_BLOCK_LIGHT("twilight_portal", 11)
ENGINE_BLOCK_LIGHT("cicada", 0)                   // aliases end_rod; TF gives it no light

// ── The Aether (AetherBlocks lightLevel11) ──────────────────────────────────
ENGINE_BLOCK_LIGHT("aether_portal", 11)
ENGINE_BLOCK_LIGHT("quicksoil_glass", 11)
ENGINE_BLOCK_LIGHT("sentry_stone", 11)
ENGINE_BLOCK_LIGHT("light_angelic_stone", 11)
ENGINE_BLOCK_LIGHT("light_hellfire_stone", 11)
ENGINE_BLOCK_LIGHT("locked_sentry_stone", 11)
ENGINE_BLOCK_LIGHT("locked_light_angelic_stone", 11)
ENGINE_BLOCK_LIGHT("locked_light_hellfire_stone", 11)
ENGINE_BLOCK_LIGHT("trapped_sentry_stone", 11)
ENGINE_BLOCK_LIGHT("trapped_light_angelic_stone", 11)
ENGINE_BLOCK_LIGHT("trapped_light_hellfire_stone", 11)
ENGINE_BLOCK_LIGHT("treasure_doorway_sentry_stone", 11)
ENGINE_BLOCK_LIGHT("treasure_doorway_light_angelic_stone", 11)
ENGINE_BLOCK_LIGHT("treasure_doorway_light_hellfire_stone", 11)

// ── Redstone plus ───────────────────────────────────────────────────────────
ENGINE_BLOCK_LIGHT("display_block", 0)            // a screen: full-bright, casts nothing
