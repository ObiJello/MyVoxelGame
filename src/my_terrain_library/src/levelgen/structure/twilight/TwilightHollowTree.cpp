#include "levelgen/structure/twilight/TwilightPieceBase.h"

#include "levelgen/structure/TwilightStructures.h"
#include "levelgen/structure/TwilightStructureData.h"
#include "levelgen/structure/OrientedPieceBehavior.h"
#include "data/worldgen/features/TwilightFeatures.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include "math/Mth.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/Block.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/VineBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Twilight Forest 4.9 hollow trees:
//   world/components/structures/type/HollowTreeStructure.java
//       (findGenerationPoint: the trunk position, height, radius, water and
//       biome checks)
//   world/components/structures/hollowtree/HollowTreePiece.java (shared
//       drawing helpers, the inter-chunk decoration random)
//   hollowtree/HollowTreeTrunk.java, HollowTreeMedBranch.java,
//   HollowTreeSmallBranch.java, HollowTreeLargeBranch.java,
//   HollowTreeRoot.java, HollowTreeLeafDungeon.java
//
// These pieces extend vanilla StructurePiece in the mod (not the legacy TF
// base), so they use OrientedPieceBehavior's vanilla orientation mapping.
// Every piece decorates from its own XoroshiroRandomSource seeded by the
// world seed and its bounding box, so the tree is identical whichever chunk
// draws which part of it; the structure random is only used for the layout.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_pieces {

using world::level::block::Blocks;
using world::level::block::VineBlock;
using world::level::block::state::properties::BlockStateProperties;
using core::Direction;
namespace tf = data::worldgen::features::twilight;

namespace {

void logWarning(const std::string& message) {
    fprintf(stderr, "[TwilightHollowTree] %s\n", message.c_str());
}

// Direction.values() order: DOWN, UP, NORTH, SOUTH, WEST, EAST.
constexpr Direction kAllDirections[6] = {
    Direction::DOWN, Direction::UP, Direction::NORTH, Direction::SOUTH, Direction::WEST, Direction::EAST};

// HollowTreePiece.PLACE_FLAG.
constexpr int kPlaceFlag = 0b10011;

// ---------------------------------------------------------------------------
// Data: IntProvider, BlockStateProvider (as the structure JSON writes them).
// ---------------------------------------------------------------------------

// IntProviders.codec(min, max): an int literal or constant / uniform /
// biased_to_bottom / clamped.
struct IntProviderSpec {
    enum class Kind { Constant, Uniform, BiasedToBottom, Clamped };
    Kind kind = Kind::Constant;
    int32_t value = 0;
    int32_t minInclusive = 0;
    int32_t maxInclusive = 0;
    std::shared_ptr<IntProviderSpec> source;  // clamped

    int32_t sample(LegacyRandomSource& random) const {
        switch (kind) {
            case Kind::Uniform:
                // Mth.randomBetweenInclusive.
                return random.nextInt(maxInclusive - minInclusive + 1) + minInclusive;
            case Kind::BiasedToBottom: {
                const int32_t bound = random.nextInt(maxInclusive - minInclusive + 1) + 1;
                return minInclusive + random.nextInt(bound);
            }
            case Kind::Clamped:
                return std::clamp(source->sample(random), minInclusive, maxInclusive);
            default:
                return value;
        }
    }

    static IntProviderSpec parse(const nlohmann::json& json) {
        IntProviderSpec spec;
        if (json.is_number_integer()) {
            spec.value = json.get<int32_t>();
            return spec;
        }
        const std::string type = json.value("type", std::string("minecraft:constant"));
        if (type == "minecraft:uniform") {
            spec.kind = Kind::Uniform;
            spec.minInclusive = json.at("min_inclusive").get<int32_t>();
            spec.maxInclusive = json.at("max_inclusive").get<int32_t>();
        } else if (type == "minecraft:biased_to_bottom") {
            spec.kind = Kind::BiasedToBottom;
            spec.minInclusive = json.at("min_inclusive").get<int32_t>();
            spec.maxInclusive = json.at("max_inclusive").get<int32_t>();
        } else if (type == "minecraft:clamped") {
            spec.kind = Kind::Clamped;
            spec.minInclusive = json.at("min_inclusive").get<int32_t>();
            spec.maxInclusive = json.at("max_inclusive").get<int32_t>();
            spec.source = std::make_shared<IntProviderSpec>(parse(json.at("source")));
        } else if (type == "minecraft:constant") {
            spec.value = json.at("value").get<int32_t>();
        } else {
            throw std::runtime_error("unsupported int provider " + type);
        }
        return spec;
    }
};

// A BlockState written as {Name, Properties}, resolved through TwilightBlocks.
BlockState* parseBlockState(const nlohmann::json& json) {
    const std::string name = json.at("Name").get<std::string>();
    if (!json.contains("Properties")) return twilight_blocks::defaultState(name);
    std::unordered_map<std::string, std::string> properties;
    for (auto it = json["Properties"].begin(); it != json["Properties"].end(); ++it) {
        properties[it.key()] = it.value().get<std::string>();
    }
    return twilight_blocks::state(name, properties);
}

// BlockStateProvider: simple_state_provider (no draw), weighted_state_provider
// (WeightedList.getRandomOrThrow: one nextInt(total)), rotated_block_provider
// (Direction.Axis.getRandom: one nextInt(3)).
struct StateProvider {
    enum class Kind { Simple, Weighted, Rotated };
    Kind kind = Kind::Simple;
    std::vector<std::pair<BlockState*, int32_t>> entries;
    int32_t totalWeight = 0;

    BlockState* getState(XoroshiroRandomSource& random) const {
        switch (kind) {
            case Kind::Weighted: {
                if (totalWeight <= 0) return nullptr;
                int32_t selection = random.nextInt(totalWeight);
                for (const auto& entry : entries) {
                    selection -= entry.second;
                    if (selection < 0) return entry.first;
                }
                return entries.back().first;
            }
            case Kind::Rotated: {
                static const core::Axis kAxes[3] = {core::Axis::X, core::Axis::Y, core::Axis::Z};
                const core::Axis axis = kAxes[random.nextInt(3)];
                BlockState* state = entries.empty() ? nullptr : entries.front().first;
                return state == nullptr ? nullptr : state->trySetValue(*BlockStateProperties::AXIS, axis);
            }
            default:
                return entries.empty() ? nullptr : entries.front().first;
        }
    }

    static StateProvider parse(const nlohmann::json& json) {
        StateProvider provider;
        const std::string type = json.value("type", std::string("minecraft:simple_state_provider"));
        if (type == "minecraft:simple_state_provider") {
            provider.entries.emplace_back(parseBlockState(json.at("state")), 1);
        } else if (type == "minecraft:rotated_block_provider") {
            provider.kind = Kind::Rotated;
            provider.entries.emplace_back(parseBlockState(json.at("state")), 1);
        } else if (type == "minecraft:weighted_state_provider") {
            provider.kind = Kind::Weighted;
            for (const nlohmann::json& entry : json.at("entries")) {
                const int32_t weight = entry.value("weight", 1);
                provider.entries.emplace_back(parseBlockState(entry.at("data")), weight);
                provider.totalWeight += weight;
            }
        } else {
            throw std::runtime_error("unsupported block state provider " + type);
        }
        return provider;
    }
};

// HollowTreeStructure's codec fields.
struct HollowTreeConfig {
    IntProviderSpec height;
    IntProviderSpec radius;
    StateProvider log;
    StateProvider wood;
    StateProvider root;
    StateProvider leaves;
    StateProvider vine;
    StateProvider bug;
    StateProvider dungeonWood;
    StateProvider dungeonAir;
    StateProvider dungeonLootBlock;
    std::string dungeonLootTable;
    std::string dungeonMonster;
    bool allowInWater = false;
};

std::shared_ptr<const HollowTreeConfig> hollowTreeConfig(const std::string& structureName) {
    static std::mutex s_mutex;
    static std::map<std::string, std::shared_ptr<const HollowTreeConfig>> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(structureName);
    if (it != s_cache.end()) return it->second;

    const nlohmann::json& json = twilight_data::structure(structureName);
    auto config = std::make_shared<HollowTreeConfig>();
    config->height = IntProviderSpec::parse(json.at("height"));
    config->radius = IntProviderSpec::parse(json.at("radius"));
    config->log = StateProvider::parse(json.at("log"));
    config->wood = StateProvider::parse(json.at("wood"));
    config->root = StateProvider::parse(json.at("root"));
    config->leaves = StateProvider::parse(json.at("leaves"));
    config->vine = StateProvider::parse(json.at("vine"));
    config->bug = StateProvider::parse(json.at("bug"));
    config->dungeonWood = StateProvider::parse(json.at("dungeon_wood"));
    config->dungeonAir = StateProvider::parse(json.at("dungeon_air"));
    config->dungeonLootBlock = StateProvider::parse(json.at("dungeon_loot_block"));
    config->dungeonLootTable = json.at("dungeon_loot_table").get<std::string>();
    config->dungeonMonster = json.at("dungeon_monster").get<std::string>();
    config->allowInWater = json.value("allow_in_water", false);
    s_cache.emplace(structureName, config);
    return config;
}

// FeatureLogic.treesReplaceable.
bool treesReplaceable(BlockState* state) {
    return tf::isReplaceable(state, true);
}

int32_t distManhattan(const core::BlockPos& a, const core::BlockPos& b) {
    return std::abs(a.getX() - b.getX()) + std::abs(a.getY() - b.getY()) + std::abs(a.getZ() - b.getZ());
}

// BoundingBox.fromCorners(a, b).inflatedBy(n).
BoundingBox branchBoundingBox(const core::BlockPos& src, const core::BlockPos& dest, int32_t extraPadding) {
    return BoundingBox(std::min(src.getX(), dest.getX()) - extraPadding,
                       std::min(src.getY(), dest.getY()) - extraPadding,
                       std::min(src.getZ(), dest.getZ()) - extraPadding,
                       std::max(src.getX(), dest.getX()) + extraPadding,
                       std::max(src.getY(), dest.getY()) + extraPadding,
                       std::max(src.getZ(), dest.getZ()) + extraPadding);
}

// Vanilla StructurePiece.setOrientation's rotation (SOUTH mirrors instead).
int vanillaRotationOrdinal(Direction orientation) {
    return (orientation == Direction::WEST || orientation == Direction::EAST)
        ? OrientedPieceBehavior::ROT_CW90 : OrientedPieceBehavior::ROT_NONE;
}

// ===========================================================================
// HollowTreePiece
// ===========================================================================
class HollowTreePiece : public OrientedPieceBehavior {
public:
    HollowTreePiece(Direction orientation, std::shared_ptr<const HollowTreeConfig> config)
        : OrientedPieceBehavior(static_cast<int>(orientation)), m_config(std::move(config)) {}

protected:
    std::shared_ptr<const HollowTreeConfig> m_config;

    // getInterChunkDecoRNG: new XoroshiroRandomSource(seed + minX * 321534781L
    // ^ minZ * 756839L) — Java precedence: (seed + minX * k1) ^ (minZ * k2).
    XoroshiroRandomSource getInterChunkDecoRNG(WorldGenLevel* level) const {
        const BoundingBox& box = m_self->boundingBox;
        const uint64_t sum = static_cast<uint64_t>(level->getSeed())
            + static_cast<uint64_t>(static_cast<int64_t>(box.minX) * 321534781LL);
        const uint64_t mixed = sum ^ static_cast<uint64_t>(static_cast<int64_t>(box.minZ) * 756839LL);
        return XoroshiroRandomSource(static_cast<int64_t>(mixed));
    }

    // placeProvidedBlock: the state is drawn only after the bounds and
    // replaceability tests; leaves get a distance (1 with leafHack).
    void placeProvidedBlock(WorldGenLevel* level, const StateProvider& provider, XoroshiroRandomSource& random,
                            int sx, int sy, int sz, const BoundingBox& chunkBB, const core::BlockPos& origin,
                            bool forcedPlace, bool leafHack) const {
        const core::BlockPos pos = worldPos(sx, sy, sz);
        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
        if (!forcedPlace && !treesReplaceable(level->getBlockState(pos))) return;
        BlockState* state = provider.getState(random);
        if (state == nullptr) return;
        if (state->hasProperty(BlockStateProperties::DISTANCE)) {
            const int distance = leafHack ? 1 : std::clamp(distManhattan(origin, pos), 1, 7);
            level->setBlock(pos, state->setValue(*BlockStateProperties::DISTANCE, distance), kPlaceFlag);
        } else {
            level->setBlock(pos, state, kPlaceFlag);
        }
    }

    // StructurePiece.fillColumnDown with a provider.
    void fillColumnDown(WorldGenLevel* level, const StateProvider& provider, XoroshiroRandomSource& random,
                        int sx, int sy, int sz, const BoundingBox& chunkBB) const {
        core::BlockPos pos = worldPos(sx, sy, sz);
        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
        while (isReplaceableByStructures(level->getBlockState(pos)) && pos.getY() > level->getMinY() + 1) {
            BlockState* state = provider.getState(random);
            if (state != nullptr) level->setBlock(pos, state, kPlaceFlag);
            pos = pos.below();
        }
    }

    // fillVineColumnDown: vines facing `direction`, down through non-fluid
    // replaceable blocks.
    void fillVineColumnDown(WorldGenLevel* level, const StateProvider& provider, XoroshiroRandomSource& random,
                            int sx, int sy, int sz, const BoundingBox& chunkBB, Direction direction) const {
        core::BlockPos pos = worldPos(sx, sy, sz);
        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
        auto nonFluidAndReplaceable = [&](const core::BlockPos& at) {
            BlockState* here = level->getBlockState(at);
            return !here->hasAnyFluid() && isReplaceableByStructures(here);
        };
        while (nonFluidAndReplaceable(pos) && pos.getY() > level->getMinY() + 1) {
            BlockState* state = provider.getState(random);
            if (state != nullptr) {
                const auto* face = VineBlock::getPropertyForFace(direction);
                if (face != nullptr) state = state->trySetValue(*face, true);
                level->setBlock(pos, state, kPlaceFlag);
            }
            pos = pos.below();
        }
    }

    // drawBresehnam: a voxel line of provider blocks through replaceable cells.
    void drawBresenham(WorldGenLevel* level, const BoundingBox& chunkBB, const core::BlockPos& start,
                       const core::BlockPos& end, const StateProvider& provider,
                       XoroshiroRandomSource& random) const {
        tf::VoxelBresenhamIterator line(start, end);
        while (line.hasNext()) {
            const core::BlockPos pos = line.next();
            if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ()) && treesReplaceable(level->getBlockState(pos))) {
                BlockState* state = provider.getState(random);
                if (state != nullptr) level->setBlock(pos, state, kPlaceFlag);
            }
        }
    }

    // drawBlockBlob: an octant-mirrored blob; `imperfect` roughens the rim.
    void drawBlockBlob(WorldGenLevel* level, const BoundingBox& chunkBB, int sx, int sy, int sz, int blobRadius,
                       XoroshiroRandomSource& random, const StateProvider& provider, bool forcedPlace,
                       bool leafHack, bool imperfect) const {
        const core::BlockPos origin = worldPos(sx, sy, sz);
        auto place = [&](int x, int y, int z) {
            placeProvidedBlock(level, provider, random, x, y, z, chunkBB, origin, forcedPlace, leafHack);
        };
        for (int dx = 0; dx <= blobRadius; ++dx) {
            for (int dy = 0; dy <= blobRadius; ++dy) {
                for (int dz = 0; dz <= blobRadius; ++dz) {
                    // How far we are from the centre.
                    int dist;
                    if (dx >= dy && dx >= dz) {
                        dist = dx + static_cast<int>(std::max(dy, dz) * 0.5 + std::min(dy, dz) * 0.25);
                    } else if (dy >= dx && dy >= dz) {
                        dist = dy + static_cast<int>(std::max(dx, dz) * 0.5 + std::min(dx, dz) * 0.25);
                    } else {
                        dist = dz + static_cast<int>(std::max(dx, dy) * 0.5 + std::min(dx, dy) * 0.25);
                    }
                    if (dist > blobRadius) continue;

                    if (imperfect && dist == blobRadius) {
                        // No cubes allowed!
                        if (dx == dy && dy == dz) continue;
                        // Randomly skip some blocks on the very edges.
                        if ((dx == dy && dz > dx && dx > 0) || (dy == dz && dx > dy && dy > 0)
                            || (dz == dx && dy > dz && dz > 0)) {
                            if (random.nextInt(2) == 0) place(sx + dx, sy + dy, sz + dz);
                            if (random.nextInt(2) == 0) place(sx + dx, sy + dy, sz - dz);
                            if (random.nextInt(2) == 0) place(sx - dx, sy + dy, sz + dz);
                            if (random.nextInt(2) == 0) place(sx - dx, sy + dy, sz - dz);
                            if (random.nextInt(2) == 0) place(sx + dx, sy - dy, sz + dz);
                            if (random.nextInt(2) == 0) place(sx + dx, sy - dy, sz - dz);
                            if (random.nextInt(2) == 0) place(sx - dx, sy - dy, sz + dz);
                            if (random.nextInt(2) == 0) place(sx - dx, sy - dy, sz - dz);
                            continue;
                        }
                    }

                    // Eight at a time.
                    place(sx + dx, sy + dy, sz + dz);
                    place(sx + dx, sy + dy, sz - dz);
                    place(sx - dx, sy + dy, sz + dz);
                    place(sx - dx, sy + dy, sz - dz);
                    place(sx + dx, sy - dy, sz + dz);
                    place(sx + dx, sy - dy, sz - dz);
                    place(sx - dx, sy - dy, sz + dz);
                    place(sx - dx, sy - dy, sz - dz);
                }
            }
        }
    }
};

// ===========================================================================
// HollowTreeTrunk
// ===========================================================================
class HollowTreeTrunk final : public HollowTreePiece {
public:
    HollowTreeTrunk(int height, int radius, std::shared_ptr<const HollowTreeConfig> config)
        : HollowTreePiece(Direction::SOUTH, std::move(config)), m_height(height), m_radius(radius) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& doNotUse,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator;
        (void)doNotUse;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        XoroshiroRandomSource decoRNG = getInterChunkDecoRNG(level);
        const HollowTreeConfig& config = *m_config;

        const int hollow = m_radius / 2;
        const Direction vineDirection = core::fromHorizontalIndex(decoRNG.nextInt(4));
        const bool vineAxisX = core::getAxis(vineDirection) == core::Axis::X;
        const core::BlockPos zero(0, 0, 0);

        for (int dx = 0; dx <= 2 * m_radius; ++dx) {
            for (int dz = 0; dz <= 2 * m_radius; ++dz) {
                // How far we are from the centre.
                const int ax = std::abs(dx - m_radius);
                const int az = std::abs(dz - m_radius);
                const int dist = static_cast<int>(std::max(ax, az) + std::min(ax, az) * 0.5);

                for (int dy = 0; dy <= m_height; ++dy) {
                    // The body of the trunk (offset: the box is one wider).
                    if (dist <= m_radius && dist > hollow) {
                        placeProvidedBlock(level, config.log, decoRNG, dx + 1, dy, dz + 1, chunkBB, zero, false, false);
                    }
                }

                // Down to the ground.
                if (dist <= m_radius) {
                    fillColumnDown(level, config.log, decoRNG, dx + 1, -1, dz + 1, chunkBB);
                }

                // Vines on one side of the hollow.
                if (dist == hollow
                    && (vineAxisX ? (dx == m_radius + hollow * core::getStepX(vineDirection))
                                  : (dz == m_radius + hollow * core::getStepZ(vineDirection)))) {
                    fillVineColumnDown(level, config.vine, decoRNG, dx + 1, m_height, dz + 1, chunkBB, vineDirection);
                }
            }
        }

        // Fireflies and cicadas.
        const int insectsA = decoRNG.nextInt(3 * m_radius);
        const int insectsB = decoRNG.nextInt(3 * m_radius);
        const int numInsects = insectsA + insectsB + 10;
        for (int i = 0; i <= numInsects; ++i) {
            const int fHeight = static_cast<int>(m_height * decoRNG.nextDouble() * 0.9) + (m_height / 10);
            const double fAngle = decoRNG.nextDouble();
            addInsect(level, decoRNG, fHeight, fAngle, chunkBB);
        }
    }

private:
    int m_height;
    int m_radius;

    void addInsect(WorldGenLevel* level, XoroshiroRandomSource& random, int fHeight, double fAngle,
                   const BoundingBox& chunkBB) const {
        const core::BlockPos bugSpot =
            tf::translate(core::BlockPos(m_radius + 1, fHeight, m_radius + 1), m_radius + 1, fAngle, 0.5);
        const int ox = worldX(bugSpot.getX(), bugSpot.getZ());
        const int oy = worldY(bugSpot.getY());
        const int oz = worldZ(bugSpot.getX(), bugSpot.getZ());
        if (!chunkBB.isInside(ox, oy, oz)) return;
        const core::BlockPos src(ox, oy, oz);

        const double fAngleWrapped = std::fmod(fAngle, 1.0);
        int facing = ROT_CW90;
        if (fAngleWrapped > 0.875 || fAngleWrapped <= 0.125) {
            facing = ROT_CW180;
        } else if (fAngleWrapped > 0.375 && fAngleWrapped <= 0.625) {
            facing = ROT_NONE;
        } else if (fAngleWrapped > 0.625) {
            facing = ROT_CCW90;
        }

        BlockState* decor = m_config->bug.getState(random);
        if (decor == nullptr) return;
        decor = state_transforms::rotateState(decor, facing);
        if (level->getBlockState(src)->canBeReplaced() && critterCanSurvive(level, decor, src)) {
            level->setBlock(src, decor, 3);
        }
    }

    // CritterBlock.canSurvive: the block behind it supports its centre, or is
    // leaves. Non-critter bug blocks use their own canSurvive.
    static bool critterCanSurvive(WorldGenLevel* level, BlockState* state, const core::BlockPos& pos) {
        if (!state->hasProperty(BlockStateProperties::FACING)) return state->canSurvive(*level, pos);
        const Direction facing = state->getValue(*BlockStateProperties::FACING);
        const core::BlockPos restingPos = pos.relative(core::getOpposite(facing));
        return world::level::block::Block::canSupportCenter(*level, restingPos, facing)
            || level->getBlockState(restingPos)->isLeaves();
    }
};

// ===========================================================================
// HollowTreeMedBranch (also the base of the small and large branches and
// the roots)
// ===========================================================================
class HollowTreeMedBranch : public HollowTreePiece {
public:
    HollowTreeMedBranch(const core::BlockPos& src, const core::BlockPos& dest, double length, double angle,
                        double tilt, bool leafy, const StateProvider* wood, const StateProvider* leaves,
                        std::shared_ptr<const HollowTreeConfig> config)
        : HollowTreePiece(Direction::SOUTH, std::move(config))
        , m_src(src), m_dest(dest), m_length(length), m_angle(angle), m_tilt(tilt), m_leafy(leafy)
        , m_wood(wood), m_leaves(leaves) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& doNotUse,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator;
        (void)doNotUse;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        XoroshiroRandomSource decoRNG = getInterChunkDecoRNG(level);
        const BoundingBox& box = self.boundingBox;

        drawBresenham(level, chunkBB, m_src, m_dest, *m_wood, decoRNG);
        drawBresenham(level, chunkBB, m_src.above(), m_dest, *m_wood, decoRNG);

        // And several small branches.
        const int numShoots = std::min(decoRNG.nextInt(3) + 1, static_cast<int>(m_length / 5));
        if (numShoots > 0) {
            const double angleInc = 0.8 / numShoots;
            for (int i = 0; i < numShoots; ++i) {
                const double angleVar = (angleInc * i) - 0.4;
                const double outVar = (decoRNG.nextDouble() * 0.8) + 0.2;
                const core::BlockPos bSrc = tf::translate(m_src, m_length * outVar, m_angle, m_tilt);
                drawSmallBranch(level, chunkBB, bSrc, std::max(m_length * static_cast<double>(0.3f), 2.0),
                                m_angle + angleVar, m_tilt, decoRNG, *m_wood, *m_leaves);
            }
        }

        // With leaves!
        if (m_leafy) {
            const int numLeafBalls = std::min(decoRNG.nextInt(3) + 1, static_cast<int>(m_length / 5));
            for (int i = 0; i < numLeafBalls; ++i) {
                const double slength =
                    static_cast<double>(decoRNG.nextFloat() * 0.6f + 0.2f) * m_length;
                const core::BlockPos local(m_src.getX() - box.minX, m_src.getY() - box.minY, m_src.getZ() - box.minZ);
                const core::BlockPos bdst = tf::translate(local, slength, m_angle, m_tilt);
                const int radius = decoRNG.nextBoolean() ? 2 : 3;
                drawBlockBlob(level, chunkBB, bdst.getX(), bdst.getY(), bdst.getZ(), radius, decoRNG, *m_leaves,
                              false, false, true);
            }
            drawBlockBlob(level, chunkBB, m_dest.getX() - box.minX, m_dest.getY() - box.minY,
                          m_dest.getZ() - box.minZ, 3, decoRNG, *m_leaves, false, false, true);
        }
    }

protected:
    core::BlockPos m_src;
    core::BlockPos m_dest;
    double m_length;
    double m_angle;
    double m_tilt;
    bool m_leafy;
    const StateProvider* m_wood;    // points into m_config
    const StateProvider* m_leaves;  // points into m_config (root block for roots)

    // drawSmallBranch: drawn straight into the world, leaf blob at its end.
    void drawSmallBranch(WorldGenLevel* level, const BoundingBox& chunkBB, const core::BlockPos& sourcePos,
                         double branchLength, double branchAngle, double branchTilt,
                         XoroshiroRandomSource& random, const StateProvider& woodProvider,
                         const StateProvider& leafProvider) const {
        const BoundingBox& box = m_self->boundingBox;
        const core::BlockPos branchDest = tf::translate(sourcePos, branchLength, branchAngle, branchTilt);
        drawBresenham(level, chunkBB, sourcePos, branchDest, woodProvider, random);
        drawBlockBlob(level, chunkBB, branchDest.getX() - box.minX, branchDest.getY() - box.minY,
                      branchDest.getZ() - box.minZ, 2, random, leafProvider, false, false, true);
    }
};

class HollowTreeSmallBranch final : public HollowTreeMedBranch {
public:
    using HollowTreeMedBranch::HollowTreeMedBranch;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& doNotUse,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator;
        (void)doNotUse;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        XoroshiroRandomSource decoRNG = getInterChunkDecoRNG(level);
        const BoundingBox& box = self.boundingBox;

        drawBresenham(level, chunkBB, m_src, m_dest, *m_wood, decoRNG);

        // With leaves!
        if (m_leafy) {
            const int leafRad = decoRNG.nextInt(2) + 1;
            drawBlockBlob(level, chunkBB, m_dest.getX() - box.minX, m_dest.getY() - box.minY,
                          m_dest.getZ() - box.minZ, leafRad, decoRNG, *m_leaves, false, false, true);
        }
    }
};

class HollowTreeLargeBranch final : public HollowTreeMedBranch {
public:
    HollowTreeLargeBranch(const core::BlockPos& src, const core::BlockPos& dest, double length, double angle,
                          double tilt, bool leafy, bool hasLeafDungeon, const StateProvider* wood,
                          const StateProvider* leaves, std::shared_ptr<const HollowTreeConfig> config)
        : HollowTreeMedBranch(src, dest, length, angle, tilt, leafy, wood, leaves, std::move(config))
        , m_hasLeafDungeon(hasLeafDungeon) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& doNotUse,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator;
        (void)doNotUse;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        XoroshiroRandomSource decoRNG = getInterChunkDecoRNG(level);
        const BoundingBox& box = self.boundingBox;

        // Main branch.
        drawBresenham(level, chunkBB, m_src, m_dest, *m_wood, decoRNG);

        // Reinforce it.
        const int reinforcements = 4;
        for (int i = 0; i <= reinforcements; ++i) {
            const int vx = (i & 2) == 0 ? 1 : 0;
            const int vy = (i & 1) == 0 ? 1 : -1;
            const int vz = (i & 2) == 0 ? 0 : 1;
            drawBresenham(level, chunkBB, m_src.offset(vx, vy, vz), m_dest, *m_wood, decoRNG);
        }

        // 1-2 small branches near the base.
        const int numSmallBranches = decoRNG.nextInt(2) + 1;
        for (int i = 0; i <= numSmallBranches; ++i) {
            const double outVar = static_cast<double>((decoRNG.nextFloat() * 0.25f) + 0.25f);
            const double angleVar = static_cast<double>(decoRNG.nextFloat() * 0.25f * ((i & 1) == 0 ? 1.0f : -1.0f));
            const core::BlockPos bsrc = tf::translate(m_src, m_length * outVar, m_angle, m_tilt);
            drawSmallBranch(level, chunkBB, bsrc, std::max(m_length * static_cast<double>(0.3f), 2.0),
                            m_angle + angleVar, m_tilt, decoRNG, *m_wood, *m_leaves);
        }

        if (m_leafy && !m_hasLeafDungeon) {
            // Leaf blob at the end.
            drawBlockBlob(level, chunkBB, m_dest.getX() - box.minX, m_dest.getY() - box.minY,
                          m_dest.getZ() - box.minZ, 3, decoRNG, *m_leaves, false, false, true);
        }
    }

private:
    bool m_hasLeafDungeon;
};

class HollowTreeRoot final : public HollowTreeMedBranch {
public:
    // HollowTreeRoot passes (wood, root) as (wood, leaves).
    using HollowTreeMedBranch::HollowTreeMedBranch;

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& doNotUse,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator;
        (void)doNotUse;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        XoroshiroRandomSource decoRNG = getInterChunkDecoRNG(level);
        const BoundingBox& box = self.boundingBox;

        const core::BlockPos rSrc(m_src.getX() - box.minX, m_src.getY() - box.minY, m_src.getZ() - box.minZ);
        const core::BlockPos rDest(m_dest.getX() - box.minX, m_dest.getY() - box.minY, m_dest.getZ() - box.minZ);

        drawRootLine(level, chunkBB, rSrc, rDest, decoRNG, *m_wood, *m_leaves);
        drawRootLine(level, chunkBB, rSrc.below(), rDest.below(), decoRNG, *m_wood, *m_leaves);
    }

private:
    // Exposed cells become wood, buried ones root; existing logs stay.
    void drawRootLine(WorldGenLevel* level, const BoundingBox& chunkBB, const core::BlockPos& rSrc,
                      const core::BlockPos& rDest, XoroshiroRandomSource& random, const StateProvider& wood,
                      const StateProvider& root) const {
        const core::BlockPos zero(0, 0, 0);
        tf::VoxelBresenhamIterator line(rSrc, rDest);
        while (line.hasNext()) {
            const core::BlockPos coords = line.next();
            const core::BlockPos pos = worldPos(coords.getX(), coords.getY(), coords.getZ());
            if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) continue;

            BlockState* block = level->getBlockState(pos);
            if (block->isLog()) continue;  // #minecraft:logs: wood, do nothing

            bool exposed = false;
            for (Direction direction : kAllDirections) {
                if (level->getBlockState(pos.relative(direction))->canBeReplaced()) {
                    exposed = true;
                    break;
                }
            }
            placeProvidedBlock(level, exposed ? wood : root, random, coords.getX(), coords.getY(), coords.getZ(),
                               chunkBB, zero, false, false);
        }
    }
};

// ===========================================================================
// HollowTreeLeafDungeon
// ===========================================================================
class HollowTreeLeafDungeon final : public HollowTreePiece {
public:
    HollowTreeLeafDungeon(Direction orientation, int radius, std::shared_ptr<const HollowTreeConfig> config)
        : HollowTreePiece(orientation, std::move(config)), m_radius(radius) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& doNotUse,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)generator;
        (void)doNotUse;
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        XoroshiroRandomSource decoRNG = getInterChunkDecoRNG(level);
        const HollowTreeConfig& config = *m_config;

        // Leaves on the outside, then wood, then air.
        drawBlockBlob(level, chunkBB, m_radius, m_radius, m_radius, 5, decoRNG, config.leaves, false, true, true);
        drawBlockBlob(level, chunkBB, m_radius, m_radius, m_radius, 3, decoRNG, config.dungeonWood, false, false, false);
        drawBlockBlob(level, chunkBB, m_radius, m_radius, m_radius, 2, decoRNG, config.dungeonAir, true, false, true);

        // Then the treasure chest, then the spawner.
        placeTreasureAtCurrentPosition(level, m_radius, m_radius - 1, m_radius, chunkBB, decoRNG);
        placeSpawnerAtCurrentPosition(level, m_radius, m_radius, m_radius, chunkBB);
    }

private:
    int m_radius;

    void placeTreasureAtCurrentPosition(WorldGenLevel* level, int x, int y, int z, const BoundingBox& chunkBB,
                                        XoroshiroRandomSource& random) const {
        static const Direction kDirections[4] = {Direction::NORTH, Direction::EAST, Direction::SOUTH, Direction::WEST};
        const Direction direction = kDirections[random.nextInt(4)];
        const core::BlockPos pos = worldPos(x, y, z).relative(direction, 2);

        BlockState* state = m_config->dungeonLootBlock.getState(random);
        if (state == nullptr) return;
        state = mirrorRotate(state);
        const std::string& id = state->getIdentifier();
        if (id == "minecraft:chest" || id == "minecraft:trapped_chest") {
            state = state->trySetValue(*BlockStateProperties::HORIZONTAL_FACING, core::getOpposite(direction));
        }

        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
        if (level->getBlockState(pos)->is(state)) return;
        level->setBlock(pos, state, 2);
        const std::string blockEntityId = tf_common::lootContainerBlockEntityId(state);
        if (!blockEntityId.empty()) {
            // RandomizableContainerBlockEntity.setLootTable(table, random.nextLong()).
            const int64_t seed = random.nextLong();
            tf_common::writeLootTable(level, pos, blockEntityId, m_config->dungeonLootTable, seed);
        }
    }

    void placeSpawnerAtCurrentPosition(WorldGenLevel* level, int x, int y, int z, const BoundingBox& chunkBB) const {
        const core::BlockPos pos = worldPos(x, y, z);
        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
        if (level->getBlockState(pos)->is(Blocks::SPAWNER)) return;
        level->setBlock(pos, Blocks::SPAWNER->defaultBlockState(), 2);
        // SpawnerBlockEntity.setEntityId(monster, rand): no draw.
        tf_common::writeSpawnerData(level, pos, m_config->dungeonMonster);
    }
};

// ===========================================================================
// Layout: HollowTreeStructure's generation stub + every addChildren.
// ===========================================================================
class HollowTreeLayout {
public:
    HollowTreeLayout(std::shared_ptr<const HollowTreeConfig> config, LegacyRandomSource& random,
                     StructureStartData& out)
        : m_config(std::move(config)), m_random(random), m_out(out) {}

    // new HollowTreeTrunk(...); addPiece; addChildren.
    void build(int height, int radius, const BoundingBox& box) {
        m_height = height;
        m_radius = radius;
        m_box = box;
        addPiece("twilightforest:tfhttr", box, Direction::SOUTH, 0,
                 std::make_shared<HollowTreeTrunk>(height, radius, m_config));
        trunkAddChildren();
    }

private:
    std::shared_ptr<const HollowTreeConfig> m_config;
    LegacyRandomSource& m_random;
    StructureStartData& m_out;
    int m_height = 0;
    int m_radius = 0;
    BoundingBox m_box;

    void addPiece(const char* type, const BoundingBox& box, Direction orientation, int genDepth,
                  std::shared_ptr<StructurePieceBehavior> behavior) {
        StructurePieceData piece;
        piece.pieceType = type;
        piece.boundingBox = box;
        piece.rotation = tf_common::rotationName(vanillaRotationOrdinal(orientation));
        piece.genDepth = genDepth;
        m_out.pieces.push_back(std::move(piece));
        m_out.behaviors.push_back(std::move(behavior));
    }

    // HollowTreeTrunk.addChildren.
    void trunkAddChildren() {
        const int index = 0;  // getGenDepth()

        // 3-5 couple branches on the way up...
        const int numBranches = m_random.nextInt(3) + 3;
        for (int i = 0; i <= numBranches; ++i) {
            const int branchHeight = static_cast<int>(m_height * m_random.nextDouble() * 0.9) + (m_height / 10);
            const double branchRotation = m_random.nextDouble();
            makeSmallBranch(index + i + 1, branchHeight, 4, branchRotation, 0.35, true);
        }

        // The crown.
        buildFullCrown(index + numBranches + 1);

        // 3-5 roots at the bottom, then several more taproots.
        buildBranchRing(index, 4, 2, 6, 0.75, 3, 5, 3, false);
        buildBranchRing(index, 2, 2, 8, 0.9, 3, 5, 3, false);
    }

    void buildFullCrown(int index) {
        const int crownRadius = m_radius * 4 + 4;
        const int bvar = m_radius + 2;
        // 3-5 main branches at the bottom of the crown.
        index += buildBranchRing(index, m_height - crownRadius, 0, crownRadius, 0.35, bvar, bvar + 2, 2, true);
        // 3-5 medium branches at the crown middle.
        index += buildBranchRing(index, m_height - (crownRadius / 2), 0, crownRadius, 0.28, bvar, bvar + 2, 1, true);
        // 2-4 main branches at the crown top.
        index += buildBranchRing(index, m_height, 0, crownRadius, 0.15, 2, 4, 2, true);
        // 3-6 medium branches going straight up.
        index += buildBranchRing(index, m_height, 0, crownRadius / 2, 0.05, bvar, bvar + 2, 1, true);
    }

    // size 0 = small, 1 = medium, 2 = large, 3 = root.
    int buildBranchRing(int index, int branchHeight, int heightVar, int length, double tilt, int minBranches,
                        int maxBranches, int size, bool leafy) {
        const int numBranches = m_random.nextInt(maxBranches - minBranches + 1) + minBranches;
        const double branchRotation = 1.0 / numBranches;
        const double branchOffset = m_random.nextDouble();

        for (int i = 0; i <= numBranches; ++i) {
            int dHeight;
            if (heightVar > 0) {
                dHeight = branchHeight - heightVar + m_random.nextInt(2 * heightVar);
            } else {
                dHeight = branchHeight;
            }
            const double rotation = i * branchRotation + branchOffset;
            if (size == 2) {
                makeLargeBranch(index, dHeight, length, rotation, tilt, leafy);
            } else if (size == 1) {
                makeMedBranch(index, dHeight, length, rotation, tilt, leafy);
            } else if (size == 3) {
                makeRoot(index, dHeight, length, rotation, tilt);
            } else {
                makeSmallBranch(index, dHeight, length, rotation, tilt, leafy);
            }
        }
        return numBranches;
    }

    // HollowTreeTrunk.getBranchSrc.
    core::BlockPos getBranchSrc(int branchHeight, double branchRotation) const {
        const int sx = m_box.minX + m_radius + 1;
        const int sy = m_box.minY + branchHeight;
        const int sz = m_box.minZ + m_radius + 1;
        return tf::translate(core::BlockPos(sx, sy, sz), m_radius, branchRotation, 0.5);
    }

    // new HollowTreeSmallBranch(index, src, length, angle, tilt, leafy, wood, leaves).
    void makeSmallBranch(int index, int branchHeight, int branchLength, double branchRotation, double branchAngle,
                         bool leafy) {
        const core::BlockPos src = getBranchSrc(branchHeight, branchRotation);
        const double length = branchLength;
        const core::BlockPos dest = tf::translate(src, length, branchRotation, branchAngle);
        addPiece("twilightforest:tfhtsb",
                 branchBoundingBox(src, dest, 3 + static_cast<int>(std::ceil(length))), Direction::SOUTH, index,
                 std::make_shared<HollowTreeSmallBranch>(src, dest, length, branchRotation, branchAngle, leafy,
                                                         &m_config->wood, &m_config->leaves, m_config));
        // HollowTreeSmallBranch.addChildren: no-op.
    }

    void makeMedBranch(int index, int branchHeight, int branchLength, double branchRotation, double branchAngle,
                       bool leafy) {
        medBranchAt(index, getBranchSrc(branchHeight, branchRotation), branchLength, branchRotation, branchAngle,
                    leafy);
    }

    // new HollowTreeMedBranch(index, src, length, angle, tilt, leafy, wood, leaves).
    void medBranchAt(int index, const core::BlockPos& src, double length, double angle, double tilt, bool leafy) {
        const core::BlockPos dest = tf::translate(src, length, angle, tilt);
        addPiece("twilightforest:tfhtmb",
                 branchBoundingBox(src, dest, 3 + static_cast<int>(std::ceil(length))), Direction::SOUTH, index,
                 std::make_shared<HollowTreeMedBranch>(src, dest, length, angle, tilt, leafy, &m_config->wood,
                                                       &m_config->leaves, m_config));
        // HollowTreeMedBranch.addChildren: StructurePiece's no-op.
    }

    // new HollowTreeLargeBranch(index, src, length, angle, tilt, leafy, rand, ...).
    void makeLargeBranch(int index, int branchHeight, int branchLength, double branchRotation, double branchAngle,
                         bool leafy) {
        const core::BlockPos src = getBranchSrc(branchHeight, branchRotation);
        const double length = branchLength;
        const double angle = branchRotation;
        const double tilt = branchAngle;
        const core::BlockPos dest = tf::translate(src, length, angle, tilt);
        // Drawn in the constructor, before addPiece.
        const bool hasLeafDungeon = m_random.nextInt(8) == 0;  // LEAF_DUNGEON_CHANCE
        addPiece("twilightforest:tfhtlb",
                 branchBoundingBox(src, dest, 3 + static_cast<int>(std::ceil(length))), Direction::SOUTH, index,
                 std::make_shared<HollowTreeLargeBranch>(src, dest, length, angle, tilt, leafy, hasLeafDungeon,
                                                         &m_config->wood, &m_config->leaves, m_config));

        // HollowTreeLargeBranch.addChildren: medium branches about halfway
        // out, alternating sides; then the leaf dungeon at the tip.
        const int numMedBranches =
            m_random.nextInt(static_cast<int>(length / 6)) + static_cast<int>(length / 8);
        for (int i = 0; i <= numMedBranches; ++i) {
            const double outVar = (m_random.nextDouble() * 0.3) + 0.3;
            const double angleVar = m_random.nextDouble() * 0.225 * ((i & 1) == 0 ? 1.0 : -1.0);
            const core::BlockPos bsrc = tf::translate(src, length * outVar, angle, tilt);
            medBranchAt(index + 2 + i, bsrc, length * 0.6, angle + angleVar, tilt, leafy);
        }
        if (hasLeafDungeon) {
            makeLeafDungeon(index + 1, dest.getX(), dest.getY(), dest.getZ());
        }
    }

    // new HollowTreeLeafDungeon(index, x, y, z, 4, ..., rand).
    void makeLeafDungeon(int index, int x, int y, int z) {
        const int radius = 4;
        const BoundingBox box(x - radius, y - radius, z - radius, x + radius, y + radius, z + radius);
        // setOrientation(StructurePiece.getRandomHorizontalDirection(random)).
        const Direction orientation = core::horizontalRandom(m_random);
        addPiece("twilightforest:tfhtld", box, orientation, index,
                 std::make_shared<HollowTreeLeafDungeon>(orientation, radius, m_config));
        // addChildren: StructurePiece's no-op.
    }

    // new HollowTreeRoot(index, src, length, angle, tilt, false, root, wood).
    void makeRoot(int index, int branchHeight, int branchLength, double branchRotation, double branchAngle) {
        const core::BlockPos src = getBranchSrc(branchHeight, branchRotation);
        const double length = branchLength;
        const core::BlockPos dest = tf::translate(src, length, branchRotation, branchAngle);
        addPiece("twilightforest:tfhtro", branchBoundingBox(src, dest, 0), Direction::SOUTH, index,
                 std::make_shared<HollowTreeRoot>(src, dest, length, branchRotation, branchAngle, false,
                                                  &m_config->wood, &m_config->root, m_config));
    }
};

} // namespace

// ============================================================================
// HollowTreeStructure.findGenerationPoint (+ Structure.findValidGenerationPoint)
// ============================================================================
bool buildHollowTree(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out) {
    std::shared_ptr<const HollowTreeConfig> config;
    try {
        config = hollowTreeConfig(info.name);
    } catch (const std::exception& e) {
        logWarning(info.name + ": " + e.what());
        return false;
    }

    // RandomSource.create(seed + chunkX * 25117L + chunkZ * 151121L).
    const int64_t positionSeed = static_cast<int64_t>(
        static_cast<uint64_t>(ctx.seed)
        + static_cast<uint64_t>(static_cast<int64_t>(ctx.chunkX) * 25117LL)
        + static_cast<uint64_t>(static_cast<int64_t>(ctx.chunkZ) * 151121LL));
    LegacyRandomSource random(positionSeed);

    // SectionPos.sectionToBlockCoord(chunk, random.nextInt(16)), x first.
    const int32_t x = (ctx.chunkX << 4) + random.nextInt(16);
    const int32_t z = (ctx.chunkZ << 4) + random.nextInt(16);
    // ChunkGenerator.getFirstOccupiedHeight = getBaseHeight - 1.
    const int32_t seaFloorY =
        ctx.generator->getBaseHeight(x, z, Heightmap::Types::OCEAN_FLOOR_WG, ctx.randomState) - 1;
    const int32_t worldY =
        ctx.generator->getBaseHeight(x, z, Heightmap::Types::WORLD_SURFACE_WG, ctx.randomState) - 1;

    // heightAccessor.getMaxY() = minY + height - 1.
    const int32_t maxY = ctx.generator->getLevelMinY() + ctx.generator->getLevelHeight() - 1;
    const int32_t height = std::min(config->height.sample(random) + worldY, maxY) - worldY;

    if (height < 16 || (!config->allowInWater && seaFloorY < worldY)) return false;

    // biomes().contains(getNoiseBiome(x >> 2, worldY >> 2, z >> 2)); the
    // Structure.findValidGenerationPoint re-check at the stub (x, worldY, z)
    // samples the same quart and cannot differ.
    const world::biome::BiomeKey biome =
        ctx.biomeSource->getNoiseBiome(x >> 2, worldY >> 2, z >> 2, *ctx.sampler);
    if (ctx.validBiomes == nullptr || ctx.validBiomes->find(biome) == ctx.validBiomes->end()) return false;

    const int32_t radius = config->radius.sample(random);
    const BoundingBox boundingBox(x - (radius + 1), worldY, z - (radius + 1),
                                  x + radius + 1, worldY + height, z + radius + 1);

    // The stub's piece builder: trunk + addChildren with the context random.
    HollowTreeLayout layout(config, ctx.random, out);
    layout.build(height, radius, boundingBox);
    return !out.pieces.empty();
}

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
