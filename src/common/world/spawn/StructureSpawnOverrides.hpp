// File: src/common/world/spawn/StructureSpawnOverrides.hpp
//
// MC Structure.spawnOverrides (StructureSpawnOverride) and the two places the
// natural spawner consults them:
//
//   NaturalSpawner.mobsAt =
//       isInNetherFortressBounds(pos) ? NetherFortressStructure.FORTRESS_ENEMIES
//                                     : ChunkGenerator.getMobsAt(pos)
//
//   ChunkGenerator.getMobsAt: for each structure referenced at pos whose
//   spawn_overrides names this category, the override's list REPLACES the
//   biome's when pos is inside a piece (bounding_box "piece") or inside the
//   start's union box ("full"). An empty list means "nothing spawns here".
//
// The per-category lists are read from data/<ns>/worldgen/structure/*.json,
// the same files the terrain library places the structures from. The boxes
// come from the chunk (Chunk::structureSpawnAreas), recorded at generation.
//
// Twilight Forest landmarks (ControlledSpawns): a structure JSON's
// "controlled_spawns" — monster lists labelled by a piece's spawn index, and
// ambient / water lists — are appended to mobsAt's list, the port of the mod's
// EntityEvents.gatherPotentialSpawns on NeoForge's PotentialSpawns event.
//
// ObeyCraft extension — districts. An override may carry
//
//   "obeycraft:districts": [ { "name": "archive_of_echoes",
//       "areas": [ { "template": "minecraft:aurelith/u_3_4",
//                    "box": [x0, y0, z0, x1, y1, z1] }, ... ],
//       "spawns": [ ...SpawnerData... ] } ]
//
// Each area is a box in one jigsaw piece's TEMPLATE-LOCAL coordinates; it is
// carried through the piece's placement (the recorded piece box and its
// rotation, pivot at the origin) to world space. Inside a district, its list
// replaces the biome's; elsewhere in the structure the override's own
// bounding_box/spawns rule applies as vanilla's does. This is how one part of
// a large jigsaw structure cut into uniform tiles (Aurelith's Archive of
// Echoes) gets its own spawns. Vanilla overrides never carry the key.
#pragma once

#include "common/entity/MobCategory.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/spawn/GeneratedMobSpawns.hpp"

namespace Game {

    class Chunk;

    namespace StructureSpawns {

        // The replacement spawn list for `category` at (x, y, z), or nullptr
        // when no structure overrides it there and the biome's list applies.
        // `chunk` is the chunk holding (x, z); `blockBelow` is the block at
        // (x, y - 1, z) — the fortress rule's nether-brick test; `biomeList`
        // is the biome's own list there (what a Twilight Forest landmark's
        // controlled spawns are appended to when no override applies).
        const BiomeSpawnList* MobsAt(const Chunk& chunk, MobCategory category,
                                     int x, int y, int z, BlockID blockBelow,
                                     const BiomeSpawnList* biomeList);

        // True when the chunk holds anything MobsAt could answer for — lets
        // the caller skip the block read for the common structure-less chunk.
        bool ChunkHasOverrides(const Chunk& chunk);

    } // namespace StructureSpawns

} // namespace Game
