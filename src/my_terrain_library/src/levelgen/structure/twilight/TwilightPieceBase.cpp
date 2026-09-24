#include "levelgen/structure/twilight/TwilightPieceBase.h"

#include "levelgen/WorldGenLevel.h"
#include "world/IChunk.h"
#include "world/level/block/Block.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

// Reference: world/components/structures/TFStructureComponent.java,
// TFStructureComponentOld.java, loot/TFLootTables.java,
// util/BoundingBoxUtils.java (Twilight Forest 4.9) and vanilla
// StructurePiece.java for the helpers TF inherits unchanged.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_pieces {

using world::level::block::Blocks;
using world::level::block::state::properties::BlockStateProperties;
using core::Direction;

namespace tf_common {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string pathOf(const std::string& id) {
    const std::size_t colon = id.find(':');
    return colon == std::string::npos ? id : id.substr(colon + 1);
}

// Twilight Forest mobs the engine registers (src/common/entity/
// GeneratedEntityTypes: the engine names them "minecraft:<slug>").
bool engineHasTwilightMob(const std::string& path) {
    static const std::set<std::string> kRegistered = {
        "bighorn_sheep", "block_and_chain_goblin", "boar", "deer", "dwarf_rabbit",
        "fire_beetle", "hedge_spider", "helmet_crab", "hostile_wolf", "king_spider",
        "kobold", "lower_goblin_knight", "maze_slime", "minotaur", "mist_wolf",
        "mosquito_swarm", "penguin", "pinch_beetle", "raven", "redcap", "redcap_sapper",
        "skeleton_druid", "slime_beetle", "squirrel", "swarm_spider", "tiny_bird",
        "towerwood_borer", "troll", "upper_goblin_knight", "winter_wolf", "wraith", "yeti",
    };
    return kRegistered.count(path) != 0;
}

// TF mobs the engine does not have yet -> the nearest vanilla mob (same role:
// size, attack style, spawn height). Used only when the mob is missing.
const char* vanillaStandIn(const std::string& path) {
    static const std::unordered_map<std::string, const char*> kStandIns = {
        {"swarm_spider", "minecraft:cave_spider"},
        {"hedge_spider", "minecraft:spider"},
        {"king_spider", "minecraft:spider"},
        {"hostile_wolf", "minecraft:wolf"},
        {"fire_beetle", "minecraft:blaze"},
        {"slime_beetle", "minecraft:slime"},
        {"pinch_beetle", "minecraft:spider"},
        {"wraith", "minecraft:vex"},
        {"redcap_sapper", "minecraft:zombie"},
        {"redcap", "minecraft:zombie"},
        {"kobold", "minecraft:zombie"},
        {"skeleton_druid", "minecraft:skeleton"},
        {"towerwood_borer", "minecraft:silverfish"},
        {"mosquito_swarm", "minecraft:vex"},
        {"maze_slime", "minecraft:slime"},
        {"minotaur", "minecraft:zombified_piglin"},
        {"carminite_broodling", "minecraft:cave_spider"},
        {"carminite_ghastling", "minecraft:ghast"},
        {"carminite_ghastguard", "minecraft:ghast"},
        {"carminite_golem", "minecraft:iron_golem"},
        {"death_tome", "minecraft:vex"},
        {"armored_giant", "minecraft:giant"},
        {"giant_miner", "minecraft:giant"},
        {"blockchain_goblin", "minecraft:vindicator"},
        {"block_and_chain_goblin", "minecraft:vindicator"},
        {"lower_goblin_knight", "minecraft:vindicator"},
        {"upper_goblin_knight", "minecraft:vindicator"},
        {"helmet_crab", "minecraft:silverfish"},
        {"winter_wolf", "minecraft:wolf"},
        {"mist_wolf", "minecraft:wolf"},
        {"yeti", "minecraft:polar_bear"},
        {"troll", "minecraft:ravager"},
        {"stable_ice_core", "minecraft:blaze"},
        {"unstable_ice_core", "minecraft:blaze"},
        {"snow_guardian", "minecraft:stray"},
        {"adherent", "minecraft:evoker"},
        {"harbinger_cube", "minecraft:magma_cube"},
        {"rising_zombie", "minecraft:zombie"},
    };
    auto it = kStandIns.find(path);
    return it == kStandIns.end() ? nullptr : it->second;
}

void logOnce(const std::string& key, const std::string& message) {
    static std::mutex s_mutex;
    static std::set<std::string> s_logged;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_logged.insert(key).second) {
        fprintf(stderr, "[TwilightStructures] %s\n", message.c_str());
    }
}

} // namespace

const char* rotationName(int rotationOrdinal) {
    switch (rotationOrdinal) {
        case OrientedPieceBehavior::ROT_CW90: return "CLOCKWISE_90";
        case OrientedPieceBehavior::ROT_CW180: return "CLOCKWISE_180";
        case OrientedPieceBehavior::ROT_CCW90: return "COUNTERCLOCKWISE_90";
        default: return "NONE";
    }
}

std::string spawnerEntityId(const std::string& entityId) {
    if (!startsWith(entityId, "twilightforest:")) {
        return entityId.find(':') == std::string::npos ? "minecraft:" + entityId : entityId;
    }
    const std::string path = pathOf(entityId);
    if (engineHasTwilightMob(path)) return "minecraft:" + path;
    const char* standIn = vanillaStandIn(path);
    const std::string mapped = standIn != nullptr ? standIn : "minecraft:spider";
    logOnce("spawner:" + path, "spawner mob " + entityId + " not in the engine - stand-in " + mapped);
    return mapped;
}

void writeSpawnerData(WorldGenLevel* level, const core::BlockPos& pos, const std::string& entityId) {
    ::world::IChunk* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4);
    if (chunk == nullptr) return;
    // BaseSpawner defaults + SpawnData (SpawnerBlockEntity.saveAdditional).
    chunk->setBlockEntityNbt(pos,
        "{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,"
        "MinSpawnDelay:200s,RequiredPlayerRange:16s,SpawnCount:4s,"
        "SpawnData:{entity:{id:\"" + spawnerEntityId(entityId) + "\"}},"
        "SpawnPotentials:[],SpawnRange:4s,components:{},"
        "id:\"minecraft:mob_spawner\"}");
}

void setSpawnerInWorld(WorldGenLevel* level, const BoundingBox& chunkBB,
                       const std::string& entityId, const core::BlockPos& pos) {
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
    if (!level->getBlockState(pos)->is(Blocks::SPAWNER)) {
        level->setBlock(pos, Blocks::SPAWNER->defaultBlockState(), 2);
    }
    // SpawnerBlockEntity.setEntityId(type, world.getRandom()): the level
    // random, and no draw at all with empty spawn potentials.
    writeSpawnerData(level, pos, entityId);
}

void writeLootTable(WorldGenLevel* level, const core::BlockPos& pos, const std::string& blockEntityId,
                    const std::string& lootTable, int64_t seed) {
    ::world::IChunk* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4);
    if (chunk == nullptr) return;
    chunk->setBlockEntityNbt(pos,
        "{LootTable:\"" + lootTable + "\",LootTableSeed:" + std::to_string(seed)
        + "l,components:{},id:\"" + blockEntityId + "\"}");
}

std::string lootContainerBlockEntityId(const BlockState* state) {
    if (state == nullptr) return std::string();
    const std::string& id = state->getIdentifier();
    // RandomizableContainerBlockEntity subclasses: chest, trapped chest,
    // barrel, dispenser/dropper, hopper, shulker boxes, crafter, decorated pot
    // is not one (it has its own loot path).
    if (id == "minecraft:chest") return "minecraft:chest";
    if (id == "minecraft:trapped_chest") return "minecraft:trapped_chest";
    if (id == "minecraft:barrel") return "minecraft:barrel";
    if (id == "minecraft:dispenser") return "minecraft:dispenser";
    if (id == "minecraft:dropper") return "minecraft:dropper";
    if (id == "minecraft:hopper") return "minecraft:hopper";
    if (id == "minecraft:crafter") return "minecraft:crafter";
    if (id.size() > 11 && id.compare(id.size() - 11, 11, "shulker_box") == 0) {
        return "minecraft:shulker_box";
    }
    return std::string();
}

int64_t chestContentsSeed(int64_t worldSeed, const core::BlockPos& pos) {
    // Java long arithmetic wraps; do it unsigned to stay defined in C++.
    const uint64_t product = static_cast<uint64_t>(worldSeed) * static_cast<uint64_t>(static_cast<int64_t>(pos.getX()));
    const uint64_t sum = product + static_cast<uint64_t>(static_cast<int64_t>(pos.getY()));
    return static_cast<int64_t>(sum ^ static_cast<uint64_t>(static_cast<int64_t>(pos.getZ())));
}

void generateChest(WorldGenLevel* level, const core::BlockPos& pos, Direction facing,
                   bool trapped, const std::string& lootTable) {
    // Resolved once: the chest and trapped chest in each horizontal facing.
    static BlockState* const* s_states = [] {
        static BlockState* states[2][6] = {};
        const char* names[2] = {"minecraft:chest", "minecraft:trapped_chest"};
        for (int t = 0; t < 2; ++t) {
            BlockState* base = Blocks::getDefaultState(names[t]);
            for (int d = 0; d < 6; ++d) {
                const Direction dir = static_cast<Direction>(d);
                if (base == nullptr) continue;
                states[t][d] = (dir == Direction::UP || dir == Direction::DOWN)
                    ? base
                    : base->trySetValue(*BlockStateProperties::HORIZONTAL_FACING, dir);
            }
        }
        return &states[0][0];
    }();
    BlockState* state = s_states[(trapped ? 6 : 0) + static_cast<int>(facing)];
    if (state == nullptr) return;
    // TFLootTables.generateLootContainer: setBlock, then setLootTable with
    // generateChestContents' position seed.
    level->setBlock(pos, state, 2);
    writeLootTable(level, pos, trapped ? "minecraft:trapped_chest" : "minecraft:chest",
                   lootTable, chestContentsSeed(level->getSeed(), pos));
}

BoundingBox getComponentToAddBoundingBox(int x, int y, int z, int minX, int minY, int minZ,
                                         int spanX, int spanY, int spanZ, int dir, bool centerBounds) {
    // CenterBounds is true for ONLY Hollow Hills, Hydra Lair, & Yeti Caves.
    if (centerBounds) {
        x += (spanX + minX) / 4;
        y += (spanY + minY) / 4;
        z += (spanZ + minZ) / 4;
    }
    switch (dir) {
        case static_cast<int>(Direction::WEST):
            return BoundingBox(x - spanZ + minZ, y + minY, z + minX, x + minZ, y + spanY + minY, z + spanX + minX);
        case static_cast<int>(Direction::NORTH):
            return BoundingBox(x - spanX - minX, y + minY, z - spanZ - minZ, x - minX, y + spanY + minY, z - minZ);
        case static_cast<int>(Direction::EAST):
            return BoundingBox(x + minZ, y + minY, z - spanX, x + spanZ + minZ, y + spanY + minY, z + minX);
        default:
            return BoundingBox(x + minX, y + minY, z + minZ, x + spanX + minX, y + spanY + minY, z + spanZ + minZ);
    }
}

bool intersectionOf(const BoundingBox& a, const BoundingBox& b, BoundingBox& out) {
    if (!a.intersects(b)) return false;
    out = BoundingBox(std::max(a.minX, b.minX), std::max(a.minY, b.minY), std::max(a.minZ, b.minZ),
                      std::min(a.maxX, b.maxX), std::min(a.maxY, b.maxY), std::min(a.maxZ, b.maxZ));
    return true;
}

int32_t javaRound(float value) {
    // Math.round(float) = floor(value + 1/2) evaluated exactly; widening to
    // double first keeps the half-way sum exact (0.49999997f rounds to 0).
    return static_cast<int32_t>(std::floor(static_cast<double>(value) + 0.5));
}

int32_t javaRound(double value) {
    return static_cast<int32_t>(static_cast<int64_t>(std::floor(value + 0.5)));
}

} // namespace tf_common

// ============================================================================
// TFStructureComponentOld
// ============================================================================

TFStructureComponentOld::TFStructureComponentOld(int orientation)
    : OrientedPieceBehavior(orientation) {
    setOrientation(orientation);
}

void TFStructureComponentOld::setOrientation(int orientation) {
    m_orientation = orientation;
    m_mirror = MIRROR_NONE;
    if (orientation < 0) {
        m_rotation = ROT_NONE;
        return;
    }
    switch (static_cast<Direction>(orientation)) {
        case Direction::SOUTH: m_rotation = ROT_CW180; break;
        case Direction::WEST: m_rotation = ROT_CCW90; break;
        case Direction::EAST: m_rotation = ROT_CW90; break;
        default: m_rotation = ROT_NONE; break;
    }
}

int TFStructureComponentOld::worldX(int x, int z) const {
    if (m_orientation < 0) return x;
    const BoundingBox& box = m_self->boundingBox;
    switch (static_cast<Direction>(m_orientation)) {
        case Direction::SOUTH: return box.minX + x;
        case Direction::WEST: return box.maxX - z;
        case Direction::NORTH: return box.maxX - x;  // TF - Add case for NORTH
        case Direction::EAST: return box.minX + z;
        default: return x;
    }
}

int TFStructureComponentOld::worldY(int y) const {
    return m_orientation < 0 ? y : y + m_self->boundingBox.minY;
}

int TFStructureComponentOld::worldZ(int x, int z) const {
    if (m_orientation < 0) return z;
    const BoundingBox& box = m_self->boundingBox;
    switch (static_cast<Direction>(m_orientation)) {
        case Direction::SOUTH: return box.minZ + z;
        case Direction::WEST: return box.minZ + x;
        case Direction::NORTH: return box.maxZ - z;
        case Direction::EAST: return box.maxZ - x;
        default: return z;
    }
}

void TFStructureComponentOld::placeBlock(WorldGenLevel* level, BlockState* state, int x, int y, int z,
                                         const BoundingBox& chunkBB) const {
    if (state == nullptr) return;
    const core::BlockPos pos = worldPos(x, y, z);
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
    state = mirrorRotate(state);
    level->setBlock(pos, state, 2);
    // worldIn.scheduleTick(pos, fluidstate.getType(), 0) when the placed
    // state carries a fluid (the fluid state is the block state's own).
    if (state->hasAnyFluid()) {
        level->scheduleTick(pos, state->hasWaterFluid() ? "minecraft:water" : "minecraft:lava", 0);
    }
}

BlockState* TFStructureComponentOld::getBlock(WorldGenLevel* level, int x, int y, int z,
                                              const BoundingBox& chunkBB) const {
    const core::BlockPos pos = worldPos(x, y, z);
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) {
        return Blocks::AIR->defaultBlockState();
    }
    return level->getBlockState(pos);
}

void TFStructureComponentOld::generateBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                                          int x0, int y0, int z0, int x1, int y1, int z1,
                                          BlockState* edgeBlock, BlockState* fillBlock, bool skipAir) const {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                if (skipAir && getBlock(level, x, y, z, chunkBB)->isAir()) continue;
                if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 && z != z1) {
                    placeBlock(level, fillBlock, x, y, z, chunkBB);
                } else {
                    placeBlock(level, edgeBlock, x, y, z, chunkBB);
                }
            }
        }
    }
}

void TFStructureComponentOld::generateAirBox(WorldGenLevel* level, const BoundingBox& chunkBB,
                                             int x0, int y0, int z0, int x1, int y1, int z1) const {
    BlockState* air = Blocks::AIR->defaultBlockState();
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                placeBlock(level, air, x, y, z, chunkBB);
            }
        }
    }
}

void TFStructureComponentOld::fillColumnDown(WorldGenLevel* level, BlockState* state, int x, int startY,
                                             int z, const BoundingBox& chunkBB) const {
    if (state == nullptr) return;
    core::BlockPos pos = worldPos(x, startY, z);
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
    while (isReplaceableByStructures(level->getBlockState(pos)) && pos.getY() > level->getMinY() + 1) {
        level->setBlock(pos, state, 2);
        pos = pos.below();
    }
}

void TFStructureComponentOld::fillWithBlocks(WorldGenLevel* level, const BoundingBox& chunkBB,
                                             int xMin, int yMin, int zMin, int xMax, int yMax, int zMax,
                                             BlockState* borderState, BlockState* interiorState,
                                             const std::function<bool(BlockState*)>& predicate) const {
    for (int y = yMin; y <= yMax; ++y) {
        for (int x = xMin; x <= xMax; ++x) {
            for (int z = zMin; z <= zMax; ++z) {
                if (!predicate(getBlock(level, x, y, z, chunkBB))) continue;
                const bool isBorder = (yMin != yMax && (y == yMin || y == yMax))
                    || (xMin != xMax && (x == xMin || x == xMax))
                    || (zMin != zMax && (z == zMin || z == zMax));
                placeBlock(level, isBorder ? borderState : interiorState, x, y, z, chunkBB);
            }
        }
    }
}

void TFStructureComponentOld::surroundBlockCardinal(WorldGenLevel* level, BlockState* state,
                                                    int x, int y, int z, const BoundingBox& chunkBB) const {
    placeBlock(level, state, x, y, z - 1, chunkBB);
    placeBlock(level, state, x, y, z + 1, chunkBB);
    placeBlock(level, state, x - 1, y, z, chunkBB);
    placeBlock(level, state, x + 1, y, z, chunkBB);
}

void TFStructureComponentOld::surroundBlockCorners(WorldGenLevel* level, BlockState* state,
                                                   int x, int y, int z, const BoundingBox& chunkBB) const {
    placeBlock(level, state, x - 1, y, z - 1, chunkBB);
    placeBlock(level, state, x - 1, y, z + 1, chunkBB);
    placeBlock(level, state, x + 1, y, z - 1, chunkBB);
    placeBlock(level, state, x + 1, y, z + 1, chunkBB);
}

void TFStructureComponentOld::setSpawner(WorldGenLevel* level, int x, int y, int z,
                                         const BoundingBox& chunkBB, const std::string& entityId) const {
    tf_common::setSpawnerInWorld(level, chunkBB, entityId, worldPos(x, y, z));
}

void TFStructureComponentOld::placeTreasureAtCurrentPosition(WorldGenLevel* level, int x, int y, int z,
                                                             const std::string& lootTable, bool trapped,
                                                             const BoundingBox& chunkBB) const {
    placeTreasureAtWorldPosition(level, lootTable, trapped, chunkBB, worldPos(x, y, z));
}

void TFStructureComponentOld::placeTreasureAtWorldPosition(WorldGenLevel* level, const std::string& lootTable,
                                                           bool trapped, const BoundingBox& chunkBB,
                                                           const core::BlockPos& pos) const {
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
    const std::string& here = level->getBlockState(pos)->getIdentifier();
    if (here == (trapped ? "minecraft:trapped_chest" : "minecraft:chest")) return;
    const Direction facing = m_orientation < 0 ? Direction::NORTH : static_cast<Direction>(m_orientation);
    tf_common::generateChest(level, pos, facing, trapped, lootTable);
}

int TFStructureComponentOld::findGroundLevel(WorldGenLevel* level, const BoundingBox& chunkBB, int start,
                                             const std::function<bool(BlockState*)>& predicate) const {
    const int cx = chunkBB.centerX();
    const int cz = chunkBB.centerZ();
    for (int y = start; y > 0; --y) {
        if (predicate(level->getBlockState(core::BlockPos(cx, y, cz)))) return y;
    }
    return 0;
}

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
