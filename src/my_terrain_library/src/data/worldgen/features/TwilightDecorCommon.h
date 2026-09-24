#pragma once

// Private helpers shared by TwilightDecorFeatures.cpp and
// TwilightTemplateFeatures.cpp (not part of the library's public headers).
// Definitions live in TwilightDecorFeatures.cpp.

#include "core/BlockPos.h"
#include "core/Direction.h"
#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minecraft {
namespace world { namespace level { namespace block { namespace state { class BlockState; }}}}
using BlockState = world::level::block::state::BlockState;
namespace levelgen { class WorldGenLevel; }

namespace data {
namespace worldgen {
namespace features {
namespace twilight_decor {

// Block.UPDATE_* flags as the Java passes them.
constexpr int kUpdateNone = 0;
constexpr int kUpdateAll = 3;             // Block.UPDATE_ALL
constexpr int kUpdateClients = 2;         // Block.UPDATE_CLIENTS
constexpr int kKnownShapeClients = 18;    // UPDATE_KNOWN_SHAPE | UPDATE_CLIENTS
constexpr int kKnownShapeAll = 19;        // 16 | 2 | 1 (GraveyardFeature flags)

using PropertyMap = std::unordered_map<std::string, std::string>;

/**
 * Resolve a block state at bootstrap: "twilightforest:x" through
 * levelgen/TwilightBlocks (real slug first, else the logged stand-in),
 * "minecraft:x" through the same table (26.x renames). Properties the
 * resolved block lacks are dropped. Null (and one log line naming
 * `feature`) when nothing resolves.
 */
BlockState* resolveState(const std::string& name, const PropertyMap& properties, const char* feature);
inline BlockState* resolveState(const std::string& name, const char* feature) {
    return resolveState(name, PropertyMap{}, feature);
}

/** True when the mod's own block is registered (no stand-in in play). */
bool isRealTwilight(const std::string& name);

/** The block id `name` resolves to only when it is the real block, else "". */
std::string realTwilightId(const std::string& name);

/**
 * FeaturePlacers.transferAllStateKeys(stateIn, stateOut): every property of
 * stateOut that stateIn also has (by name, when stateOut accepts the value)
 * takes stateIn's value. Cached per (stateIn, target block); thread-safe.
 */
BlockState* transferAllStateKeys(BlockState* stateIn, BlockState* stateOut);

/** A hand-expanded block tag: exact block ids plus vanilla tag names. */
struct BlockSet {
    std::vector<std::string> ids;
    std::vector<std::string> tags;   // "minecraft:..." tags the library loads

    bool contains(BlockState* state) const;
};

/** #minecraft:substrate_overworld (26.x) = the library's #minecraft:dirt. */
bool isSubstrateOverworld(BlockState* state);

/** #minecraft:leaves or a leaves block. */
bool isLeaves(BlockState* state);

/** #minecraft:logs or a log block. */
bool isLogs(BlockState* state);

/** FeatureLogic.isBlockOk: state.isSolid(). */
bool isBlockOk(BlockState* state);

/**
 * FeatureLogic.isBlockNotOk: liquid, bedrock, a GiantBlock, #clouds or
 * hardened_dark_leaves (TF members only while registered for real).
 */
bool isBlockNotOk(BlockState* state);

/** WorldGenRegion.hasChunk for the chunk holding `pos`. */
bool hasChunkAt(levelgen::WorldGenLevel& level, const core::BlockPos& pos);

/** LevelReader.hasChunk(chunkX, chunkZ). */
bool hasChunk(levelgen::WorldGenLevel& level, int32_t chunkX, int32_t chunkZ);

/**
 * FeatureUtil.isAreaSuitable(world, pos, xWidth, height, zWidth,
 * underwaterAllowed): flat natural ground below, air or replaceables above.
 */
bool isAreaSuitable(levelgen::WorldGenLevel& level, const core::BlockPos& pos,
                    int xWidth, int height, int zWidth, bool underwaterAllowed = false);

/** removeBlock(pos, false) for a position with no fluid: air, UPDATE_ALL. */
bool removeBlock(levelgen::WorldGenLevel& level, const core::BlockPos& pos);

/** Chunk.markPosForPostprocessing(pos). */
void markPosForPostprocessing(levelgen::WorldGenLevel& level, const core::BlockPos& pos);

/** Feature.markAboveForPostProcessing(level, pos). */
void markAboveForPostProcessing(levelgen::WorldGenLevel& level, const core::BlockPos& pos);

/**
 * TFLootTables.generateChestContents seed: level.getSeed() * x + y ^ z
 * (Java precedence: ((seed * x) + y) ^ z in long arithmetic).
 */
int64_t tfLootSeed(const levelgen::WorldGenLevel& level, const core::BlockPos& pos);

/**
 * Store the saved RandomizableContainer block entity
 * {LootTable, LootTableSeed, components, id} at `pos` (the B8 payload shape
 * StructurePiece.createChest writes). `blockEntityId` is the BE type id
 * ("minecraft:chest", "minecraft:trapped_chest", "minecraft:barrel").
 */
void setLootContainer(levelgen::WorldGenLevel& level, const core::BlockPos& pos,
                      const std::string& blockEntityId, const std::string& lootTable, int64_t seed);

/** A worldgen spawner block entity naming `entityId` (BaseSpawner defaults). */
void setSpawnerEntity(levelgen::WorldGenLevel& level, const core::BlockPos& pos, const std::string& entityId);

/** Rotation.rotate(Direction) — Y-axis directions are unchanged. rotation = Rotation ordinal. */
core::Direction rotateDirection(int rotation, core::Direction direction);

/** Mirror.mirror(Direction). mirror = Mirror ordinal (NONE, LEFT_RIGHT, FRONT_BACK). */
core::Direction mirrorDirection(int mirror, core::Direction direction);

/** BlockPos.rotate(Rotation). */
core::BlockPos rotatePos(const core::BlockPos& pos, int rotation);

/** BlockState.rotate(Rotation) / mirror(Mirror) via the structure transforms. */
BlockState* rotateState(BlockState* state, int rotation);
BlockState* mirrorState(BlockState* state, int mirror);

/** Canonical Java name of a Direction ("north", ...). */
const char* directionName(core::Direction direction);

} // namespace twilight_decor
} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
