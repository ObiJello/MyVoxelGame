#pragma once

#include "core/BlockPos.h"
#include <cstdint>
#include <functional>
#include <string>

// Twilight Forest 4.9 — util/landmarks/LegacyLandmarkPlacements.java.
//
// The Twilight Forest keeps its pre-1.18 landmark grid: one landmark per
// 256 x 256 block region (16 x 16 chunks), its centre jittered by up to
// +/-3 chunks by a hash of the region coordinates. Which landmark goes there
// is decided by the key biome at the region (BIOME_2_STRUCTURES) or, for the
// "variety" regions, by a fixed layout (two lich towers and two naga
// courtyards near the middle of every 2048-block area) plus a weighted draw.
//
// Pure functions of their arguments (and the world seed for the draw), so
// safe on every worldgen thread. Used by the landmark_grid /
// avoid_landmark_grid structure placements (StructureSets.cpp), the landmark
// structures (TwilightStructures.cpp), the dark-forest canopy blanket
// (ChunkGenerator.cpp) and TF features that avoid landmark centres.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_landmarks {

/**
 * getNearestCenterXZ(chunkX, chunkZ, height): the BLOCK position of the
 * landmark centre of the region containing the chunk.
 */
core::BlockPos getNearestCenterXZ(int32_t chunkX, int32_t chunkZ, int32_t height = 0);

/** chunkHasLandmarkCenter: the chunk holds its region's landmark centre. */
bool chunkHasLandmarkCenter(int32_t chunkX, int32_t chunkZ);

/** blockIsInLandmarkCenter(blockX, blockZ). */
bool blockIsInLandmarkCenter(int32_t blockX, int32_t blockZ);

/**
 * blockNearLandmarkCenter(blockX, blockZ, range) — verbatim, including the
 * mod's operator-precedence slip (`blockX >> 4 + x` is `blockX >> (4 + x)`),
 * which is what the mod's features actually test.
 */
bool blockNearLandmarkCenter(int32_t blockX, int32_t blockZ, int32_t range);

/** manhattanDistanceFromLandmarkCenter(chunkX, chunkZ), in chunks. */
int32_t manhattanDistanceFromLandmarkCenter(int32_t chunkX, int32_t chunkZ);

/**
 * pickVarietyLandmark(chunkX, chunkZ, worldSeed): the "twilightforest:<id>"
 * structure the region's variety slot holds — lich_tower / naga_courtyard at
 * the four fixed region offsets, else VARIETY_LANDMARKS.getRandom(
 * LegacyRandomSource(seed + chunkX * 25117 + chunkZ * 151121)) over the
 * region-rounded chunk coordinates. The mod's structure ids are
 * small_hollow_hill / medium_hollow_hill / large_hollow_hill (TFStructures'
 * HOLLOW_HILL_* keys), hedge_maze, naga_courtyard, lich_tower.
 */
std::string pickVarietyLandmark(int32_t chunkX, int32_t chunkZ, int64_t worldSeed);

/**
 * pickLandmarkForChunk: BIOME_2_STRUCTURES for the biome at the region's
 * centre block ((chunk << 4) + 8, y 0), else pickVarietyLandmark. `biomeAt`
 * returns the biome key at a block position.
 */
std::string pickLandmarkForChunk(int32_t chunkX, int32_t chunkZ, int64_t worldSeed,
                                 const std::function<std::string(int32_t, int32_t, int32_t)>& biomeAt);

/** Java Math.round(float) for the region rounding (floor(x + 0.5f)). */
int32_t javaRoundFloat(float value);

} // namespace twilight_landmarks
} // namespace structure
} // namespace levelgen
} // namespace minecraft
