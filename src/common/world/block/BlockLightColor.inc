// File: src/common/world/block/BlockLightColor.inc
//
// HAND-MAINTAINED colour of the light the engine's coloured emitters give
// (engine feature: coloured light — ChunkLight's header note). Read by
// BlockLightProperties::Init: a row turns the block's emission (the level,
// from GeneratedBlockLight.inc / EngineBlockLight.inc, unchanged) into three
// channels, scaled so the brightest channel IS that level. Every block not
// listed gives white light, as in vanilla.
//
//   BLOCK_LIGHT_COLOR("registry_slug", 0xRRGGBB)
//
// The values match ColoredTorches::kColor (that header's flame colours).

// ── Dyed torches (2026-10-10), standing and wall ──────────────────────────
BLOCK_LIGHT_COLOR("white_torch", 0xFFFFFF)
BLOCK_LIGHT_COLOR("white_wall_torch", 0xFFFFFF)
BLOCK_LIGHT_COLOR("orange_torch", 0xFF8A1F)
BLOCK_LIGHT_COLOR("orange_wall_torch", 0xFF8A1F)
BLOCK_LIGHT_COLOR("magenta_torch", 0xFF3DF0)
BLOCK_LIGHT_COLOR("magenta_wall_torch", 0xFF3DF0)
BLOCK_LIGHT_COLOR("light_blue_torch", 0x5CC8FF)
BLOCK_LIGHT_COLOR("light_blue_wall_torch", 0x5CC8FF)
BLOCK_LIGHT_COLOR("yellow_torch", 0xFFE23A)
BLOCK_LIGHT_COLOR("yellow_wall_torch", 0xFFE23A)
BLOCK_LIGHT_COLOR("lime_torch", 0x7DFF2A)
BLOCK_LIGHT_COLOR("lime_wall_torch", 0x7DFF2A)
BLOCK_LIGHT_COLOR("pink_torch", 0xFF7FB6)
BLOCK_LIGHT_COLOR("pink_wall_torch", 0xFF7FB6)
BLOCK_LIGHT_COLOR("gray_torch", 0xA9B3C2)
BLOCK_LIGHT_COLOR("gray_wall_torch", 0xA9B3C2)
BLOCK_LIGHT_COLOR("light_gray_torch", 0xD5DCE6)
BLOCK_LIGHT_COLOR("light_gray_wall_torch", 0xD5DCE6)
BLOCK_LIGHT_COLOR("cyan_torch", 0x1FE6E6)
BLOCK_LIGHT_COLOR("cyan_wall_torch", 0x1FE6E6)
BLOCK_LIGHT_COLOR("purple_torch", 0xA040FF)
BLOCK_LIGHT_COLOR("purple_wall_torch", 0xA040FF)
BLOCK_LIGHT_COLOR("blue_torch", 0x3050FF)
BLOCK_LIGHT_COLOR("blue_wall_torch", 0x3050FF)
BLOCK_LIGHT_COLOR("brown_torch", 0xD08848)
BLOCK_LIGHT_COLOR("brown_wall_torch", 0xD08848)
BLOCK_LIGHT_COLOR("green_torch", 0x2ED12E)
BLOCK_LIGHT_COLOR("green_wall_torch", 0x2ED12E)
BLOCK_LIGHT_COLOR("red_torch", 0xFF2A1A)
BLOCK_LIGHT_COLOR("red_wall_torch", 0xFF2A1A)
BLOCK_LIGHT_COLOR("black_torch", 0x6A3CFF)
BLOCK_LIGHT_COLOR("black_wall_torch", 0x6A3CFF)
