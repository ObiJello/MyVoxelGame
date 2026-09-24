// File: src/common/world/biome/BiomeZoom.hpp
//
// MC net.minecraft.world.level.biome.BiomeManager — the "fuzzy zoom" from the
// 4x4x4 noise-biome grid a chunk stores to the biome of one BLOCK.
//
// A chunk keeps one biome per 4x4x4 cell (a quart). MC never reads a block's
// biome by plain division: LevelReader.getBiome(pos) offsets the position by
// -2, looks at the 8 surrounding quart corners, jitters each one's distance
// by a seeded hash (getFiddledDistance) and takes the nearest. That is what
// makes biome borders ragged per block instead of 4-block stair steps, and it
// is what spawning, block tint, F3, precipitation and mob variants all see.
//
// The environment attributes that are NOT full-resolution (fog and sky
// colours, water fog, music) read the unzoomed grid instead —
// getNoiseBiomeAtPosition / getNoiseBiomeAtQuart — so those callers keep the
// plain quart lookup.
//
// The seed is BiomeManager.obfuscateSeed(worldSeed), the same for every
// dimension of a world; the server sends it to the client (MC's hashedSeed).
#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Game::BiomeZoom {

    // MC BiomeManager.obfuscateSeed: Hashing.sha256().hashLong(seed).asLong().
    int64_t ObfuscateSeed(int64_t worldSeed);

    // MC BiomeManager.getBiome(x, y, z) up to the source lookup: the quart
    // whose noise biome block (x, y, z) shows. Quart Y is NOT clamped here —
    // MC clamps it in ChunkAccess.getNoiseBiome, and so do the callers.
    glm::ivec3 NoiseQuartAt(int64_t zoomSeed, int x, int y, int z);

} // namespace Game::BiomeZoom
