#include "levelgen/structure/AetherStructures.h"
#include "nbt/AllTags.h"

#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/structure/ProcessorLists.h"
#include "levelgen/AetherBlocks.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/AetherFeatures.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "random/LegacyRandomSource.h"
#include "math/Mth.h"
#include "world/IChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "external/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The Aether 1.5.10 — world/structure/{LargeAercloudStructure,
// BronzeDungeonStructure}.java, world/structurepiece/{LargeAercloudChunk,
// AetherTemplateStructurePiece}.java, world/structurepiece/bronzedungeon/*.java,
// world/BlockLogicUtil.java. Structure JSON: data/aether/worldgen/structure/.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace AetherStructures {

namespace {

using nlohmann::json;
using world::level::block::Blocks;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void logOnce(const std::string& key, const std::string& message) {
    static std::mutex s_mutex;
    static std::set<std::string> s_logged;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_logged.insert(key).second) {
        std::cerr << "[AetherStructures] " << message << std::endl;
    }
}

// The structure JSON, parsed once per StructureInfo.
const json& structureJson(const StructureInfo& info) {
    static std::mutex s_mutex;
    static std::unordered_map<const StructureInfo*, json> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(&info);
    if (it != s_cache.end()) return it->second;
    json parsed = info.modJson.empty() ? json::object() : json::parse(info.modJson);
    return s_cache.emplace(&info, std::move(parsed)).first->second;
}

// Direction (horizontal) as the (x, z) step; Java's enum constants.
enum class Dir { NORTH, SOUTH, WEST, EAST };

int stepX(Dir d) { return d == Dir::WEST ? -1 : (d == Dir::EAST ? 1 : 0); }
int stepZ(Dir d) { return d == Dir::NORTH ? -1 : (d == Dir::SOUTH ? 1 : 0); }
bool axisIsX(Dir d) { return d == Dir::WEST || d == Dir::EAST; }

// Rotation ordinals: NONE 0, CLOCKWISE_90 1, CLOCKWISE_180 2, COUNTERCLOCKWISE_90 3.
int rotated(int rotation, int by) { return (rotation + by) & 3; }

// Rotation.rotate(Direction.SOUTH).
Dir rotateSouth(int rotation) {
    switch (rotation & 3) {
        case 1: return Dir::WEST;
        case 2: return Dir::NORTH;
        case 3: return Dir::EAST;
        default: return Dir::SOUTH;
    }
}

const char* rotationName(int rotation) {
    switch (rotation & 3) {
        case 1: return "CLOCKWISE_90";
        case 2: return "CLOCKWISE_180";
        case 3: return "COUNTERCLOCKWISE_90";
        default: return "NONE";
    }
}

// StructureTemplate.getBoundingBox(startPos, rotation, pivot, Mirror.NONE, size).
BoundingBox templateBoundingBox(int sizeX, int sizeY, int sizeZ, const core::BlockPos& startPos,
                                int rotation, const core::BlockPos& pivot) {
    TemplatePlaceSettings settings;
    settings.rotation = rotation;
    settings.mirror = 0;
    settings.rotationPivot = pivot;
    core::BlockPos c1 = TemplateEngine::calculateRelativePosition(settings, core::BlockPos(0, 0, 0));
    core::BlockPos c2 = TemplateEngine::calculateRelativePosition(
        settings, core::BlockPos(sizeX - 1, sizeY - 1, sizeZ - 1));
    BoundingBox box(std::min(c1.getX(), c2.getX()), std::min(c1.getY(), c2.getY()),
                    std::min(c1.getZ(), c2.getZ()), std::max(c1.getX(), c2.getX()),
                    std::max(c1.getY(), c2.getY()), std::max(c1.getZ(), c2.getZ()));
    box.move(startPos.getX(), startPos.getY(), startPos.getZ());
    return box;
}

// BlockLogicUtil.tunnelFromEvenSquareRoom(box, direction, width).
core::BlockPos tunnelFromEvenSquareRoom(const BoundingBox& box, Dir direction, int width) {
    const int offsetFromCenter = ((axisIsX(direction) ? box.getZSpan() : box.getXSpan()) + 1) >> 1;
    const int sidedOffset = width >> 1;
    const int sx = stepX(direction);
    const int sz = stepZ(direction);
    const int xOffset = sx * offsetFromCenter - sz * sidedOffset - std::max(0, sx) + std::min(0, sz);
    const int zOffset = sz * offsetFromCenter + sx * sidedOffset - std::max(0, sz) - std::max(0, sx);
    return core::BlockPos(box.centerX() + xOffset, box.centerY() - (box.getYSpan() >> 1),
                          box.centerZ() + zOffset);
}

// Java Math.floorDiv(int, 16) for block -> chunk.
int blockToChunk(int v) { return v >> 4; }

// ---------------------------------------------------------------------------
// LargeAercloudChunk — world/structurepiece/LargeAercloudChunk.java
// ---------------------------------------------------------------------------

class LargeAercloudChunkBehavior final : public StructurePieceBehavior {
public:
    LargeAercloudChunkBehavior(std::vector<core::BlockPos> positions, BlockState* block)
        : m_positions(std::move(positions)), m_block(block) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos; (void)self;
        if (m_block == nullptr) return;
        // positions.removeIf(pos -> placeBlock(...)): every position inside
        // the chunk box is consumed; it is set only where the world is air.
        // blocks.getState(random, pos) is a simple provider (no draw).
        std::vector<core::BlockPos> remaining;
        for (const core::BlockPos& pos : m_positions) {
            if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) {
                if (level->isEmptyBlock(pos)) level->setBlock(pos, m_block, 2);
            } else {
                remaining.push_back(pos);
            }
        }
        m_positions.swap(remaining);
    }

    // Reference: LargeAercloudChunk.addAdditionalSaveData "Positions" -
    // the cloud blocks not yet placed (the chunks still to come).
    void saveState(nbt::CompoundTag& tag) const override {
        auto positions = std::make_unique<nbt::ListTag>();
        for (const core::BlockPos& pos : m_positions) {
            positions->add(std::make_unique<nbt::IntArrayTag>(
                std::vector<int32_t>{pos.getX(), pos.getY(), pos.getZ()}));
        }
        tag.put("Positions", std::move(positions));
    }
    void loadState(const nbt::CompoundTag& tag, StructurePieceData&) override {
        const nbt::ListTag* positions = tag.getListPtr("Positions");
        if (positions == nullptr) return;
        std::vector<core::BlockPos> restored;
        for (size_t i = 0; i < positions->size(); ++i) {
            const nbt::Tag* value = positions->get(i);
            if (value == nullptr || value->getId() != nbt::TagType::TAG_INT_ARRAY) continue;
            const std::vector<int32_t> xyz = *value->asIntArray();
            if (xyz.size() == 3) restored.emplace_back(xyz[0], xyz[1], xyz[2]);
        }
        m_positions.swap(restored);
    }

private:
    std::vector<core::BlockPos> m_positions;
    BlockState* m_block;
};

// Hash for the LinkedHashMap<ChunkPos, ...> / LinkedHashSet<BlockPos> ports.
struct PosKey {
    int x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
        return std::hash<int64_t>()((static_cast<int64_t>(k.x) * 73856093)
                                    ^ (static_cast<int64_t>(k.y) * 19349663)
                                    ^ (static_cast<int64_t>(k.z) * 83492791));
    }
};

// LargeAercloudStructure.findGenerationPoint + generatePieces.
bool generateLargeAercloud(const StructureInfo& info, GenerationContext& ctx,
                           StructureStartData& out,
                           const std::function<bool(int, int, int)>& validBiomeAt) {
    const json& j = structureJson(info);
    const int size = j.value("size", 3);
    const int rangeY = j.value("rangeY", 32);
    std::string blockName = "aether:cold_aercloud";
    if (j.contains("blocks") && j["blocks"].contains("state")) {
        blockName = j["blocks"]["state"].value("Name", blockName);
    }

    // Structure.onTopOfChunkCenter(context, WORLD_SURFACE_WG, ...): stub at
    // the chunk middle, biome check there, then the (lazy) pieces.
    const int middleX = ctx.chunkX * 16 + 8;
    const int middleZ = ctx.chunkZ * 16 + 8;
    const int stubY = ctx.generator->getBaseHeight(middleX, middleZ, Heightmap::Types::WORLD_SURFACE_WG,
                                                   ctx.randomState) - 1;
    if (!validBiomeAt(middleX, stubY, middleZ)) return false;

    LegacyRandomSource& random = ctx.random;
    const bool direction = random.nextBoolean();
    const int minBuildHeight = ctx.generator->getLevelMinY();
    const int initialY = minBuildHeight + random.nextInt(rangeY);
    int x = ctx.chunkX * 16;
    int y = initialY;
    int z = ctx.chunkZ * 16;
    const int xTendency = random.nextInt(3) - 1;
    const int zTendency = random.nextInt(3) - 1;

    // LinkedHashSet<BlockPos> positions; LinkedHashMap<ChunkPos, ...> chunks.
    std::vector<core::BlockPos> positions;
    std::unordered_map<PosKey, bool, PosKeyHash> seen;
    std::vector<std::pair<int, int>> chunkOrder;
    std::set<std::pair<int, int>> chunkSeen;

    for (int amount = 0; amount < 64; ++amount) {
        x += random.nextInt(3) - 1 + xTendency;
        y += random.nextInt(10) == 0 ? random.nextInt(3) - 1 : 0;
        z += direction ? random.nextInt(3) - 1 + zTendency : -(random.nextInt(3) - 1 + zTendency);

        // The loop bounds re-draw the random on every check, as in Java.
        for (int x1 = x; x1 < x + random.nextInt(4) + 3 * size; ++x1) {
            for (int y1 = y; y1 < y + random.nextInt(1) + 2; ++y1) {
                for (int z1 = z; z1 < z + random.nextInt(4) + 3 * size; ++z1) {
                    if (std::abs(x1 - x) + std::abs(y1 - y) + std::abs(z1 - z) < 4 * size + random.nextInt(2)) {
                        const PosKey key{x1, y1, z1};
                        if (!seen.count(key)) {
                            seen.emplace(key, true);
                            positions.emplace_back(x1, y1, z1);
                        }
                        const std::pair<int, int> chunk{blockToChunk(x1), blockToChunk(z1)};
                        if (chunkSeen.insert(chunk).second) chunkOrder.push_back(chunk);
                    }
                }
            }
        }
    }

    const int finalY = y;
    // Direction.Plane.HORIZONTAL.getRandomDirection(random): the orientation
    // draw (nextInt(4)); it does not move the absolute cloud positions.
    const int orientation = random.nextInt(4);
    static const char* const kOrientationRotation[4] = {"NONE", "CLOCKWISE_90", "NONE", "CLOCKWISE_90"};

    BlockState* block = aether_blocks::defaultState(blockName);
    if (block == nullptr) {
        logOnce("aercloud-block", blockName + " does not resolve - large aerclouds skipped");
        return false;
    }

    // Group the positions by chunk (insertion order kept), one piece per
    // chunk holding blocks.
    std::map<std::pair<int, int>, std::vector<core::BlockPos>> byChunk;
    for (const core::BlockPos& pos : positions) {
        byChunk[{blockToChunk(pos.getX()), blockToChunk(pos.getZ())}].push_back(pos);
    }
    for (const auto& chunk : chunkOrder) {
        auto it = byChunk.find(chunk);
        if (it == byChunk.end() || it->second.empty()) continue;
        StructurePieceData piece;
        piece.pieceType = "aether:large_aercloud";
        piece.boundingBox = BoundingBox(chunk.first * 16, std::max(initialY - 16, 0), chunk.second * 16,
                                        chunk.first * 16 + 15, finalY + 16, chunk.second * 16 + 15);
        piece.rotation = kOrientationRotation[orientation & 3];
        out.pieces.push_back(std::move(piece));
        out.behaviors.push_back(std::make_shared<LargeAercloudChunkBehavior>(it->second, block));
    }
    return !out.pieces.empty();
}

// ---------------------------------------------------------------------------
// Bronze dungeon — pieces
// ---------------------------------------------------------------------------

const char* const kBronzeRoomType = "aether:bronze_dungeon_room";
const char* const kBronzeBossType = "aether:bronze_boss_room";
const char* const kBronzeTunnelType = "aether:bronze_tunnel";
const char* const kBronzeRuinsType = "aether:bronze_surface_ruins";

// Loot tables (AetherLoot.BRONZE_DUNGEON / BRONZE_DUNGEON_REWARD).
const char* const kBronzeLoot = "aether:chests/dungeon/bronze/bronze_dungeon";
const char* const kBronzeRewardLoot = "aether:chests/dungeon/bronze/bronze_dungeon_reward";

void setLootChest(WorldGenLevel* level, const core::BlockPos& pos, const char* lootTable, int64_t seed) {
    if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
        chunk->setBlockEntityNbt(pos, std::string("{LootTable:\"") + lootTable + "\",LootTableSeed:"
                                          + std::to_string(seed) + "l,components:{},id:\"minecraft:chest\"}");
    }
}

// Where a boss would be spawned. Bosses come later; until then the boss room
// reports the spot once per room so a dungeon is easy to find and check.
void markPendingBoss(const char* bossId, const core::BlockPos& pos) {
    const std::string key = std::string(bossId) + "@" + std::to_string(pos.getX()) + ","
                          + std::to_string(pos.getY()) + "," + std::to_string(pos.getZ());
    logOnce(key, std::string("PENDING BOSS ") + bossId + " at " + std::to_string(pos.getX()) + " "
                 + std::to_string(pos.getY()) + " " + std::to_string(pos.getZ())
                 + " (template entity not spawned - bosses are a later pass)");
}

// Loot tables (AetherLoot.SILVER_DUNGEON / SILVER_DUNGEON_REWARD / GOLD_DUNGEON_REWARD).
const char* const kSilverLoot = "aether:chests/dungeon/silver/silver_dungeon";
const char* const kSilverRewardLoot = "aether:chests/dungeon/silver/silver_dungeon_reward";

/**
 * AetherTemplateStructurePiece subclasses: TemplateStructurePiece.postProcess
 * (template placed with the piece's processor list, then its DATA markers)
 * plus each class's handleDataMarker:
 *   BRONZE_ROOM  BronzeDungeonRoom   every marker -> air; "Chest" -> chest or mimic
 *   SILVER_ROOM  SilverDungeonRoom   "Chest" -> air, chest or mimic at a random
 *                                    spot of the piece box
 *   REWARD       Bronze/SilverBossRoom "Treasure Chest" -> reward loot below, air
 *   NONE         BronzeTunnel, SilverTemplePiece, SilverFloorPiece
 * A boss room also reports its template boss entity (markPendingBoss).
 */
class AetherTemplateBehavior final : public StructurePieceBehavior {
public:
    enum class Markers { NONE, BRONZE_ROOM, SILVER_ROOM, REWARD };

    struct Config {
        Markers markers = Markers::NONE;
        std::string templateId;
        core::BlockPos templatePosition{0, 0, 0};
        int rotation = 0;
        core::BlockPos pivot{0, 0, 0};
        std::string processors;          // processor list id ("" = none)
        BoundingBox pieceBox;            // this.boundingBox (SilverDungeonRoom chest spot)
        const char* rewardLoot = nullptr;
        const char* bossId = nullptr;    // template boss entity, reported only
        core::BlockPos bossLocal{0, 0, 0};
        // SilverBossRoom.makeBoxProcessor: locked angelic stone on the
        // template border lights up (0.05), before the list processors.
        bool silverBorderLights = false;
        int sizeX = 0, sizeY = 0, sizeZ = 0;
    };

    explicit AetherTemplateBehavior(Config config) : m_config(std::move(config)) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator; (void)chunkPos; (void)self;
        TemplatePlaceSettings settings;
        settings.rotation = m_config.rotation;
        settings.mirror = 0;
        settings.rotationPivot = m_config.pivot;
        settings.ignoreAir = false;
        settings.keepLiquids = true;   // StructurePlaceSettings default (APPLY_WATERLOGGING)
        if (m_config.silverBorderLights) {
            appendSilverBorderProcessor(settings);
        }
        if (!m_config.processors.empty()) {
            ProcessorLists::appendProcessors(m_config.processors, settings, level);
        }
        if (!TemplateEngine::placeInWorld(level, m_config.templateId, m_config.templatePosition,
                                          referencePos, settings, random, chunkBB)) {
            return;
        }
        for (const TemplateEngine::DataMarker& marker :
             TemplateEngine::dataMarkers(m_config.templateId, m_config.templatePosition, settings, chunkBB)) {
            handleDataMarker(marker.metadata, marker.pos, level, random, chunkBB);
        }
        if (m_config.bossId != nullptr) {
            const core::BlockPos& t = m_config.templatePosition;
            core::BlockPos boss = TemplateEngine::calculateRelativePosition(settings, m_config.bossLocal)
                                      .offset(t.getX(), t.getY(), t.getZ());
            if (chunkBB.isInside(boss.getX(), boss.getY(), boss.getZ())) {
                markPendingBoss(m_config.bossId, boss);
            }
        }
    }

private:
    void appendSilverBorderProcessor(TemplatePlaceSettings& settings) const {
        // RuleProcessor([RandomBlockMatchTest(locked_angelic_stone, 0.05),
        // AlwaysTrueTest, BorderBoxPosTest(0, 1, 0, x-1, y-1, z-1)] ->
        // locked_light_angelic_stone): per-block positional random, input
        // draw first; BorderBoxPosTest reads the template-local position.
        const std::string lockedName = aether_blocks::resolveName("aether:locked_angelic_stone");
        world::level::block::Block* locked = lockedName.empty() ? nullptr : Blocks::getBlock(lockedName);
        BlockState* light = aether_blocks::defaultState("aether:locked_light_angelic_stone");
        if (locked == nullptr || light == nullptr) return;
        const int maxX = m_config.sizeX - 1;
        const int maxY = m_config.sizeY - 1;
        const int maxZ = m_config.sizeZ - 1;
        settings.processors.push_back(
            [locked, light, maxX, maxY, maxZ](const core::BlockPos& worldPos, BlockState* state,
                                               const core::BlockPos& local, BlockState*,
                                               const core::BlockPos&) -> BlockState* {
                LegacyRandomSource ruleRandom(Mth::getSeed(worldPos.getX(), worldPos.getY(), worldPos.getZ()));
                if (state == nullptr || !state->is(locked)) return state;
                if (!(ruleRandom.nextFloat() < 0.05f)) return state;
                const bool border = local.getX() == 0 || local.getX() == maxX
                                 || local.getY() == 1 || local.getY() == maxY
                                 || local.getZ() == 0 || local.getZ() == maxZ;
                return border ? light : state;
            });
    }

    // StructurePiece.createChest(level, box, random, pos, loot, state) with
    // the chest-or-mimic pick already drawn.
    void createChestOrMimic(WorldGenLevel* level, const BoundingBox& box, WorldgenRandom& random,
                            const core::BlockPos& pos, bool plainChest, const char* loot) {
        if (!box.isInside(pos.getX(), pos.getY(), pos.getZ())
            || level->getBlockState(pos)->is(Blocks::CHEST)) {
            return;
        }
        if (plainChest) {
            level->setBlock(pos, Blocks::CHEST->defaultBlockState(), 2);
            // ChestBlockEntity.setLootTable(loot, random.nextLong()).
            setLootChest(level, pos, loot, random.nextLong());
        } else {
            // aether:chest_mimic — its block entity is not a ChestBlockEntity,
            // so no loot seed is drawn. Until the mimic lands its stand-in is
            // an empty chest.
            BlockState* mimic = aether_blocks::defaultState("aether:chest_mimic");
            level->setBlock(pos, mimic ? mimic : Blocks::CHEST->defaultBlockState(), 2);
        }
    }

    void handleDataMarker(const std::string& name, const core::BlockPos& pos, WorldGenLevel* level,
                          WorldgenRandom& random, const BoundingBox& box) {
        BlockState* air = Blocks::AIR->defaultBlockState();
        switch (m_config.markers) {
            case Markers::BRONZE_ROOM:
                // BronzeDungeonRoom.handleDataMarker.
                level->setBlock(pos, air, 2);
                if (name == "Chest") {
                    const bool plainChest = random.nextInt(5) > 1;
                    createChestOrMimic(level, box, random, pos, plainChest, kBronzeLoot);
                }
                break;
            case Markers::SILVER_ROOM:
                // SilverDungeonRoom.handleDataMarker + placeChestOrMimic.
                if (name == "Chest") {
                    level->setBlock(pos, air, 2);
                    const BoundingBox& piece = m_config.pieceBox;
                    const int cx = piece.minX + random.nextInt(piece.getXSpan());
                    const int cz = piece.minZ + random.nextInt(piece.getZSpan());
                    const core::BlockPos chestPos(cx, pos.getY(), cz);
                    const core::BlockPos at = box.isInside(cx, pos.getY(), cz) ? chestPos : pos;
                    const bool plainChest = random.nextInt(5) > 1;
                    // Direction.from2DDataValue(random.nextInt(4)) — drawn, but
                    // the Java discards the setValue result.
                    (void)random.nextInt(4);
                    createChestOrMimic(level, box, random, at, plainChest, kSilverLoot);
                }
                break;
            case Markers::REWARD:
                // Bronze/SilverBossRoom.handleDataMarker.
                if (name == "Treasure Chest" && m_config.rewardLoot != nullptr) {
                    const core::BlockPos chest = pos.below();
                    // The template's treasure chest is a RandomizableContainer:
                    // setLootTable(<reward>, random.nextLong()).
                    const int64_t seed = random.nextLong();
                    BlockState* below = level->getBlockState(chest);
                    if (below != nullptr && !below->isAir()) {
                        setLootChest(level, chest, m_config.rewardLoot, seed);
                    }
                    // TreasureChestBlockEntity.setDungeonType: no engine
                    // counterpart yet (the chest is not locked).
                    level->setBlock(pos, air, 2);
                }
                break;
            case Markers::NONE:
                break;
        }
    }

    Config m_config;
};

// ---------------------------------------------------------------------------
// BronzeDungeonSurfaceRuins — bronzedungeon/BronzeDungeonSurfaceRuins.java
// ---------------------------------------------------------------------------

using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::WeightedStateProvider;
using levelgen::feature::stateproviders::WeightedStateEntry;

struct RuinsProviders {
    std::shared_ptr<WeightedStateProvider> blocks;
    std::shared_ptr<WeightedStateProvider> tops;
    std::shared_ptr<WeightedStateProvider> bottoms;
    // MIXED_FLOWER_PATCH: Feature.FLOWER over grassPatch(purple 1 : white 1, 24).
    std::shared_ptr<WeightedStateProvider> flowers;
    std::unique_ptr<ConfiguredFeature> flowerInner;
    std::unique_ptr<placement::PlacedFeature> flowerInnerPlaced;
    std::unique_ptr<placement::BlockPredicateFilter> onlyInAir;
    std::unique_ptr<RandomPatchConfiguration> patchConfig;
    std::unique_ptr<ConfiguredFeature> flowerPatch;
};

const RuinsProviders& ruinsProviders() {
    static RuinsProviders providers;
    static std::once_flag once;
    std::call_once(once, [] {
        auto st = [](const char* name, const std::unordered_map<std::string, std::string>& props) {
            BlockState* s = aether_blocks::state(name, props);
            return s ? s : Blocks::AIR->defaultBlockState();
        };
        BlockState* holystone = st("aether:holystone", {{"double_drops", "true"}});
        BlockState* mossy = st("aether:mossy_holystone", {{"double_drops", "true"}});
        BlockState* slab = st("aether:holystone_slab", {});
        BlockState* mossySlab = st("aether:mossy_holystone_slab", {});
        BlockState* slabTop = st("aether:holystone_slab", {{"type", "top"}});
        BlockState* mossySlabTop = st("aether:mossy_holystone_slab", {{"type", "top"}});
        providers.blocks = std::make_shared<WeightedStateProvider>(std::vector<WeightedStateEntry>{
            WeightedStateEntry(holystone, 3), WeightedStateEntry(mossy, 1)});
        providers.tops = std::make_shared<WeightedStateProvider>(std::vector<WeightedStateEntry>{
            WeightedStateEntry(holystone, 6), WeightedStateEntry(slab, 6),
            WeightedStateEntry(mossy, 3), WeightedStateEntry(mossySlab, 3)});
        providers.bottoms = std::make_shared<WeightedStateProvider>(std::vector<WeightedStateEntry>{
            WeightedStateEntry(holystone, 6), WeightedStateEntry(slabTop, 6),
            WeightedStateEntry(mossy, 3), WeightedStateEntry(mossySlabTop, 3)});

        BlockState* purple = st("aether:purple_flower", {});
        BlockState* white = st("aether:white_flower", {});
        providers.flowers = std::make_shared<WeightedStateProvider>(std::vector<WeightedStateEntry>{
            WeightedStateEntry(purple, 1), WeightedStateEntry(white, 1)});
        static SimpleBlockFeature s_simpleBlock;
        static RandomPatchFeature s_randomPatch;
        providers.flowerInner = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlock, SimpleBlockConfiguration(providers.flowers.get(), false));
        providers.onlyInAir = std::make_unique<placement::BlockPredicateFilter>(
            placement::BlockPredicateFilter::forPredicate(blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE));
        providers.flowerInnerPlaced = std::make_unique<placement::PlacedFeature>(
            providers.flowerInner.get(), std::vector<placement::PlacementModifier*>{providers.onlyInAir.get()},
            "aether:bronze_ruins_flower_inline");
        providers.patchConfig = std::make_unique<RandomPatchConfiguration>(
            24, 7, 3, providers.flowerInnerPlaced.get());
        providers.flowerPatch = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatch, *providers.patchConfig);
    });
    return providers;
}

bool isIslandBlock(BlockState* state) {
    // #aether:aether_island_blocks = aether_dirt, aether_grass_block, holystone.
    if (state == nullptr) return false;
    const std::string& id = state->getIdentifier();
    return id == "minecraft:aether_dirt" || id == "minecraft:aether_grass_block"
        || id == "minecraft:holystone";
}

class BronzeSurfaceRuinsBehavior final : public StructurePieceBehavior {
public:
    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)chunkPos; (void)referencePos;
        const BoundingBox& box = self.boundingBox;
        for (int x = box.minX; x <= box.maxX; ++x) {
            generateTunnelWallColumn(level, random, chunkBB, box, x, box.minZ);
            generateTunnelWallColumn(level, random, chunkBB, box, x, box.maxZ);
        }
        for (int z = box.minZ + 1; z < box.maxZ; ++z) {
            generateTunnelWallColumn(level, random, chunkBB, box, box.minX, z);
            generateTunnelWallColumn(level, random, chunkBB, box, box.maxX, z);
        }
        if (chunkBB.isInside(box.centerX(), box.centerY(), box.centerZ())) {
            // level.getHeightmapPos(OCEAN_FLOOR_WG, chunkBounds.getCenter()).
            const int cx = chunkBB.centerX();
            const int cz = chunkBB.centerZ();
            const core::BlockPos aboveTopBlock(cx, level->getHeight(Heightmap::Types::OCEAN_FLOOR_WG, cx, cz), cz);
            ruinsProviders().flowerPatch->place(level, generator, random, aboveTopBlock);
        }
    }

private:
    static void generateTunnelWallColumn(WorldGenLevel* level, WorldgenRandom& random,
                                         const BoundingBox& chunkBB, const BoundingBox& box, int x, int z) {
        if (!chunkBB.isInside(x, box.minY, z)) return;
        const RuinsProviders& p = ruinsProviders();
        // Java: ((x + z) & 1) == 1 || random.nextBoolean() ? n(4) + n(4) + 1 : 0
        const bool trim = ((x + z) & 1) == 1 || random.nextBoolean();
        int trimmer = 0;
        if (trim) {
            const int a = random.nextInt(4);
            const int b = random.nextInt(4);
            trimmer = a + b + 1;
        }
        const int r1 = random.nextInt(2);
        const int r2 = random.nextInt(2);
        const int wallColumnHeight = std::max(r1 - r2 - trimmer, -2);

        const int ySurfaceAir = level->getHeight(Heightmap::Types::OCEAN_FLOOR_WG, x, z);
        const core::BlockPos wallColumnStart(x, ySurfaceAir, z);

        placeColumnBlocks(level, random,
                          scanColumnForPlacement(level, random, box, wallColumnHeight, ySurfaceAir, wallColumnStart));

        const core::BlockPos topPos = wallColumnStart.above(wallColumnHeight);
        const std::shared_ptr<WeightedStateProvider>& topProvider = wallColumnHeight < 1 ? p.blocks : p.tops;
        level->setBlock(topPos, topProvider->getState(random, topPos), 3);
    }

    static void placeColumnBlocks(WorldGenLevel* level, WorldgenRandom& random,
                                  const std::vector<core::BlockPos>& forPlacement) {
        if (forPlacement.empty()) return;
        const RuinsProviders& p = ruinsProviders();
        core::BlockPos lastPos = forPlacement.front().below();
        for (const core::BlockPos& posAt : forPlacement) {
            const bool hasSkippedGap = posAt.getY() - lastPos.getY() != 1;
            if (hasSkippedGap) {
                BlockState* at = level->getBlockState(posAt);
                const auto& tailProvider = (at == nullptr || at->isAir()) ? p.bottoms : p.blocks;
                level->setBlock(posAt, tailProvider->getState(random, posAt), 3);
                const core::BlockPos capPos = lastPos.above();
                BlockState* capAt = level->getBlockState(capPos);
                const auto& capProvider = (capAt == nullptr || capAt->isAir()) ? p.tops : p.blocks;
                level->setBlock(capPos, capProvider->getState(random, capPos), 3);
            } else {
                level->setBlock(posAt, p.blocks->getState(random, posAt), 3);
            }
            lastPos = posAt;
        }
    }

    static std::vector<core::BlockPos> scanColumnForPlacement(WorldGenLevel* level, WorldgenRandom& random,
                                                              const BoundingBox& box, int wallColumnHeight,
                                                              int ySurfaceAir, const core::BlockPos& start) {
        // Math.round(random.nextFloat() * wallColumnHeight): floor(f + 0.5f).
        const float f = random.nextFloat() * static_cast<float>(wallColumnHeight);
        const int depthOffset = static_cast<int>(std::floor(f + 0.5f));
        std::vector<core::BlockPos> toPlace;
        for (int dY = box.minY - ySurfaceAir; dY < wallColumnHeight; ++dY) {
            const core::BlockPos posAt = start.above(dY);
            BlockState* blockAt = level->getBlockState(posAt);
            const bool airOrIsland = blockAt == nullptr || blockAt->isAir() || isIslandBlock(blockAt);
            if (dY >= -4 || (airOrIsland && isIslandBlock(level->getBlockState(posAt.below(depthOffset))))) {
                toPlace.push_back(posAt);
            }
        }
        return toPlace;
    }
};

// ---------------------------------------------------------------------------
// Bronze dungeon — BronzeDungeonBuilder.java
// ---------------------------------------------------------------------------

struct BronzeSettings {
    int maxRooms = 8;
    int aboveBottom = 32;
    int belowTop = 24;
    std::string roomProcessors = "aether:bronze_room";
    std::string tunnelProcessors = "aether:bronze_tunnel";
    std::string bossProcessors = "aether:bronze_boss_room";
};

struct TemplateSize {
    int x = 0, y = 0, z = 0;
};

TemplateSize templateSize(const std::string& id) {
    const FullTemplateData& data = TemplateEngine::get(id);
    TemplateSize size;
    if (!data.empty()) {
        size.x = data.sizeX;
        size.y = data.sizeY;
        size.z = data.sizeZ;
    }
    return size;
}

struct BronzePiece {
    enum class Kind { ROOM, BOSS, TUNNEL, RUINS };
    Kind kind = Kind::ROOM;
    std::string name;               // template short name ("chest_room", ...)
    core::BlockPos templatePosition{0, 0, 0};
    int rotation = 0;
    core::BlockPos pivot{0, 0, 0};
    BoundingBox box;
    std::string processors;
};

class BronzeDungeonBuilder {
public:
    BronzeDungeonBuilder(GenerationContext& ctx, const BronzeSettings& settings)
        : m_ctx(ctx), m_random(ctx.random), m_settings(settings) {
        const TemplateSize node = templateSize("aether:bronze_dungeon/chest_room");
        m_nodeWidth = node.x;
        const TemplateSize edge = templateSize("aether:bronze_dungeon/square_tunnel");
        m_edgeWidth = edge.x;
        m_edgeLength = edge.z;
        m_maxSize = std::max(3, settings.maxRooms);
    }

    // initializeDungeon(startPos, context, builder).
    void initializeDungeon(const core::BlockPos& startPos, StructureStartData& out) {
        const TemplateSize boss = templateSize("aether:bronze_dungeon/boss_room");
        int rotation = 0;
        if (!getBossRoomRotation(startPos, startPos.offset(boss.x, boss.y, boss.z), rotation)) {
            return;   // not enough covered space for the first rooms
        }
        BronzePiece* bossRoom = chooseRoom("boss_room", startPos, rotation, m_settings.bossProcessors);
        const Dir direction = rotateSouth(bossRoom->rotation);
        BoundingBox raised = bossRoom->box;
        raised.move(0, 2, 0);
        core::BlockPos pos = tunnelFromEvenSquareRoom(raised, direction, m_edgeWidth);
        BronzePiece* hallway = chooseRoom("square_tunnel", pos, bossRoom->rotation, m_settings.roomProcessors);
        pos = tunnelFromEvenSquareRoom(hallway->box, direction, m_nodeWidth);
        BronzePiece* defaultRoom = chooseRoom("chest_room", pos, hallway->rotation, m_settings.roomProcessors);

        m_nodes.push_back(bossRoom);
        m_nodes.push_back(defaultRoom);
        connect(bossRoom, defaultRoom, hallway, direction);

        for (int i = 2; i < m_maxSize - 1; ++i) {
            propagateRooms(defaultRoom, false);
        }
        propagateRooms(defaultRoom, true);
        BronzePiece* lobby = m_nodes.back();
        buildEndTunnel(lobby, startPos);
        buildSurfaceTunnel();
        populatePieces(out);
    }

private:
    struct Connection {
        BronzePiece* start;
        BronzePiece* end;
        BronzePiece* hallway;
    };

    BronzePiece* makePiece(const std::string& name, const core::BlockPos& pos, int rotation,
                           const std::string& processors) {
        auto piece = std::make_unique<BronzePiece>();
        piece->name = name;
        piece->templatePosition = pos;
        piece->rotation = rotation & 3;
        piece->processors = processors;
        const TemplateSize size = templateSize("aether:bronze_dungeon/" + name);
        if (name == "boss_room") {
            // AetherTemplateStructurePiece.makeSettingsWithPivot.
            piece->kind = BronzePiece::Kind::BOSS;
            piece->pivot = core::BlockPos(size.x >> 1, 0, size.z >> 1);
        } else if (name == "end_corridor") {
            piece->kind = BronzePiece::Kind::TUNNEL;
        } else {
            piece->kind = BronzePiece::Kind::ROOM;
        }
        piece->box = templateBoundingBox(size.x, size.y, size.z, pos, piece->rotation, piece->pivot);
        BronzePiece* raw = piece.get();
        m_arena.push_back(std::move(piece));
        return raw;
    }

    // chooseRoom: ROOM_OPTIONS[name].getRandomValue(random) — one entry of
    // weight 1, which still draws nextInt(1).
    BronzePiece* chooseRoom(const std::string& name, const core::BlockPos& pos, int rotation,
                            const std::string& processors) {
        (void)m_random.nextInt(1);
        return makePiece(name, pos, rotation, processors);
    }

    void connect(BronzePiece* start, BronzePiece* end, BronzePiece* hallway, Dir direction) {
        auto& map = m_edges[start];
        auto existing = map.find(direction);
        if (existing != map.end()) {
            // HashMap.put replaces the edge (its hallway leaves the builder).
            m_connectionOrder.erase(std::remove(m_connectionOrder.begin(), m_connectionOrder.end(),
                                                existing->second.hallway),
                                    m_connectionOrder.end());
        }
        map[direction] = Connection{start, end, hallway};
        m_connectionOrder.push_back(hallway);
    }

    bool hasConnection(BronzePiece* node, Dir direction) const {
        auto it = m_edges.find(node);
        return it != m_edges.end() && it->second.count(direction) != 0;
    }

    // StructurePiece.findCollisionPiece(nodes, box).
    BronzePiece* findCollisionPiece(const BoundingBox& box) const {
        for (BronzePiece* piece : m_nodes) {
            if (piece != nullptr && piece->box.intersects(box)) return piece;
        }
        return nullptr;
    }

    bool propagateRooms(BronzePiece* currentNode, bool placeLobby) {
        int rotation = currentNode->rotation;
        std::vector<int> rotations = {rotated(rotation, 3), rotation, rotated(rotation, 1)};
        const std::string roomName = placeLobby ? "lobby" : "chest_room";

        for (int i = 3; i > 0; --i) {
            const int pick = m_random.nextInt(i);
            rotation = rotations[static_cast<size_t>(pick)];
            rotations.erase(rotations.begin() + pick);
            const Dir direction = rotateSouth(rotation);
            if (hasConnection(currentNode, direction)) {
                if (propagateRooms(m_edges[currentNode][direction].end, placeLobby)) {
                    return true;
                }
            } else {
                core::BlockPos pos = tunnelFromEvenSquareRoom(currentNode->box, direction, m_edgeWidth);
                BronzePiece* hallway = chooseRoom("square_tunnel", pos, rotation, m_settings.roomProcessors);
                pos = tunnelFromEvenSquareRoom(hallway->box, direction, m_nodeWidth);
                BronzePiece* room = chooseRoom(roomName, pos, rotation, m_settings.roomProcessors);
                BronzePiece* collisionPiece = findCollisionPiece(room->box);

                if (isCloseToCenter(room->templatePosition) && isCoveredAtPos(room->box)) {
                    if (collisionPiece == nullptr) {
                        connect(currentNode, room, hallway, direction);
                        m_nodes.push_back(room);
                        return true;
                    } else if (collisionPiece->kind != BronzePiece::Kind::BOSS) {
                        // If there's a piece in the way, see if it already
                        // connects back to this node; if not, make one.
                        bool flag = false;
                        for (const auto& [dir, connection] : m_edges[collisionPiece]) {
                            (void)dir;
                            if (connection.end == currentNode) { flag = true; break; }
                        }
                        if (!flag) {
                            connect(currentNode, room, hallway, direction);
                        }
                    }
                }
            }
        }
        return false;
    }

    void buildEndTunnel(BronzePiece* lobby, const core::BlockPos& origin) {
        int rotation = lobby->rotation;
        std::vector<int> rotations = {rotated(rotation, 3), rotation, rotated(rotation, 1)};
        std::vector<BronzePiece*> longestTunnel;
        bool haveLongest = false;
        for (int i = 3; i > 0; --i) {
            std::vector<BronzePiece*> tunnel;
            const int pick = m_random.nextInt(i);
            rotation = rotations[static_cast<size_t>(pick)];
            rotations.erase(rotations.begin() + pick);
            const Dir direction = rotateSouth(rotation);
            if (buildTunnelFromRoom(lobby, tunnel, rotation, direction, origin)) {
                longestTunnel = tunnel;
                haveLongest = true;
                break;
            } else if (!haveLongest || tunnel.size() > longestTunnel.size()) {
                longestTunnel = tunnel;
                haveLongest = true;
            }
        }
        m_nodes.insert(m_nodes.end(), longestTunnel.begin(), longestTunnel.end());
    }

    bool buildTunnelFromRoom(BronzePiece* connectedRoom, std::vector<BronzePiece*>& list, int rotation,
                             Dir direction, const core::BlockPos& origin) {
        const TemplateSize entranceSize = templateSize("aether:bronze_dungeon/entrance");
        core::BlockPos startPos = tunnelFromEvenSquareRoom(connectedRoom->box, direction, entranceSize.x);
        BronzePiece* entrance = chooseRoom("entrance", startPos, rotation, m_settings.roomProcessors);
        list.push_back(entrance);
        startPos = startPos.offset(stepX(direction), 0, stepZ(direction));

        const int length = std::max(1, entranceSize.z);
        bool noOverlap = false;
        bool reachedAir = false;
        core::BlockPos pos = startPos;
        int i = 0;
        do {
            pos = startPos.offset(stepX(direction) * i, 0, stepZ(direction) * i);
            BronzePiece* tunnel = chooseRoom("end_corridor", pos, rotation, m_settings.tunnelProcessors);

            // Skip the connected piece, since the tunnel will be digging into it.
            BronzePiece* col = nullptr;
            for (BronzePiece* piece : m_nodes) {
                if (piece != nullptr && piece != connectedRoom && piece->box.intersects(tunnel->box)) {
                    col = piece;
                    break;
                }
            }
            if (col != nullptr) {
                break;
            }
            noOverlap = true;
            list.push_back(tunnel);
            connectedRoom = tunnel;
            i += length;

            // If the tunnel doesn't find an opening, we can try making another one.
            if (checkForAirAtPos(pos.getX(), pos.getY(), pos.getZ())
                && checkForAirAtPos(pos.getX(), tunnel->box.maxY, pos.getZ())) {
                reachedAir = true;
                break;
            }
        } while (std::abs(origin.getX() - pos.getX()) < 100 && std::abs(origin.getZ() - pos.getZ()) < 100);

        return noOverlap && reachedAir;
    }

    // seekLastRoomNode(minWidth).
    BronzePiece* seekLastRoomNode(int minWidth) const {
        for (auto it = m_nodes.rbegin(); it != m_nodes.rend(); ++it) {
            if ((*it)->box.getXSpan() > minWidth && (*it)->box.getZSpan() > minWidth) return *it;
        }
        return nullptr;
    }

    void buildSurfaceTunnel() {
        const int shrink = 3;
        BronzePiece* lobby = seekLastRoomNode(shrink * 2);
        if (lobby == nullptr) return;
        const BoundingBox& lobbyBounds = lobby->box;
        // chunkGenerator.getFirstOccupiedHeight(center, OCEAN_FLOOR_WG).
        const int topSurfaceY = m_ctx.generator->getBaseHeight(lobbyBounds.centerX(), lobbyBounds.centerZ(),
                                                               Heightmap::Types::OCEAN_FLOOR_WG,
                                                               m_ctx.randomState) - 1;
        const int roomCeiling = lobbyBounds.maxY + 1;
        if (roomCeiling > topSurfaceY) return;
        const int ruinsTopY = std::max(roomCeiling, topSurfaceY + 4);
        const int minX = lobbyBounds.minX + shrink;
        const int minZ = lobbyBounds.minZ + shrink;
        const int maxX = lobbyBounds.maxX - shrink;
        const int maxZ = lobbyBounds.maxZ - shrink;
        auto ruins = std::make_unique<BronzePiece>();
        ruins->kind = BronzePiece::Kind::RUINS;
        ruins->name = "surface_ruins";
        ruins->box = BoundingBox(std::min(minX, maxX), roomCeiling, std::min(minZ, maxZ),
                                 std::max(minX, maxX), ruinsTopY, std::max(minZ, maxZ));
        m_nodes.push_back(ruins.get());
        m_arena.push_back(std::move(ruins));
    }

    // populatePiecesBuilder: every node but the boss room, then the hallways,
    // then the boss room last (its doorway blocks must not be dug into). The
    // Java hallway order is HashMap order (identity hashes, varying per run);
    // the port uses creation order.
    void populatePieces(StructureStartData& out) {
        if (m_nodes.empty()) return;
        BronzePiece* bossRoom = m_nodes.front();
        for (size_t i = 1; i < m_nodes.size(); ++i) emit(m_nodes[i], out);
        for (BronzePiece* hallway : m_connectionOrder) emit(hallway, out);
        emit(bossRoom, out);
    }

    void emit(BronzePiece* piece, StructureStartData& out) {
        StructurePieceData data;
        data.boundingBox = piece->box;
        data.genDepth = 0;
        if (piece->kind == BronzePiece::Kind::RUINS) {
            data.pieceType = kBronzeRuinsType;
            data.rotation = "NONE";   // setOrientation(SOUTH)
            out.pieces.push_back(std::move(data));
            out.behaviors.push_back(std::make_shared<BronzeSurfaceRuinsBehavior>());
            return;
        }
        AetherTemplateBehavior::Config config;
        config.templateId = "aether:bronze_dungeon/" + piece->name;
        config.templatePosition = piece->templatePosition;
        config.rotation = piece->rotation;
        config.pivot = piece->pivot;
        config.processors = piece->processors;
        config.pieceBox = piece->box;
        data.pieceType = kBronzeRoomType;
        config.markers = AetherTemplateBehavior::Markers::BRONZE_ROOM;
        if (piece->kind == BronzePiece::Kind::BOSS) {
            data.pieceType = kBronzeBossType;
            config.markers = AetherTemplateBehavior::Markers::REWARD;
            config.rewardLoot = kBronzeRewardLoot;
            // Our boss_room template (tools/gen_aether_structures.py): the
            // Slider's spot is the dais centre.
            config.bossId = "aether:slider";
            config.bossLocal = core::BlockPos(8, 3, 8);
        } else if (piece->kind == BronzePiece::Kind::TUNNEL) {
            data.pieceType = kBronzeTunnelType;
            config.markers = AetherTemplateBehavior::Markers::NONE;
        }
        data.rotation = rotationName(piece->rotation);
        data.detail = config.templateId;
        out.pieces.push_back(std::move(data));
        out.behaviors.push_back(std::make_shared<AetherTemplateBehavior>(std::move(config)));
    }

    // chunkPos.getChessboardDistance(new ChunkPos(pos)) <= 3, against the
    // ORIGINAL context chunk (not the searched neighbour).
    bool isCloseToCenter(const core::BlockPos& pos) const {
        const int dx = std::abs(blockToChunk(pos.getX()) - m_ctx.chunkX);
        const int dz = std::abs(blockToChunk(pos.getZ()) - m_ctx.chunkZ);
        return std::max(dx, dz) <= 3;
    }

    const std::vector<BlockState*>& column(int x, int z) {
        const int64_t key = (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z);
        auto it = m_columns.find(key);
        if (it != m_columns.end()) return it->second;
        std::vector<BlockState*> col;
        m_ctx.generator->getBaseColumn(x, z, m_ctx.randomState, col);
        return m_columns.emplace(key, std::move(col)).first->second;
    }

    // NoiseColumn.getBlock(y): air outside the column.
    BlockState* columnBlock(const std::vector<BlockState*>& col, int y) const {
        const int i = y - m_ctx.generator->getBaseColumnMinY();
        if (i < 0 || i >= static_cast<int>(col.size())) return nullptr;
        return col[static_cast<size_t>(i)];
    }

    bool checkForAirAtPos(int x, int y, int z) {
        BlockState* state = columnBlock(column(x, z), y);
        return state == nullptr || state->isAir();
    }

    // Air, or #aether:non_bronze_dungeon_spawnable (water).
    static bool unsuitable(BlockState* state) {
        return state == nullptr || state->isAir() || state->getIdentifier() == "minecraft:water";
    }

    bool isCoveredAtPos(const BoundingBox& room) {
        const int minX = room.minX - 1;
        const int minZ = room.minZ - 1;
        const int maxX = room.maxX + 1;
        const int maxZ = room.maxZ + 1;
        const std::pair<int, int> corners[4] = {{minX, minZ}, {minX, maxZ}, {maxX, minZ}, {maxX, maxZ}};
        for (const auto& corner : corners) {
            const std::vector<BlockState*>& col = column(corner.first, corner.second);
            for (int y = room.minY - 1; y <= room.maxY + 1; ++y) {
                if (unsuitable(columnBlock(col, y))) return false;
            }
        }
        return true;
    }

    bool getBossRoomRotation(const core::BlockPos& minPos, const core::BlockPos& maxPos, int& outRotation) {
        const TemplateSize chest = templateSize("aether:bronze_dungeon/chest_room");
        const BoundingBox bossBox(minPos.getX(), minPos.getY(), minPos.getZ(),
                                  maxPos.getX(), maxPos.getY(), maxPos.getZ());
        // Rotation.getShuffled(random) = Util.shuffledCopy(values(), random).
        int order[4] = {0, 1, 2, 3};
        for (int i = 4; i > 1; --i) {
            const int swapTo = m_random.nextInt(i);
            std::swap(order[i - 1], order[swapTo]);
        }
        for (int rotation : order) {
            const Dir direction = rotateSouth(rotation);
            core::BlockPos neighbor = tunnelFromEvenSquareRoom(bossBox, direction, m_nodeWidth);
            neighbor = neighbor.offset(stepX(direction) * (m_edgeLength + bossBox.getXSpan()), 0,
                                       stepZ(direction) * (m_edgeLength + bossBox.getZSpan()));
            if (isCoveredAtPos(templateBoundingBox(chest.x, chest.y, chest.z, neighbor, rotation,
                                                   core::BlockPos(0, 0, 0)))) {
                outRotation = rotation;
                return true;
            }
        }
        return false;
    }

    GenerationContext& m_ctx;
    LegacyRandomSource& m_random;
    BronzeSettings m_settings;
    int m_nodeWidth = 0;
    int m_edgeWidth = 0;
    int m_edgeLength = 0;
    int m_maxSize = 3;

    std::vector<std::unique_ptr<BronzePiece>> m_arena;
    std::vector<BronzePiece*> m_nodes;
    std::map<BronzePiece*, std::map<Dir, Connection>> m_edges;
    std::vector<BronzePiece*> m_connectionOrder;   // hallways, creation order
    std::unordered_map<int64_t, std::vector<BlockState*>> m_columns;
};

// BronzeDungeonStructure.checkEachCornerAtY / findStartingHeight.
int findStartingHeight(GenerationContext& ctx, int chunkX, int chunkZ, int aboveBottom, int belowTop,
                       int roomHeight) {
    const int minX = chunkX * 16 - 1;
    const int minZ = chunkZ * 16 - 1;
    const int maxX = chunkX * 16 + 15 + 1;
    const int maxZ = chunkZ * 16 + 15 + 1;
    std::vector<BlockState*> columns[4];
    ctx.generator->getBaseColumn(minX, minZ, ctx.randomState, columns[0]);
    ctx.generator->getBaseColumn(minX, maxZ, ctx.randomState, columns[1]);
    ctx.generator->getBaseColumn(maxX, minZ, ctx.randomState, columns[2]);
    ctx.generator->getBaseColumn(maxX, maxZ, ctx.randomState, columns[3]);
    const int columnMinY = ctx.generator->getBaseColumnMinY();
    auto blockAt = [&](const std::vector<BlockState*>& col, int y) -> BlockState* {
        const int i = y - columnMinY;
        if (i < 0 || i >= static_cast<int>(col.size())) return nullptr;
        return col[static_cast<size_t>(i)];
    };
    auto cornersSolidAt = [&](int y) {
        for (const auto& col : columns) {
            BlockState* state = blockAt(col, y);
            if (state == nullptr || state->isAir() || state->getIdentifier() == "minecraft:water") return false;
        }
        return true;
    };
    const int minBuild = ctx.generator->getLevelMinY();
    const int maxBuild = minBuild + ctx.generator->getLevelHeight();
    int height = minBuild;
    const int maxHeight = maxBuild - belowTop;
    int thickness = roomHeight + 2;
    int currentThickness = 0;
    for (int y = height + aboveBottom; y <= maxHeight; ++y) {
        if (cornersSolidAt(y)) {
            ++currentThickness;
        } else {
            if (currentThickness > thickness) {
                thickness = currentThickness;
                height = y;
            }
            currentThickness = 0;
        }
    }
    const int offset = (thickness + roomHeight) / 2;
    height -= offset;
    return height;
}

BronzeSettings bronzeSettings(const StructureInfo& info) {
    const json& j = structureJson(info);
    BronzeSettings s;
    s.maxRooms = j.value("maxrooms", s.maxRooms);
    s.aboveBottom = j.value("aboveBottom", s.aboveBottom);
    s.belowTop = j.value("belowTop", s.belowTop);
    if (j.contains("processor_settings")) {
        const json& p = j["processor_settings"];
        s.roomProcessors = p.value("generic_room_processors", s.roomProcessors);
        s.tunnelProcessors = p.value("tunnel_processors", s.tunnelProcessors);
        s.bossProcessors = p.value("boss_room_processors", s.bossProcessors);
    }
    return s;
}

bool bronzeTemplatesPresent() {
    static const char* const kTemplates[] = {
        "aether:bronze_dungeon/boss_room", "aether:bronze_dungeon/chest_room",
        "aether:bronze_dungeon/end_corridor", "aether:bronze_dungeon/entrance",
        "aether:bronze_dungeon/lobby", "aether:bronze_dungeon/square_tunnel"};
    for (const char* id : kTemplates) {
        if (TemplateEngine::get(id).empty()) {
            logOnce(std::string("missing:") + id, std::string("template ") + id
                    + " missing (data/aether/structure) - bronze dungeons skipped");
            return false;
        }
    }
    return true;
}

// BronzeDungeonStructure.findGenerationPoint + generatePieces.
bool generateBronzeDungeon(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                           const std::function<bool(int, int, int)>& validBiomeAt) {
    if (!bronzeTemplatesPresent()) return false;
    const BronzeSettings settings = bronzeSettings(info);
    const int roomHeight = templateSize("aether:bronze_dungeon/boss_room").y;
    const int minBuild = ctx.generator->getLevelMinY();

    int chunkX = ctx.chunkX;
    int chunkZ = ctx.chunkZ;
    int height = findStartingHeight(ctx, chunkX, chunkZ, settings.aboveBottom, settings.belowTop, roomHeight);
    // To make structure placement more reliable, check the surrounding 8
    // chunks for suitable locations.
    if (height <= minBuild) {
        bool found = false;
        for (int x = -1; x <= 1 && !found; ++x) {
            for (int z = -1; z <= 1; ++z) {
                if (x == 0 && z == 0) continue;
                const int y = findStartingHeight(ctx, ctx.chunkX + x, ctx.chunkZ + z,
                                                 settings.aboveBottom, settings.belowTop, roomHeight);
                if (y > minBuild) {
                    height = y;
                    chunkX = ctx.chunkX + x;
                    chunkZ = ctx.chunkZ + z;
                    found = true;
                    break;
                }
            }
        }
        if (height <= minBuild) return false;
    }
    const core::BlockPos blockPos(chunkX * 16, height, chunkZ * 16);
    // GenerationStub with a lazy piece consumer: the biome check at the stub
    // runs before any piece is built.
    if (!validBiomeAt(blockPos.getX(), blockPos.getY(), blockPos.getZ())) return false;

    BronzeDungeonBuilder builder(ctx, settings);
    builder.initializeDungeon(blockPos, out);
    return !out.pieces.empty();
}

// ---------------------------------------------------------------------------
// Silver dungeon — SilverDungeonStructure.java + silverdungeon/*.java
// ---------------------------------------------------------------------------

// Rotation.rotate(direction) for a horizontal direction: clockwise quarter
// turns over N -> E -> S -> W.
Dir rotateDir(Dir direction, int rotation) {
    static const Dir kClockwise[4] = {Dir::NORTH, Dir::EAST, Dir::SOUTH, Dir::WEST};
    int index = 0;
    for (int i = 0; i < 4; ++i) {
        if (kClockwise[i] == direction) index = i;
    }
    return kClockwise[(index + (rotation & 3)) & 3];
}

core::BlockPos relative(const core::BlockPos& pos, Dir direction, int distance) {
    return pos.offset(stepX(direction) * distance, 0, stepZ(direction) * distance);
}

struct SilverSettings {
    int maxY = 128;
    int belowTerrain = 2;
    int aboveTerrain = 18;
    int minY = 35;
    int rangeY = 70;
    std::string roomProcessors = "aether:silver_room";
    std::string floorProcessors = "aether:silver_floor";
    std::string bossProcessors = "aether:silver_boss_room";
};

SilverSettings silverSettings(const StructureInfo& info) {
    const json& j = structureJson(info);
    SilverSettings s;
    s.maxY = j.value("maxY", s.maxY);
    s.belowTerrain = j.value("belowTerrain", s.belowTerrain);
    s.aboveTerrain = j.value("aboveTerrain", s.aboveTerrain);
    s.minY = j.value("minY", s.minY);
    s.rangeY = j.value("rangeY", s.rangeY);
    if (j.contains("processor_settings")) {
        const json& p = j["processor_settings"];
        s.roomProcessors = p.value("generic_room_processors", s.roomProcessors);
        s.floorProcessors = p.value("floor_processors", s.floorProcessors);
        s.bossProcessors = p.value("boss_room_processors", s.bossProcessors);
    }
    return s;
}

bool silverTemplatesPresent() {
    static const char* const kTemplates[] = {
        "aether:silver_dungeon/boss_door", "aether:silver_dungeon/boss_room",
        "aether:silver_dungeon/chest_room", "aether:silver_dungeon/door",
        "aether:silver_dungeon/floor", "aether:silver_dungeon/rear",
        "aether:silver_dungeon/skeleton", "aether:silver_dungeon/staircase",
        "aether:silver_dungeon/tall_staircase", "aether:silver_dungeon/wall"};
    for (const char* id : kTemplates) {
        if (TemplateEngine::get(id).empty()) {
            logOnce(std::string("missing:") + id, std::string("template ") + id
                    + " missing (data/aether/structure) - silver dungeons skipped");
            return false;
        }
    }
    return true;
}

class SilverPieces {
public:
    enum class Kind { TEMPLE, FLOOR, ROOM, BOSS };

    SilverPieces(const SilverSettings& settings, StructureStartData& out) : m_settings(settings), m_out(out) {}

    // Adds one template piece; returns its bounding box.
    BoundingBox add(Kind kind, const std::string& name, const core::BlockPos& pos, int rotation) {
        const std::string id = "aether:silver_dungeon/" + name;
        const TemplateSize size = templateSize(id);
        AetherTemplateBehavior::Config config;
        config.templateId = id;
        config.templatePosition = pos;
        config.rotation = rotation & 3;
        config.sizeX = size.x;
        config.sizeY = size.y;
        config.sizeZ = size.z;
        StructurePieceData data;
        switch (kind) {
            case Kind::TEMPLE:
                data.pieceType = "aether:silver_temple_piece";
                config.processors = m_settings.roomProcessors;
                break;
            case Kind::FLOOR:
                data.pieceType = "aether:silver_floor_piece";
                config.processors = m_settings.floorProcessors;
                break;
            case Kind::ROOM:
                // SilverDungeonRoom.makeSettings: pivot (x/2 - 4, 0, z/2 - 4).
                data.pieceType = "aether:silver_dungeon_room";
                config.processors = m_settings.roomProcessors;
                config.pivot = core::BlockPos(size.x / 2 - 4, 0, size.z / 2 - 4);
                config.markers = AetherTemplateBehavior::Markers::SILVER_ROOM;
                break;
            case Kind::BOSS:
                data.pieceType = "aether:silver_boss_room";
                config.processors = m_settings.bossProcessors;
                config.markers = AetherTemplateBehavior::Markers::REWARD;
                config.rewardLoot = kSilverRewardLoot;
                config.silverBorderLights = true;
                config.bossId = "aether:valkyrie_queen";    // hall centre of our template
                config.bossLocal = core::BlockPos(11, 2, 12);
                break;
        }
        config.pieceBox = templateBoundingBox(size.x, size.y, size.z, pos, config.rotation, config.pivot);
        data.boundingBox = config.pieceBox;
        data.rotation = rotationName(config.rotation);
        data.detail = id;
        const BoundingBox box = config.pieceBox;
        m_out.pieces.push_back(std::move(data));
        m_out.behaviors.push_back(std::make_shared<AetherTemplateBehavior>(std::move(config)));
        return box;
    }

private:
    const SilverSettings& m_settings;
    StructureStartData& m_out;
};

// SilverDungeonStructure.buildCloudBed: 100 small cloud walks under the
// dungeon, one LargeAercloudChunk piece per chunk they touch.
void buildSilverCloudBed(LegacyRandomSource& random, const core::BlockPos& origin, Dir direction,
                         StructureStartData& out) {
    int xBounds = 77;
    int zBounds = 50;
    int ox = origin.getX();
    int oy = origin.getY() - 1;
    int oz = origin.getZ();
    switch (direction) {
        case Dir::SOUTH: xBounds = 50; zBounds = 77; ox += -10; oz += -11; break;
        case Dir::NORTH: xBounds = 50; zBounds = 77; ox += -40; oz += -66; break;
        case Dir::EAST:  xBounds = 77; zBounds = 50; ox += -11; oz += -40; break;
        case Dir::WEST:  xBounds = 77; zBounds = 50; ox += -66; oz += -10; break;
    }

    std::vector<core::BlockPos> positions;
    std::unordered_map<PosKey, bool, PosKeyHash> seen;
    std::vector<std::pair<int, int>> chunkOrder;
    std::set<std::pair<int, int>> chunkSeen;
    for (int tries = 0; tries < 100; ++tries) {
        int x = ox + random.nextInt(xBounds);
        int y = oy;
        int z = oz + random.nextInt(zBounds);
        const int xTendency = random.nextInt(3) - 1;
        const int zTendency = random.nextInt(3) - 1;
        for (int n = 0; n < 10; ++n) {
            x += random.nextInt(3) - 1 + xTendency;
            if (random.nextBoolean()) {
                y += random.nextInt(3) - 1;
            }
            z += random.nextInt(3) - 1 + zTendency;
            for (int x1 = x; x1 < x + random.nextInt(4) + 3; ++x1) {
                for (int y1 = y; y1 < y + random.nextInt(1) + 2; ++y1) {
                    for (int z1 = z; z1 < z + random.nextInt(4) + 3; ++z1) {
                        if (std::abs(x1 - x) + std::abs(y1 - y) + std::abs(z1 - z) < 4 + random.nextInt(2)) {
                            const PosKey key{x1, y1, z1};
                            if (!seen.count(key)) {
                                seen.emplace(key, true);
                                positions.emplace_back(x1, y1, z1);
                            }
                            const std::pair<int, int> chunk{blockToChunk(x1), blockToChunk(z1)};
                            if (chunkSeen.insert(chunk).second) chunkOrder.push_back(chunk);
                        }
                    }
                }
            }
        }
    }

    BlockState* cloud = aether_blocks::defaultState("aether:cold_aercloud");
    if (cloud == nullptr) return;
    std::map<std::pair<int, int>, std::vector<core::BlockPos>> byChunk;
    for (const core::BlockPos& pos : positions) {
        byChunk[{blockToChunk(pos.getX()), blockToChunk(pos.getZ())}].push_back(pos);
    }
    // Java iterates a HashMap<ChunkPos, ...>; the port keeps first-touch
    // order (piece order only, no draws depend on it).
    for (const auto& chunk : chunkOrder) {
        StructurePieceData piece;
        piece.pieceType = "aether:large_aercloud";
        piece.boundingBox = BoundingBox(chunk.first * 16, origin.getY(), chunk.second * 16,
                                        chunk.first * 16 + 15, origin.getY(), chunk.second * 16 + 15);
        out.pieces.push_back(std::move(piece));
        out.behaviors.push_back(std::make_shared<LargeAercloudChunkBehavior>(byChunk[chunk], cloud));
    }
}

// SilverDungeonBuilder — the 3x3x3 room grid.
class SilverDungeonBuilder {
public:
    static constexpr int CHEST_ROOM = 0b1;
    static constexpr int STAIRS = 0b10;
    static constexpr int FINAL_STAIRS = 0b100;
    static constexpr int STAIRS_MIDDLE = 0b1000;
    static constexpr int STAIRS_TOP = 0b10000;
    static constexpr int NORTH_DOOR = 0b100000;
    static constexpr int WEST_DOOR = 0b1000000;
    static constexpr int VISITED = 0b10000000;
    static constexpr int W = 3, H = 3, L = 3;

    explicit SilverDungeonBuilder(LegacyRandomSource& random) : m_random(random) {
        for (auto& plane : m_grid) for (auto& row : plane) for (int& cell : row) cell = 0;
        populateGrid();
    }

    void assembleDungeon(SilverPieces& pieces, core::BlockPos startPos, int rotation, Dir direction) {
        const int dx = stepX(direction);
        const int dz = stepZ(direction);
        startPos = startPos.offset(dz * 5 - dx, 5, -dx * 5 - dz);
        const int sideways = rotated(rotation, 1);
        for (int y = H - 1; y >= 0; --y) {
            const int oy = startPos.getY() + y * 5;
            for (int z = 0; z < L; ++z) {
                for (int x = 0; x < W; ++x) {
                    const int ox = startPos.getX() + (dz * x * 7) + (dx * z * 7);
                    const int oz = startPos.getZ() + (dz * z * 7) - (dx * x * 7);
                    const core::BlockPos offset(ox, oy, oz);
                    const int room = m_grid[x][y][z];
                    pieces.add(SilverPieces::Kind::FLOOR, "floor", offset.offset(dx + dz, -1, dz - dx), rotation);
                    pieces.add(SilverPieces::Kind::TEMPLE, (room & NORTH_DOOR) == NORTH_DOOR ? "door" : "wall",
                               offset.offset(dz, 0, -dx), rotation);
                    pieces.add(SilverPieces::Kind::TEMPLE, (room & WEST_DOOR) == WEST_DOOR ? "door" : "wall",
                               relative(offset, direction, 1), sideways);
                    if ((room & FINAL_STAIRS) == FINAL_STAIRS) {
                        pieces.add(SilverPieces::Kind::ROOM, "tall_staircase", offset.offset(2, 0, 2), rotation);
                        pieces.add(SilverPieces::Kind::TEMPLE, "boss_door", offset.offset(dz * 3, 0, -dx * 3), rotation);
                    } else if ((room & STAIRS) == STAIRS) {
                        pieces.add(SilverPieces::Kind::ROOM, "staircase", offset.offset(2, 0, 2), rotation);
                    } else if ((room & CHEST_ROOM) == CHEST_ROOM) {
                        pieces.add(SilverPieces::Kind::ROOM, "chest_room", offset.offset(3, 0, 3), rotation);
                    }
                }
            }
        }
    }

private:
    void populateGrid() {
        const int finalStairsX = m_random.nextInt(W);
        m_grid[finalStairsX][0][0] = FINAL_STAIRS;
        m_grid[finalStairsX][1][0] = STAIRS_MIDDLE;
        m_grid[finalStairsX][2][0] = STAIRS_TOP;

        const int firstStairsX = m_random.nextInt(W);
        m_grid[firstStairsX][0][1] = STAIRS;
        m_grid[firstStairsX][1][1] = STAIRS_TOP;

        const int secondStairsX = m_random.nextInt(W);
        m_grid[secondStairsX][1][2] = STAIRS;
        m_grid[secondStairsX][2][2] = STAIRS_TOP;

        for (int y = 0; y < H; ++y) {
            traverseRooms(1, y, 1, 0);
            for (int z = 0; z < L; ++z) {
                for (int x = 0; x < W; ++x) {
                    // Java: (room & 0b11111) == 0 && random.nextInt(3) != 0
                    if ((m_grid[x][y][z] & 0b11111) == 0 && m_random.nextInt(3) != 0) {
                        m_grid[x][y][z] |= CHEST_ROOM;
                    }
                }
            }
        }
    }

    bool traverseRooms(int x, int y, int z, int typesToAvoid) {
        if (x < 0 || x >= W || z < 0 || z >= L) return false;
        const int room = m_grid[x][y][z];
        if ((room & typesToAvoid) > 0) return false;   // stairs never side by side
        if ((room & VISITED) == VISITED) return m_random.nextInt(3) == 0;
        m_grid[x][y][z] |= VISITED;

        const int blacklist = setNeighborBlacklist(room);
        std::vector<Dir> directions = {Dir::NORTH, Dir::WEST, Dir::SOUTH, Dir::EAST};
        for (int i = static_cast<int>(directions.size()); i > 0; --i) {
            const int index = m_random.nextInt(i);
            const Dir dir = directions[static_cast<size_t>(index)];
            directions.erase(directions.begin() + index);
            switch (dir) {
                case Dir::NORTH:
                    if (traverseRooms(x, y, z - 1, blacklist)) m_grid[x][y][z] |= NORTH_DOOR;
                    break;
                case Dir::SOUTH:
                    if (traverseRooms(x, y, z + 1, blacklist)) m_grid[x][y][z + 1] |= NORTH_DOOR;
                    break;
                case Dir::WEST:
                    if (traverseRooms(x - 1, y, z, blacklist)) m_grid[x][y][z] |= WEST_DOOR;
                    break;
                case Dir::EAST:
                    if (traverseRooms(x + 1, y, z, blacklist)) m_grid[x + 1][y][z] |= WEST_DOOR;
                    break;
            }
        }
        return true;
    }

    static int setNeighborBlacklist(int roomType) {
        int blacklist = FINAL_STAIRS | STAIRS_MIDDLE;
        if ((roomType & STAIRS_TOP) == STAIRS_TOP) blacklist |= STAIRS;
        if ((roomType & STAIRS) == STAIRS) blacklist |= STAIRS_TOP;
        return blacklist;
    }

    LegacyRandomSource& m_random;
    int m_grid[W][H][L];
};

// SilverDungeonStructure.findGenerationPoint + generatePieces.
bool generateSilverDungeon(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                           const std::function<bool(int, int, int)>& validBiomeAt) {
    if (!silverTemplatesPresent()) return false;
    const SilverSettings settings = silverSettings(info);
    LegacyRandomSource& random = ctx.random;

    const int x = ctx.chunkX * 16 + 8;
    const int z = ctx.chunkZ * 16 + 8;
    const int maxHeight = settings.maxY;
    const int minHeight = ctx.generator->getBaseHeight(x, z, Heightmap::Types::WORLD_SURFACE_WG, ctx.randomState)
                        - settings.belowTerrain;
    int height;
    if (random.nextInt(5) < 3) {
        height = minHeight + settings.aboveTerrain;
        if (height < maxHeight) {
            height += random.nextInt(maxHeight - height);
        }
    } else {
        height = std::max(minHeight, settings.minY + random.nextInt(settings.rangeY));
    }
    const core::BlockPos input(x, height, z);
    // Lazy piece consumer: the biome check at the stub comes first.
    if (!validBiomeAt(input.getX(), input.getY(), input.getZ())) return false;

    const int rotation = random.nextInt(4);   // Rotation.getRandom
    const Dir direction = rotateDir(Dir::SOUTH, rotation);
    const core::BlockPos elevatedPos =
        relative(relative(input, rotateDir(Dir::NORTH, rotation), 54), rotateDir(Dir::WEST, rotation), 15);

    buildSilverCloudBed(random, elevatedPos, direction, out);

    SilverPieces pieces(settings, out);
    const BoundingBox rear = pieces.add(SilverPieces::Kind::TEMPLE, "rear", elevatedPos, rotation);

    const int dx = stepX(direction);
    const int dz = stepZ(direction);
    const core::BlockPos bossRoomPos = elevatedPos.offset((dx + dz) * 5, 3, (dz - dx) * 5);
    pieces.add(SilverPieces::Kind::BOSS, "boss_room", bossRoomPos, rotation);

    const core::BlockPos offsetPos = elevatedPos.offset(dx * rear.getXSpan(), 0, dz * rear.getZSpan());
    pieces.add(SilverPieces::Kind::TEMPLE, "skeleton", offsetPos, rotation);

    SilverDungeonBuilder grid(random);
    grid.assembleDungeon(pieces, offsetPos, rotation, direction);
    // SilverDungeonStructure.afterPlace hands the Valkyrie Queen the dungeon
    // bounds; the queen is not spawned yet (markPendingBoss in the boss room).
    return !out.pieces.empty();
}

// ---------------------------------------------------------------------------
// Gold dungeon — GoldDungeonStructure.java + golddungeon/*.java
// ---------------------------------------------------------------------------

const char* const kGoldRewardLoot = "aether:chests/dungeon/gold/gold_dungeon_reward";

// 1.21.1 Mth.sin / Mth.cos (float argument, float lookup table).
float mthSinF(float value) {
    const int32_t index = static_cast<int32_t>(value * 10430.378f) & 65535;
    return static_cast<float>(std::sin(static_cast<double>(index) * 3.141592653589793 * 2.0 / 65536.0));
}
float mthCosF(float value) {
    const int32_t index = static_cast<int32_t>(value * 10430.378f + 16384.0f) & 65535;
    return static_cast<float>(std::sin(static_cast<double>(index) * 3.141592653589793 * 2.0 / 65536.0));
}
// Mth.PI / Mth.TWO_PI (floats).
constexpr float kMthPi = 3.1415927f;
constexpr float kMthTwoPi = 6.2831855f;

int mthFloor(double v) {
    const int i = static_cast<int>(v);
    return v < static_cast<double>(i) ? i - 1 : i;
}

// #aether:aether_dirt / #aether:holystone (engine slugs).
bool isAetherDirtTag(BlockState* state) {
    if (state == nullptr) return false;
    const std::string& id = state->getIdentifier();
    return id == "minecraft:aether_grass_block" || id == "minecraft:enchanted_aether_grass_block"
        || id == "minecraft:aether_dirt";
}
bool isHolystoneTag(BlockState* state) {
    if (state == nullptr) return false;
    const std::string& id = state->getIdentifier();
    return id == "minecraft:holystone" || id == "minecraft:mossy_holystone";
}

// GoldStubCave — a short tunnel of spheres carved through aether dirt and
// holystone, from the piece's (single-block) box centre.
class GoldStubCaveBehavior final : public StructurePieceBehavior {
public:
    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator; (void)chunkBB; (void)chunkPos; (void)referencePos;
        const BoundingBox& box = self.boundingBox;
        const float f = random.nextFloat() * kMthPi;
        const int i = box.centerX();
        const int j = box.centerY();
        const int k = box.centerZ();
        const double offset = 30.0 / 8.0;
        const double sinF = static_cast<double>(mthSinF(f));
        const double cosF = static_cast<double>(mthCosF(f));
        const double lowerX = i + sinF * offset;
        const double upperX = i - sinF * offset;
        const double lowerZ = k + cosF * offset;
        const double upperZ = k - cosF * offset;
        const double lowerY = j + random.nextInt(3) + 2;
        const double upperY = j + random.nextInt(3) + 2;
        BlockState* air = Blocks::AIR->defaultBlockState();

        for (int length = 0; length <= 30; ++length) {
            const double radius = length / 30.0;
            const double x = lowerX + (upperX - lowerX) * radius;
            const double y = lowerY + (upperY - lowerY) * radius;
            const double z = lowerZ + (upperZ - lowerZ) * radius;
            const double magnitude = random.nextDouble() * 30 / 16.0;
            double width = (std::sin(radius * static_cast<double>(kMthPi)) + 1.0) * magnitude + 1.0;
            width /= 2;
            const int minX = mthFloor(x - width);
            const int minY = mthFloor(y - width);
            const int minZ = mthFloor(z - width);
            const int maxX = mthFloor(x + width);
            const int maxY = mthFloor(y + width);
            const int maxZ = mthFloor(z + width);
            for (int xo = minX; xo <= maxX; ++xo) {
                const double dx = (xo + 0.5 - x) / width;
                const double xDistance = dx * dx;
                if (xDistance >= 1.0) continue;
                for (int yo = minY; yo <= maxY; ++yo) {
                    const double dy = (yo + 0.5 - y) / width;
                    const double yDistance = dy * dy;
                    if (xDistance + yDistance >= 1.0) continue;
                    for (int zo = minZ; zo <= maxZ; ++zo) {
                        const double dz = (zo + 0.5 - z) / width;
                        const double zDistance = dz * dz;
                        if (xDistance + yDistance + zDistance < 1.0) {
                            const core::BlockPos pos(xo, yo, zo);
                            BlockState* state = level->getBlockState(pos);
                            if (isAetherDirtTag(state) || isHolystoneTag(state)) {
                                level->setBlock(pos, air, 2);
                            }
                        }
                    }
                }
            }
        }
    }
};

struct GoldSettings {
    int stubCount = 8;
    int belowTerrain = 20;
    int minY = 40;
    int rangeY = 60;
    int stubFoliageRarity = 64;
    std::string islandProcessors = "aether:gold_island";
    std::string tunnelProcessors = "aether:gold_tunnel";
    std::string bossProcessors = "aether:gold_boss_room";
};

GoldSettings goldSettings(const StructureInfo& info) {
    const json& j = structureJson(info);
    GoldSettings s;
    s.stubCount = j.value("stubcount", s.stubCount);
    s.belowTerrain = j.value("belowTerrain", s.belowTerrain);
    s.minY = j.value("minY", s.minY);
    s.rangeY = j.value("rangeY", s.rangeY);
    if (j.contains("stubFoliage") && j["stubFoliage"].contains("placement")) {
        for (const json& modifier : j["stubFoliage"]["placement"]) {
            if (modifier.value("type", std::string()) == "minecraft:rarity_filter") {
                s.stubFoliageRarity = modifier.value("chance", s.stubFoliageRarity);
            }
        }
    }
    if (j.contains("processor_settings")) {
        const json& p = j["processor_settings"];
        s.islandProcessors = p.value("island_processors", s.islandProcessors);
        s.tunnelProcessors = p.value("tunnel_processors", s.tunnelProcessors);
        s.bossProcessors = p.value("boss_room_processors", s.bossProcessors);
    }
    return s;
}

bool goldTemplatesPresent() {
    static const char* const kTemplates[] = {
        "aether:gold_dungeon/boss_room", "aether:gold_dungeon/island",
        "aether:gold_dungeon/stub", "aether:gold_dungeon/tunnel"};
    for (const char* id : kTemplates) {
        if (TemplateEngine::get(id).empty()) {
            logOnce(std::string("missing:") + id, std::string("template ") + id
                    + " missing (data/aether/structure) - gold dungeons skipped");
            return false;
        }
    }
    return true;
}

// The two foliage placements GoldDungeonStructure.afterPlace scatters:
//   islandFoliage  aether:gold_dungeon_island_foliage — only in air, rarity 16,
//                  random_selector(golden oak tree behind would_survive
//                  (golden_oak_sapling) at 0.66, default a dandelion/poppy)
//   stubFoliage    golden_oak_tree, rarity 64
struct GoldFoliage {
    placement::PlacedFeature* island = nullptr;
    placement::PlacedFeature* stub = nullptr;
    std::vector<std::unique_ptr<placement::PlacementModifier>> modifiers;
    std::vector<std::unique_ptr<placement::PlacedFeature>> placed;
    std::vector<std::unique_ptr<ConfiguredFeature>> configured;
    std::vector<std::shared_ptr<blockpredicates::BlockPredicate>> predicates;
    std::unique_ptr<RandomFeatureConfiguration> selectorConfig;
    std::shared_ptr<WeightedStateProvider> flowers;
};

const GoldFoliage& goldFoliage(int stubRarity) {
    static GoldFoliage foliage;
    static std::once_flag once;
    std::call_once(once, [stubRarity] {
        using ::minecraft::data::worldgen::features::AetherFeatures;
        if (!AetherFeatures::isInitialized()) AetherFeatures::bootstrap();
        ConfiguredFeature* goldenOak = AetherFeatures::GOLDEN_OAK_TREE;
        if (goldenOak == nullptr) {
            logOnce("gold-foliage", "GOLDEN_OAK_TREE unavailable - gold dungeon islands get no trees");
            return;
        }
        auto keep = [&](std::unique_ptr<placement::PlacementModifier> m) {
            placement::PlacementModifier* raw = m.get();
            foliage.modifiers.push_back(std::move(m));
            return raw;
        };
        auto place = [&](ConfiguredFeature* feature, std::vector<placement::PlacementModifier*> mods,
                         const char* name) {
            auto p = std::make_unique<placement::PlacedFeature>(feature, mods, name);
            placement::PlacedFeature* raw = p.get();
            foliage.placed.push_back(std::move(p));
            return raw;
        };
        placement::PlacementModifier* inAir = keep(std::make_unique<placement::BlockPredicateFilter>(
            placement::BlockPredicateFilter::forPredicate(blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE)));

        std::vector<placement::PlacementModifier*> oakFilter;
        if (BlockState* sapling = aether_blocks::defaultState("aether:golden_oak_sapling")) {
            auto predicate = blockpredicates::BlockPredicate::wouldSurvive(sapling, core::Vec3i::ZERO());
            foliage.predicates.push_back(predicate);
            oakFilter.push_back(keep(std::make_unique<placement::BlockPredicateFilter>(
                placement::BlockPredicateFilter::forPredicate(predicate))));
        }
        placement::PlacedFeature* oak = place(goldenOak, oakFilter, "aether:gold_dungeon_golden_oak_inline");

        // single_gold_dungeon_flower: simple_block, dandelion 1 : poppy 1.
        BlockState* dandelion = Blocks::getDefaultState("minecraft:dandelion");
        BlockState* poppy = Blocks::getDefaultState("minecraft:poppy");
        foliage.flowers = std::make_shared<WeightedStateProvider>(std::vector<WeightedStateEntry>{
            WeightedStateEntry(dandelion, 1), WeightedStateEntry(poppy, 1)});
        static SimpleBlockFeature s_simpleBlock;
        foliage.configured.push_back(std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlock, SimpleBlockConfiguration(foliage.flowers.get(), false)));
        placement::PlacedFeature* flower = place(foliage.configured.back().get(), {inAir},
                                                 "aether:single_gold_dungeon_flower_inline");

        std::vector<WeightedPlacedFeature> weighted;
        weighted.emplace_back(oak, 0.66f);
        foliage.selectorConfig = std::make_unique<RandomFeatureConfiguration>(std::move(weighted), flower);
        static RandomSelectorFeature s_selector;
        foliage.configured.push_back(std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
            &s_selector, *foliage.selectorConfig));
        foliage.island = place(foliage.configured.back().get(),
            {inAir, keep(std::make_unique<placement::RarityFilter>(placement::RarityFilter::onAverageOnceEvery(16)))},
            "aether:gold_dungeon_island_foliage");
        foliage.stub = place(goldenOak,
            {keep(std::make_unique<placement::RarityFilter>(placement::RarityFilter::onAverageOnceEvery(stubRarity)))},
            "aether:gold_dungeon_stub_foliage");
    });
    return foliage;
}

// GoldDungeonStructure.placeGoldenOaks + iterateColumn.
void placeGoldenOaks(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& box, const BoundingBox& chunkBox, placement::PlacedFeature* feature) {
    if (feature == nullptr) return;
    const int minX = std::max(chunkBox.minX, box.minX);
    const int minZ = std::max(chunkBox.minZ, box.minZ);
    const int maxX = std::min(chunkBox.maxX, box.maxX);
    const int maxZ = std::min(chunkBox.maxZ, box.maxZ);
    const int minY = box.minY + mthFloor((box.maxY - box.minY) * 0.75);
    const int maxY = box.maxY;
    for (int x = minX; x < maxX; ++x) {
        for (int z = minZ; z < maxZ; ++z) {
            for (int y = maxY; y > minY; --y) {
                if (isAetherDirtTag(level->getBlockState(core::BlockPos(x, y, z)))) {
                    feature->place(level, generator, random, core::BlockPos(x, y + 1, z));
                    break;
                }
            }
        }
    }
}

// BlockLogicUtil.tunnelFromOddSquareRoom(box, direction, width).
core::BlockPos tunnelFromOddSquareRoom(const BoundingBox& box, Dir direction, int width) {
    const int offsetFromCenter = (axisIsX(direction) ? box.getZSpan() : box.getXSpan()) >> 1;
    const int sidedOffset = width >> 1;
    const int sx = stepX(direction);
    const int sz = stepZ(direction);
    const int xOffset = sx * offsetFromCenter - sz * sidedOffset - std::max(0, sx);
    const int zOffset = sz * offsetFromCenter + sx * sidedOffset - std::max(0, sz);
    return core::BlockPos(box.centerX() + xOffset, box.centerY() - (box.getYSpan() >> 1),
                          box.centerZ() + zOffset);
}

// GoldDungeonStructure.findGenerationPoint + generatePieces + afterPlace.
bool generateGoldDungeon(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                         const std::function<bool(int, int, int)>& validBiomeAt) {
    if (!goldTemplatesPresent()) return false;
    const GoldSettings settings = goldSettings(info);
    LegacyRandomSource& random = ctx.random;

    const int x = ctx.chunkX * 16 + 8;
    const int z = ctx.chunkZ * 16 + 8;
    // We want the Gold Dungeon to sometimes blend in with the terrain, and
    // sometimes float in the sky, but never be fully buried.
    const int terrainHeight = ctx.generator->getBaseHeight(x, z, Heightmap::Types::WORLD_SURFACE_WG, ctx.randomState)
                            - settings.belowTerrain;
    const int height = std::max(terrainHeight, settings.minY + random.nextInt(settings.rangeY));
    const core::BlockPos elevatedPos(x, height, z);
    if (!validBiomeAt(elevatedPos.getX(), elevatedPos.getY(), elevatedPos.getZ())) return false;

    struct GoldPiece {
        enum class Kind { ISLAND, STUB, CAVE, TUNNEL, BOSS } kind;
        std::string name;
        core::BlockPos pos{0, 0, 0};
        int rotation = 0;
        core::BlockPos pivot{0, 0, 0};
        BoundingBox box;
        std::string processors;
    };
    std::vector<GoldPiece> pieces;
    auto templatePiece = [&](GoldPiece::Kind kind, const std::string& name, const core::BlockPos& pos,
                             int rotation, const core::BlockPos& pivot, const std::string& processors) {
        const TemplateSize size = templateSize("aether:gold_dungeon/" + name);
        GoldPiece piece{kind, name, pos, rotation & 3, pivot,
                        templateBoundingBox(size.x, size.y, size.z, pos, rotation & 3, pivot), processors};
        pieces.push_back(piece);
        return piece;
    };

    const GoldPiece island = templatePiece(GoldPiece::Kind::ISLAND, "island", elevatedPos, 0,
                                           core::BlockPos(0, 0, 0), settings.islandProcessors);
    const core::BlockPos centerPos(island.box.centerX(), island.box.centerY(), island.box.centerZ());

    // getStubOffset: size / -2 (Java int division).
    const TemplateSize stubSize = templateSize("aether:gold_dungeon/stub");
    const int stubOffX = stubSize.x / -2;
    const int stubOffY = stubSize.y / -2;
    const int stubOffZ = stubSize.z / -2;

    // addIslandStubs.
    const int stubCount = settings.stubCount + random.nextInt(5);
    for (int n = 0; n < stubCount; ++n) {
        const float angle = random.nextFloat() * kMthTwoPi;
        const float distance = ((random.nextFloat() * 0.125f) + 0.7f) * 24.0f;
        const int xOffset = mthFloor(std::cos(static_cast<double>(angle)) * static_cast<double>(distance));
        const int yOffset = -mthFloor(24.0 * static_cast<double>(random.nextFloat()) * 0.3);
        const int zOffset = mthFloor(-std::sin(static_cast<double>(angle)) * static_cast<double>(distance));
        const core::BlockPos stubPos = centerPos.offset(xOffset, yOffset, zOffset)
                                                .offset(stubOffX, stubOffY, stubOffZ);
        templatePiece(GoldPiece::Kind::STUB, "stub", stubPos, 0, core::BlockPos(0, 0, 0), settings.islandProcessors);
    }

    // placeGumdropCaves.
    for (int count = 0; count < 18; ++count) {
        const int a = random.nextInt(24);
        const int b = random.nextInt(24);
        const int cx = centerPos.getX() + a - b;
        const int c = random.nextInt(24);
        const int d = random.nextInt(24);
        const int cy = centerPos.getY() + c - d;
        const int e = random.nextInt(24);
        const int f = random.nextInt(24);
        const int cz = centerPos.getZ() + e - f;
        GoldPiece cave{GoldPiece::Kind::CAVE, "gumdrop_cave", core::BlockPos(cx, cy, cz), 0,
                       core::BlockPos(0, 0, 0), BoundingBox(cx, cy, cz, cx, cy, cz), ""};
        pieces.push_back(cave);
    }

    // The boss room: Rotation.getRandom, offset by getBossRoomOffset.
    const int rotation = random.nextInt(4);
    const Dir bossDirection = rotateSouth(rotation);
    const TemplateSize bossSize = templateSize("aether:gold_dungeon/boss_room");
    const core::BlockPos bossPos = centerPos.offset(bossSize.x / -2 - stepX(bossDirection), bossSize.y / -2,
                                                    bossSize.z / -2 - stepZ(bossDirection));
    // makeSettingsWithPivot: pivot (x >> 1, 0, z >> 1).
    const core::BlockPos bossPivot(bossSize.x >> 1, 0, bossSize.z >> 1);
    const BoundingBox bossBox = templateBoundingBox(bossSize.x, bossSize.y, bossSize.z, bossPos, rotation, bossPivot);

    // tunnelFromBossRoom: the tunnel piece goes in before the boss room.
    const TemplateSize tunnelSize = templateSize("aether:gold_dungeon/tunnel");
    core::BlockPos startPos = tunnelFromOddSquareRoom(bossBox, bossDirection, tunnelSize.x);
    startPos = startPos.offset(stepX(bossDirection) * 3, 1, stepZ(bossDirection) * 3);
    const GoldPiece tunnel = templatePiece(GoldPiece::Kind::TUNNEL, "tunnel", startPos, rotation,
                                           core::BlockPos(0, 0, 0), settings.tunnelProcessors);
    const core::BlockPos endPos = tunnelFromEvenSquareRoom(tunnel.box, bossDirection, tunnelSize.x);
    // getFirstFreeHeight = getBaseHeight.
    const int verticalOffset = ctx.generator->getBaseHeight(endPos.getX(), endPos.getZ(),
                                                            Heightmap::Types::WORLD_SURFACE_WG, ctx.randomState)
                             - startPos.getY();
    templatePiece(GoldPiece::Kind::BOSS, "boss_room", bossPos, rotation, bossPivot, settings.bossProcessors);

    // builder.offsetPiecesVertically: every box (and template position) moves.
    if (verticalOffset > 0) {
        for (GoldPiece& piece : pieces) {
            piece.box.move(0, verticalOffset, 0);
            piece.pos = piece.pos.offset(0, verticalOffset, 0);
        }
    }

    for (const GoldPiece& piece : pieces) {
        StructurePieceData data;
        data.boundingBox = piece.box;
        data.rotation = rotationName(piece.rotation);
        if (piece.kind == GoldPiece::Kind::CAVE) {
            data.pieceType = "aether:gumdrop_cave";
            out.pieces.push_back(std::move(data));
            out.behaviors.push_back(std::make_shared<GoldStubCaveBehavior>());
            continue;
        }
        AetherTemplateBehavior::Config config;
        config.templateId = "aether:gold_dungeon/" + piece.name;
        config.templatePosition = piece.pos;
        config.rotation = piece.rotation;
        config.pivot = piece.pivot;
        config.processors = piece.processors;
        config.pieceBox = piece.box;
        switch (piece.kind) {
            case GoldPiece::Kind::ISLAND: data.pieceType = "aether:gold_island"; break;
            case GoldPiece::Kind::STUB:   data.pieceType = "aether:gold_stub"; break;
            case GoldPiece::Kind::TUNNEL: data.pieceType = "aether:gold_tunnel"; break;
            case GoldPiece::Kind::BOSS:
                data.pieceType = "aether:gold_boss_room";
                config.markers = AetherTemplateBehavior::Markers::REWARD;
                config.rewardLoot = kGoldRewardLoot;
                config.bossId = "aether:sun_spirit";        // arena centre of our template
                config.bossLocal = core::BlockPos(11, 1, 14);
                break;
            case GoldPiece::Kind::CAVE: break;
        }
        data.detail = config.templateId;
        out.pieces.push_back(std::move(data));
        out.behaviors.push_back(std::make_shared<AetherTemplateBehavior>(std::move(config)));
    }

    // afterPlace: golden oaks (and flowers) on the island and stub tops.
    const int stubRarity = settings.stubFoliageRarity;
    out.afterPlace = [stubRarity](WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                                  const BoundingBox& chunkBox, const ::world::ChunkPos& chunkPos,
                                  StructureStartData& start) {
        (void)chunkPos;
        const GoldFoliage& foliage = goldFoliage(stubRarity);
        for (const StructurePieceData& piece : start.pieces) {
            if (piece.pieceType == "aether:gold_island") {
                placeGoldenOaks(level, generator, random, piece.boundingBox, chunkBox, foliage.island);
            } else if (piece.pieceType == "aether:gold_stub") {
                placeGoldenOaks(level, generator, random, piece.boundingBox, chunkBox, foliage.stub);
            }
        }
    };
    return !out.pieces.empty();
}

} // namespace

bool isImplemented(const StructureInfo& info) {
    return info.type == "aether:large_aercloud"
        || info.type == "aether:bronze_dungeon"
        || info.type == "aether:silver_dungeon"
        || info.type == "aether:gold_dungeon";
}

bool generate(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
              const std::function<bool(int x, int y, int z)>& validBiomeAt) {
    // A template or block that cannot load must not escape: an exception out
    // of structure-start generation aborts the worldgen worker. It fails this
    // start instead (logged once per reason).
    try {
        if (info.type == "aether:large_aercloud") {
            return generateLargeAercloud(info, ctx, out, validBiomeAt);
        }
        if (info.type == "aether:bronze_dungeon") {
            return generateBronzeDungeon(info, ctx, out, validBiomeAt);
        }
        if (info.type == "aether:silver_dungeon") {
            return generateSilverDungeon(info, ctx, out, validBiomeAt);
        }
        if (info.type == "aether:gold_dungeon") {
            return generateGoldDungeon(info, ctx, out, validBiomeAt);
        }
    } catch (const std::exception& e) {
        logOnce(info.name + "|" + e.what(), info.name + " start failed: " + e.what());
        out.pieces.clear();
        out.behaviors.clear();
        return false;
    }
    return false;
}

} // namespace AetherStructures
} // namespace structure
} // namespace levelgen
} // namespace minecraft
