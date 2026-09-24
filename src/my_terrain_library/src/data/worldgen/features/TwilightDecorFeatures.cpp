#include "data/worldgen/features/TwilightDecorFeatures.h"
#include "data/worldgen/features/TwilightTemplateFeatures.h"
#include "data/worldgen/features/TwilightFeatures.h"
#include "data/worldgen/features/TwilightFeatureRegistry.h"
#include "data/worldgen/features/TwilightSpikes.h"
#include "TwilightDecorCommon.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/TwilightLandmarks.h"
#include "levelgen/structure/twilight/TwilightPieceBase.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/Heightmap.h"
#include "world/IChunk.h"
#include "world/biome/Biome.h"
#include "world/level/block/Block.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/VineBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — decoration configured features
// (data/twilightforest/worldgen/configured_feature/*.json and the Java
// classes named at each piece, world/components/feature/**).
//
// Blocks resolve once, at bootstrap, through levelgen/TwilightBlocks: the
// mod's own block when the engine registers it, else the logged stand-in.
// Stand-ins relied on while the real blocks are missing (TwilightBlocks.cpp):
//   raspberry/blueberry/blackberry/maloberry_bush  -> sweet_berry_bush
//   iron/gold/copper/essence_oreberry_bush         -> sweet_berry_bush
//   huge_lily_pad, huge_water_lily                 -> lily_pad
//   brown_thorns                                   -> the "thorns" stand-in
//                                                     (dark_oak_log; see below)
//   thorn_leaves -> oak_leaves, thorn_rose -> rose_bush / poppy
//   fire_jet, smoker                               -> magma_block
//   canopy_fence -> spruce_fence, firefly_jar / cicada_jar -> lantern
//   huge_mushgloom -> brown_mushroom_block, huge_mushgloom_stem -> mushroom_stem
//   uberous_soil -> farmland / dirt, deadrock -> stone
// Properties a stand-in lacks are dropped (hasProperty-style degrade).
//
// TF block tags expanded by hand (TF tags are not loaded) — sources under
// mods_reference/.../generated/resources/data/twilightforest/tags/block/:
//   tf_berry_bushes_survive  = #minecraft:substrate_overworld, snow_block
//   tf_berry_bushes_replace  = #minecraft:replaceable, #minecraft:flowers, twilightforest:mayapple
//   oreberry_bushes_survive  = #c:stones, #minecraft:stone_bricks, #c:ores_in_ground/{stone,
//                              deepslate,netherrack}, #c:cobblestones, #c:netherracks,
//                              giant_cobblestone, polished andesite/diorite/granite,
//                              smooth_stone, infested_{chiseled,cracked,mossy,}stone_bricks
//   huge_mushgloom_placeable = #minecraft:substrate_overworld, mycelium, podzol,
//                              crimson_nylium, warped_nylium
//   twilightforest:landmark (structure tag) = the 16 landmark structures
// #minecraft:substrate_overworld (26.x) is read as the library's
// #minecraft:dirt, whose contents are that union (as pass one does).

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using levelgen::placement::PlacedFeature;
using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::WeightedStateProvider;
using levelgen::feature::stateproviders::WeightedStateEntry;
using levelgen::blockpredicates::matchesBlockTagName;
using world::level::block::Block;
using world::level::block::Blocks;
using world::level::block::state::properties::BlockStateProperties;

// ============================================================================
// twilight_decor:: shared helpers (declared in TwilightDecorCommon.h)
// ============================================================================
namespace twilight_decor {

namespace {

bool hasTag(BlockState* state, const char* tag) {
    return state != nullptr && matchesBlockTagName(state, tag);
}

std::string propertyKey(const PropertyMap& properties) {
    std::vector<std::pair<std::string, std::string>> sorted(properties.begin(), properties.end());
    std::sort(sorted.begin(), sorted.end());
    std::string key;
    for (const auto& [k, v] : sorted) key += k + "=" + v + ",";
    return key;
}

} // namespace

BlockState* resolveState(const std::string& name, const PropertyMap& properties, const char* feature) {
    BlockState* state = properties.empty()
        ? levelgen::twilight_blocks::defaultState(name)
        : levelgen::twilight_blocks::state(name, properties);
    if (state == nullptr) {
        // One line per (block, feature): bootstrap runs once.
        std::fprintf(stderr, "[TwilightDecorFeatures] %s does not resolve - %s not registered\n",
                     name.c_str(), feature);
    }
    return state;
}

bool isRealTwilight(const std::string& name) {
    return !levelgen::twilight_blocks::resolveName(name).empty()
        && !levelgen::twilight_blocks::isStandIn(name);
}

std::string realTwilightId(const std::string& name) {
    return isRealTwilight(name) ? levelgen::twilight_blocks::resolveName(name) : std::string();
}

BlockState* transferAllStateKeys(BlockState* stateIn, BlockState* stateOut) {
    if (stateIn == nullptr || stateOut == nullptr) return stateOut;
    Block* outBlock = stateOut->getBlock();
    if (outBlock == nullptr) return stateOut;

    static std::mutex s_mutex;
    static std::map<std::pair<const BlockState*, const Block*>, BlockState*> s_cache;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_cache.find({stateIn, outBlock});
        if (it != s_cache.end()) return it->second;
    }

    // for (Property p : stateOut.getProperties()) transferStateKey(stateIn, stateOut, p)
    const auto inProps = stateIn->getProperties();
    PropertyMap target = stateOut->getProperties();
    const auto& possible = outBlock->getStateDefinition().getPossibleStates();
    for (auto& [key, value] : target) {
        auto in = inProps.find(key);
        if (in == inProps.end()) continue;
        // Only a value the target property accepts transfers (same Property).
        bool accepted = false;
        for (BlockState* candidate : possible) {
            const auto props = candidate->getProperties();
            auto v = props.find(key);
            if (v != props.end() && v->second == in->second) { accepted = true; break; }
        }
        if (accepted) value = in->second;
    }
    BlockState* result = stateOut;
    const std::string wanted = propertyKey(target);
    for (BlockState* candidate : possible) {
        if (propertyKey(candidate->getProperties()) == wanted) { result = candidate; break; }
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    s_cache[{stateIn, outBlock}] = result;
    return result;
}

bool BlockSet::contains(BlockState* state) const {
    if (state == nullptr) return false;
    const std::string& id = state->getIdentifier();
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) return true;
    for (const std::string& tag : tags) {
        if (matchesBlockTagName(state, tag)) return true;
    }
    return false;
}

bool isSubstrateOverworld(BlockState* state) {
    return hasTag(state, "minecraft:substrate_overworld");
}

bool isLeaves(BlockState* state) {
    return state != nullptr && (state->isLeaves() || matchesBlockTagName(state, "minecraft:leaves"));
}

bool isLogs(BlockState* state) {
    return state != nullptr && (state->isLog() || matchesBlockTagName(state, "minecraft:logs"));
}

bool isBlockOk(BlockState* state) {
    return state != nullptr && state->isSolid();
}

bool isBlockNotOk(BlockState* state) {
    if (state == nullptr) return false;
    static const BlockSet s_notOk = [] {
        BlockSet set;
        set.ids.push_back("minecraft:bedrock");
        for (const char* name : {"twilightforest:giant_cobblestone", "twilightforest:giant_log",
                                 "twilightforest:giant_leaves", "twilightforest:giant_obsidian",
                                 "twilightforest:fluffy_cloud", "twilightforest:wispy_cloud",
                                 "twilightforest:rainy_cloud", "twilightforest:snowy_cloud",
                                 "twilightforest:hardened_dark_leaves"}) {
            const std::string id = realTwilightId(name);
            if (!id.empty()) set.ids.push_back(id);
        }
        return set;
    }();
    return state->isFluid() || s_notOk.contains(state);
}

bool hasChunk(WorldGenLevel& level, int32_t chunkX, int32_t chunkZ) {
    return level.getChunk(chunkX, chunkZ) != nullptr;
}

bool hasChunkAt(WorldGenLevel& level, const core::BlockPos& pos) {
    return hasChunk(level, pos.getX() >> 4, pos.getZ() >> 4);
}

bool isAreaSuitable(WorldGenLevel& level, const core::BlockPos& pos, int xWidth, int height, int zWidth,
                    bool underwaterAllowed) {
    // FeatureUtil.isAreaSuitable
    for (int cx = 0; cx < xWidth; ++cx) {
        for (int cz = 0; cz < zWidth; ++cz) {
            const core::BlockPos column = pos.offset(cx, 0, cz);
            if (!hasChunkAt(level, column)) return false;

            BlockState* state = level.getBlockState(column.below());
            if (state == nullptr || !state->isSolidRender() || isBlockNotOk(state)) {
                if (underwaterAllowed && state != nullptr && state->isFluid()) continue;
                return false;
            }
            for (int cy = 0; cy < height; ++cy) {
                const core::BlockPos above = column.above(cy);
                BlockState* aboveState = level.getBlockState(above);
                if (!level.isEmptyBlock(above) && (aboveState == nullptr || !aboveState->canBeReplaced())) {
                    if (underwaterAllowed && aboveState != nullptr && aboveState->isFluid()) continue;
                    return false;
                }
            }
        }
    }
    return true;
}

bool removeBlock(WorldGenLevel& level, const core::BlockPos& pos) {
    BlockState* air = Blocks::getDefaultState("minecraft:air");
    return air != nullptr && level.setBlock(pos, air, kUpdateAll);
}

void markPosForPostprocessing(WorldGenLevel& level, const core::BlockPos& pos) {
    if (::world::IChunk* chunk = level.getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
        chunk->markPosForPostprocessing(pos);
    }
}

void markAboveForPostProcessing(WorldGenLevel& level, const core::BlockPos& placePos) {
    // Feature.markAboveForPostProcessing
    core::BlockPos::MutableBlockPos pos(placePos.getX(), placePos.getY(), placePos.getZ());
    for (int i = 0; i < 2; ++i) {
        pos.move(0, 1, 0);
        BlockState* state = level.getBlockState(pos);
        if (state != nullptr && state->isAir()) return;
        markPosForPostprocessing(level, pos);
    }
}

int64_t tfLootSeed(const WorldGenLevel& level, const core::BlockPos& pos) {
    // long arithmetic with Java wrap-around
    const uint64_t product = static_cast<uint64_t>(level.getSeed()) * static_cast<uint64_t>(static_cast<int64_t>(pos.getX()));
    const uint64_t sum = product + static_cast<uint64_t>(static_cast<int64_t>(pos.getY()));
    return static_cast<int64_t>(sum ^ static_cast<uint64_t>(static_cast<int64_t>(pos.getZ())));
}

void setLootContainer(WorldGenLevel& level, const core::BlockPos& pos, const std::string& blockEntityId,
                      const std::string& lootTable, int64_t seed) {
    if (::world::IChunk* chunk = level.getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
        chunk->setBlockEntityNbt(pos, "{LootTable:\"" + lootTable + "\",LootTableSeed:" + std::to_string(seed)
                                      + "l,components:{},id:\"" + blockEntityId + "\"}");
    }
}

void setSpawnerEntity(WorldGenLevel& level, const core::BlockPos& pos, const std::string& entityId) {
    // SpawnerBlockEntity.setEntityId: the empty spawn-potentials list draws
    // nothing; the saved BE is the BaseSpawner defaults plus the entity. The
    // id goes through tf_common::spawnerEntityId like every other TF spawner:
    // a "twilightforest:" mob is stored under the engine's slug, or its
    // vanilla stand-in when the engine lacks it (rising_zombie -> zombie) —
    // an id the engine cannot resolve would leave the spawner idle.
    if (::world::IChunk* chunk = level.getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
        chunk->setBlockEntityNbt(pos,
            std::string("{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,MinSpawnDelay:200s,"
                        "RequiredPlayerRange:16s,SpawnCount:4s,SpawnData:{entity:{id:\"")
            + levelgen::structure::twilight_pieces::tf_common::spawnerEntityId(entityId)
            + "\"}},SpawnPotentials:[],SpawnRange:4s,components:{},id:\"minecraft:mob_spawner\"}");
    }
}

core::Direction rotateDirection(int rotation, core::Direction direction) {
    if (core::getAxis(direction) == core::Axis::Y) return direction;
    switch (rotation & 3) {
        case 1: return core::rotateYClockwise(direction);
        case 2: return core::getOpposite(direction);
        case 3: return core::rotateYCounterClockwise(direction);
        default: return direction;
    }
}

core::Direction mirrorDirection(int mirror, core::Direction direction) {
    if (mirror == 2 && core::getAxis(direction) == core::Axis::X) return core::getOpposite(direction);   // FRONT_BACK
    if (mirror == 1 && core::getAxis(direction) == core::Axis::Z) return core::getOpposite(direction);   // LEFT_RIGHT
    return direction;
}

core::BlockPos rotatePos(const core::BlockPos& pos, int rotation) {
    switch (rotation & 3) {
        case 1: return core::BlockPos(-pos.getZ(), pos.getY(), pos.getX());
        case 2: return core::BlockPos(-pos.getX(), pos.getY(), -pos.getZ());
        case 3: return core::BlockPos(pos.getZ(), pos.getY(), -pos.getX());
        default: return pos;
    }
}

BlockState* rotateState(BlockState* state, int rotation) {
    return state == nullptr ? nullptr : levelgen::structure::state_transforms::rotateState(state, rotation & 3);
}

BlockState* mirrorState(BlockState* state, int mirror) {
    return state == nullptr ? nullptr : levelgen::structure::state_transforms::mirrorState(state, mirror);
}

const char* directionName(core::Direction direction) {
    return core::getName(direction);
}

} // namespace twilight_decor

namespace {

using namespace twilight_decor;

// Direction.values() order (DOWN, UP, NORTH, SOUTH, WEST, EAST).
constexpr core::Direction kAllDirections[6] = {
    core::Direction::DOWN, core::Direction::UP, core::Direction::NORTH,
    core::Direction::SOUTH, core::Direction::WEST, core::Direction::EAST};

int seaLevelOf(ChunkGenerator* generator) {
    // WorldUtil.getGeneratorSeaLevel; TFDimensionData.SEALEVEL (0) otherwise.
    return generator != nullptr ? generator->getSeaLevel() : 0;
}

// SnowLayerBlock.canSurvive (26.x): the snow-layer tags, else a full top
// face (leaves have a full collision cube), else an 8-layer snow block.
bool snowCanSurvive(WorldGenLevel& level, const core::BlockPos& pos) {
    const core::BlockPos belowPos = pos.below();
    BlockState* below = level.getBlockState(belowPos);
    if (below == nullptr) return false;
    if (matchesBlockTagName(below, "minecraft:cannot_support_snow_layer")) return false;
    if (matchesBlockTagName(below, "minecraft:support_override_snow_layer")) return true;
    if (below->isLeaves()) return true;
    if (below->isCollisionShapeFullBlock(level, belowPos)) return true;
    return below->getIdentifier() == "minecraft:snow"
        && below->hasProperty(BlockStateProperties::LAYERS)
        && below->getValueOrElse(*BlockStateProperties::LAYERS, 1) == 8;
}

// Biome.shouldSnow (26.x). Block light is 0 during generation.
bool shouldSnow(WorldGenLevel& level, const world::biome::Biome* biome, const core::BlockPos& pos, int seaLevel) {
    if (biome == nullptr) return false;
    if (biome->getPrecipitationAt(pos, seaLevel) != world::biome::Precipitation::SNOW) return false;
    if (pos.getY() < level.getMinY() || pos.getY() >= level.getMaxY()) return false;
    BlockState* state = level.getBlockState(pos);
    if (state == nullptr) return false;
    return (state->isAir() || state->getIdentifier() == "minecraft:snow") && snowCanSurvive(level, pos);
}

// Worldgen raw brightness (engine convention, see Blocks.cpp
// MushroomBlockImpl): 15 in a skylight dimension, 0 without.
int worldgenRawBrightness(const WorldGenLevel& level) {
    return level.hasSkyLight() ? 15 : 0;
}

// LevelReader.canSeeSkyFromBelowWater. canSeeSky during generation is the
// skylight flag (the same convention as the brightness above); the scan
// below sea level stops on any light-dampening non-liquid block —
// getLightDampening() > 0: an opaque full block, leaves, or a waterlogged
// (fluid-holding, non-liquid) block.
bool canSeeSkyFromBelowWater(WorldGenLevel& level, const core::BlockPos& pos, int seaLevel) {
    if (!level.hasSkyLight()) return false;
    if (pos.getY() >= seaLevel) return true;
    for (int y = seaLevel - 1; y > pos.getY(); --y) {
        const core::BlockPos scan(pos.getX(), y, pos.getZ());
        BlockState* state = level.getBlockState(scan);
        if (state == nullptr || state->isFluid()) continue;
        const bool dampens = state->isSolidRender() || state->isLeaves() || state->hasAnyFluid();
        if (dampens) return false;
    }
    return true;
}

// LevelReader.isAreaLoaded(center, range) = hasChunksAt of the box.
bool isAreaLoaded(WorldGenLevel& level, const core::BlockPos& center, int range) {
    const int minY = center.getY() - range;
    const int maxY = center.getY() + range;
    if (maxY < level.getMinY() || minY > level.getMaxY() - 1) return false;
    const int minCX = (center.getX() - range) >> 4;
    const int maxCX = (center.getX() + range) >> 4;
    const int minCZ = (center.getZ() - range) >> 4;
    const int maxCZ = (center.getZ() + range) >> 4;
    for (int cx = minCX; cx <= maxCX; ++cx) {
        for (int cz = minCZ; cz <= maxCZ; ++cz) {
            if (!hasChunk(level, cx, cz)) return false;
        }
    }
    return true;
}

// ============================================================================
// BerryBushFeature — feature/BerryBushFeature.java, config/BerryBushConfig.java
// ============================================================================
struct BerryBushConfig {
    // bush.trySetValue(AGE_3, age), then trySetValue(SNOW_LAYERS, 1) when snowy.
    std::array<std::array<BlockState*, 2>, 4> states{};
    BlockSet generatesOn;
    bool canBeSnowy = true;
};

class BerryBushFeature : public Feature<BerryBushConfig> {
public:
    BerryBushFeature(BlockSet replaceable, BlockState* snow)
        : m_replaceable(std::move(replaceable)), m_snow(snow) {}

    bool place(FeaturePlaceContext<BerryBushConfig>& context) override {
        WorldGenLevel* level = context.level();
        if (level == nullptr) return false;
        const BerryBushConfig& config = context.config();
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& random = context.random();

        if (!config.generatesOn.contains(level->getBlockState(pos.below()))) return false;

        const bool snowy = config.canBeSnowy
            && shouldSnow(*level, level->getBiome(pos), pos, seaLevelOf(context.chunkGenerator()));
        switch (chooseSize(random)) {
            case Size::LARGE:  return generateLargeNode(*level, pos, config, random, snowy);
            case Size::MEDIUM: return generateMediumNode(*level, pos, config, random, snowy);
            case Size::SMALL:  return generateSmallNode(*level, pos, config, random, snowy);
            case Size::TINY:
            default:           return setBush(*level, pos, config, random.nextInt(4), snowy);
        }
    }

private:
    enum class Size { TINY, SMALL, MEDIUM, LARGE };

    static int taxicab(int a, int b) { return std::abs(a) + std::abs(b); }

    // WorldUtil.getRandomElementWithWeights over LARGE 1, MEDIUM 2, SMALL 4, TINY 3
    static Size chooseSize(WorldgenRandom& random) {
        static const std::pair<Size, float> kWeights[4] = {
            {Size::LARGE, 1.0f}, {Size::MEDIUM, 2.0f}, {Size::SMALL, 4.0f}, {Size::TINY, 3.0f}};
        const float totalWeight = static_cast<float>(1.0 + 2.0 + 4.0 + 3.0);
        float randomValue = random.nextFloat() * totalWeight;
        for (const auto& [size, weight] : kWeights) {
            randomValue -= weight;
            if (randomValue < 0.0f) return size;
        }
        return kWeights[random.nextInt(4)].first;   // Util.getRandom fallback (unreachable)
    }

    bool generateLargeNode(WorldGenLevel& level, const core::BlockPos& pos, const BerryBushConfig& config,
                           WorldgenRandom& random, bool snowy) {
        bool placed = false;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                placed |= setBushRandomAge(level, pos.offset(dx, -2, dz), config, random, snowy);

        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -1; dy <= 0; ++dy)
                for (int dz = -2; dz <= 2; ++dz)
                    if (taxicab(dx, dz) < 4)
                        placed |= setBushRandomAge(level, pos.offset(dx, dy, dz), config, random, snowy);

        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                placed |= setBushRandomAge(level, pos.offset(dx, 1, dz), config, random, snowy);
        return placed;
    }

    bool generateMediumNode(WorldGenLevel& level, const core::BlockPos& pos, const BerryBushConfig& config,
                            WorldgenRandom& random, bool snowy) {
        bool placed = false;
        for (int dy = -1; dy <= 2; ++dy) {
            const int maxTaxicabDistance = std::min(2 - dy, 2);
            for (int dx = -maxTaxicabDistance; dx <= maxTaxicabDistance; ++dx) {
                for (int dz = -maxTaxicabDistance; dz <= maxTaxicabDistance; ++dz) {
                    if (taxicab(dx, dz) < 2 * maxTaxicabDistance || random.nextBoolean())
                        placed |= setBushRandomAge(level, pos.offset(dx, dy, dz), config, random, snowy);
                }
            }
        }
        return placed;
    }

    bool generateSmallNode(WorldGenLevel& level, const core::BlockPos& pos, const BerryBushConfig& config,
                           WorldgenRandom& random, bool snowy) {
        bool placed = setBushRandomAge(level, pos, config, random, snowy);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 0; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    if (taxicab(dx, dz) == 1 && random.nextBoolean())
                        placed |= setBush(level, pos.offset(dx, dy, dz), config, random.nextInt(4), snowy);
        return placed;
    }

    // setBush(level, pos, state, generatesOn, random, snowy): the age draw is
    // an argument, so it happens before any test.
    bool setBushRandomAge(WorldGenLevel& level, const core::BlockPos& pos, const BerryBushConfig& config,
                          WorldgenRandom& random, bool snowy) {
        const int age = random.nextFloat() < 0.2f ? 3 : 2;   // DEFAULT_RIPE_PROBABILITY
        return setBush(level, pos, config, age, snowy);
    }

    bool setBush(WorldGenLevel& level, const core::BlockPos& pos, const BerryBushConfig& config, int age, bool snowy) {
        BlockState* toReplace = level.getBlockState(pos);
        if (toReplace == nullptr || !m_replaceable.contains(toReplace)
            || matchesBlockTagName(toReplace, "minecraft:features_cannot_replace")
            || toReplace->hasAnyFluid()) {
            return false;
        }
        BlockState* below = level.getBlockState(pos.below());
        if (!config.generatesOn.contains(below) && age < 2) return false;

        BlockState* base = config.states[static_cast<size_t>(age & 3)][0];
        BlockState* toPlace = base;
        if (snowy && !(below != nullptr && base != nullptr && below->getBlock() == base->getBlock())) {
            toPlace = config.states[static_cast<size_t>(age & 3)][1];
        }
        if (toPlace == nullptr) return false;
        level.setBlock(pos, toPlace, kUpdateAll);
        twilight_decor::markAboveForPostProcessing(level, pos);

        if (snowy && age >= 2 && m_snow != nullptr) level.setBlock(pos.above(), m_snow, kUpdateAll);
        return true;
    }

    BlockSet m_replaceable;   // #twilightforest:tf_berry_bushes_replace
    BlockState* m_snow;
};

// ============================================================================
// UndergroundPlantFeature — feature/UndergroundPlantFeature.java with its
// maxCount / spawnInStructure variants ("twilightforest:oreberry_bushes" =
// maxCount 1; "twilightforest:underground_plants_in_structure" = in
// structures allowed) and the landmark exclusion
// (util/landmarks/LandmarkUtil.locateNearestLandmarkStart).
// ============================================================================
class TFUndergroundPlantFeature : public Feature<BlockStateConfiguration> {
public:
    enum class Survival {
        OREBERRY,          // OreBerryBushBlock / TFBushBlock.canSurvive
        OREBERRY_IN_LIGHT, // essence oreberry (surviveInLight)
        MUSHGLOOM          // MushgloomBlock.canSurvive
    };

    TFUndergroundPlantFeature(int maxCount, bool spawnInStructure, Survival survival,
                              BlockSet oreberrySurvive, std::string uberousSoilId,
                              BlockState* trollvidr, BlockState* unripeTrollber,
                              std::vector<std::string> landmarkStructures)
        : m_maxCount(maxCount), m_spawnInStructure(spawnInStructure), m_survival(survival)
        , m_oreberrySurvive(std::move(oreberrySurvive)), m_uberousSoilId(std::move(uberousSoilId))
        , m_trollvidr(trollvidr), m_unripeTrollber(unripeTrollber)
        , m_landmarks(std::move(landmarkStructures)) {}

    bool place(FeaturePlaceContext<BlockStateConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr || context.config().state == nullptr) return false;
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        int x = origin.getX();
        int z = origin.getZ();
        int placed = 0;
        for (int y = origin.getY(); y > world->getMinY(); --y) {
            if (placed >= m_maxCount) break;

            const core::BlockPos pos(x, y, z);
            if (!world->isEmptyBlock(pos) || random.nextInt(6) == 0) {
                // origin + nextInt(4) - nextInt(4), left to right
                int dx = random.nextInt(4);
                dx -= random.nextInt(4);
                int dz = random.nextInt(4);
                dz -= random.nextInt(4);
                x = origin.getX() + dx;
                z = origin.getZ() + dz;
                continue;
            }

            BlockState* state = context.config().state;
            if (m_trollvidr != nullptr && state->getBlock() == m_trollvidr->getBlock()
                && random.nextInt(10) == 0 && m_unripeTrollber != nullptr) {
                state = m_unripeTrollber;
            }
            if (canSurvive(state, *world, pos) && (m_spawnInStructure || !insideLandmark(*world, pos))) {
                world->setBlock(pos, state, kKnownShapeClients);
                ++placed;
            }
        }
        return placed > 0;
    }

private:
    bool canSurvive(BlockState* state, WorldGenLevel& level, const core::BlockPos& pos) const {
        // An unripe trollber swapped in for trollvidr answers with its own
        // canSurvive (none of the features ported here configure trollvidr).
        if (m_unripeTrollber != nullptr && state == m_unripeTrollber) return state->canSurvive(level, pos);
        BlockState* below = level.getBlockState(pos.below());
        switch (m_survival) {
            case Survival::OREBERRY:
            case Survival::OREBERRY_IN_LIGHT: {
                // TFBushBlock.canSurvive: canBePlacedAt(below) or the same bush
                // below at age >= MAX_AGE - 1; OreBerryBushBlock adds the light
                // rule unless surviveInLight.
                const bool sameMature = below != nullptr && below->getBlock() == state->getBlock()
                    && below->hasProperty(BlockStateProperties::AGE_3)
                    && below->getValueOrElse(*BlockStateProperties::AGE_3, 0) >= 2;
                const bool supported = m_oreberrySurvive.contains(below) || sameMature;
                const bool lightOk = m_survival == Survival::OREBERRY_IN_LIGHT || worldgenRawBrightness(level) < 13;
                return supported && lightOk;
            }
            case Survival::MUSHGLOOM:
                // MushgloomBlock.canSurvive (the mod tests the face at `pos`).
                return below != nullptr
                    && (below->isFaceSturdy(level, pos, core::Direction::UP)
                        || (!m_uberousSoilId.empty() && below->getIdentifier() == m_uberousSoilId));
        }
        return false;
    }

    // LandmarkUtil.locateNearestLandmarkStart(level, chunkX, chunkZ) then
    // the start's bounding box: the first #twilightforest:landmark start in
    // the landmark-centre chunk (checkReady: that chunk must be present).
    bool insideLandmark(WorldGenLevel& level, const core::BlockPos& pos) const {
        const core::BlockPos center = levelgen::structure::twilight_landmarks::getNearestCenterXZ(
            pos.getX() >> 4, pos.getZ() >> 4);
        const int cx = center.getX() >> 4;
        const int cz = center.getZ() >> 4;
        ::world::IChunk* chunk = level.getChunk(cx, cz);
        if (chunk == nullptr) return false;
        for (const auto& [name, start] : chunk->getAllStructureStarts()) {
            if (std::find(m_landmarks.begin(), m_landmarks.end(), name) == m_landmarks.end()) continue;
            return start.boundingBox.isInside(pos.getX(), pos.getY(), pos.getZ());
        }
        return false;
    }

    int m_maxCount;
    bool m_spawnInStructure;
    Survival m_survival;
    BlockSet m_oreberrySurvive;
    std::string m_uberousSoilId;
    BlockState* m_trollvidr;
    BlockState* m_unripeTrollber;
    std::vector<std::string> m_landmarks;
};

// ============================================================================
// HugeLilypadFeature — feature/HugeLilypadFeature.java
// ============================================================================
class HugeLilypadFeature : public Feature<NoneFeatureConfiguration> {
public:
    // states[facing2D][piece]: facing = Direction.from2DDataValue (S, W, N, E),
    // piece NW, NE, SE, SW.
    explicit HugeLilypadFeature(std::array<std::array<BlockState*, 4>, 4> states) : m_states(states) {}

    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& random = context.random();

        for (int i = 0; i < 10; ++i) {
            int dx = random.nextInt(8);
            dx -= random.nextInt(8);
            int dy = random.nextInt(4);
            dy -= random.nextInt(4);
            int dz = random.nextInt(8);
            dz -= random.nextInt(8);
            const core::BlockPos dPos = pos.offset(dx, dy, dz);

            if (shouldPlacePadAt(*world, dPos) && isAreaLoaded(*world, dPos, 1)) {
                const int facing = random.nextInt(4);
                const auto& pieces = m_states[static_cast<size_t>(facing)];
                if (pieces[0]) world->setBlock(dPos, pieces[0], kKnownShapeClients);
                if (pieces[1]) world->setBlock(dPos.east(), pieces[1], kKnownShapeClients);
                if (pieces[2]) world->setBlock(dPos.east().south(), pieces[2], kKnownShapeClients);
                if (pieces[3]) world->setBlock(dPos.south(), pieces[3], kKnownShapeClients);
            }
        }
        return true;
    }

private:
    static bool isWater(BlockState* state) {
        return state != nullptr && state->getIdentifier() == "minecraft:water";
    }
    static bool shouldPlacePadAt(WorldGenLevel& world, const core::BlockPos& pos) {
        return world.isEmptyBlock(pos) && isWater(world.getBlockState(pos.below()))
            && world.isEmptyBlock(pos.east()) && isWater(world.getBlockState(pos.east().below()))
            && world.isEmptyBlock(pos.south()) && isWater(world.getBlockState(pos.south().below()))
            && world.isEmptyBlock(pos.east().south()) && isWater(world.getBlockState(pos.east().south().below()));
    }

    std::array<std::array<BlockState*, 4>, 4> m_states;
};

// ============================================================================
// HugeWaterLilyFeature — feature/HugeWaterLilyFeature.java
// ============================================================================
class HugeWaterLilyFeature : public Feature<NoneFeatureConfiguration> {
public:
    explicit HugeWaterLilyFeature(BlockState* lily) : m_lily(lily) {}

    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& random = context.random();

        for (int i = 0; i < 4; ++i) {
            int dx = random.nextInt(8);
            dx -= random.nextInt(8);
            int dy = random.nextInt(4);
            dy -= random.nextInt(4);
            int dz = random.nextInt(8);
            dz -= random.nextInt(8);
            const core::BlockPos p = pos.offset(dx, dy, dz);
            BlockState* below = world->getBlockState(p.below());
            if (world->isEmptyBlock(p) && below != nullptr && below->getIdentifier() == "minecraft:water") {
                world->setBlock(p, m_lily, kKnownShapeClients);
            }
        }
        return true;
    }

private:
    BlockState* m_lily;
};

// ============================================================================
// WebFeature — feature/WebFeature.java
// ============================================================================
class WebFeature : public Feature<NoneFeatureConfiguration> {
public:
    explicit WebFeature(BlockState* cobweb) : m_cobweb(cobweb) {}

    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        WorldgenRandom& random = context.random();
        const core::BlockPos& origin = context.origin();

        // world.getMaxY() is inclusive in 26.x (the library's is exclusive).
        const int bound = (world->getMaxY() - 1) - origin.getY();
        if (bound <= 0) return false;   // Java's nextInt would throw here
        core::BlockPos pos = origin.above(random.nextInt(bound));
        while (pos.getY() > origin.getY()) {
            pos = pos.below();
            BlockState* state = world->getBlockState(pos);
            // isValidMaterial: #logs or #leaves
            if (world->isEmptyBlock(pos.below()) && (isLogs(state) || isLeaves(state))) {
                const bool onLeaf = isLeaves(state) && random.nextBoolean();
                world->setBlock(onLeaf ? pos : pos.below(), m_cobweb, kKnownShapeClients);
                return true;
            }
        }
        return false;
    }

private:
    BlockState* m_cobweb;
};

// ============================================================================
// ThornFeature — feature/ThornFeature.java, config/ThornsConfig.java
// ============================================================================
struct ThornsConfig {
    int maxSpread = 7;
    int chanceOfBranch = 3;
    int chanceOfLeaf = 3;
    int chanceLeafIsRose = 50;
};

class ThornFeature : public Feature<ThornsConfig> {
public:
    ThornFeature(std::array<BlockState*, 3> thornsByAxis, BlockState* leaves, std::array<BlockState*, 6> roseByFacing)
        : m_thorns(thornsByAxis), m_leaves(leaves), m_roses(roseByFacing) {}

    bool place(FeaturePlaceContext<ThornsConfig>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        WorldgenRandom& rand = context.random();
        const core::BlockPos& pos = context.origin();
        const int seaLevel = seaLevelOf(context.chunkGenerator());

        // make a 3-5 long stack going up
        const int nextLength = 2 + rand.nextInt(4);
        int maxLength = 2 + rand.nextInt(4);
        maxLength += rand.nextInt(4);
        maxLength += rand.nextInt(4);

        placeThorns(*world, rand, pos, nextLength, core::Direction::UP, maxLength, pos, context.config(), true, seaLevel);
        return true;
    }

private:
    static int axisIndex(core::Direction dir) {
        switch (core::getAxis(dir)) {
            case core::Axis::X: return 0;
            case core::Axis::Y: return 1;
            default: return 2;
        }
    }

    void placeThorns(WorldGenLevel& world, WorldgenRandom& rand, const core::BlockPos& pos, int length,
                     core::Direction dir, int maxLength, const core::BlockPos& oPos,
                     const ThornsConfig& config, bool avoidGiantCloud, int seaLevel) {
        bool complete = false;
        for (int i = 0; i < length; ++i) {
            const core::BlockPos dPos = pos.relative(dir, i);

            // Makes it avoid the troll clouds
            if (!avoidGiantCloud || checkIsUnderCloud(world, pos, dPos, seaLevel)) {
                if (std::abs(dPos.getX() - oPos.getX()) < config.maxSpread
                    && std::abs(dPos.getZ() - oPos.getZ()) < config.maxSpread
                    && canPlaceThorns(world, dPos)) {
                    world.setBlock(dPos, m_thorns[static_cast<size_t>(axisIndex(dir))], kUpdateClients);
                    markPosForPostprocessing(world, dPos);

                    // did we make it to the end?
                    if (i == length - 1) {
                        complete = true;
                        // maybe a leaf? or a rose?
                        if (rand.nextInt(config.chanceOfLeaf) == 0 && world.isEmptyBlock(dPos.relative(dir))) {
                            if (rand.nextInt(config.chanceLeafIsRose) > 0) {
                                if (m_leaves) world.setBlock(dPos.relative(dir), m_leaves, kUpdateAll);
                            } else {
                                BlockState* rose = m_roses[static_cast<size_t>(dir)];
                                if (rose) world.setBlock(dPos.relative(dir), rose, kUpdateAll);
                            }
                        }
                    }
                } else {
                    break;
                }
            } else {
                break;
            }
        }

        // add another off the end
        if (complete && maxLength > 1) {
            const core::Direction nextDir = kAllDirections[rand.nextInt(6)];   // Direction.getRandom
            const core::BlockPos nextPos = pos.relative(dir, length - 1).relative(nextDir);
            const int nextLength = 1 + rand.nextInt(maxLength);
            placeThorns(world, rand, nextPos, nextLength, nextDir, maxLength - 1, oPos, config, false, seaLevel);
        }

        // maybe another branch off the middle
        if (complete && length > 3 && rand.nextInt(config.chanceOfBranch) == 0) {
            const int middle = rand.nextInt(length);
            const core::Direction nextDir = kAllDirections[rand.nextInt(6)];
            const core::BlockPos nextPos = pos.relative(dir, middle).relative(nextDir);
            const int nextLength = 1 + rand.nextInt(maxLength);
            placeThorns(world, rand, nextPos, nextLength, nextDir, maxLength - 1, oPos, config, false, seaLevel);
        }

        // maybe a leaf
        if (complete && length > 3 && rand.nextInt(config.chanceOfLeaf) == 0) {
            const int middle = rand.nextInt(length);
            const core::Direction nextDir = kAllDirections[rand.nextInt(6)];
            const core::BlockPos nextPos = pos.relative(dir, middle).relative(nextDir);
            if (world.isEmptyBlock(nextPos) && m_leaves) world.setBlock(nextPos, m_leaves, kUpdateAll);
        }
    }

    static bool checkIsUnderCloud(WorldGenLevel& world, const core::BlockPos& pos, const core::BlockPos& dPos,
                                  int seaLevel) {
        return hasChunk(world, pos.getX() >> 4, pos.getZ() >> 4)
            && std::max(dPos.getY(), world.getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, dPos.getX(), dPos.getZ()))
               <= seaLevel + 150;
    }

    static bool canPlaceThorns(WorldGenLevel& world, const core::BlockPos& pos) {
        BlockState* state = world.getBlockState(pos);
        return state != nullptr && (state->isAir() || isLeaves(state));
    }

    std::array<BlockState*, 3> m_thorns;   // axis X, Y, Z
    BlockState* m_leaves;                  // thorn_leaves, distance 1
    std::array<BlockState*, 6> m_roses;    // thorn_rose facing, by Direction ordinal
};

// ============================================================================
// EnchantedForestVinesFeature — feature/EnchantedForestVinesFeature.java
// ============================================================================
class EnchantedForestVinesFeature : public Feature<NoneFeatureConfiguration> {
public:
    // vineStates[mask]: bit 0 up, 1 north, 2 south, 3 west, 4 east.
    EnchantedForestVinesFeature(std::array<BlockState*, 32> vineStates, std::string rainbowLeavesId)
        : m_vines(vineStates), m_rainbowLeavesId(std::move(rainbowLeavesId)) {}

    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return true;
        setAllPossibleStates(*world, context.random(), context.origin());
        return true;
    }

private:
    static constexpr int kRarity = 7;
    static constexpr int kExtraRarityOnTrees = 8;

    // The mod collects the 16x16 columns into a HashMap<Pair<Integer,
    // Integer>, ...> and iterates its entrySet; each column's draws depend on
    // its height, so the visit order is part of the RNG stream. Pair's hash is
    // x ^ z, so the 256 keys fall in 16 same-hash tree bins of a 512-slot
    // table: the buckets are visited in slot order ((h ^ (h >>> 16)) & 511),
    // reproduced exactly here. Inside one bin the mod's order follows the
    // tree built from System.identityHashCode tie-breaks (Pair is not
    // self-Comparable), which differs from run to run even in Java; the
    // deterministic insertion order (x, then z) is kept there.
    void setAllPossibleStates(WorldGenLevel& world, WorldgenRandom& random, const core::BlockPos& pos) {
        struct Column { int x; int z; uint32_t slot; int order; };
        std::vector<Column> columns;
        columns.reserve(256);
        int order = 0;
        for (int x = pos.getX(); x < pos.getX() + 16; ++x) {
            for (int z = pos.getZ(); z < pos.getZ() + 16; ++z) {
                const uint32_t h = static_cast<uint32_t>(x ^ z);
                const uint32_t spread = h ^ (h >> 16);
                columns.push_back({x, z, spread & 511u, order++});
            }
        }
        std::stable_sort(columns.begin(), columns.end(),
                         [](const Column& a, const Column& b) { return a.slot < b.slot; });

        for (const Column& column : columns) {
            // getColumnY: (-10, max of the four neighbours' MOTION_BLOCKING heights)
            const int h1 = world.getHeight(Heightmap::Types::MOTION_BLOCKING, column.x - 1, column.z);
            const int h2 = world.getHeight(Heightmap::Types::MOTION_BLOCKING, column.x + 1, column.z);
            const int h3 = world.getHeight(Heightmap::Types::MOTION_BLOCKING, column.x, column.z - 1);
            const int h4 = world.getHeight(Heightmap::Types::MOTION_BLOCKING, column.x, column.z + 1);
            const int maxY = std::max(std::max(h1, h2), std::max(h3, h4));
            for (int y = -10; y <= maxY; ++y) {
                setVine(world, random, core::BlockPos(column.x, y, column.z));
            }
        }
    }

    static int faceBit(core::Direction dir) {
        switch (dir) {
            case core::Direction::UP: return 1;
            case core::Direction::NORTH: return 2;
            case core::Direction::SOUTH: return 4;
            case core::Direction::WEST: return 8;
            case core::Direction::EAST: return 16;
            default: return 0;
        }
    }

    void setVine(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos) {
        // BlockState.isEmpty() — an air block.
        if (random.nextInt(kRarity) > 0) return;
        BlockState* here = level.getBlockState(pos);
        if (here == nullptr || !here->isAir() || !isSuitableBiome(level, pos)) return;

        int mask = 0;
        bool empty = true;
        bool isTree = true;
        for (core::Direction dir : kAllDirections) {
            if (dir == core::Direction::DOWN) continue;
            const core::BlockPos relativePos = pos.relative(dir);
            BlockState* neighbour = level.getBlockState(relativePos);
            if (world::level::block::VineBlock::isAcceptableNeighbour(level, relativePos, dir)
                && !(neighbour != nullptr && !m_rainbowLeavesId.empty() && neighbour->getIdentifier() == m_rainbowLeavesId)) {
                if (!(isLogs(neighbour) || isLeaves(neighbour))) isTree = false;
                mask |= faceBit(dir);
                if (core::getAxis(dir) != core::Axis::Y) empty = false;
            }
        }
        if (isTree && random.nextInt(kExtraRarityOnTrees) > 0) return;

        if (!empty) {
            BlockState* vine = m_vines[static_cast<size_t>(mask)];
            if (vine) level.setBlock(pos, vine, kUpdateAll);   // Feature.setBlock
        }
    }

    static bool isSuitableBiome(WorldGenLevel& level, const core::BlockPos& pos) {
        const world::biome::Biome* biome = level.getBiome(pos);
        if (biome == nullptr) return false;
        const std::string& name = biome->getName();
        const size_t colon = name.find(':');
        return (colon == std::string::npos ? name : name.substr(colon + 1)) == "enchanted_forest";
    }

    std::array<BlockState*, 32> m_vines;
    std::string m_rainbowLeavesId;   // #... is(TFBlocks.RAINBOW_OAK_LEAVES) (resolved block)
};

// ============================================================================
// FireJetFeature — feature/FireJetFeature.java
// ============================================================================
class FireJetFeature : public Feature<BlockStateConfiguration> {
public:
    FireJetFeature(BlockState* grass, BlockState* lava, BlockState* stone)
        : m_grass(grass), m_lava(lava), m_stone(stone) {}

    bool place(FeaturePlaceContext<BlockStateConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        BlockState* jet = context.config().state;
        if (world == nullptr || jet == nullptr) return false;
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& rand = context.random();
        const int seaLevel = seaLevelOf(context.chunkGenerator());

        if (!isAreaSuitable(*world, pos, 5, 2, 5)) return false;

        for (int i = 0; i < 4; ++i) {
            int dx = rand.nextInt(8);
            dx -= rand.nextInt(8);
            int dy = rand.nextInt(4);
            dy -= rand.nextInt(4);
            int dz = rand.nextInt(8);
            dz -= rand.nextInt(8);
            const core::BlockPos dPos = pos.offset(dx, dy, dz);

            if (world->isEmptyBlock(dPos) && canSeeSkyFromBelowWater(*world, dPos, seaLevel)
                && isSubstrateOverworld(world->getBlockState(dPos.below()))
                && isSubstrateOverworld(world->getBlockState(dPos.east().below()))
                && isSubstrateOverworld(world->getBlockState(dPos.west().below()))
                && isSubstrateOverworld(world->getBlockState(dPos.south().below()))
                && isSubstrateOverworld(world->getBlockState(dPos.north().below()))) {
                // create blocks around the jet/smoker, just in case
                for (int gx = -2; gx <= 2; ++gx)
                    for (int gz = -2; gz <= 2; ++gz)
                        world->setBlock(dPos.offset(gx, -1, gz), m_grass, kUpdateNone);

                // jet
                world->setBlock(dPos.below(), jet, kUpdateNone);

                // create reservoir with stone walls
                for (int rx = -2; rx <= 2; ++rx) {
                    for (int rz = -2; rz <= 2; ++rz) {
                        const core::BlockPos dPos2 = dPos.offset(rx, -2, rz);
                        if ((rx == 1 || rx == 0 || rx == -1) && (rz == 1 || rz == 0 || rz == -1)) {
                            world->setBlock(dPos2, m_lava, kUpdateNone);   // lava reservoir
                        } else {
                            BlockState* existing = world->getBlockState(dPos2);
                            if (!(existing != nullptr && existing->getIdentifier() == "minecraft:lava")) {
                                world->setBlock(dPos2, m_stone, kUpdateNone);   // only stone where there is no lava
                            }
                        }
                        world->setBlock(dPos2.below(), m_stone, kUpdateNone);
                    }
                }
            }
        }
        return true;
    }

private:
    BlockState* m_grass;
    BlockState* m_lava;
    BlockState* m_stone;
};

// ============================================================================
// LampostFeature — feature/LampostFeature.java
// ============================================================================
class LampostFeature : public Feature<BlockStateConfiguration> {
public:
    explicit LampostFeature(BlockState* canopyFence) : m_fence(canopyFence) {}

    bool place(FeaturePlaceContext<BlockStateConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        BlockState* lamp = context.config().state;
        if (world == nullptr || lamp == nullptr) return false;
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& rand = context.random();

        // we should start on a grass block
        BlockState* below = world->getBlockState(pos.below());
        if (below == nullptr || below->getIdentifier() != "minecraft:grass_block") return false;

        const int height = 1 + rand.nextInt(4);
        for (int dy = 0; dy <= height; ++dy) {
            BlockState* state = world->getBlockState(pos.above(dy));
            if (state == nullptr || (!state->isAir() && !state->canBeReplaced())) return false;
        }

        for (int dy = 0; dy < height; ++dy) world->setBlock(pos.above(dy), m_fence, kKnownShapeClients);
        // config.state.rotate(ROTATIONS[rand.nextInt(4)])
        world->setBlock(pos.above(height), rotateState(lamp, rand.nextInt(4)), kKnownShapeClients);
        return true;
    }

private:
    BlockState* m_fence;
};

// ============================================================================
// FoundationFeature — feature/FoundationFeature.java with
// config/RuinedFoundationConfig.java (foundation.json values)
// ============================================================================
struct RuinedFoundationConfig {
    // dimensions
    int wallWidthMin = 5, wallWidthMax = 9;          // wall_width uniform
    int wallHeightMin = 1, wallHeightMax = 5;        // wall_heights uniform
    std::vector<std::pair<int, int>> basementHeight; // weighted_list of constants
    float floorChanceMin = -2.0f, floorChanceMax = 1.0f;   // random_floor_chance uniform [min, max)
    // blocks
    std::shared_ptr<BlockStateProvider> floor;
    std::shared_ptr<BlockStateProvider> basementPosts;
    std::shared_ptr<BlockStateProvider> lootContainer;
    std::shared_ptr<BlockStateProvider> wallBlock;
    std::shared_ptr<BlockStateProvider> wallTop;
    std::shared_ptr<BlockStateProvider> decayedWall;
    std::shared_ptr<BlockStateProvider> decayedTop;
    std::string lootTable;
};

class FoundationFeature : public Feature<RuinedFoundationConfig> {
public:
    explicit FoundationFeature(BlockState* air) : m_air(air) {}

    bool place(FeaturePlaceContext<RuinedFoundationConfig>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        const RuinedFoundationConfig& config = context.config();
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& rand = context.random();

        const int xWidth = sampleUniform(rand, config.wallWidthMin, config.wallWidthMax);
        const int zWidth = sampleUniform(rand, config.wallWidthMin, config.wallWidthMax);

        if (!isAreaSuitable(*world, pos.offset(1, 0, 1), xWidth - 1, 4, zWidth - 1)) return false;

        generateFoundation(*world, rand, pos, xWidth, zWidth, config);

        const int basementDepth = sampleBasement(rand, config);
        if (basementDepth > 0) {
            generateBasement(xWidth - 2, zWidth - 2, basementDepth, *world, pos.offset(1, -3, 1), rand, config);
        }
        return true;
    }

private:
    // UniformInt.sample = Mth.randomBetweenInclusive
    static int sampleUniform(WorldgenRandom& rand, int min, int max) {
        return rand.nextInt(max - min + 1) + min;
    }

    // WeightedListInt.sample: getRandomOrThrow (nextInt(total)), then the
    // constant sample (no draw).
    static int sampleBasement(WorldgenRandom& rand, const RuinedFoundationConfig& config) {
        int total = 0;
        for (const auto& entry : config.basementHeight) total += entry.second;
        if (total <= 0) return 0;
        int selection = rand.nextInt(total);
        for (const auto& entry : config.basementHeight) {
            selection -= entry.second;
            if (selection < 0) return entry.first;
        }
        return 0;
    }

    // UniformFloat.sample = nextFloat() * (max - min) + min
    static float sampleFloorTest(WorldgenRandom& rand, const RuinedFoundationConfig& config) {
        return rand.nextFloat() * (config.floorChanceMax - config.floorChanceMin) + config.floorChanceMin;
    }

    // FeatureLogic.getRotation: the available wall rotations (north NONE,
    // east CLOCKWISE_90, south CLOCKWISE_180, west COUNTERCLOCKWISE_90) are
    // Util.shuffle'd and then Util.getRandom picks one.
    static int getRotation(WorldgenRandom& rand, bool north, bool east, bool south, bool west) {
        std::vector<int> available;
        if (north) available.push_back(0);
        if (east) available.push_back(1);
        if (south) available.push_back(2);
        if (west) available.push_back(3);
        if (available.empty()) return 0;
        for (int i = static_cast<int>(available.size()); i > 1; --i) {
            const int swapTo = rand.nextInt(i);
            std::swap(available[static_cast<size_t>(i - 1)], available[static_cast<size_t>(swapTo)]);
        }
        return available[static_cast<size_t>(rand.nextInt(static_cast<int>(available.size())))];
    }

    // FeatureLogic.wallVolumeRotation: -1 = not on the wall
    static int wallVolumeRotation(WorldgenRandom& rand, int dX, int dZ, int xBound, int zBound) {
        const bool westWall = dX == 0;
        const bool northWall = dZ == 0;
        const bool eastWall = dX == xBound;
        const bool southWall = dZ == zBound;
        if (westWall || northWall || eastWall || southWall) {
            return getRotation(rand, northWall, eastWall, southWall, westWall);
        }
        return -1;
    }

    void generateFoundation(WorldGenLevel& world, WorldgenRandom& rand, const core::BlockPos& origin,
                            int xWidth, int zWidth, const RuinedFoundationConfig& config) {
        for (int dX = 0; dX <= xWidth; ++dX) {
            for (int dZ = 0; dZ <= zWidth; ++dZ) {
                // stone on the edges
                const int wallRotation = wallVolumeRotation(rand, dX, dZ, xWidth, zWidth);
                if (wallRotation >= 0) {
                    const int height = sampleUniform(rand, config.wallHeightMin, config.wallHeightMax);
                    for (int yBlock = 0; yBlock < height; ++yBlock) {
                        setWallBlock(world, rand, *config.wallBlock, *config.decayedWall, yBlock,
                                     origin.offset(dX, yBlock - 1, dZ), wallRotation);
                    }
                    setWallBlock(world, rand, *config.wallTop, *config.decayedTop, height,
                                 origin.offset(dX, height - 1, dZ), wallRotation);
                } else if (sampleFloorTest(rand, config) <= 0.0f) {
                    // destroyed wooden plank floor
                    setAndUpdate(world, rand, *config.floor, origin.offset(dX, -1, dZ), 0);
                }
            }
        }
    }

    void setWallBlock(WorldGenLevel& world, WorldgenRandom& rand, const BlockStateProvider& main,
                      const BlockStateProvider& decay, int yBlock, const core::BlockPos& placeAt, int rotation) {
        // rollDecay: nextInt(decayRarity + 1) >= 1 ? main : decay
        const BlockStateProvider& provider = rand.nextInt(yBlock + 1) >= 1 ? main : decay;
        setAndUpdate(world, rand, provider, placeAt, rotation);
    }

    void generateBasement(int xWidth, int zWidth, int depth, WorldGenLevel& world, const core::BlockPos& ceilingPos,
                          WorldgenRandom& rand, const RuinedFoundationConfig& config) {
        if (xWidth < 1 || zWidth < 1 || depth < 1) return;

        const int chestX = rollChestCoord(xWidth, rand);
        const int chestZ = rollChestCoord(zWidth, rand);

        // clear basement
        for (int dX = 0; dX <= xWidth; ++dX) {
            for (int dZ = 0; dZ <= zWidth; ++dZ) {
                int cornerOverlap = 0;
                if (dX == 0) ++cornerOverlap;
                if (dZ == 0) ++cornerOverlap;
                if (dX == xWidth) ++cornerOverlap;
                if (dZ == zWidth) ++cornerOverlap;
                const bool isInCorner = cornerOverlap > 1;

                for (int dY = 1 - depth; dY <= 0; ++dY) {
                    const core::BlockPos placeAt = ceilingPos.offset(dX, dY, dZ);
                    world.setBlock(placeAt, m_air, kUpdateAll);
                    if (isInCorner) setAndUpdate(world, rand, *config.basementPosts, placeAt, 0);
                }

                if ((dX == chestX && dZ == chestZ)
                    || (cornerOverlap == 0 && sampleFloorTest(rand, config) <= 0.0f)) {
                    // destroyed wooden plank floor, by chance or under the chest
                    setAndUpdate(world, rand, *config.floor, ceilingPos.offset(dX, -depth, dZ), 0);
                }
            }
        }

        // make chest
        const core::BlockPos lootPos = ceilingPos.offset(chestX, 1 - depth, chestZ);
        BlockState* container = config.lootContainer->getState(rand, lootPos);
        if (container == nullptr) return;
        world.setBlock(lootPos, container, kUpdateAll);
        BlockState* placed = world.getBlockState(lootPos);
        if (placed != nullptr && placed->getBlock() == container->getBlock()) {
            // the loot container's own block-entity type id
            setLootContainer(world, lootPos, container->getIdentifier(), config.lootTable, tfLootSeed(world, lootPos));
        }
    }

    static int rollChestCoord(int width, WorldgenRandom& rand) {
        if (width < 3) return rand.nextInt(std::max(0, width) + 1);   // no room to not be on an edge
        return rand.nextInt(std::max(0, width - 1) + 1) + 1;
    }

    void setAndUpdate(WorldGenLevel& world, WorldgenRandom& rand, const BlockStateProvider& provider,
                      const core::BlockPos& placeAt, int rotation) {
        BlockState* state = rotateState(provider.getState(rand, placeAt), rotation);
        if (state == nullptr) return;
        if (state->hasProperty(BlockStateProperties::WATERLOGGED)) {
            BlockState* here = world.getBlockState(placeAt);
            BlockState* above = world.getBlockState(placeAt.above());
            const bool hasWaterOrAbove = (here != nullptr && here->hasWaterFluid())
                                      || (above != nullptr && above->hasWaterFluid());
            if (hasWaterOrAbove) state = state->setValue(*BlockStateProperties::WATERLOGGED, true);
        }
        world.setBlock(placeAt, state, kUpdateAll);
        markPosForPostprocessing(world, placeAt);
    }

    BlockState* m_air;
};

// ============================================================================
// MonolithFeature — feature/MonolithFeature.java
// ============================================================================
class MonolithFeature : public Feature<NoneFeatureConfiguration> {
public:
    MonolithFeature(BlockState* obsidian, BlockState* lapis) : m_obsidian(obsidian), m_lapis(lapis) {}

    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& rand = context.random();

        const int ht = rand.nextInt(10) + 10;
        const int dir = rand.nextInt(4);

        if (!isAreaSuitable(*world, pos, 2, ht, 2)) return false;

        const int threeQuarters = static_cast<int>(ht * 0.75);
        const int half = static_cast<int>(ht * 0.5);
        int h0, h1, h2, h3;
        switch (dir) {
            case 0:  h0 = ht; h1 = threeQuarters; h2 = threeQuarters; h3 = half; break;
            case 1:  h0 = half; h1 = ht; h2 = threeQuarters; h3 = threeQuarters; break;
            case 2:  h0 = threeQuarters; h1 = half; h2 = ht; h3 = threeQuarters; break;
            default: h0 = threeQuarters; h1 = threeQuarters; h2 = half; h3 = ht; break;
        }

        auto column = [&](int dx, int dz, int h) {
            for (int cy = 0; cy <= h; ++cy) {
                world->setBlock(pos.offset(dx, cy - 1, dz), cy == ht ? m_lapis : m_obsidian, kUpdateAll);
            }
        };
        column(0, 0, h0);
        column(1, 0, h1);
        column(0, 1, h2);
        column(1, 1, h3);

        // Spawn a few ravens nearby. The engine's terrain library cannot add
        // entities during generation; the positions and the yaw draw
        // (Raven.snapTo(dPos, rand.nextFloat() * 360, 0)) are kept so the
        // stream matches the mod.
        for (int i = 0; i < 2; ++i) {
            int dx = rand.nextInt(8);
            dx -= rand.nextInt(8);
            int dz = rand.nextInt(8);
            dz -= rand.nextInt(8);
            const core::BlockPos dPos = pos.offset(dx, 0, dz);
            const int y = world->getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, dPos.getX(), dPos.getZ());
            if (y > 0) {
                (void)rand.nextFloat();
            }
        }
        return true;
    }

private:
    BlockState* m_obsidian;
    BlockState* m_lapis;
};

// ============================================================================
// BigMushgloomFeature — feature/BigMushgloomFeature.java over 26.1's
// AbstractHugeMushroomFeature (config can_place_on)
// ============================================================================
struct BigMushgloomConfig {
    BlockState* cap = nullptr;    // huge_mushgloom (down=false, sides/up true)
    BlockState* stem = nullptr;   // huge_mushgloom_stem (up/down false, sides true)
    int foliageRadius = 1;
    BlockSet canPlaceOn;          // #twilightforest:huge_mushgloom_placeable
    // getSphericalMushroomBlockState(cap, x, y, z, r, capHeight) by face mask
    // (bit 0 up, 1 west, 2 east, 3 north, 4 south).
    std::array<BlockState*, 32> capFaces{};
};

class BigMushgloomFeature : public Feature<BigMushgloomConfig> {
public:
    bool place(FeaturePlaceContext<BigMushgloomConfig>& context) override {
        WorldGenLevel* level = context.level();
        const BigMushgloomConfig& config = context.config();
        if (level == nullptr || config.cap == nullptr || config.stem == nullptr) return false;
        WorldgenRandom& random = context.random();
        const core::BlockPos& origin = context.origin();

        const int treeHeight = 2 + random.nextInt(2);   // getTreeHeight
        if (!isValidPosition(*level, origin, treeHeight, config)) return false;
        makeCap(*level, random, origin, treeHeight, config);
        placeTrunk(*level, origin, treeHeight, config);
        return true;
    }

private:
    // getTreeRadiusForHeight(-1, -1, foliageRadius, dy): dy <= 2 ? 0 : radius
    static int radiusForHeight(int foliageRadius, int dy) { return dy <= 2 ? 0 : foliageRadius; }

    static bool isValidPosition(WorldGenLevel& level, const core::BlockPos& origin, int treeHeight,
                                const BigMushgloomConfig& config) {
        const int y = origin.getY();
        // 26.x getMaxY() is inclusive (the library's is exclusive)
        if (y < level.getMinY() + 1 || y + treeHeight + 1 > level.getMaxY() - 1) return false;
        if (!config.canPlaceOn.contains(level.getBlockState(origin.below()))) return false;
        for (int dy = 0; dy <= treeHeight; ++dy) {
            const int radius = radiusForHeight(config.foliageRadius, dy);
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    BlockState* state = level.getBlockState(origin.offset(dx, dy, dz));
                    if (state != nullptr && !state->isAir() && !isLeaves(state)) return false;
                }
            }
        }
        return true;
    }

    static void makeCap(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos, int height,
                        const BigMushgloomConfig& config) {
        const int r = config.foliageRadius;
        const int capHeight = random.nextBoolean() ? 1 : 2;
        for (int y = 0; y < capHeight; ++y) {
            for (int x = -r; x <= r; ++x) {
                for (int z = -r; z <= r; ++z) {
                    const core::BlockPos capPos = pos.offset(x, height + y, z);
                    BlockState* existing = level.getBlockState(capPos);
                    if (existing != nullptr && existing->isSolidRender()) continue;
                    // FeatureLogic.getSphericalMushroomBlockState
                    const int mask = (y >= capHeight - 1 ? 1 : 0) | (x == -r ? 2 : 0) | (x == r ? 4 : 0)
                                   | (z == -r ? 8 : 0) | (z == r ? 16 : 0);
                    BlockState* state = config.capFaces[static_cast<size_t>(mask)];
                    level.setBlock(capPos, state != nullptr ? state : config.cap, kUpdateAll);
                }
            }
        }
    }

    static void placeTrunk(WorldGenLevel& level, const core::BlockPos& origin, int treeHeight,
                           const BigMushgloomConfig& config) {
        for (int dy = 0; dy < treeHeight; ++dy) {
            const core::BlockPos p = origin.above(dy);
            BlockState* current = level.getBlockState(p);
            if (current == nullptr || current->isAir()
                || matchesBlockTagName(current, "minecraft:replaceable_by_mushrooms")) {
                level.setBlock(p, config.stem, kUpdateAll);
            }
        }
    }
};

// ============================================================================
// CheckAbovePatchFeature — feature/CheckAbovePatchFeature.java
// ("twilightforest:mycelium_blob"): BaseDiskFeature plus the air-above test.
// ============================================================================
struct PatchConfig {
    BlockState* state = nullptr;             // simple_state_provider
    int radiusMin = 0, radiusMax = 0;        // radius uniform
    int halfHeight = 0;
    std::vector<std::string> targetIds;      // matching_blocks
};

class CheckAbovePatchFeature : public Feature<PatchConfig> {
public:
    bool place(FeaturePlaceContext<PatchConfig>& context) override {
        WorldGenLevel* level = context.level();
        const PatchConfig& config = context.config();
        if (level == nullptr || config.state == nullptr) return false;
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        bool placed = false;
        const int top = origin.getY() + config.halfHeight;
        const int bottom = origin.getY() - config.halfHeight - 1;
        const int radius = random.nextInt(config.radiusMax - config.radiusMin + 1) + config.radiusMin;

        // BlockPos.betweenClosed: x fastest, then y, then z.
        for (int z = origin.getZ() - radius; z <= origin.getZ() + radius; ++z) {
            for (int x = origin.getX() - radius; x <= origin.getX() + radius; ++x) {
                const int dx = x - origin.getX();
                const int dz = z - origin.getZ();
                if (dx * dx + dz * dz <= radius * radius) {
                    placed |= placeColumn(*level, config, top, bottom, x, z);
                }
            }
        }
        return placed;
    }

private:
    static bool placeColumn(WorldGenLevel& level, const PatchConfig& config, int start, int end, int x, int z) {
        bool placed = false;
        for (int y = start; y > end; --y) {
            const core::BlockPos pos(x, y, z);
            BlockState* state = level.getBlockState(pos);
            BlockState* above = level.getBlockState(pos.above());
            const bool target = state != nullptr
                && std::find(config.targetIds.begin(), config.targetIds.end(), state->getIdentifier()) != config.targetIds.end();
            if (target && above != nullptr && above->canBeReplaced()) {
                level.setBlock(pos, config.state, kUpdateClients);
                twilight_decor::markAboveForPostProcessing(level, pos);
                placed = true;
            }
        }
        return placed;
    }
};

// ============================================================================
// BlockSpikeFeature — feature/BlockSpikeFeature.java ("twilightforest:
// block_spike"): one STONE_STALACTITE stalagmite (TwilightSpikes.cpp).
// ============================================================================
class BlockSpikeFeature : public Feature<NoneFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        if (context.level() == nullptr) return false;
        return twilight::startSpike(*context.level(), context.origin(), twilight::stoneStalactite(),
                                    context.random(), false);
    }
};

// ============================================================================
// Storage
// ============================================================================
bool s_initialized = false;
std::mutex s_bootstrapMutex;

std::vector<std::unique_ptr<PlacedFeature>> s_inlinePlaced;
std::vector<std::shared_ptr<BlockStateProvider>> s_providers;
std::unique_ptr<BerryBushFeature> s_berryBushFeature;
std::vector<std::unique_ptr<TFUndergroundPlantFeature>> s_plantFeatures;
std::unique_ptr<HugeLilypadFeature> s_lilyPadFeature;
std::unique_ptr<HugeWaterLilyFeature> s_waterLilyFeature;
std::unique_ptr<WebFeature> s_webFeature;
std::unique_ptr<ThornFeature> s_thornFeature;
std::unique_ptr<EnchantedForestVinesFeature> s_vinesFeature;
std::unique_ptr<FireJetFeature> s_fireJetFeature;
std::unique_ptr<LampostFeature> s_lampostFeature;
std::unique_ptr<FoundationFeature> s_foundationFeature;
std::unique_ptr<MonolithFeature> s_monolithFeature;
BigMushgloomFeature s_bigMushgloomFeature;
CheckAbovePatchFeature s_patchFeature;
BlockSpikeFeature s_blockSpikeFeature;
RandomSelectorFeature s_randomSelectorFeature;

} // namespace

// ============================================================================
// Bootstrap
// ============================================================================
bool TwilightDecorFeatures::isInitialized() {
    return s_initialized;
}

void TwilightDecorFeatures::bootstrap() {
    std::lock_guard<std::mutex> lock(s_bootstrapMutex);
    if (s_initialized) return;
    if (!TwilightFeatures::isInitialized()) TwilightFeatures::bootstrap();

    using twilight::registerOwned;
    auto simpleProvider = [](BlockState* state) -> std::shared_ptr<BlockStateProvider> {
        auto provider = std::make_shared<levelgen::feature::stateproviders::SimpleStateProvider>(state);
        s_providers.push_back(provider);
        return provider;
    };
    auto weightedProvider = [](std::vector<WeightedStateEntry> entries) -> std::shared_ptr<BlockStateProvider> {
        auto provider = std::make_shared<WeightedStateProvider>(entries);
        s_providers.push_back(provider);
        return provider;
    };
    auto inlinePlaced = [](ConfiguredFeature* feature, const std::string& name) -> PlacedFeature* {
        if (feature == nullptr) return nullptr;
        auto placed = std::make_unique<PlacedFeature>(feature, std::vector<placement::PlacementModifier*>{}, name);
        PlacedFeature* raw = placed.get();
        s_inlinePlaced.push_back(std::move(placed));
        return raw;
    };

    BlockState* air = resolveState("minecraft:air", "decoration features");
    BlockState* snow = resolveState("minecraft:snow", "berry bushes");

    // #minecraft:substrate_overworld as the library tag
    BlockSet substrate;
    substrate.tags.push_back("minecraft:substrate_overworld");

    // ------------------------------------------------------- berry bushes
    // raspberry/blueberry/blackberry/maloberry_bushes.json: bush age 0,
    // layers 0; generates_on #tf_berry_bushes_survive; can_generate_snowy.
    {
        BlockSet replaceable;   // #twilightforest:tf_berry_bushes_replace
        replaceable.tags = {"minecraft:replaceable", "minecraft:flowers"};
        const std::string mayapple = levelgen::twilight_blocks::resolveName("twilightforest:mayapple");
        if (!mayapple.empty()) replaceable.ids.push_back(mayapple);
        s_berryBushFeature = std::make_unique<BerryBushFeature>(std::move(replaceable), snow);

        BlockSet generatesOn = substrate;   // #twilightforest:tf_berry_bushes_survive
        generatesOn.ids.push_back("minecraft:snow_block");

        for (const char* bush : {"raspberry", "blueberry", "blackberry", "maloberry"}) {
            const std::string blockName = std::string("twilightforest:") + bush + "_bush";
            const std::string id = std::string("twilightforest:") + bush + "_bushes";
            BerryBushConfig config;
            config.generatesOn = generatesOn;
            config.canBeSnowy = true;
            bool ok = true;
            for (int age = 0; age < 4; ++age) {
                for (int snowy = 0; snowy < 2; ++snowy) {
                    config.states[static_cast<size_t>(age)][static_cast<size_t>(snowy)] = resolveState(
                        blockName, PropertyMap{{"age", std::to_string(age)}, {"layers", snowy ? "1" : "0"}}, id.c_str());
                    ok = ok && config.states[static_cast<size_t>(age)][static_cast<size_t>(snowy)] != nullptr;
                }
            }
            if (!ok) continue;
            registerOwned(id, std::make_unique<ConfiguredFeatureImpl<BerryBushConfig, BerryBushFeature>>(
                s_berryBushFeature.get(), config));
        }
    }

    // #twilightforest:landmark (structure tag)
    const std::vector<std::string> landmarks = {
        "twilightforest:hedge_maze", "twilightforest:quest_grove", "twilightforest:mushroom_tower",
        "twilightforest:small_hollow_hill", "twilightforest:medium_hollow_hill", "twilightforest:large_hollow_hill",
        "twilightforest:naga_courtyard", "twilightforest:lich_tower", "twilightforest:labyrinth",
        "twilightforest:hydra_lair", "twilightforest:knight_stronghold", "twilightforest:dark_tower",
        "twilightforest:yeti_cave", "twilightforest:aurora_palace", "twilightforest:troll_cave",
        "twilightforest:final_castle"};

    BlockState* trollvidr = isRealTwilight("twilightforest:trollvidr")
        ? levelgen::twilight_blocks::defaultState("twilightforest:trollvidr") : nullptr;
    BlockState* unripeTrollber = isRealTwilight("twilightforest:unripe_trollber")
        ? levelgen::twilight_blocks::defaultState("twilightforest:unripe_trollber") : nullptr;
    const std::string uberousSoilId = levelgen::twilight_blocks::resolveName("twilightforest:uberous_soil");

    // ------------------------------------------------------- oreberries
    // iron/gold/copper/essence_oreberries.json: twilightforest:oreberry_bushes
    // (UndergroundPlantFeature maxCount 1), state age 3, layers 0.
    {
        BlockSet survive;   // #twilightforest:oreberry_bushes_survive
        survive.tags.push_back("minecraft:stone_bricks");
        survive.ids = {
            // #c:stones
            "minecraft:stone", "minecraft:andesite", "minecraft:diorite", "minecraft:granite",
            "minecraft:deepslate", "minecraft:tuff",
            // #c:ores_in_ground/stone
            "minecraft:coal_ore", "minecraft:copper_ore", "minecraft:diamond_ore", "minecraft:emerald_ore",
            "minecraft:gold_ore", "minecraft:iron_ore", "minecraft:lapis_ore", "minecraft:redstone_ore",
            // #c:ores_in_ground/deepslate
            "minecraft:deepslate_coal_ore", "minecraft:deepslate_copper_ore", "minecraft:deepslate_diamond_ore",
            "minecraft:deepslate_emerald_ore", "minecraft:deepslate_gold_ore", "minecraft:deepslate_iron_ore",
            "minecraft:deepslate_lapis_ore", "minecraft:deepslate_redstone_ore",
            // #c:ores_in_ground/netherrack
            "minecraft:nether_gold_ore", "minecraft:nether_quartz_ore",
            // #c:cobblestones
            "minecraft:cobblestone", "minecraft:infested_cobblestone", "minecraft:mossy_cobblestone",
            "minecraft:cobbled_deepslate",
            // #c:netherracks
            "minecraft:netherrack",
            "minecraft:polished_andesite", "minecraft:polished_diorite", "minecraft:polished_granite",
            "minecraft:smooth_stone", "minecraft:infested_chiseled_stone_bricks",
            "minecraft:infested_cracked_stone_bricks", "minecraft:infested_mossy_stone_bricks",
            "minecraft:infested_stone_bricks"};
        const std::string giantCobble = realTwilightId("twilightforest:giant_cobblestone");
        if (!giantCobble.empty()) survive.ids.push_back(giantCobble);

        struct Oreberry { const char* block; const char* id; bool inLight; };
        const Oreberry oreberries[] = {
            {"twilightforest:iron_oreberry_bush", "twilightforest:iron_oreberries", false},
            {"twilightforest:gold_oreberry_bush", "twilightforest:gold_oreberries", false},
            {"twilightforest:copper_oreberry_bush", "twilightforest:copper_oreberries", false},
            {"twilightforest:essence_oreberry_bush", "twilightforest:essence_oreberries", true}};
        for (const Oreberry& berry : oreberries) {
            BlockState* state = resolveState(berry.block, PropertyMap{{"age", "3"}, {"layers", "0"}}, berry.id);
            if (state == nullptr) continue;
            auto feature = std::make_unique<TFUndergroundPlantFeature>(
                1, false,
                berry.inLight ? TFUndergroundPlantFeature::Survival::OREBERRY_IN_LIGHT
                              : TFUndergroundPlantFeature::Survival::OREBERRY,
                survive, uberousSoilId, trollvidr, unripeTrollber, landmarks);
            registerOwned(berry.id, std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, TFUndergroundPlantFeature>>(
                feature.get(), BlockStateConfiguration(state)));
            s_plantFeatures.push_back(std::move(feature));
        }
    }

    // ------------------------------------------------------- troll_mushglooms
    // twilightforest:underground_plants_in_structure, mushgloom.
    if (BlockState* mushgloom = resolveState("twilightforest:mushgloom", "troll_mushglooms")) {
        auto feature = std::make_unique<TFUndergroundPlantFeature>(
            std::numeric_limits<int>::max(), true, TFUndergroundPlantFeature::Survival::MUSHGLOOM,
            BlockSet{}, uberousSoilId, trollvidr, unripeTrollber, landmarks);
        registerOwned("twilightforest:troll_mushglooms",
            std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, TFUndergroundPlantFeature>>(
                feature.get(), BlockStateConfiguration(mushgloom)));
        s_plantFeatures.push_back(std::move(feature));
    }

    // ------------------------------------------------------- lily pads
    {
        // HugeLilyPadBlock FACING (from2DDataValue: south, west, north, east) x PIECE
        static const char* kFacing2D[4] = {"south", "west", "north", "east"};
        static const char* kPieces[4] = {"nw", "ne", "se", "sw"};
        std::array<std::array<BlockState*, 4>, 4> pads{};
        bool ok = true;
        for (int f = 0; f < 4; ++f) {
            for (int p = 0; p < 4; ++p) {
                pads[static_cast<size_t>(f)][static_cast<size_t>(p)] = resolveState(
                    "twilightforest:huge_lily_pad", PropertyMap{{"facing", kFacing2D[f]}, {"piece", kPieces[p]}},
                    "huge_lily_pad");
                ok = ok && pads[static_cast<size_t>(f)][static_cast<size_t>(p)] != nullptr;
            }
        }
        if (ok) {
            s_lilyPadFeature = std::make_unique<HugeLilypadFeature>(pads);
            registerOwned("twilightforest:huge_lily_pad",
                std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, HugeLilypadFeature>>(
                    s_lilyPadFeature.get(), NoneFeatureConfiguration::INSTANCE));
        }
        if (BlockState* lily = resolveState("twilightforest:huge_water_lily", "huge_water_lily")) {
            s_waterLilyFeature = std::make_unique<HugeWaterLilyFeature>(lily);
            registerOwned("twilightforest:huge_water_lily",
                std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, HugeWaterLilyFeature>>(
                    s_waterLilyFeature.get(), NoneFeatureConfiguration::INSTANCE));
        }
    }

    // ------------------------------------------------------- webs
    if (BlockState* cobweb = resolveState("minecraft:cobweb", "webs")) {
        s_webFeature = std::make_unique<WebFeature>(cobweb);
        registerOwned("twilightforest:webs",
            std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, WebFeature>>(
                s_webFeature.get(), NoneFeatureConfiguration::INSTANCE));
    }

    // ------------------------------------------------------- thorns
    // thorns.json: max_spread 7, chance_of_branch 3, chance_of_leaf 3,
    // chance_leaf_is_rose 50. The mod places BROWN_THORNS; while it is
    // unregistered the TwilightBlocks "thorns" stand-in (a log with AXIS) is
    // used instead of brown_thorns' generic shape fallback (stone).
    {
        const char* thornsName = isRealTwilight("twilightforest:brown_thorns")
            ? "twilightforest:brown_thorns" : "twilightforest:thorns";
        std::array<BlockState*, 3> thorns{};
        static const char* kAxes[3] = {"x", "y", "z"};
        bool ok = true;
        for (int a = 0; a < 3; ++a) {
            thorns[static_cast<size_t>(a)] = resolveState(thornsName, PropertyMap{{"axis", kAxes[a]}}, "thorns");
            ok = ok && thorns[static_cast<size_t>(a)] != nullptr;
        }
        BlockState* leaves = resolveState("twilightforest:thorn_leaves", PropertyMap{{"distance", "1"}}, "thorns");
        std::array<BlockState*, 6> roses{};
        for (core::Direction dir : kAllDirections) {
            roses[static_cast<size_t>(dir)] = resolveState(
                "twilightforest:thorn_rose", PropertyMap{{"facing", directionName(dir)}}, "thorns");
        }
        if (ok) {
            s_thornFeature = std::make_unique<ThornFeature>(thorns, leaves, roses);
            ThornsConfig config;
            config.maxSpread = 7;
            config.chanceOfBranch = 3;
            config.chanceOfLeaf = 3;
            config.chanceLeafIsRose = 50;
            registerOwned("twilightforest:thorns",
                std::make_unique<ConfiguredFeatureImpl<ThornsConfig, ThornFeature>>(s_thornFeature.get(), config));
        }
    }

    // ------------------------------------------------------- enchanted_forest_vines
    {
        std::array<BlockState*, 32> vines{};
        bool ok = true;
        for (int mask = 0; mask < 32; ++mask) {
            vines[static_cast<size_t>(mask)] = resolveState("minecraft:vine", PropertyMap{
                {"up", (mask & 1) ? "true" : "false"}, {"north", (mask & 2) ? "true" : "false"},
                {"south", (mask & 4) ? "true" : "false"}, {"west", (mask & 8) ? "true" : "false"},
                {"east", (mask & 16) ? "true" : "false"}}, "enchanted_forest_vines");
            ok = ok && vines[static_cast<size_t>(mask)] != nullptr;
        }
        if (ok) {
            s_vinesFeature = std::make_unique<EnchantedForestVinesFeature>(
                vines, levelgen::twilight_blocks::resolveName("twilightforest:rainbow_oak_leaves"));
            registerOwned("twilightforest:enchanted_forest_vines",
                std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, EnchantedForestVinesFeature>>(
                    s_vinesFeature.get(), NoneFeatureConfiguration::INSTANCE));
        }
    }

    // ------------------------------------------------------- fire_jet / smoker
    {
        BlockState* grass = resolveState("minecraft:grass_block", "fire_jet");
        BlockState* lava = resolveState("minecraft:lava", "fire_jet");
        BlockState* stone = resolveState("minecraft:stone", "fire_jet");
        if (grass && lava && stone) {
            s_fireJetFeature = std::make_unique<FireJetFeature>(grass, lava, stone);
            // fire_jet.json: twilightforest:fire_jet[state=idle]
            if (BlockState* jet = resolveState("twilightforest:fire_jet", PropertyMap{{"state", "idle"}}, "fire_jet")) {
                registerOwned("twilightforest:fire_jet",
                    std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, FireJetFeature>>(
                        s_fireJetFeature.get(), BlockStateConfiguration(jet)));
            }
            // smoker.json: twilightforest:smoker. TwilightBlocks would hand
            // back vanilla minecraft:smoker (the furnace owns the slug), so
            // the TF smoker is taken from minecraft:tf_smoker when registered
            // and otherwise from its magma_block stand-in.
            BlockState* smoker = nullptr;
            if (Blocks::getBlock("minecraft:tf_smoker") != nullptr) {
                smoker = Blocks::getDefaultState("minecraft:tf_smoker");
            } else {
                BlockState* resolved = levelgen::twilight_blocks::defaultState("twilightforest:smoker");
                const bool vanillaFurnace = resolved != nullptr && resolved->getIdentifier() == "minecraft:smoker";
                smoker = vanillaFurnace ? resolveState("minecraft:magma_block", "smoker") : resolved;
                if (vanillaFurnace) {
                    std::fprintf(stderr, "[TwilightDecorFeatures] twilightforest:smoker collides with vanilla "
                                         "minecraft:smoker - stand-in minecraft:magma_block\n");
                }
            }
            if (smoker != nullptr) {
                registerOwned("twilightforest:smoker",
                    std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, FireJetFeature>>(
                        s_fireJetFeature.get(), BlockStateConfiguration(smoker)));
            }
        }
    }

    // ------------------------------------------------------- lampposts
    if (BlockState* canopyFence = resolveState("twilightforest:canopy_fence", "lampposts")) {
        s_lampostFeature = std::make_unique<LampostFeature>(canopyFence);
        auto lamppost = [&](const char* id, BlockState* lamp) -> ConfiguredFeature* {
            if (lamp == nullptr) return nullptr;
            return registerOwned(id, std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, LampostFeature>>(
                s_lampostFeature.get(), BlockStateConfiguration(lamp)));
        };
        ConfiguredFeature* firefly = lamppost("twilightforest:firefly_lamppost",
            resolveState("twilightforest:firefly_jar", PropertyMap{{"waterlogged", "false"}}, "firefly_lamppost"));
        ConfiguredFeature* cicada = lamppost("twilightforest:cicada_lamppost",
            resolveState("twilightforest:cicada_jar", PropertyMap{{"waterlogged", "false"}}, "cicada_lamppost"));
        lamppost("twilightforest:pumpkin_lamppost",
            resolveState("minecraft:jack_o_lantern", PropertyMap{{"facing", "north"}}, "pumpkin_lamppost"));

        // lamppost_placer.json: random_selector cicada 0.1, default firefly
        if (firefly != nullptr && cicada != nullptr) {
            RandomFeatureConfiguration config(
                {WeightedPlacedFeature(inlinePlaced(cicada, "lamppost_placer_cicada"), 0.1f)},
                inlinePlaced(firefly, "lamppost_placer_default"));
            registerOwned("twilightforest:lamppost_placer",
                std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                    &s_randomSelectorFeature, config));
        }
    }

    // ------------------------------------------------------- foundation
    {
        auto st = [](const char* name, PropertyMap props) { return resolveState(name, props, "foundation"); };
        BlockState* planks = st("minecraft:oak_planks", {});
        BlockState* oakSlab = st("minecraft:oak_slab", {{"type", "bottom"}, {"waterlogged", "false"}});
        auto oakStairs = [&](const char* facing) {
            return st("minecraft:oak_stairs", {{"facing", facing}, {"half", "bottom"}, {"shape", "straight"}, {"waterlogged", "false"}});
        };
        BlockState* stairsN = oakStairs("north");
        BlockState* stairsE = oakStairs("east");
        BlockState* stairsS = oakStairs("south");
        BlockState* stairsW = oakStairs("west");
        BlockState* fence = st("minecraft:oak_fence", {{"east", "false"}, {"north", "false"}, {"south", "false"},
                                                       {"waterlogged", "false"}, {"west", "false"}});
        BlockState* mossy = st("minecraft:mossy_cobblestone", {});
        BlockState* mossySlab = st("minecraft:mossy_cobblestone_slab", {{"type", "bottom"}, {"waterlogged", "false"}});
        auto mossyStairs = [&](const char* facing) {
            return st("minecraft:mossy_cobblestone_stairs", {{"facing", facing}, {"half", "bottom"}, {"shape", "straight"}, {"waterlogged", "false"}});
        };
        BlockState* mossyStairsE = mossyStairs("east");
        BlockState* mossyStairsW = mossyStairs("west");
        BlockState* chest = st("minecraft:chest", {{"type", "single"}, {"facing", "north"}, {"waterlogged", "false"}});
        BlockState* cobble = st("minecraft:cobblestone", {});
        BlockState* cobbleSlab = st("minecraft:cobblestone_slab", {{"type", "bottom"}, {"waterlogged", "false"}});
        auto cobbleStairs = [&](const char* facing) {
            return st("minecraft:cobblestone_stairs", {{"facing", facing}, {"half", "bottom"}, {"shape", "straight"}, {"waterlogged", "false"}});
        };
        BlockState* cobbleStairsE = cobbleStairs("east");
        BlockState* cobbleStairsW = cobbleStairs("west");

        if (planks && oakSlab && stairsN && stairsE && stairsS && stairsW && fence && mossy && mossySlab
            && mossyStairsE && mossyStairsW && chest && cobble && cobbleSlab && cobbleStairsE && cobbleStairsW && air) {
            RuinedFoundationConfig config;
            config.wallWidthMin = 5;  config.wallWidthMax = 9;
            config.wallHeightMin = 1; config.wallHeightMax = 5;
            config.basementHeight = {{3, 1}, {0, 1}};
            config.floorChanceMin = -2.0f; config.floorChanceMax = 1.0f;
            config.floor = weightedProvider({WeightedStateEntry(planks, 39), WeightedStateEntry(oakSlab, 1),
                                             WeightedStateEntry(stairsN, 6), WeightedStateEntry(stairsE, 2),
                                             WeightedStateEntry(stairsS, 6), WeightedStateEntry(stairsW, 2)});
            config.basementPosts = simpleProvider(fence);
            config.lootContainer = simpleProvider(chest);
            config.wallBlock = simpleProvider(cobble);
            config.wallTop = weightedProvider({WeightedStateEntry(cobble, 5), WeightedStateEntry(cobbleSlab, 1),
                                               WeightedStateEntry(cobbleStairsE, 2), WeightedStateEntry(cobbleStairsW, 2)});
            config.decayedWall = simpleProvider(mossy);
            config.decayedTop = weightedProvider({WeightedStateEntry(mossy, 5), WeightedStateEntry(mossySlab, 1),
                                                  WeightedStateEntry(mossyStairsE, 2), WeightedStateEntry(mossyStairsW, 2)});
            config.lootTable = "twilightforest:foundation_basement";
            s_foundationFeature = std::make_unique<FoundationFeature>(air);
            registerOwned("twilightforest:foundation",
                std::make_unique<ConfiguredFeatureImpl<RuinedFoundationConfig, FoundationFeature>>(
                    s_foundationFeature.get(), config));
        }
    }

    // ------------------------------------------------------- monolith
    {
        BlockState* obsidian = resolveState("minecraft:obsidian", "monolith");
        BlockState* lapis = resolveState("minecraft:lapis_block", "monolith");
        if (obsidian && lapis) {
            s_monolithFeature = std::make_unique<MonolithFeature>(obsidian, lapis);
            registerOwned("twilightforest:monolith",
                std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, MonolithFeature>>(
                    s_monolithFeature.get(), NoneFeatureConfiguration::INSTANCE));
        }
    }

    // ------------------------------------------------------- big mushgloom
    // mushroom/big_mushgloom.json: cap huge_mushgloom (down false, rest
    // true), stem huge_mushgloom_stem (up/down false), foliage radius 1,
    // can_place_on #twilightforest:huge_mushgloom_placeable.
    {
        BigMushgloomConfig config;
        config.cap = resolveState("twilightforest:huge_mushgloom", PropertyMap{{"down", "false"}, {"east", "true"},
            {"north", "true"}, {"south", "true"}, {"up", "true"}, {"west", "true"}}, "mushroom/big_mushgloom");
        config.stem = resolveState("twilightforest:huge_mushgloom_stem", PropertyMap{{"down", "false"}, {"east", "true"},
            {"north", "true"}, {"south", "true"}, {"up", "false"}, {"west", "true"}}, "mushroom/big_mushgloom");
        config.foliageRadius = 1;
        config.canPlaceOn = substrate;
        config.canPlaceOn.ids = {"minecraft:mycelium", "minecraft:podzol", "minecraft:crimson_nylium",
                                 "minecraft:warped_nylium"};
        if (config.cap && config.stem) {
            // hasAllMushroomsProperties: up + the four sides; otherwise the
            // cap state is placed unchanged.
            const auto capProps = config.cap->getProperties();
            const bool hasAll = capProps.count("up") && capProps.count("west") && capProps.count("east")
                             && capProps.count("north") && capProps.count("south");
            for (int mask = 0; mask < 32; ++mask) {
                if (!hasAll) { config.capFaces[static_cast<size_t>(mask)] = config.cap; continue; }
                PropertyMap props = {{"down", "false"},
                    {"up", (mask & 1) ? "true" : "false"}, {"west", (mask & 2) ? "true" : "false"},
                    {"east", (mask & 4) ? "true" : "false"}, {"north", (mask & 8) ? "true" : "false"},
                    {"south", (mask & 16) ? "true" : "false"}};
                config.capFaces[static_cast<size_t>(mask)] =
                    resolveState("twilightforest:huge_mushgloom", props, "mushroom/big_mushgloom");
            }
            registerOwned("twilightforest:mushroom/big_mushgloom",
                std::make_unique<ConfiguredFeatureImpl<BigMushgloomConfig, BigMushgloomFeature>>(
                    &s_bigMushgloomFeature, config));
        }
    }

    // ------------------------------------------------------- mycelium blobs
    // mycelium_blob / troll_cave_dirt / troll_cave_mycelium /
    // uberous_soil_patch_{big,small}.json (twilightforest:mycelium_blob).
    {
        auto patch = [&](const char* id, BlockState* state, int rMin, int rMax, int halfHeight,
                         std::vector<std::string> targets) {
            if (state == nullptr) return;
            PatchConfig config;
            config.state = state;
            config.radiusMin = rMin;
            config.radiusMax = rMax;
            config.halfHeight = halfHeight;
            config.targetIds = std::move(targets);
            registerOwned(id, std::make_unique<ConfiguredFeatureImpl<PatchConfig, CheckAbovePatchFeature>>(
                &s_patchFeature, config));
        };
        BlockState* mycelium = resolveState("minecraft:mycelium", PropertyMap{{"snowy", "false"}}, "mycelium blobs");
        BlockState* dirt = resolveState("minecraft:dirt", "troll_cave_dirt");
        BlockState* uberous = resolveState("twilightforest:uberous_soil", "uberous_soil_patch");
        // matching_blocks ["minecraft:stone", "twilightforest:deadrock"]: the
        // deadrock entry only while the real block is registered.
        std::vector<std::string> caveTargets = {"minecraft:stone"};
        const std::string deadrock = realTwilightId("twilightforest:deadrock");
        if (!deadrock.empty()) caveTargets.push_back(deadrock);
        const std::vector<std::string> soilTargets = {"minecraft:podzol", "minecraft:coarse_dirt", "minecraft:dirt"};

        patch("twilightforest:mycelium_blob", mycelium, 4, 6, 3, {"minecraft:grass_block"});
        patch("twilightforest:troll_cave_dirt", dirt, 2, 5, 0, caveTargets);
        patch("twilightforest:troll_cave_mycelium", mycelium, 3, 5, 0, caveTargets);
        patch("twilightforest:uberous_soil_patch_big", uberous, 4, 8, 1, soilTargets);
        patch("twilightforest:uberous_soil_patch_small", uberous, 2, 3, 0, soilTargets);
    }

    // ------------------------------------------------------- outside_stalagmite
    // twilightforest:block_spike (BlockSpikeFeature.STONE_STALACTITE).
    if (twilight::stoneStalactite().singleOre != nullptr) {
        registerOwned("twilightforest:outside_stalagmite",
            std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, BlockSpikeFeature>>(
                &s_blockSpikeFeature, NoneFeatureConfiguration::INSTANCE));
    }

    // ------------------------------------------------------- templates
    TwilightTemplateFeatures::bootstrap();

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
