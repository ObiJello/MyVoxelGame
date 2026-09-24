#pragma once

#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/Structures.h"
#include "random/LegacyRandomSource.h"
#include "world/ChunkPos.h"
#include <cstdint>
#include <functional>
#include <string>

// Twilight Forest 4.9 structures (data/twilightforest/worldgen/structure/*.json).
//
// Dispatcher (TwilightStructures.cpp): Structures::generate hands every
// "twilightforest:*" structure type here. Landmarks (the LandmarkStructure
// family: hollow hills, hedge maze, naga courtyard, quest grove, lich tower,
// ...) share one flow, ported from structures/util/LandmarkStructure.java:
//
//   findValidGenerationPoint  key biome at the landmark's biome-grid tile
//                             (TFBiomeProvider.getMainBiome((round(cx/16) << 6)
//                             + 2, same for z), in quarts) must be in the
//                             structure's biome tag;
//   findGenerationPoint       x = (cx << 4) + 7, z = (cz << 4) + 7 (center_in_
//                             chunk, default true), y = adjustForTerrain
//                             (decoration_clearance.adjust_structure_elevation:
//                             WORLD_SURFACE_WG first-occupied height clamped to
//                             [sea + 1, sea + 7], else sea level);
//   getFirstPiece             the per-type builder below, with
//                             firstPieceRandom = RandomSource.create(seed +
//                             cx * 25117 + cz * 151121) (a LegacyRandomSource);
//   generateFromStartingPiece the same builder: addChildren with ctx.random
//                             (the structure GenerationContext random).
//
// A builder fills out.pieces (layout) AND out.behaviors (postProcess, one per
// piece, same index) and returns false when getFirstPiece would return null.
// The bounding box, and its +12 inflation for terrain_adaptation != none, are
// applied by Structures::generate afterwards, as for vanilla structures.
//
// Custom terrain (structures/CustomDensitySource.java): a structure type may
// supply a terraformer — a density term ADDED to the beardifier for every
// chunk the start references (asmhooks/WorldgenHooks.gatherCustomTerrain ->
// chunkgenerators/CustomTerrainBeardifier). StructureGeneration::
// createBeardifier collects them.

namespace minecraft {
namespace levelgen {
namespace structure {

/**
 * A structure's custom terrain density at a block (DensityFunction.compute on
 * a FunctionContext at blockX/Y/Z). Empty = the structure adds nothing.
 */
using TwilightTerraformer = std::function<double(int32_t blockX, int32_t blockY, int32_t blockZ)>;

namespace TwilightStructures {

/** True for every "twilightforest:*" structure type with a ported builder. */
bool isImplemented(const StructureInfo& info);

/** True when `type` is a Twilight Forest structure type (ported or not). */
bool isTwilightType(const std::string& type);

/**
 * Structure.generate's findValidGenerationPoint + stub for a TF structure.
 * Fills out.pieces / out.behaviors; the caller sets the bounding box.
 */
bool generate(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out);

/**
 * CustomDensitySource.getStructureTerraformer(chunkPos, start) for `start`'s
 * structure, or an empty function when its type has none.
 */
TwilightTerraformer terraformer(const StructureInfo& info, const StructureStartData& start,
                                const ::world::ChunkPos& chunkPos);

/**
 * DecorationClearance (structures/util/DecorationClearance.java) of a TF
 * structure, from its JSON "decoration_clearance". Null when the structure
 * is not a DecorationClearance (no such field) — it never blocks features.
 */
struct DecorationClearance {
    float chunkClearanceRadius = 1.0f;
    bool surfaceDecorations = true;
    bool undergroundDecorations = true;
    bool vegetation = true;
    bool adjustElevation = false;
};
const DecorationClearance* decorationClearance(const std::string& structureName);

} // namespace TwilightStructures

// ============================================================================
// Per-type builders. Each is DEFINED in the file that ports that structure's
// pieces (levelgen/structure/twilight/*.cpp); the dispatcher only calls them.
// ============================================================================
namespace twilight_pieces {

/** Landmark builder: getFirstPiece + generateFromStartingPiece (see above). */
using LandmarkBuilder = bool (*)(const StructureInfo& info, GenerationContext& ctx,
                                 LegacyRandomSource& firstPieceRandom,
                                 int32_t x, int32_t y, int32_t z, StructureStartData& out);

// HollowHillStructure / HollowHillComponent ("twilightforest:hollow_hill").
bool buildHollowHill(const StructureInfo& info, GenerationContext& ctx,
                     LegacyRandomSource& firstPieceRandom,
                     int32_t x, int32_t y, int32_t z, StructureStartData& out);
TwilightTerraformer hollowHillTerraformer(const StructureInfo& info, const StructureStartData& start,
                                          const ::world::ChunkPos& chunkPos);

// HedgeMazeStructure / HedgeMazeComponent ("twilightforest:hedge_maze").
bool buildHedgeMaze(const StructureInfo& info, GenerationContext& ctx,
                    LegacyRandomSource& firstPieceRandom,
                    int32_t x, int32_t y, int32_t z, StructureStartData& out);
TwilightTerraformer hedgeMazeTerraformer(const StructureInfo& info, const StructureStartData& start,
                                         const ::world::ChunkPos& chunkPos);

// HollowTreeStructure ("twilightforest:hollow_tree") — NOT a landmark: it runs
// its own findGenerationPoint (avoid_landmark_grid placement), so it gets the
// bare context and does the whole Structure.findValidGenerationPoint itself.
bool buildHollowTree(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out);

// NagaCourtyardStructure ("twilightforest:naga_courtyard") — no boss: the
// naga spawner is a marker block.
bool buildNagaCourtyard(const StructureInfo& info, GenerationContext& ctx,
                        LegacyRandomSource& firstPieceRandom,
                        int32_t x, int32_t y, int32_t z, StructureStartData& out);
TwilightTerraformer nagaCourtyardTerraformer(const StructureInfo& info, const StructureStartData& start,
                                             const ::world::ChunkPos& chunkPos);

// QuestGroveStructure ("twilightforest:quest_grove").
bool buildQuestGrove(const StructureInfo& info, GenerationContext& ctx,
                     LegacyRandomSource& firstPieceRandom,
                     int32_t x, int32_t y, int32_t z, StructureStartData& out);

// LichTowerStructure ("twilightforest:lich_tower") — no lich: the boss
// spawner is a marker block.
bool buildLichTower(const StructureInfo& info, GenerationContext& ctx,
                    LegacyRandomSource& firstPieceRandom,
                    int32_t x, int32_t y, int32_t z, StructureStartData& out);
TwilightTerraformer lichTowerTerraformer(const StructureInfo& info, const StructureStartData& start,
                                         const ::world::ChunkPos& chunkPos);

} // namespace twilight_pieces

} // namespace structure
} // namespace levelgen
} // namespace minecraft
