#pragma once

#include "levelgen/density/DensitySampler.h"

#include <memory>

#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "world/IChunk.h"
#include <vector>

// Reference: net/minecraft/world/level/chunk/ChunkGenerator.java
// createStructures() (:425) and createReferences() (:503), wired from
// ChunkStatusTasks.generateStructureStarts / generateStructureReferences.

namespace minecraft {
namespace levelgen {
class ChunkGenerator;
class RandomState;
class WorldGenLevel;
class Beardifier;
namespace structure {

namespace StructureGeneration {

/**
 * Reference: ChunkGenerator.createStructures(). Iterates the state's
 * possibleStructureSets in registry order; per set, skips when any of the
 * set's structures already has a valid start here; on a placement-positive
 * chunk runs the single-entry path or the weighted pick-and-retry loop
 * (WorldgenRandom(Legacy(0)).setLargeFeatureSeed(levelSeed, cx, cz)).
 * Structures not yet ported fail like an invalid start (logged once).
 */
void createStructures(ChunkGeneratorStructureState& state,
                      ChunkGenerator* generator,
                      RandomState* randomState,
                      ::world::IChunk* chunk);

/**
 * Reference: ChunkGenerator.createReferences(). Scans the +-8 neighborhood in
 * the dependency grid, adds a reference into `chunk` for every valid neighbor
 * start whose bounding box intersects this chunk's block square. No RNG.
 * Grid convention: chunks[gridZ][gridX], gridX = cx - centerX + radius.
 */
void createReferences(const std::vector<std::vector<::world::IChunk*>>& chunks,
                      ::world::IChunk* chunk);

/**
 * Engine hand-off for spawn_overrides (see IChunk::StructureSpawnArea): for
 * every structure with spawn overrides referenced by `chunk`, resolve each
 * referenced start through `level` and record its union box and the pieces
 * over this chunk's column. Run after the chunk's decoration, so a piece that
 * settles its height in postProcess (ScatteredFeaturePiece
 * .updateAverageGroundHeight - the swamp hut) is recorded where it ended up,
 * as Java's saved start would hold it. No RNG.
 */
void recordSpawnOverrideAreas(WorldGenLevel* level, ::world::IChunk* chunk);

/**
 * Reference: Beardifier.forStructuresInChunk() + StructureManager
 * .startsForStructure(ChunkPos, predicate). Reads `chunk`'s reference map,
 * resolves each referenced start from the dependency grid (same convention as
 * createReferences), and collects Rigid pieces + jigsaw junctions for every
 * piece within 12 blocks of the chunk. Returns nullptr for Java's EMPTY
 * (the terrain reads the beardifier context field's default, 0).
 *
 * Ordering note: Java iterates a HashMap<Structure, LongSet> (identity-hash
 * order) and a LongOpenHashSet; we iterate name-sorted std::map and numeric
 * std::set. The FP sum order only differs when pieces of two different
 * adapted structures (or two starts of one structure) fall within kernel
 * range (~12 blocks) of the same density point - contributions are exactly
 * 0.0 beyond that.
 */
std::shared_ptr<const levelgen::density::DensitySampler> createBeardifier(
    const std::vector<std::vector<::world::IChunk*>>& chunks,
    ::world::IChunk* chunk);

} // namespace StructureGeneration

} // namespace structure
} // namespace levelgen
} // namespace minecraft
