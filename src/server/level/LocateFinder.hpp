// File: src/server/level/LocateFinder.hpp
//
// The two searches behind /locate, ported from
//   ChunkGenerator.findNearestMapStructure (+ getNearestGeneratedStructure /
//   getStructureGeneratingAt) and BiomeSource.findClosestBiome3d,
// on top of the terrain library's structure placement state and biome source.
//
// Structures: for every wanted structure, each structure set that contains
// it is scanned by its placement. Concentric rings (strongholds) are the
// precomputed ring positions, nearest first. Random spread is walked ring by
// ring of spacing cells outward from the player's chunk (radius 0..100); the
// first ring with any hit wins. A candidate chunk counts when the placement
// says it is a structure chunk AND the structure's own generation
// (Structures::generate — the biome check and the layout, exactly what a
// chunk reaching STRUCTURE_STARTS runs) produces a valid start. Generation
// is seed-deterministic, so "would start here" is "does start here"; MC's
// StructureCheck cache only saves it the recomputation.
//
// Biomes: the biome source is sampled on a 32-block horizontal / 64-block
// vertical lattice in a square spiral out to 6400 blocks, Y alternating
// outward from the player's height; the first sample whose biome is in the
// wanted set wins.
//
// Reported Y: MC prints the matching SAMPLE's Y for a biome (often inside
// rock or in the air, biomes being 3D) and "~" for a structure, so its
// click-to-teleport keeps the player's own height. This game reports the
// terrain surface of the found column for both (LocateSurfaceY) so the
// teleport lands on the ground.
//
// Both run on the server thread and can take a while on a miss (MC's do
// too); the command logs the time as LocateCommand does.
#pragma once

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Server {

    class ServerLevel;

    struct LocateResult {
        glm::ivec3  pos;
        std::string id;   // "minecraft:village_plains" / "minecraft:plains"
    };

    // Structure ids as registered ("minecraft:village_plains"); unknown ids
    // are skipped. Empty when nothing generates within the radius.
    std::optional<LocateResult> FindNearestStructure(ServerLevel& level,
                                                     const std::vector<std::string>& structureIds,
                                                     const glm::ivec3& from,
                                                     int maxSearchRadius = 100);

    // Biome ids as the biome source names them ("minecraft:plains").
    std::optional<LocateResult> FindClosestBiome(ServerLevel& level,
                                                 const std::unordered_set<std::string>& biomeIds,
                                                 const glm::ivec3& from,
                                                 int maxSearchRadius = 6400,
                                                 int sampleResolutionHorizontal = 32,
                                                 int sampleResolutionVertical = 64);

    // The Y a player stands at on the terrain surface of column (x, z). A
    // loaded chunk answers from its MOTION_BLOCKING heightmap (trees, water
    // and player edits included); an unloaded column is read off the
    // generator's noise column (MC getBaseHeight, WORLD_SURFACE_WG), which
    // needs no chunk. Under a ceiling (the nether) it is the first
    // two-high air gap BELOW the roof, not the top of the bedrock.
    // `fallbackY` when the column cannot be answered (void column, or the
    // generator is not ready).
    int LocateSurfaceY(ServerLevel& level, int x, int z, int fallbackY);

    // Is this name a tag? "#..." always; a bare name when a tag file of that
    // name exists (data/<ns>/tags/worldgen/<kind>/<path>.json) — this game
    // lets "is_jungle" stand for "#minecraft:is_jungle", which MC would
    // reject as an unknown biome. `kind` is "structure" or "biome".
    bool IsWorldgenTag(const std::string& kind, const std::string& idOrTag);

    // A structure tag → its structure ids (nested tags followed); a bare id
    // → just itself. Empty when the tag file does not exist.
    std::vector<std::string> ResolveStructureIdOrTag(const std::string& idOrTag);
    // Same for biomes, through the library's biome-tag resolver.
    std::unordered_set<std::string> ResolveBiomeIdOrTag(const std::string& idOrTag);

} // namespace Server
