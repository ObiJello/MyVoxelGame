#pragma once

#include "levelgen/structure/StructureStartData.h"
#include "nbt/CompoundTag.h"
#include "world/ChunkPos.h"
#include <cstdint>
#include <memory>
#include <vector>

// Reference: SerializableChunkData.packStructureData / unpackStructureStart /
// unpackStructureReferences, StructureStart.createTag / loadStaticStart and
// StructurePiece.createTag.
//
// A chunk's "structures" compound. References are MC's exactly: structure id
// -> long array of start chunks. Starts keep MC's layout — {id, ChunkX,
// ChunkZ, references, Children:[{id, BB, O, GD, ...piece state}]} — but live
// under "obeycraft:starts" rather than "starts": this port's pieces are not
// MC's 56 piece classes, so they cannot carry every field vanilla's piece
// loaders require, and vanilla must not try to parse them. MC's own "starts"
// is written empty; vanilla opening the world simply sees no pending starts.
//
// Loading does what vanilla does for ocean monuments
// (OceanMonumentStructure.regeneratePiecesAfterLoad) for every structure: the
// start is regenerated from the seed, and the saved record hands back what
// the seed cannot reproduce — the reference count, each piece's bounding box
// (pieces that settle on the terrain move it) and each piece's placement
// state (StructurePieceBehavior::saveState / loadState).

namespace minecraft {
namespace world {
class IChunk;
}
namespace levelgen {
namespace structure {
namespace StructureSerialization {

// The key under "structures" that holds this port's starts.
constexpr const char* kStartsKey = "obeycraft:starts";

/**
 * Reference: SerializableChunkData.packStructureData.
 */
std::unique_ptr<nbt::CompoundTag> packStructureData(const world::ChunkPos& pos,
                                                    const StructureStartMap& starts,
                                                    const StructureReferenceMap& references);

/**
 * A chunk's "structures" compound, encoded as an NBT payload (the compound's
 * entries and closing End byte) for an embedder that stores it inside its own
 * chunk format. Call where nothing places structure pieces concurrently.
 */
std::vector<uint8_t> encodeStructureData(const world::ChunkPos& pos,
                                         const StructureStartMap& starts,
                                         const StructureReferenceMap& references);

/**
 * Reference: SerializableChunkData.unpackStructureReferences - references
 * farther than 8 chunks are discarded with a warning.
 */
void unpackReferences(const nbt::CompoundTag& structures, const world::ChunkPos& pos,
                      world::IChunk& chunk);

/**
 * The saved starts of a "structures" compound, or null when there are none.
 */
const nbt::CompoundTag* savedStarts(const nbt::CompoundTag& structures);

/**
 * Hand the saved state back to the chunk's regenerated starts. A saved start
 * the seed no longer produces, or whose piece list no longer matches, is
 * reported and left as regenerated.
 */
void restoreStarts(const nbt::CompoundTag& savedStarts, world::IChunk& chunk);

} // namespace StructureSerialization
} // namespace structure
} // namespace levelgen
} // namespace minecraft
