#include "data/worldgen/features/TwilightFeatures.h"
#include "data/worldgen/features/TwilightFeatureRegistry.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "data/worldgen/features/VegetationFeatures.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/feature/configurations/TreeConfiguration.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/featuresize/FeatureSize.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/structure/templatesystem/RuleTest.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/Heightmap.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "synth/NormalNoise.h"
#include "math/Mth.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — data/twilightforest/worldgen/configured_feature/**.json
// and the Java classes named at each piece.
//
// Stand-ins for TF blocks that are not ported yet (pass one), each the
// nearest block the engine has:
//   twilightforest:canopy_wood      -> minecraft:canopy_log (axis y; the TF
//                                      "wood" is the log's bark on all faces)
//   twilightforest:dark_wood        -> minecraft:dark_log
//   twilightforest:mangrove_wood    -> minecraft:tf_mangrove_log
//   twilightforest:mangrove_root    -> minecraft:mangrove_roots (vanilla)
//   twilightforest:rainbow_oak_leaves  -> minecraft:twilight_oak_leaves
//   twilightforest:rainbow_oak_sapling -> minecraft:twilight_oak_sapling
//   twilightforest:firefly_jar / cicada_jar -> minecraft:lantern (hanging)
//   twilightforest:rope             -> minecraft:iron_chain (axis y)
// TF's own mangrove_log/leaves/sapling are ported as tf_mangrove_* (the
// vanilla names are taken), which is the same block, not a stand-in.
// Tags the library has no data for: #minecraft:substrate_overworld (26.x:
// #dirt + #mud + #moss_blocks + #grass_blocks) is read as #minecraft:dirt,
// whose 1.21 contents are exactly that union.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using namespace levelgen::placement;
using levelgen::feature::TreeFeature;
using levelgen::feature::configurations::TreeConfiguration;
using levelgen::feature::configurations::TreeConfigurationBuilder;
using levelgen::feature::trunkplacers::TrunkPlacer;
using levelgen::feature::trunkplacers::StraightTrunkPlacer;
using levelgen::feature::trunkplacers::FancyTrunkPlacer;
using levelgen::feature::trunkplacers::GiantTrunkPlacer;
using levelgen::feature::trunkplacers::LevelReader;
using levelgen::feature::trunkplacers::TrunkSetter;
using levelgen::feature::foliageplacers::FoliagePlacer;
using levelgen::feature::foliageplacers::FoliageAttachment;
using levelgen::feature::foliageplacers::FoliageSetter;
using levelgen::feature::foliageplacers::BlobFoliagePlacer;
using levelgen::feature::foliageplacers::FancyFoliagePlacer;
using levelgen::feature::foliageplacers::BushFoliagePlacer;
using levelgen::feature::foliageplacers::SpruceFoliagePlacer;
using levelgen::feature::foliageplacers::MegaPineFoliagePlacer;
using levelgen::feature::featuresize::TwoLayersFeatureSize;
using levelgen::feature::treedecorators::TreeDecorator;
using levelgen::feature::treedecorators::DecoratorContext;
using levelgen::feature::treedecorators::LeaveVineDecorator;
using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::SimpleStateProvider;
using levelgen::feature::stateproviders::WeightedStateProvider;
using levelgen::feature::stateproviders::WeightedStateEntry;
using levelgen::feature::stateproviders::NoiseProvider;
using levelgen::structure::templatesystem::TagMatchTest;
using levelgen::structure::templatesystem::RuleTest;
using levelgen::blockpredicates::matchesBlockTagName;

namespace {

constexpr int kFeatureFlags = 2;            // Feature.setBlock / Block.UPDATE_CLIENTS
constexpr int kKnownShapeFlags = 18;        // UPDATE_KNOWN_SHAPE | UPDATE_CLIENTS
constexpr float kTwoPi = 6.2831855f;        // Mth.TWO_PI

bool hasTag(BlockState* state, const char* tag) {
    return state != nullptr && matchesBlockTagName(state, tag);
}

bool isNamed(BlockState* state, const char* name) {
    return state != nullptr && state->getIdentifier() == name;
}

// FeatureLogic.IS_REPLACEABLE_AIR: canBeReplaced() || isAir()
bool isReplaceableAir(BlockState* state) {
    return state != nullptr && (state->canBeReplaced() || state->isAir());
}

bool isReplaceableAirAt(WorldGenLevel& level, const core::BlockPos& pos) {
    return isReplaceableAir(level.getBlockState(pos));
}

// #minecraft:logs (the engine's logs also carry the isLog flag).
bool isLogs(BlockState* state) {
    return state != nullptr && (state->isLog() || matchesBlockTagName(state, "minecraft:logs"));
}

// #twilightforest:tree_roots_skip = #logs, root, liveroot_block,
// mangrove_root, time_wood, #features_cannot_replace (FeatureLogic.ROOT_SHOULD_SKIP).
bool rootShouldSkip(BlockState* state) {
    if (state == nullptr) return false;
    return isLogs(state)
        || isNamed(state, "minecraft:root")
        || isNamed(state, "minecraft:liveroot_block")
        || isNamed(state, "minecraft:mangrove_roots")   // mangrove_root stand-in
        || hasTag(state, "minecraft:features_cannot_replace");
}

// FeatureLogic.hasEmptyNeighborExceptBelow
bool hasEmptyNeighborExceptBelow(WorldGenLevel& level, const core::BlockPos& pos) {
    return isReplaceableAirAt(level, pos.above())
        || isReplaceableAirAt(level, pos.north())
        || isReplaceableAirAt(level, pos.south())
        || isReplaceableAirAt(level, pos.west())
        || isReplaceableAirAt(level, pos.east());
}

// FeatureLogic.hasSolidNeighbor
bool hasSolidNeighbor(WorldGenLevel& level, const core::BlockPos& pos) {
    return !(isReplaceableAirAt(level, pos.below())
          && isReplaceableAirAt(level, pos.north())
          && isReplaceableAirAt(level, pos.south())
          && isReplaceableAirAt(level, pos.west())
          && isReplaceableAirAt(level, pos.east())
          && isReplaceableAirAt(level, pos.above()));
}

// FeatureLogic.canRootGrowIn
bool canRootGrowIn(WorldGenLevel& level, const core::BlockPos& pos) {
    BlockState* state = level.getBlockState(pos);
    if (isReplaceableAir(state)) {
        return hasSolidNeighbor(level, pos);
    }
    return twilight::isReplaceable(state, false);
}

// FeatureUtil.anyBelowMatch(pos, depth, predicate): pos.below(depth) .. pos.
template<typename Pred>
bool anyBelowMatch(const core::BlockPos& pos, int depth, Pred&& predicate) {
    for (int dy = -depth; dy <= 0; ++dy) {
        if (predicate(pos.offset(0, dy, 0))) return true;
    }
    return false;
}

using BlockSetter = std::function<void(const core::BlockPos&, BlockState*)>;

// FeaturePlacers.placeIfValidRootPos
bool placeIfValidRootPos(WorldGenLevel& level, const BlockSetter& setter, int penetrability,
                         WorldgenRandom& random, const core::BlockPos& pos,
                         const BlockStateProvider& provider) {
    if (!anyBelowMatch(pos, penetrability - 1,
                       [&level](const core::BlockPos& p) { return !canRootGrowIn(level, p); })) {
        setter(pos, provider.getState(random, pos));
        return true;
    }
    return false;
}

// FeaturePlacers.traceRoot
void traceRoot(WorldGenLevel& level, const BlockSetter& setter, int penetrability,
               WorldgenRandom& random, const BlockStateProvider& dirtRoot,
               twilight::VoxelBresenhamIterator& tracer) {
    while (tracer.hasNext()) {
        const core::BlockPos rootPos = tracer.next();
        if (anyBelowMatch(rootPos, penetrability - 1, [&level](const core::BlockPos& p) {
                return rootShouldSkip(level.getBlockState(p));
            })) {
            return;
        }
        if (!placeIfValidRootPos(level, setter, penetrability, random, rootPos, dirtRoot)) {
            return;
        }
    }
}

// FeaturePlacers.traceExposedRoot
void traceExposedRoot(WorldGenLevel& level, const BlockSetter& setter, int penetrability,
                      WorldgenRandom& random, const BlockStateProvider& exposedRoot,
                      const BlockStateProvider& dirtRoot, twilight::VoxelBresenhamIterator& tracer) {
    while (tracer.hasNext()) {
        const core::BlockPos exposedPos = tracer.next();
        if (rootShouldSkip(level.getBlockState(exposedPos))) {
            continue;
        }
        if (hasEmptyNeighborExceptBelow(level, exposedPos)) {
            // The predicate draws the exposed state per checked block, as Java.
            if (anyBelowMatch(exposedPos, penetrability - 1, [&](const core::BlockPos& p) {
                    BlockState* state = level.getBlockState(p);
                    return state != nullptr && !twilight::isReplaceable(state, false)
                        && state != exposedRoot.getState(random, exposedPos);
                })) {
                return;
            }
            setter(exposedPos, exposedRoot.getState(random, exposedPos));
        } else {
            if (placeIfValidRootPos(level, setter, penetrability, random, exposedPos, dirtRoot)) {
                traceRoot(level, setter, penetrability, random, dirtRoot, tracer);
            }
            return;
        }
    }
}

} // namespace

// ============================================================================
// twilight:: helpers
// ============================================================================
namespace twilight {

VoxelBresenhamIterator::VoxelBresenhamIterator(const core::BlockPos& from, const core::BlockPos& to)
    : m_x(from.getX()), m_y(from.getY()), m_z(from.getZ()) {
    const int32_t xVec = to.getX() - m_x;
    const int32_t yVec = to.getY() - m_y;
    const int32_t zVec = to.getZ() - m_z;
    const int32_t absDx = std::abs(xVec);
    const int32_t absDy = std::abs(yVec);
    const int32_t absDz = std::abs(zVec);
    m_xInc = xVec < 0 ? -1 : 1;
    m_yInc = yVec < 0 ? -1 : 1;
    m_zInc = zVec < 0 ? -1 : 1;
    m_doubleAbsDx = absDx << 1;
    m_doubleAbsDy = absDy << 1;
    m_doubleAbsDz = absDz << 1;
    if (absDx >= absDy && absDx >= absDz) {
        m_err1 = m_doubleAbsDy - absDx;
        m_err2 = m_doubleAbsDz - absDx;
        m_direction = Axis::X;
        m_length = absDx + 1;
    } else if (absDy >= absDx && absDy >= absDz) {
        m_err1 = m_doubleAbsDx - absDy;
        m_err2 = m_doubleAbsDz - absDy;
        m_direction = Axis::Y;
        m_length = absDy + 1;
    } else {
        m_err1 = m_doubleAbsDy - absDz;
        m_err2 = m_doubleAbsDx - absDz;
        m_direction = Axis::Z;
        m_length = absDz + 1;
    }
}

core::BlockPos VoxelBresenhamIterator::next() {
    const core::BlockPos out(m_x, m_y, m_z);
    if (hasNext()) {
        switch (m_direction) {
            case Axis::X:
                if (m_err1 > 0) { m_y += m_yInc; m_err1 -= m_doubleAbsDx; }
                if (m_err2 > 0) { m_z += m_zInc; m_err2 -= m_doubleAbsDx; }
                m_err1 += m_doubleAbsDy;
                m_err2 += m_doubleAbsDz;
                m_x += m_xInc;
                break;
            case Axis::Y:
                if (m_err1 > 0) { m_x += m_xInc; m_err1 -= m_doubleAbsDy; }
                if (m_err2 > 0) { m_z += m_zInc; m_err2 -= m_doubleAbsDy; }
                m_err1 += m_doubleAbsDx;
                m_err2 += m_doubleAbsDz;
                m_y += m_yInc;
                break;
            case Axis::Z:
                if (m_err1 > 0) { m_y += m_yInc; m_err1 -= m_doubleAbsDz; }
                if (m_err2 > 0) { m_x += m_xInc; m_err2 -= m_doubleAbsDz; }
                m_err1 += m_doubleAbsDy;
                m_err2 += m_doubleAbsDx;
                m_z += m_zInc;
                break;
        }
        ++m_i;
    }
    return out;
}

core::BlockPos translate(const core::BlockPos& pos, double distance, double angle, double tilt) {
    const double pi = 3.141592653589793;
    const double rangle = angle * 2.0 * pi;
    const double rtilt = tilt * pi;
    // Java Math.round(double) = floor(x + 0.5)
    auto round = [](double v) { return static_cast<int32_t>(std::floor(v + 0.5)); };
    return pos.offset(
        round(std::sin(rangle) * std::sin(rtilt) * distance),
        round(std::cos(rtilt) * distance),
        round(std::cos(rangle) * std::sin(rtilt) * distance));
}

bool isReplaceable(BlockState* state, bool includeFlowers) {
    if (state == nullptr) return false;
    // #twilightforest:worldgen_replaceables = #lush_ground_replaceable + #replaceable_by_trees
    const bool replaceable = state->canBeReplaced()
        || matchesBlockTagName(state, "minecraft:lush_ground_replaceable")
        || matchesBlockTagName(state, "minecraft:replaceable_by_trees")
        || (includeFlowers && matchesBlockTagName(state, "minecraft:flowers"));
    return replaceable && !matchesBlockTagName(state, "minecraft:features_cannot_replace");
}

bool validTreePos(WorldGenLevel& level, const core::BlockPos& pos) {
    return TreeFeature::validTreePos(level, pos) || hasTag(level.getBlockState(pos), "minecraft:flowers");
}

} // namespace twilight

// ============================================================================
// BranchingTrunkPlacer — treeplacers/BranchingTrunkPlacer.java
// ============================================================================
std::vector<FoliageAttachment> BranchingTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<BlockStateProvider> trunkProvider,
    std::shared_ptr<BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    (void)dirtProvider;
    (void)forceDirt;   // the mod's placer never sets dirt below the trunk
    std::vector<FoliageAttachment> leafAttachments;

    if (m_preventExposedRoot) {
        // Direction.Plane.HORIZONTAL order: NORTH, EAST, SOUTH, WEST
        for (int i = 0; i < 4; ++i) {
            const core::Direction direction = core::horizontalPlaneDirection(i);
            if (level.isStateAtPosition(origin.below().relative(direction),
                                        [](BlockState* s) { return s != nullptr && s->canBeReplaced(); })) {
                trunkSetter(origin.below(), trunkProvider->getState(random, origin.below()));
                break;
            }
        }
    }

    int height = treeHeight;
    for (int y = 0; y <= height; ++y) {
        if (!placeLog(level, trunkSetter, random, origin.above(y), trunkProvider)) {
            height = y;
            break;
        }
    }

    leafAttachments.emplace_back(origin.above(height), 0, false);

    const int numBranches = m_branchesConfig.branchCount
        + random.nextInt(m_branchesConfig.randomAddBranches + 1);
    const float offset = random.nextFloat();
    for (int b = 0; b < numBranches; ++b) {
        buildBranch(level, trunkSetter, origin, leafAttachments,
                    height - m_branchDownwardOffset + b, m_branchesConfig.length,
                    m_branchesConfig.spacingYaw * b + offset, m_branchesConfig.downwardsPitch, random);
    }
    return leafAttachments;
}

void BranchingTrunkPlacer::buildBranch(LevelReader& level, TrunkSetter& trunkSetter,
                                       const core::BlockPos& pos,
                                       std::vector<FoliageAttachment>& leafBlocks,
                                       int height, double length, double angle, double tilt,
                                       WorldgenRandom& random) {
    const core::BlockPos src = pos.above(height);
    const core::BlockPos dest = twilight::translate(src, length, angle, tilt);

    if (m_perpendicularBranches) {
        twilight::VoxelBresenhamIterator line(src, core::BlockPos(dest.getX(), src.getY(), dest.getZ()));
        while (line.hasNext()) placeWood(level, trunkSetter, random, line.next());
        const int max = std::max(src.getY(), dest.getY());
        for (int i = std::min(src.getY(), dest.getY()); i < max + 1; ++i) {
            placeWood(level, trunkSetter, random, core::BlockPos(dest.getX(), i, dest.getZ()));
        }
    } else {
        twilight::VoxelBresenhamIterator line(src, dest);
        while (line.hasNext()) placeWood(level, trunkSetter, random, line.next());
    }

    placeWood(level, trunkSetter, random, dest.east());
    placeWood(level, trunkSetter, random, dest.west());
    placeWood(level, trunkSetter, random, dest.south());
    placeWood(level, trunkSetter, random, dest.north());

    leafBlocks.emplace_back(dest, 0, false);
}

bool BranchingTrunkPlacer::placeWood(LevelReader& level, TrunkSetter& trunkSetter,
                                     WorldgenRandom& random, const core::BlockPos& pos) {
    if (validTreePos(level, pos)) {
        trunkSetter(pos, m_branchesConfig.branchProvider->getState(random, pos));
        return true;
    }
    return false;
}

// ============================================================================
// LeafSpheroidFoliagePlacer — treeplacers/LeafSpheroidFoliagePlacer.java with
// FeaturePlacers.placeSpheroid (the verticalBias overload) and placeLeaf
// ============================================================================
namespace {
std::shared_ptr<carver::IntProvider> constantRadius(float horizontalRadius) {
    return std::make_shared<carver::ConstantInt>(static_cast<int32_t>(horizontalRadius));
}

void placeLeaf(FoliageSetter& setter, WorldgenRandom& random, const BlockStateProvider& provider,
               const core::BlockPos& pos) {
    if (setter.canPlace(pos)) {
        setter.set(pos, provider.getState(random, pos));
    }
}
} // namespace

LeafSpheroidFoliagePlacer::LeafSpheroidFoliagePlacer(float horizontalRadius, float verticalRadius,
                                                     std::shared_ptr<carver::IntProvider> yOffset,
                                                     int randomHorizontal, int randomVertical,
                                                     float verticalBias, int shagFactor)
    : FoliagePlacer(constantRadius(horizontalRadius), std::move(yOffset))
    , m_horizontalRadius(horizontalRadius)
    , m_verticalRadius(verticalRadius)
    , m_verticalBias(verticalBias)
    , m_randomHorizontal(randomHorizontal)
    , m_randomVertical(randomVertical)
    , m_shagFactor(shagFactor) {}

void LeafSpheroidFoliagePlacer::createFoliageImpl(
    FoliageSetter& foliageSetter,
    WorldgenRandom& random,
    std::shared_ptr<BlockStateProvider> foliageProvider,
    int treeHeight,
    const FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    (void)treeHeight; (void)foliageHeight; (void)leafRadius;
    const BlockStateProvider& provider = *foliageProvider;
    const core::BlockPos center = attachment.pos().above(offset);

    // Java evaluates the xz radius (with its draw) before the y radius.
    const float xzRadius = static_cast<float>(attachment.radiusOffset()) + m_horizontalRadius
        + static_cast<float>(random.nextInt(m_randomHorizontal + 1));
    const float yRadius = static_cast<float>(attachment.radiusOffset()) + m_verticalRadius
        + static_cast<float>(random.nextInt(m_randomVertical + 1));

    // FeaturePlacers.placeSpheroid(world, setter, VALID_TREE_POS, random,
    //                              center, xzRadius, yRadius, verticalBias, provider)
    const float xzRadiusSquared = xzRadius * xzRadius;
    const float yRadiusSquared = yRadius * yRadius;
    const float superRadiusSquared = xzRadiusSquared * yRadiusSquared;
    placeLeaf(foliageSetter, random, provider, center);

    for (int y = 0; static_cast<float>(y) <= yRadius; ++y) {
        placeLeaf(foliageSetter, random, provider, center.offset(0, y, 0));
        placeLeaf(foliageSetter, random, provider, center.offset(0, -y, 0));
    }

    for (int x = 0; static_cast<float>(x) <= xzRadius; ++x) {
        for (int z = 1; static_cast<float>(z) <= xzRadius; ++z) {
            if (static_cast<float>(x * x + z * z) > xzRadiusSquared) continue;

            placeLeaf(foliageSetter, random, provider, center.offset(x, 0, z));
            placeLeaf(foliageSetter, random, provider, center.offset(-x, 0, -z));
            placeLeaf(foliageSetter, random, provider, center.offset(-z, 0, x));
            placeLeaf(foliageSetter, random, provider, center.offset(z, 0, -x));

            for (int y = 1; static_cast<float>(y) <= yRadius; ++y) {
                const float xzSquare = static_cast<float>(x * x + z * z) * yRadiusSquared;
                const float up = static_cast<float>(y) - m_verticalBias;
                const float down = static_cast<float>(y) + m_verticalBias;

                if (xzSquare + (up * up) * xzRadiusSquared <= superRadiusSquared) {
                    placeLeaf(foliageSetter, random, provider, center.offset(x, y, z));
                    placeLeaf(foliageSetter, random, provider, center.offset(-x, y, -z));
                    placeLeaf(foliageSetter, random, provider, center.offset(-z, y, x));
                    placeLeaf(foliageSetter, random, provider, center.offset(z, y, -x));
                }
                if (xzSquare + (down * down) * xzRadiusSquared <= superRadiusSquared) {
                    placeLeaf(foliageSetter, random, provider, center.offset(x, -y, z));
                    placeLeaf(foliageSetter, random, provider, center.offset(-x, -y, -z));
                    placeLeaf(foliageSetter, random, provider, center.offset(-z, -y, x));
                    placeLeaf(foliageSetter, random, provider, center.offset(z, -y, -x));
                }
            }
        }
    }

    for (int i = 0; i < m_shagFactor; ++i) {
        const float randomYaw = random.nextFloat() * kTwoPi;
        const float randomPitch = random.nextFloat() * 2.0f - 1.0f;
        const float yUnit = Mth::sqrt(1.0f - randomPitch * randomPitch);
        const float xCircleOffset = yUnit * Mth::cos(randomYaw) * (m_horizontalRadius - 1.0f);
        const float zCircleOffset = yUnit * Mth::sin(randomYaw) * (m_horizontalRadius - 1.0f);

        const core::BlockPos placement = center.offset(
            static_cast<int32_t>(xCircleOffset + static_cast<float>(static_cast<int32_t>(xCircleOffset) >> 31)),
            static_cast<int32_t>(randomPitch * (m_verticalRadius + 0.25f) + m_verticalBias),
            static_cast<int32_t>(zCircleOffset + static_cast<float>(static_cast<int32_t>(zCircleOffset) >> 31)));

        // placeLeafCluster: the pos, east, south and south-east
        placeLeaf(foliageSetter, random, provider, placement);
        placeLeaf(foliageSetter, random, provider, placement.east());
        placeLeaf(foliageSetter, random, provider, placement.south());
        placeLeaf(foliageSetter, random, provider, placement.offset(1, 0, 1));
    }
}

// ============================================================================
// TreeRootsDecorator — treeplacers/TreeRootsDecorator.java
// ============================================================================
void TreeRootsDecorator::place(DecoratorContext& context) {
    if (context.logs().empty() || context.level() == nullptr || !m_rootBlock) return;
    WorldGenLevel& level = *context.level();
    WorldgenRandom& random = context.random();

    const int numBranches = m_strands + random.nextInt(m_addExtraStrands + 1);
    const float offset = random.nextFloat();
    const core::BlockPos startPos = context.logs().front().above(m_yOffset);
    const BlockSetter setter = [&context](const core::BlockPos& pos, BlockState* state) {
        if (state) context.setBlock(pos, state);
    };

    for (int i = 0; i < numBranches; ++i) {
        const core::BlockPos dest = twilight::translate(
            startPos.below(i + 2), static_cast<double>(m_length),
            0.3 * i + static_cast<double>(offset), 0.8);
        twilight::VoxelBresenhamIterator tracer(startPos.below(), dest);
        if (m_surfaceBlock) {
            traceExposedRoot(level, setter, m_rootPenetrability, random, *m_surfaceBlock, *m_rootBlock, tracer);
        } else {
            traceRoot(level, setter, m_rootPenetrability, random, *m_rootBlock, tracer);
        }
    }
}

// ============================================================================
// TrunkSideDecorator — treeplacers/TrunkSideDecorator.java
// ============================================================================
void TrunkSideDecorator::place(DecoratorContext& context) {
    const int blockCount = static_cast<int>(context.logs().size());
    if (blockCount == 0 || !m_decoration) return;
    WorldgenRandom& random = context.random();

    for (int attempt = 0; attempt < m_count; ++attempt) {
        if (random.nextFloat() >= m_probability) continue;

        const core::BlockPos logPos = context.logs()[static_cast<size_t>(random.nextInt(blockCount))];
        const core::Direction direction = core::horizontalRandom(random);
        const core::BlockPos newPos = logPos.offset(core::getStepX(direction), 0, core::getStepZ(direction));
        if (context.isAir(newPos)) {
            // Java asks the provider twice (once for the property check).
            BlockState* probe = m_decoration->getState(random, newPos);
            if (probe && probe->hasProperty(BlockStateProperties::FACING)) {
                BlockState* state = m_decoration->getState(random, newPos);
                context.setBlock(newPos, state->setValue(*BlockStateProperties::FACING, direction));
            } else if (probe) {
                context.setBlock(newPos, m_decoration->getState(random, newPos));
            }
        }
    }
}

// ============================================================================
// DangleFromTreeDecorator — treeplacers/DangleFromTreeDecorator.java
// ============================================================================
void DangleFromTreeDecorator::place(DecoratorContext& context) {
    if (context.leaves().empty() || context.logs().empty()) return;
    WorldgenRandom& random = context.random();

    int totalTries = m_count + random.nextInt(m_randomAddCount + 1);
    const int leafTotal = static_cast<int>(context.leaves().size());
    totalTries = std::min(totalTries, leafTotal);

    auto isAirAt = [&context](const core::BlockPos& p) {
        BlockState* s = context.getBlockState(p);
        return s != nullptr && s->isAir();
    };

    for (int attempt = 0; attempt < totalTries; ++attempt) {
        bool clearedOfPossibleLeaves = false;
        core::BlockPos pos = context.leaves()[static_cast<size_t>(random.nextInt(leafTotal))];

        // Don't place a rope where the trunk is. (Java compares the leaf's X
        // with the first log's Y — kept as written.)
        if (pos.getX() == context.logs().front().getY() && pos.getZ() == context.logs().front().getZ()) return;

        int cordLength = m_baseLength + random.nextInt(m_randomAddLength + 1);
        for (int ropeUnrolling = 1; ropeUnrolling <= cordLength; ++ropeUnrolling) {
            const bool isAir = isAirAt(pos.below(ropeUnrolling));
            if (!clearedOfPossibleLeaves && isAir) clearedOfPossibleLeaves = true;
            if (clearedOfPossibleLeaves && !isAir) {
                cordLength = ropeUnrolling - 1;
                break;
            }
        }

        if (cordLength > m_minimumRequiredLength) {
            BlockState* rope = m_rope ? m_rope->getState(random, pos) : nullptr;
            for (int ropeUnrolling = 1; ropeUnrolling < cordLength; ++ropeUnrolling) {
                pos = pos.below(1);
                if (isAirAt(pos) && rope) context.setBlock(pos, rope);
            }
            pos = pos.below(1);
            if (context.isAir(pos) && m_baggage) {
                BlockState* baggage = m_baggage->getState(random, pos);
                if (baggage) context.setBlock(pos, baggage);
            }
        }
    }
}

// ============================================================================
// DarkCanopyTreeFeature — trees/DarkCanopyTreeFeature.java
// ============================================================================
bool DarkCanopyTreeFeature::place(FeaturePlaceContext<TreeConfiguration>& context) {
    WorldGenLevel* reader = context.level();
    if (!reader || !m_treeFeature) return false;
    core::BlockPos pos = context.origin();

    // If we are given leaves as a starting position, seek substrate underneath.
    bool foundDirt = false;
    for (int dy = pos.getY(); dy >= reader->getMinY(); --dy) {
        BlockState* state = reader->getBlockState(core::BlockPos(pos.getX(), dy - 1, pos.getZ()));
        if (hasTag(state, "minecraft:substrate_overworld")) {   // #substrate_overworld
            foundDirt = true;
            pos = core::BlockPos(pos.getX(), dy, pos.getZ());
            break;
        } else if (hasTag(state, "minecraft:base_stone_overworld") || hasTag(state, "minecraft:sand")) {
            break;
        }
    }
    if (!foundDirt) return false;

    for (int i = 0; i < 4; ++i) {
        if (!twilight::validTreePos(*reader, pos.above(i))) return false;
    }

    // Do not grow next to another tree.
    for (int i = 0; i < 4; ++i) {
        if (isLogs(reader->getBlockState(pos.relative(core::horizontalPlaneDirection(i))))) return false;
    }

    // The mod's doPlace/getMaxFreeTreeHeight are a [VanillaCopy] of TreeFeature.
    return m_treeFeature->place(*reader, context.chunkGenerator(), context.random(), pos, context.config());
}

// ============================================================================
// CanopyMushroomFeature — trees/{Canopy,BrownCanopy,RedCanopy}MushroomFeature.java
// ============================================================================
namespace {
bool isEdge(int x, int edge) { return x == edge || x == -edge; }
bool isCornerInSquare(int x, int z, int edge) { return isEdge(x, edge) && isEdge(z, edge); }

bool hasHorizontalMushroomProperties(BlockState* state) {
    return state && state->hasProperty(BlockStateProperties::WEST) && state->hasProperty(BlockStateProperties::EAST)
        && state->hasProperty(BlockStateProperties::NORTH) && state->hasProperty(BlockStateProperties::SOUTH);
}

// FeatureLogic.getHorizontalMushroomBlockState
BlockState* horizontalMushroomState(BlockState* state, int x, int z, int r) {
    if (!hasHorizontalMushroomProperties(state)) return state;
    const bool west = x == -r || (isEdge(z, r) && x == 1 - r);
    const bool east = x == r || (isEdge(z, r) && x == r - 1);
    const bool north = z == -r || (isEdge(x, r) && z == 1 - r);
    const bool south = z == r || (isEdge(x, r) && z == r - 1);
    state = state->setValue(*BlockStateProperties::WEST, west);
    state = state->setValue(*BlockStateProperties::EAST, east);
    state = state->setValue(*BlockStateProperties::NORTH, north);
    return state->setValue(*BlockStateProperties::SOUTH, south);
}

bool hasCapProperties(BlockState* state) {
    return hasHorizontalMushroomProperties(state) && state->hasProperty(BlockStateProperties::UP);
}

BlockState* capFaces(BlockState* state, bool up, bool west, bool east, bool north, bool south) {
    state = state->setValue(*BlockStateProperties::UP, up);
    state = state->setValue(*BlockStateProperties::WEST, west);
    state = state->setValue(*BlockStateProperties::EAST, east);
    state = state->setValue(*BlockStateProperties::NORTH, north);
    return state->setValue(*BlockStateProperties::SOUTH, south);
}

// RedCanopyMushroomFeature.isInsideSmoothShape
bool isInsideSmoothShape(int height, int j, int x, int y, int z) {
    const int i = y - (height - 2);
    if (i == 4 || std::abs(x) > j || std::abs(z) > j) return false;
    if (i >= 2) return true;
    const bool xMinMax = x == -j || x == j;
    const bool zMinMax = z == -j || z == j;
    if (i == 1 && ((xMinMax && std::abs(z) == j - 1) || (zMinMax && std::abs(x) == j - 1))) return false;
    return xMinMax != zMinMax || (std::abs(x) == std::abs(z) && std::abs(x) == j - 1);
}
} // namespace

bool CanopyMushroomFeature::isReplaceableAt(WorldGenLevel* level, const core::BlockPos& pos) {
    BlockState* state = level->getBlockState(pos);
    return state != nullptr && twilight::isReplaceable(state, true) && !state->isSolidRender();
}

int CanopyMushroomFeature::getTreeHeight(WorldgenRandom& random) const {
    const int height = 9 + random.nextInt(5);
    return m_kind == Kind::BROWN ? height : height + 3;
}

int CanopyMushroomFeature::getBranches(WorldgenRandom& random) const {
    if (m_kind == Kind::BROWN) return std::max(random.nextInt(5), 3);
    return 3;
}

double CanopyMushroomFeature::getLength(WorldgenRandom& random) const {
    if (m_kind == Kind::BROWN) return static_cast<double>(9 - random.nextInt(2));
    return static_cast<double>(10 + random.nextInt(2));
}

// AbstractHugeMushroomFeature.isValidPosition (26.1) with the canopy
// getTreeRadiusForHeight (0 up to 3, else foliageRadius * 1.5). can_place_on
// is #huge_{brown,red}_mushroom_can_place_on: dirt or mushroom-grow blocks.
bool CanopyMushroomFeature::isValidPosition(WorldGenLevel* level, const core::BlockPos& origin, int treeHeight,
                                            const HugeMushroomFeatureConfiguration& config) const {
    const int y = origin.getY();
    if (y < level->getMinY() + 1 || y + treeHeight + 1 > level->getMaxY()) return false;
    BlockState* below = level->getBlockState(origin.below());
    if (!FeatureHelpers::isDirt(below) && !FeatureHelpers::isMushroomGrowBlock(below)) return false;

    for (int dy = 0; dy <= treeHeight; ++dy) {
        const int radius = dy <= 3 ? 0 : static_cast<int>(static_cast<float>(config.foliageRadius) * 1.5f);
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                BlockState* state = level->getBlockState(origin.offset(dx, dy, dz));
                if (state && !state->isAir() && !state->isLeaves() && !hasTag(state, "minecraft:leaves")) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool CanopyMushroomFeature::place(FeaturePlaceContext<HugeMushroomFeatureConfiguration>& context) {
    WorldGenLevel* level = context.level();
    if (!level) return false;
    const HugeMushroomFeatureConfiguration& config = context.config();
    if (!config.capProvider || !config.stemProvider) return false;
    WorldgenRandom& random = context.random();
    const core::BlockPos& origin = context.origin();

    const int treeHeight = getTreeHeight(random);
    if (!isValidPosition(level, origin, treeHeight, config)) return false;
    makeCap(level, random, origin, treeHeight, config);
    placeTrunk(level, random, origin, config, treeHeight);
    return true;
}

bool CanopyMushroomFeature::addFirefly(WorldGenLevel* level, const core::BlockPos& pos, WorldgenRandom& random) {
    const core::Direction direction = core::fromIndex(random.nextInt(6));   // Direction.getRandom
    if (core::getAxis(direction) == core::Axis::Y) return false;
    const core::BlockPos bugPos = pos.relative(direction);
    if (!isReplaceableAt(level, bugPos)) return false;
    BlockState* firefly = minecraft::world::level::block::Blocks::getDefaultState("minecraft:firefly");
    if (!firefly || !firefly->hasProperty(BlockStateProperties::FACING)) return false;
    setBlock(level, bugPos, firefly->setValue(*BlockStateProperties::FACING, direction));
    return true;
}

void CanopyMushroomFeature::placeTrunk(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos,
                                       const HugeMushroomFeatureConfiguration& config, int height) {
    int bugsLeft = std::max(0, random.nextInt(10) - 4) / 2;

    for (int i = 0; i < height; ++i) {
        const core::BlockPos stemPos = pos.above(i);
        if (isReplaceableAt(level, stemPos)) {
            setBlock(level, stemPos, config.stemProvider->getState(random, pos));
            if (bugsLeft > 0 && i > height / 2 && random.nextInt(10) == 9) {
                if (addFirefly(level, stemPos, random)) --bugsLeft;
            }
        } else {
            height = i;
            break;
        }
    }

    const int numBranches = getBranches(random);
    const float offset = random.nextFloat();
    const HugeMushroomFeatureConfiguration branchConfig(config.capProvider, config.stemProvider,
                                                        config.foliageRadius - 1);
    for (int b = 0; b < numBranches; ++b) {
        const double length = getLength(random);
        bugsLeft = buildABranch(level, pos, height - 6 + b, length, 0.3 * b + static_cast<double>(offset),
                                random, branchConfig, bugsLeft);
    }
}

int CanopyMushroomFeature::buildABranch(WorldGenLevel* level, const core::BlockPos& pos, int height,
                                        double length, double angle, WorldgenRandom& random,
                                        const HugeMushroomFeatureConfiguration& config, int bugsLeft) {
    const core::BlockPos src = pos.above(height);
    const core::BlockPos dest = twilight::translate(src, length, angle, 0.2);

    twilight::VoxelBresenhamIterator line(src, core::BlockPos(dest.getX(), src.getY(), dest.getZ()));
    while (line.hasNext()) {
        const core::BlockPos pixel = line.next();
        BlockState* state = config.stemProvider->getState(random, pos);
        if (state && state->hasProperty(BlockStateProperties::UP) && state->hasProperty(BlockStateProperties::DOWN)) {
            state = state->setValue(*BlockStateProperties::DOWN, true);
            state = state->setValue(*BlockStateProperties::UP, true);
        }
        if (isReplaceableAt(level, pixel)) setBlock(level, pixel, state);
    }

    const int minY = std::min(src.getY(), dest.getY());
    const int max = std::max(src.getY(), dest.getY());
    for (int i = minY; i < max + 1; ++i) {
        BlockState* state = config.stemProvider->getState(random, pos);
        if (state && state->hasProperty(BlockStateProperties::DOWN) && i == minY) {
            state = state->setValue(*BlockStateProperties::DOWN, true);
        }
        const core::BlockPos blockPos(dest.getX(), i, dest.getZ());
        if (isReplaceableAt(level, blockPos)) setBlock(level, blockPos, state);

        if (bugsLeft > 0 && i > minY / 2 && random.nextInt(20) == 0) {
            if (addFirefly(level, blockPos, random)) --bugsLeft;
        }
    }

    makeCap(level, random, dest, 1, config);   // branch caps sit one above the branch end
    return bugsLeft;
}

void CanopyMushroomFeature::makeCap(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos,
                                    int height, const HugeMushroomFeatureConfiguration& config) {
    switch (m_kind) {
        case Kind::RED_VANILLA:  makeVanillaCap(level, random, pos, height, config); break;
        case Kind::RED_SMOOTH:   makeSmoothCap(level, random, pos, height, config); break;
        case Kind::RED_SPHEROID: makeSpheroidCap(level, random, pos, height, config); break;
        case Kind::BROWN:
        case Kind::RED_FLAT:     makeFlatCap(level, random, pos, height, config); break;
    }
}

// CanopyMushroomFeature.makeCap — the big brown mushroom's flat cap
void CanopyMushroomFeature::makeFlatCap(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos,
                                        int height, const HugeMushroomFeatureConfiguration& config) {
    const int r = config.foliageRadius;
    for (int x = -r; x <= r; ++x) {
        for (int z = -r; z <= r; ++z) {
            if (isCornerInSquare(x, z, r)) continue;
            const core::BlockPos capPos = pos.offset(x, height, z);
            if (isReplaceableAt(level, capPos)) {
                BlockState* state = horizontalMushroomState(config.capProvider->getState(random, pos), x, z, r);
                if (state) setBlock(level, capPos, state);
            }
        }
    }
}

// RedCanopyMushroomFeature.makeVanillaCap
void CanopyMushroomFeature::makeVanillaCap(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos,
                                           int height, const HugeMushroomFeatureConfiguration& config) {
    for (int y = height - 3; y <= height; ++y) {
        const int j = y < height ? config.foliageRadius : config.foliageRadius - 1;
        const int k = config.foliageRadius - 2;
        for (int x = -j; x <= j; ++x) {
            for (int z = -j; z <= j; ++z) {
                const bool xMinMax = x == -j || x == j;
                const bool zMinMax = z == -j || z == j;
                if (y >= height || xMinMax != zMinMax) {
                    const core::BlockPos capPos = pos.offset(x, y, z);
                    if (isReplaceableAt(level, capPos)) {
                        BlockState* state = config.capProvider->getState(random, pos);
                        if (hasCapProperties(state)) {
                            state = capFaces(state, y >= height - 1, x < -k, x > k, z < -k, z > k);
                        }
                        if (state) setBlock(level, capPos, state);
                    }
                }
            }
        }
    }
}

// RedCanopyMushroomFeature.makeSmoothCap
void CanopyMushroomFeature::makeSmoothCap(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos,
                                          int height, const HugeMushroomFeatureConfiguration& config) {
    for (int y = height - 2; y <= height + 1; ++y) {
        const int j = config.foliageRadius - std::max(0, y - (height - 1)) + 1;
        for (int x = -j; x <= j; ++x) {
            for (int z = -j; z <= j; ++z) {
                if (!isInsideSmoothShape(height, j, x, y, z)) continue;
                const core::BlockPos capPos = pos.offset(x, y, z);
                if (isReplaceableAt(level, capPos)) {
                    BlockState* state = config.capProvider->getState(random, pos);
                    if (hasCapProperties(state)) {
                        state = capFaces(state,
                            !isInsideSmoothShape(height, j - (y > height - 2 ? 1 : 0), x, y + 1, z),
                            !isInsideSmoothShape(height, j, x - 1, y, z) && x < 0,
                            !isInsideSmoothShape(height, j, x + 1, y, z) && x > 0,
                            !isInsideSmoothShape(height, j, x, y, z - 1) && z < 0,
                            !isInsideSmoothShape(height, j, x, y, z + 1) && z > 0);
                    }
                    if (state) setBlock(level, capPos, state);
                }
            }
        }
    }
}

// RedCanopyMushroomFeature.makeSpheroidCap
void CanopyMushroomFeature::makeSpheroidCap(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos,
                                            int height, const HugeMushroomFeatureConfiguration& config) {
    for (int y = height - 2; y <= height; ++y) {
        const int j = y == height - 1 ? config.foliageRadius + 2 : config.foliageRadius + 1;
        for (int x = -j; x <= j; ++x) {
            for (int z = -j; z <= j; ++z) {
                const double distance = std::sqrt(static_cast<double>(x * x + z * z));
                const double maxDistance = static_cast<double>(j) + 0.1;
                if (distance > maxDistance) continue;
                const core::BlockPos capPos = pos.offset(x, y, z);
                if (isReplaceableAt(level, capPos)) {
                    BlockState* state = config.capProvider->getState(random, pos);
                    if (hasCapProperties(state)) {
                        state = capFaces(state,
                            y > height - 2 && (y == height || distance > maxDistance - 1.0),
                            std::sqrt(static_cast<double>((x - 1) * (x - 1) + z * z)) > maxDistance,
                            std::sqrt(static_cast<double>((x + 1) * (x + 1) + z * z)) > maxDistance,
                            std::sqrt(static_cast<double>(x * x + (z - 1) * (z - 1))) > maxDistance,
                            std::sqrt(static_cast<double>(x * x + (z + 1) * (z + 1))) > maxDistance);
                    }
                    if (state) setBlock(level, capPos, state);
                }
            }
        }
    }
}

// ============================================================================
// WeightedListFeature — feature/WeightedListFeature.java
// ============================================================================
bool WeightedListFeature::place(FeaturePlaceContext<WeightedListFeatureConfig>& context) {
    const WeightedListFeatureConfig& config = context.config();
    int32_t totalWeight = 0;
    for (const auto& entry : config.entries) totalWeight += entry.second;
    if (totalWeight <= 0) return false;

    // WeightedList.getRandom: one nextInt(totalWeight), walked in list order.
    int32_t selection = context.random().nextInt(totalWeight);
    for (const auto& entry : config.entries) {
        selection -= entry.second;
        if (selection < 0) {
            return entry.first != nullptr
                && entry.first->place(context.level(), context.chunkGenerator(), context.random(), context.origin());
        }
    }
    return false;
}

// ============================================================================
// TFSmallLakeFeature — feature/TFSmallLakeFeature.java
// ============================================================================
namespace {
// #twilightforest:small_lakes_dont_replace
bool smallLakesDontReplace(BlockState* state) {
    return state != nullptr && (hasTag(state, "minecraft:features_cannot_replace") || isLogs(state)
        || state->isLeaves() || hasTag(state, "minecraft:leaves")
        || isNamed(state, "minecraft:root") || isNamed(state, "minecraft:liveroot_block")
        || isNamed(state, "minecraft:mushroom_stem"));
}
} // namespace

bool TFSmallLakeFeature::place(FeaturePlaceContext<SmallLakeConfiguration>& context) {
    WorldGenLevel* level = context.level();
    WorldgenRandom& random = context.random();
    const SmallLakeConfiguration& config = context.config();
    if (!level || !config.fluid) return false;
    core::BlockPos blockpos = context.origin();

    if (blockpos.getY() <= level->getMinY() + 4) return false;
    blockpos = blockpos.below(4);

    std::vector<bool> booleans(2048, false);
    const int count = random.nextInt(4) + 4;
    for (int j = 0; j < count; ++j) {
        const double d0 = random.nextDouble() * 6.0 + 3.0;
        const double d1 = random.nextDouble() * 4.0 + 2.0;
        const double d2 = random.nextDouble() * 6.0 + 3.0;
        const double d3 = random.nextDouble() * (16.0 - d0 - 2.0) + 1.0 + d0 / 2.0;
        const double d4 = random.nextDouble() * (8.0 - d1 - 4.0) + 2.0 + d1 / 2.0;
        const double d5 = random.nextDouble() * (16.0 - d2 - 2.0) + 1.0 + d2 / 2.0;
        for (int l = 1; l < 15; ++l) {
            for (int i1 = 1; i1 < 15; ++i1) {
                for (int j1 = 1; j1 < 7; ++j1) {
                    const double d6 = (static_cast<double>(l) - d3) / (d0 / 2.0);
                    const double d7 = (static_cast<double>(j1) - d4) / (d1 / 2.0);
                    const double d8 = (static_cast<double>(i1) - d5) / (d2 / 2.0);
                    if (d6 * d6 + d7 * d7 + d8 * d8 < 1.0) {
                        booleans[static_cast<size_t>((l * 16 + i1) * 8 + j1)] = true;
                    }
                }
            }
        }
    }

    auto at = [&booleans](int x, int z, int y) { return booleans[static_cast<size_t>((x * 16 + z) * 8 + y)]; };
    auto isBorder = [&](int x, int z, int y) {
        return !at(x, z, y)
            && ((x < 15 && at(x + 1, z, y)) || (x > 0 && at(x - 1, z, y))
             || (z < 15 && at(x, z + 1, y)) || (z > 0 && at(x, z - 1, y))
             || (y < 7 && at(x, z, y + 1)) || (y > 0 && at(x, z, y - 1)));
    };

    BlockState* fluidState = config.fluid;   // simple_state_provider: no draw

    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 0; y < 8; ++y) {
                if (!isBorder(x, z, y)) continue;
                BlockState* state = level->getBlockState(blockpos.offset(x, y, z));
                if (!state) return false;
                if (y >= 4 && state->isFluid()) return false;
                if (y < 4 && !state->isSolid() && state != fluidState) return false;
            }
        }
    }

    BlockState* iceState = config.ice;
    BlockState* air = minecraft::world::level::block::Blocks::getDefaultState("minecraft:cave_air");

    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 0; y < 8; ++y) {
                if (!at(x, z, y)) continue;
                const core::BlockPos offset = blockpos.offset(x, y, z);
                if (smallLakesDontReplace(level->getBlockState(offset))
                    || smallLakesDontReplace(level->getBlockState(offset.above()))) {
                    continue;
                }
                if (y >= 4) {
                    if (air) level->setBlock(offset, air, kFeatureFlags);
                    markAboveForPostProcessing(level, offset);
                    continue;
                }
                if (y == 3 && iceState) {
                    level->setBlock(offset, iceState, kFeatureFlags);
                    continue;
                }
                level->setBlock(offset, fluidState, kFeatureFlags);
            }
        }
    }

    if (config.barrier && !config.barrier->isAir()) {
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                for (int y = 0; y < 8; ++y) {
                    if (isBorder(x, z, y) && (y < 4 || random.nextInt(2) != 0)) {
                        const core::BlockPos barrierPos = blockpos.offset(x, y, z);
                        BlockState* state = level->getBlockState(barrierPos);
                        if (state && state->isSolid() && !hasTag(state, "minecraft:lava_pool_stone_cannot_replace")) {
                            level->setBlock(barrierPos, config.barrier, kFeatureFlags);
                            markAboveForPostProcessing(level, barrierPos);
                        }
                    }
                }
            }
        }
    }
    return true;
}

// ============================================================================
// FallenLeavesFeature — feature/FallenLeavesFeature.java
// ============================================================================
bool FallenLeavesFeature::canPlace(const core::BlockPos& pos, WorldGenLevel* level) const {
    BlockState* here = level->getBlockState(pos);
    BlockState* below = level->getBlockState(pos.below());
    if (!here || !below) return false;
    return !isNamed(here, "minecraft:fallen_leaves")
        && (level->isEmptyBlock(pos) || isNamed(here, "minecraft:mayapple") || here->canBeReplaced())
        && (hasTag(below, "minecraft:substrate_overworld") || below->hasWaterFluid());   // #substrate_overworld / water
}

bool FallenLeavesFeature::place(FeaturePlaceContext<NoneFeatureConfiguration>& context) {
    WorldGenLevel* level = context.level();
    if (!level) return false;
    BlockState* leaves = minecraft::world::level::block::Blocks::getDefaultState("minecraft:fallen_leaves");
    if (!leaves || !leaves->hasProperty(BlockStateProperties::LAYERS)) return false;
    WorldgenRandom& random = context.random();
    const core::BlockPos& origin = context.origin();
    const core::BlockPos position(origin.getX(),
                                  level->getHeight(Heightmap::Types::WORLD_SURFACE_WG, origin.getX(), origin.getZ()),
                                  origin.getZ());

    if (!canPlace(position, level)) return false;
    BlockState* below = level->getBlockState(position.below());
    if (below && below->hasAnyFluid()) {
        generateFlatPileOnWater(level, position, random, leaves);
        return true;
    }
    const int startHeight = random.nextInt(6) + 1;
    level->setBlock(position, leaves->setValue(*BlockStateProperties::LAYERS, startHeight), kKnownShapeFlags);
    for (int i = 0; i < startHeight; ++i) {
        generateCircleOfLeaves(level, position, random, i, startHeight - i - 1, leaves);
        if (random.nextInt(3) == 0) ++i;
    }
    return true;
}

void FallenLeavesFeature::generateFlatPileOnWater(WorldGenLevel* level, const core::BlockPos& pos,
                                                  WorldgenRandom& random, BlockState* leaves) {
    for (int x = 0; x < 5; ++x) {
        for (int z = 0; z < 5; ++z) {
            if (random.nextInt(3) != 0) continue;
            bool found = false;
            int y = 2;
            do {
                if (canPlace(pos.offset(x, y, z), level)) { found = true; break; }
                --y;
            } while (y >= -2);
            if (!found) continue;
            const core::BlockPos finalPos = pos.offset(x, y, z);
            if (leaves->canSurvive(*level, finalPos)) level->setBlock(finalPos, leaves, kKnownShapeFlags);
        }
    }
}

void FallenLeavesFeature::generateCircleOfLeaves(WorldGenLevel* level, const core::BlockPos& origin,
                                                 WorldgenRandom& random, int radius, int height, BlockState* leaves) {
    for (int i1 = origin.getX() - radius; i1 <= origin.getX() + radius; ++i1) {
        for (int j1 = origin.getZ() - radius; j1 <= origin.getZ() + radius; ++j1) {
            const int k1 = i1 - origin.getX();
            const int l1 = j1 - origin.getZ();
            if (k1 * k1 + l1 * l1 <= radius * radius) {
                const int trueHeight = height - random.nextInt(3);
                if (trueHeight > 0) {
                    checkAndGenerateLeafPile(level, core::BlockPos(i1, origin.getY(), j1), trueHeight, leaves);
                }
            }
        }
    }
}

void FallenLeavesFeature::checkAndGenerateLeafPile(WorldGenLevel* level, const core::BlockPos& pos,
                                                   int pileLayer, BlockState* leaves) {
    bool found = false;
    int y = 0;
    do {
        if (canPlace(pos.offset(0, y, 0), level)) { found = true; break; }
        --y;
    } while (y >= -2);
    if (!found) return;
    const core::BlockPos finalPos = pos.offset(0, y, 0);
    if (leaves->canSurvive(*level, finalPos)) {
        level->setBlock(finalPos, leaves->setValue(*BlockStateProperties::LAYERS, pileLayer), kKnownShapeFlags);
    }
}

// ============================================================================
// UndergroundPlantFeature — feature/UndergroundPlantFeature.java
// ============================================================================
namespace {
// TorchberryPlantBlock.canSurvive = TFPlantBlock.canPlaceRootAt: the block
// above is in #twilightforest:plants_hang_on (#substrate_overworld, moss
// block, mangrove_root, root, liveroot_block). The engine registers the
// plant as a plain bush, whose own canSurvive looks down, so the hanging
// rule is applied here.
bool canPlantSurvive(BlockState* state, WorldGenLevel& level, const core::BlockPos& pos) {
    if (isNamed(state, "minecraft:torchberry_plant")) {
        BlockState* above = level.getBlockState(pos.above());
        return hasTag(above, "minecraft:substrate_overworld") || isNamed(above, "minecraft:moss_block")
            || isNamed(above, "minecraft:mangrove_roots") || isNamed(above, "minecraft:root")
            || isNamed(above, "minecraft:liveroot_block");
    }
    return state->canSurvive(level, pos);
}
} // namespace

bool UndergroundPlantFeature::place(FeaturePlaceContext<BlockStateConfiguration>& context) {
    WorldGenLevel* world = context.level();
    BlockState* state = context.config().state;
    if (!world || !state) return false;
    WorldgenRandom& random = context.random();
    const core::BlockPos& origin = context.origin();

    int x = origin.getX();
    int z = origin.getZ();
    int placed = 0;
    for (int y = origin.getY(); y > world->getMinY(); --y) {
        const core::BlockPos pos(x, y, z);
        if (!world->isEmptyBlock(pos) || random.nextInt(6) == 0) {
            // origin + nextInt(4) - nextInt(4), sequenced as Java evaluates it
            int dx = random.nextInt(4);
            dx -= random.nextInt(4);
            int dz = random.nextInt(4);
            dz -= random.nextInt(4);
            x = origin.getX() + dx;
            z = origin.getZ() + dz;
            continue;
        }
        if (canPlantSurvive(state, *world, pos)) {
            world->setBlock(pos, state, kKnownShapeFlags);
            ++placed;
        }
    }
    return placed > 0;
}

// ============================================================================
// Registry
// ============================================================================

bool TwilightFeatures::s_initialized = false;

ConfiguredFeature* TwilightFeatures::TWILIGHT_OAK_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::LARGE_TWILIGHT_OAK_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::SWAMPY_OAK_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::OAK_BUSH = nullptr;
ConfiguredFeature* TwilightFeatures::CANOPY_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::FIREFLY_CANOPY_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::DEAD_CANOPY_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::MANGROVE_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::DARKWOOD_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_FOREST_OAK_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_FOREST_BIRCH_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_OAK_BUSH = nullptr;
ConfiguredFeature* TwilightFeatures::VANILLA_OAK_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::VANILLA_BIRCH_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::RAINBOW_OAK = nullptr;
ConfiguredFeature* TwilightFeatures::LARGE_RAINBOW_OAK = nullptr;
ConfiguredFeature* TwilightFeatures::MEGA_SPRUCE_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::SNOWY_SPRUCE_TREE = nullptr;
ConfiguredFeature* TwilightFeatures::CANOPY_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::DENSE_CANOPY_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::FIREFLY_FOREST_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::ENCHANTED_FOREST_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::HIGHLANDS_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::SNOWY_FOREST_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::VANILLA_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_FOREST_TREES = nullptr;
ConfiguredFeature* TwilightFeatures::VANILLA_MUSHROOMS = nullptr;
ConfiguredFeature* TwilightFeatures::BROWN_CANOPY_MUSHROOM = nullptr;
ConfiguredFeature* TwilightFeatures::RED_CANOPY_MUSHROOM = nullptr;
ConfiguredFeature* TwilightFeatures::CANOPY_MUSHROOMS_DENSE = nullptr;
ConfiguredFeature* TwilightFeatures::CANOPY_MUSHROOMS_SPARSE = nullptr;
ConfiguredFeature* TwilightFeatures::MUSHGLOOM_CLUSTER = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_MUSHGLOOMS = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_BROWN_MUSHROOMS = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_RED_MUSHROOMS = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_DEAD_BUSHES = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_PUMPKINS = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_GRASS = nullptr;
ConfiguredFeature* TwilightFeatures::DARK_FERNS = nullptr;
ConfiguredFeature* TwilightFeatures::FIDDLEHEAD = nullptr;
ConfiguredFeature* TwilightFeatures::MAYAPPLE = nullptr;
ConfiguredFeature* TwilightFeatures::FLOWER_PLACER = nullptr;
ConfiguredFeature* TwilightFeatures::FLOWER_PLACER_ALT = nullptr;
ConfiguredFeature* TwilightFeatures::DENSE_FERNS = nullptr;
ConfiguredFeature* TwilightFeatures::DENSE_LARGE_FERNS = nullptr;
ConfiguredFeature* TwilightFeatures::GRASS = nullptr;
ConfiguredFeature* TwilightFeatures::GRASS_JUNGLE = nullptr;
ConfiguredFeature* TwilightFeatures::TAIGA_GRASS = nullptr;
ConfiguredFeature* TwilightFeatures::TORCH_BERRIES = nullptr;
ConfiguredFeature* TwilightFeatures::FALLEN_LEAVES = nullptr;
ConfiguredFeature* TwilightFeatures::WATER_LAKE = nullptr;
ConfiguredFeature* TwilightFeatures::LAVA_LAKE = nullptr;
ConfiguredFeature* TwilightFeatures::WATER_FROZEN = nullptr;
ConfiguredFeature* TwilightFeatures::LEGACY_COAL_ORE = nullptr;
ConfiguredFeature* TwilightFeatures::LEGACY_IRON_ORE = nullptr;
ConfiguredFeature* TwilightFeatures::LEGACY_GOLD_ORE = nullptr;
ConfiguredFeature* TwilightFeatures::LEGACY_REDSTONE_ORE = nullptr;
ConfiguredFeature* TwilightFeatures::LEGACY_DIAMOND_ORE = nullptr;
ConfiguredFeature* TwilightFeatures::LEGACY_LAPIS_ORE = nullptr;
ConfiguredFeature* TwilightFeatures::LEGACY_COPPER_ORE = nullptr;
ConfiguredFeature* TwilightFeatures::SMALL_ANDESITE = nullptr;
ConfiguredFeature* TwilightFeatures::SMALL_DIORITE = nullptr;
ConfiguredFeature* TwilightFeatures::SMALL_GRANITE = nullptr;

// Owned storage (unique_ptr / shared_ptr: raw pointers handed out stay valid
// when the vectors grow)
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<PlacedFeature>> s_inlinePlaced;
static std::vector<std::unique_ptr<TreeConfiguration>> s_treeConfigs;
static std::vector<std::unique_ptr<RandomFeatureConfiguration>> s_randomConfigs;
static std::vector<std::shared_ptr<BlockStateProvider>> s_stateProviders;

static std::shared_ptr<TreeFeature> s_treeFeature;
static std::unique_ptr<DarkCanopyTreeFeature> s_darkCanopyTreeFeature;
static RandomSelectorFeature s_randomSelectorFeature;
static RandomBooleanSelectorFeature s_randomBooleanSelectorFeature;
static NoOpFeature s_noOpFeature;
static SimpleBlockFeature s_simpleBlockFeature;
static OreFeature s_oreFeature;
static CanopyMushroomFeature s_brownCanopyMushroom(CanopyMushroomFeature::Kind::BROWN);
static CanopyMushroomFeature s_redVanillaCanopyMushroom(CanopyMushroomFeature::Kind::RED_VANILLA);
static CanopyMushroomFeature s_redSmoothCanopyMushroom(CanopyMushroomFeature::Kind::RED_SMOOTH);
static CanopyMushroomFeature s_redSpheroidCanopyMushroom(CanopyMushroomFeature::Kind::RED_SPHEROID);
static CanopyMushroomFeature s_redFlatCanopyMushroom(CanopyMushroomFeature::Kind::RED_FLAT);
static WeightedListFeature s_weightedListFeature;
static TFSmallLakeFeature s_smallLakeFeature;
static FallenLeavesFeature s_fallenLeavesFeature;
static UndergroundPlantFeature s_undergroundPlantFeature;

void TwilightFeatures::bootstrap() {
    if (s_initialized) return;

    // Runs inside the shared BiomeFeatureRegistry::bootstrap() (every
    // dimension), so a missing TF block never throws: that feature stays
    // null, TwilightPlacements skips it and addFeature warns.
    auto block = [](const char* name, const char* feature) -> BlockState* {
        BlockState* state = minecraft::world::level::block::Blocks::getDefaultState(name);
        if (!state) {
            fprintf(stderr, "[TwilightFeatures] %s missing from the block registry - %s skipped\n",
                    name, feature);
        }
        return state;
    };

    auto own = [](std::unique_ptr<ConfiguredFeature> feature) -> ConfiguredFeature* {
        ConfiguredFeature* raw = feature.get();
        s_features.push_back(std::move(feature));
        return raw;
    };

    auto simpleProvider = [](BlockState* state) -> std::shared_ptr<BlockStateProvider> {
        auto provider = std::make_shared<SimpleStateProvider>(state);
        s_stateProviders.push_back(provider);
        return provider;
    };

    auto weightedProvider = [](std::vector<WeightedStateEntry> entries) -> std::shared_ptr<BlockStateProvider> {
        auto provider = std::make_shared<WeightedStateProvider>(entries);
        s_stateProviders.push_back(provider);
        return provider;
    };

    auto constantInt = [](int32_t value) -> std::shared_ptr<carver::IntProvider> {
        return std::make_shared<carver::ConstantInt>(value);
    };

    // An inline placed feature ({"feature": ..., "placement": []}).
    auto inlinePlaced = [](ConfiguredFeature* feature, const std::string& name) -> PlacedFeature* {
        if (!feature) return nullptr;
        auto placed = std::make_unique<PlacedFeature>(feature, std::vector<PlacementModifier*>{}, name);
        PlacedFeature* raw = placed.get();
        s_inlinePlaced.push_back(std::move(placed));
        return raw;
    };

    s_treeFeature = std::make_shared<TreeFeature>();
    s_darkCanopyTreeFeature = std::make_unique<DarkCanopyTreeFeature>(s_treeFeature);

    // ------------------------------------------------ shared tree pieces
    BlockState* root = block("minecraft:root", "tree roots");
    BlockState* liveroot = block("minecraft:liveroot_block", "tree roots");
    // twilightforest:tree_roots with ground roots root:6 / liveroot_block:1
    // (weights 4/1 for the mangrove), strands 3 + [0,1], length 5, y 0,
    // penetrability 1.
    auto treeRoots = [&](int rootWeight, int length,
                         std::shared_ptr<BlockStateProvider> exposed) -> std::shared_ptr<TreeDecorator> {
        if (!root || !liveroot) return nullptr;
        auto ground = weightedProvider({WeightedStateEntry(root, rootWeight), WeightedStateEntry(liveroot, 1)});
        return std::make_shared<TreeRootsDecorator>(3, 1, length, 0, std::move(exposed), ground, 1);
    };
    // twilightforest:trunkside_decorator with a firefly (facing is set per
    // placement, so the JSON's facing is irrelevant).
    BlockState* firefly = block("minecraft:firefly", "firefly trunk decorators");
    auto fireflies = [&](int count, float probability) -> std::shared_ptr<TreeDecorator> {
        if (!firefly) return nullptr;
        return std::make_shared<TrunkSideDecorator>(count, probability, simpleProvider(firefly));
    };

    // A minecraft:tree (or twilightforest:dark_canopy_tree) configured feature.
    auto tree = [&](BlockState* log, BlockState* leaves,
                    std::shared_ptr<TrunkPlacer> trunkPlacer,
                    std::shared_ptr<FoliagePlacer> foliagePlacer,
                    std::shared_ptr<levelgen::feature::featuresize::FeatureSize> featureSize,
                    std::vector<std::shared_ptr<TreeDecorator>> decorators,
                    bool ignoreVines, bool darkCanopy) -> ConfiguredFeature* {
        if (!log || !leaves) return nullptr;
        std::vector<std::shared_ptr<TreeDecorator>> present;
        for (auto& decorator : decorators) {
            if (decorator) present.push_back(std::move(decorator));
        }
        // below_trunk_provider "not #cannot_replace_below_tree_trunk -> dirt"
        // is the library's default dirt provider (setDirtAt skips dirt).
        TreeConfigurationBuilder builder(simpleProvider(log), std::move(trunkPlacer),
                                         simpleProvider(leaves), std::move(foliagePlacer),
                                         std::move(featureSize));
        builder.decorators(present);
        if (ignoreVines) builder.ignoreVines();
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        ConfiguredFeature* raw = nullptr;
        if (darkCanopy) {
            raw = own(std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, DarkCanopyTreeFeature>>(
                s_darkCanopyTreeFeature.get(), *config));
        } else {
            raw = own(std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
                s_treeFeature.get(), *config));
        }
        s_treeConfigs.push_back(std::move(config));
        return raw;
    };

    auto blob = [&](int radius, int offset, int height) -> std::shared_ptr<FoliagePlacer> {
        return std::make_shared<BlobFoliagePlacer>(constantInt(radius), constantInt(offset), height);
    };
    auto spheroid = [&](float hr, float vr, int addH, int addV, float bias, int shag) -> std::shared_ptr<FoliagePlacer> {
        return std::make_shared<LeafSpheroidFoliagePlacer>(hr, vr, constantInt(0), addH, addV, bias, shag);
    };

    BlockState* twilightOakLog = block("minecraft:twilight_oak_log", "twilight oak trees");
    BlockState* twilightOakLeaves = block("minecraft:twilight_oak_leaves", "twilight oak trees");
    BlockState* canopyLog = block("minecraft:canopy_log", "canopy trees");
    BlockState* canopyLeaves = block("minecraft:canopy_leaves", "canopy trees");
    BlockState* mangroveLog = block("minecraft:tf_mangrove_log", "mangrove tree");
    BlockState* mangroveLeaves = block("minecraft:tf_mangrove_leaves", "mangrove tree");
    BlockState* darkLog = block("minecraft:dark_log", "darkwood trees");
    BlockState* hardenedDarkLeaves = block("minecraft:hardened_dark_leaves", "darkwood tree");
    BlockState* air = block("minecraft:air", "dead canopy tree");
    BlockState* oakLog = block("minecraft:oak_log", "vanilla trees");
    BlockState* oakLeaves = block("minecraft:oak_leaves", "vanilla trees");
    BlockState* birchLog = block("minecraft:birch_log", "vanilla trees");
    BlockState* birchLeaves = block("minecraft:birch_leaves", "vanilla trees");
    BlockState* spruceLog = block("minecraft:spruce_log", "spruce trees");
    BlockState* spruceLeaves = block("minecraft:spruce_leaves", "spruce trees");

    // canopy_tree.json / firefly_canopy_tree.json / dead_canopy_tree.json:
    // branching_trunk_placer(20, 5, 5), branches canopy_wood x 3 + [0,1],
    // length 10, yaw spacing 0.3, pitch 0.2, start 12 down, no perpendicular
    // branches, exposed-root guard on.
    auto canopyTrunk = [&]() -> std::shared_ptr<TrunkPlacer> {
        if (!canopyLog) return nullptr;
        return std::make_shared<BranchingTrunkPlacer>(20, 5, 5, 12,
            BranchesConfig{simpleProvider(canopyLog) /* canopy_wood stand-in */, 3, 1, 10.0, 1.0, 0.3, 0.2},
            false, true);
    };
    // darkwood_tree.json: branching_trunk_placer(9, 1, 1), dark_wood x 4,
    // length 8, yaw 0.23, pitch 0.23, start 6 down.
    auto darkTrunk = [&]() -> std::shared_ptr<TrunkPlacer> {
        if (!darkLog) return nullptr;
        return std::make_shared<BranchingTrunkPlacer>(9, 1, 1, 6,
            BranchesConfig{simpleProvider(darkLog) /* dark_wood stand-in */, 4, 0, 8.0, 2.0, 0.23, 0.23},
            false, false);
    };

    // twilight_oak_tree.json: straight(4, 2, 0), blob(2, 0, 3), two_layers(1, 0, 1), roots.
    TWILIGHT_OAK_TREE = tree(twilightOakLog, twilightOakLeaves,
        std::make_shared<StraightTrunkPlacer>(4, 2, 0), blob(2, 0, 3),
        std::make_shared<TwoLayersFeatureSize>(1, 0, 1), {treeRoots(6, 5, nullptr)}, false, false);

    // large_twilight_oak_tree.json: fancy(3, 11, 0), fancy foliage(2, 4, 4),
    // two_layers(4, 1, 0, min clipped 4), roots.
    LARGE_TWILIGHT_OAK_TREE = tree(twilightOakLog, twilightOakLeaves,
        std::make_shared<FancyTrunkPlacer>(3, 11, 0),
        std::make_shared<FancyFoliagePlacer>(constantInt(2), constantInt(4), 4),
        std::make_shared<TwoLayersFeatureSize>(4, 1, 0, std::optional<int>(4)),
        {treeRoots(6, 5, nullptr)}, false, false);

    // swampy_oak_tree.json: the twilight oak plus leave_vine(0.125).
    SWAMPY_OAK_TREE = tree(twilightOakLog, twilightOakLeaves,
        std::make_shared<StraightTrunkPlacer>(4, 2, 0), blob(2, 0, 3),
        std::make_shared<TwoLayersFeatureSize>(1, 0, 1),
        {treeRoots(6, 5, nullptr), std::make_shared<LeaveVineDecorator>(0.125f)}, false, false);

    // oak_bush.json: oak log / leaves, straight(1, 0, 0), bush(2, 1, 2), two_layers(0, 0, 0).
    OAK_BUSH = tree(oakLog, oakLeaves, std::make_shared<StraightTrunkPlacer>(1, 0, 0),
        std::make_shared<BushFoliagePlacer>(constantInt(2), constantInt(1), 2),
        std::make_shared<TwoLayersFeatureSize>(0, 0, 0), {}, true, false);

    // canopy_tree.json: spheroid(4.1231055, 1.5, +0, +0, bias -0.2, shag 24),
    // two_layers(20, 0, 5); decorators trunkside firefly(2, 1.0), roots.
    CANOPY_TREE = tree(canopyLog, canopyLeaves, canopyTrunk(),
        spheroid(4.1231055f, 1.5f, 0, 0, -0.2f, 24),
        std::make_shared<TwoLayersFeatureSize>(20, 0, 5),
        {fireflies(2, 1.0f), treeRoots(6, 5, nullptr)}, true, false);

    // firefly_canopy_tree.json: as canopy_tree with two_layers(20, 1, 5) and
    // decorators roots, firefly(2, 1.0), firefly(4, 0.5), dangle(1 + [0,1],
    // min 2, length 5 + [0,15], rope rope:3 / iron_chain:1, baggage
    // firefly_jar:10 / cicada_jar:1).
    {
        std::shared_ptr<TreeDecorator> dangle;
        BlockState* chain = block("minecraft:iron_chain", "firefly canopy tree ropes");
        BlockState* lantern = block("minecraft:lantern", "firefly canopy tree jars");
        if (chain && lantern) {
            BlockState* hangingLantern = lantern->hasProperty(BlockStateProperties::HANGING)
                ? lantern->setValue(*BlockStateProperties::HANGING, true) : lantern;
            dangle = std::make_shared<DangleFromTreeDecorator>(1, 1, 2, 5, 15,
                weightedProvider({WeightedStateEntry(chain, 3) /* rope stand-in */, WeightedStateEntry(chain, 1)}),
                weightedProvider({WeightedStateEntry(hangingLantern, 10) /* firefly_jar stand-in */,
                                  WeightedStateEntry(hangingLantern, 1) /* cicada_jar stand-in */}));
        }
        FIREFLY_CANOPY_TREE = tree(canopyLog, canopyLeaves, canopyTrunk(),
            spheroid(4.1231055f, 1.5f, 0, 0, -0.2f, 24),
            std::make_shared<TwoLayersFeatureSize>(20, 1, 5),
            {treeRoots(6, 5, nullptr), fireflies(2, 1.0f), fireflies(4, 0.5f), dangle}, true, false);
    }

    // dead_canopy_tree.json: the canopy trunk with air foliage (spheroid of
    // radius 0), firefly(2, 1.0), roots.
    DEAD_CANOPY_TREE = tree(canopyLog, air, canopyTrunk(),
        spheroid(0.0f, 0.0f, 0, 0, 0.0f, 0),
        std::make_shared<TwoLayersFeatureSize>(20, 0, 5),
        {fireflies(2, 1.0f), treeRoots(6, 5, nullptr)}, true, false);

    // mangrove_tree.json: trunk_mover_upper(4, branching(7, 4, 0), mangrove_wood
    // x 0 + [0,3], length 6, yaw 0.3, pitch 0.25, start 6 down); spheroid
    // (2.5, 1.5, +2, +0, bias -0.25, shag 15); two_layers(4, 1, 1); decorators
    // firefly(2, 1.0), roots with exposed mangrove_root (root:4 /
    // liveroot:1, length 12), leave_vine(0.125).
    if (mangroveLog) {
        auto inner = std::make_shared<BranchingTrunkPlacer>(7, 4, 0, 6,
            BranchesConfig{simpleProvider(mangroveLog) /* mangrove_wood stand-in */, 0, 3, 6.0, 2.0, 0.3, 0.25},
            false, false);
        BlockState* mangroveRoots = block("minecraft:mangrove_roots", "mangrove exposed roots");
        MANGROVE_TREE = tree(mangroveLog, mangroveLeaves,
            std::make_shared<TrunkRiser>(4, 7, 4, 0, inner),
            spheroid(2.5f, 1.5f, 2, 0, -0.25f, 15),
            std::make_shared<TwoLayersFeatureSize>(4, 1, 1),
            {fireflies(2, 1.0f),
             treeRoots(4, 12, mangroveRoots ? simpleProvider(mangroveRoots) : nullptr),
             std::make_shared<LeaveVineDecorator>(0.125f)},
            false, false);
    }

    // darkwood_tree.json (dark_canopy_tree): spheroid(4.5, 2.25, +1, +0,
    // bias 0.45, shag 36) of hardened_dark_leaves, two_layers(4, 1, 1), roots.
    DARKWOOD_TREE = tree(darkLog, hardenedDarkLeaves, darkTrunk(),
        spheroid(4.5f, 2.25f, 1, 0, 0.45f, 36),
        std::make_shared<TwoLayersFeatureSize>(4, 1, 1), {treeRoots(6, 5, nullptr)}, true, true);

    // dark_forest_oak_tree / dark_forest_birch_tree / dark_oak_bush.json (dark_canopy_tree)
    DARK_FOREST_OAK_TREE = tree(oakLog, oakLeaves, std::make_shared<StraightTrunkPlacer>(4, 2, 0),
        blob(2, 0, 3), std::make_shared<TwoLayersFeatureSize>(1, 0, 1), {}, true, true);
    DARK_FOREST_BIRCH_TREE = tree(birchLog, birchLeaves, std::make_shared<StraightTrunkPlacer>(5, 2, 0),
        blob(2, 0, 3), std::make_shared<TwoLayersFeatureSize>(1, 0, 1), {}, true, true);
    DARK_OAK_BUSH = tree(oakLog, oakLeaves, std::make_shared<StraightTrunkPlacer>(1, 0, 0),
        std::make_shared<BushFoliagePlacer>(constantInt(2), constantInt(1), 2),
        std::make_shared<TwoLayersFeatureSize>(0, 0, 0), {}, true, true);

    // vanilla_oak_tree / vanilla_birch_tree.json
    VANILLA_OAK_TREE = tree(oakLog, oakLeaves, std::make_shared<StraightTrunkPlacer>(4, 2, 0),
        blob(2, 0, 3), std::make_shared<TwoLayersFeatureSize>(1, 0, 1), {}, true, false);
    VANILLA_BIRCH_TREE = tree(birchLog, birchLeaves, std::make_shared<StraightTrunkPlacer>(5, 2, 0),
        blob(2, 0, 3), std::make_shared<TwoLayersFeatureSize>(1, 0, 1), {}, true, false);

    // rainbow_oak / large_rainbow_oak.json — rainbow_oak_leaves stand-in:
    // twilight_oak_leaves.
    RAINBOW_OAK = tree(twilightOakLog, twilightOakLeaves, std::make_shared<StraightTrunkPlacer>(4, 2, 0),
        blob(2, 0, 3), std::make_shared<TwoLayersFeatureSize>(1, 1, 1), {treeRoots(6, 5, nullptr)}, false, false);
    LARGE_RAINBOW_OAK = tree(twilightOakLog, twilightOakLeaves, std::make_shared<FancyTrunkPlacer>(3, 11, 0),
        std::make_shared<FancyFoliagePlacer>(constantInt(2), constantInt(4), 4),
        std::make_shared<TwoLayersFeatureSize>(4, 1, 0, std::optional<int>(4)),
        {treeRoots(6, 5, nullptr)}, false, false);

    // mega_spruce_tree.json: giant(13, 2, 14), mega_pine(radius 0, offset 0,
    // crown uniform 13..17), two_layers(4, 1, 2); no decorators.
    MEGA_SPRUCE_TREE = tree(spruceLog, spruceLeaves, std::make_shared<GiantTrunkPlacer>(13, 2, 14),
        std::make_shared<MegaPineFoliagePlacer>(constantInt(0), constantInt(0),
                                                std::make_shared<carver::UniformInt>(13, 17)),
        std::make_shared<TwoLayersFeatureSize>(4, 1, 2), {}, false, false);

    // snowy_spruce_tree.json (twilightforest:anywhere_tree = SnowTreeFeature,
    // a [VanillaCopy] of TreeFeature): straight(5, 2, 1), spruce foliage
    // (radius 2..3, offset 0..2, trunk height 1..2), two_layers(2, 0, 2).
    SNOWY_SPRUCE_TREE = tree(spruceLog, spruceLeaves, std::make_shared<StraightTrunkPlacer>(5, 2, 1),
        std::make_shared<SpruceFoliagePlacer>(std::make_shared<carver::UniformInt>(2, 3),
                                              std::make_shared<carver::UniformInt>(0, 2),
                                              std::make_shared<carver::UniformInt>(1, 2)),
        std::make_shared<TwoLayersFeatureSize>(2, 0, 2), {}, true, false);

    // ---------------------------------------------------- tree selectors
    // minecraft:random_selector: entries in file order, then the default.
    auto selector = [&](std::vector<std::pair<ConfiguredFeature*, float>> entries,
                        ConfiguredFeature* fallback, const std::string& name) -> ConfiguredFeature* {
        if (!fallback) return nullptr;
        std::vector<WeightedPlacedFeature> weighted;
        int index = 0;
        for (const auto& entry : entries) {
            // A null entry is kept (its chance still draws); selecting it
            // places nothing, as the library's selector treats a null feature.
            weighted.emplace_back(inlinePlaced(entry.first, name + "_" + std::to_string(index++)), entry.second);
        }
        auto config = std::make_unique<RandomFeatureConfiguration>(
            std::move(weighted), inlinePlaced(fallback, name + "_default"));
        ConfiguredFeature* raw = own(std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
            &s_randomSelectorFeature, *config));
        s_randomConfigs.push_back(std::move(config));
        return raw;
    };

    CANOPY_TREES = selector({{CANOPY_TREE, 0.6f}}, TWILIGHT_OAK_TREE, "canopy_trees");
    DENSE_CANOPY_TREES = selector({{CANOPY_TREE, 0.7f}}, TWILIGHT_OAK_TREE, "dense_canopy_trees");
    FIREFLY_FOREST_TREES = selector({{CANOPY_TREE, 0.33f}, {FIREFLY_CANOPY_TREE, 0.45f}},
                                    TWILIGHT_OAK_TREE, "firefly_forest_trees");
    ENCHANTED_FOREST_TREES = selector({{VANILLA_OAK_TREE, 0.15f}, {VANILLA_BIRCH_TREE, 0.15f},
                                       {LARGE_RAINBOW_OAK, 0.15f}}, RAINBOW_OAK, "enchanted_forest_trees");
    HIGHLANDS_TREES = selector({{VANILLA_BIRCH_TREE, 0.25f}, {TreeFeatures::SPRUCE, 0.25f},
                                {TreeFeatures::PINE, 0.1f}}, MEGA_SPRUCE_TREE, "highlands_trees");
    // large_winter_tree (twilightforest:large_winter_tree) is pass two; its
    // 0.125 entry is kept with the mega spruce in its place.
    SNOWY_FOREST_TREES = selector({{MEGA_SPRUCE_TREE, 0.33f}, {MEGA_SPRUCE_TREE, 0.125f}},
                                  SNOWY_SPRUCE_TREE, "snowy_forest_trees");
    VANILLA_TREES = selector({{VANILLA_BIRCH_TREE, 0.25f}, {VANILLA_OAK_TREE, 0.25f}},
                             TWILIGHT_OAK_TREE, "vanilla_trees");
    DARK_FOREST_TREES = selector({{DARK_FOREST_BIRCH_TREE, 0.2f}, {DARK_FOREST_OAK_TREE, 0.2f},
                                  {DARK_OAK_BUSH, 0.4f}}, DARKWOOD_TREE, "dark_forest_trees");

    // tree/selector/vanilla/vanilla_mushrooms.json: random_boolean_selector
    // (true: huge_red_mushroom, false: huge_brown_mushroom).
    if (VegetationFeatures::HUGE_RED_MUSHROOM && VegetationFeatures::HUGE_BROWN_MUSHROOM) {
        RandomBooleanFeatureConfiguration config(
            inlinePlaced(VegetationFeatures::HUGE_RED_MUSHROOM, "vanilla_mushrooms_true"),
            inlinePlaced(VegetationFeatures::HUGE_BROWN_MUSHROOM, "vanilla_mushrooms_false"));
        VANILLA_MUSHROOMS = own(std::make_unique<ConfiguredFeatureImpl<RandomBooleanFeatureConfiguration, RandomBooleanSelectorFeature>>(
            &s_randomBooleanSelectorFeature, config));
    }

    // ------------------------------------------------------ mushrooms
    // cap: down=false, the rest true; stem: up=false, down=false, sides true.
    auto mushroomState = [](BlockState* state, bool up) -> BlockState* {
        if (!state) return nullptr;
        if (state->hasProperty(BlockStateProperties::DOWN)) state = state->setValue(*BlockStateProperties::DOWN, false);
        if (state->hasProperty(BlockStateProperties::UP)) state = state->setValue(*BlockStateProperties::UP, up);
        return state;
    };
    BlockState* stem = mushroomState(block("minecraft:mushroom_stem", "canopy mushrooms"), false);
    BlockState* brownCap = mushroomState(block("minecraft:brown_mushroom_block", "brown canopy mushroom"), true);
    BlockState* redCap = mushroomState(block("minecraft:red_mushroom_block", "red canopy mushrooms"), true);
    auto canopyMushroom = [&](CanopyMushroomFeature* feature, BlockState* cap) -> ConfiguredFeature* {
        if (!stem || !cap) return nullptr;
        return own(std::make_unique<ConfiguredFeatureImpl<HugeMushroomFeatureConfiguration, CanopyMushroomFeature>>(
            feature, HugeMushroomFeatureConfiguration(simpleProvider(cap), simpleProvider(stem), 3)));
    };
    BROWN_CANOPY_MUSHROOM = canopyMushroom(&s_brownCanopyMushroom, brownCap);
    // red_canopy_mushroom.json: twilightforest:weighted_list vanilla:33,
    // smooth:33, spheroid:33, flat:1.
    {
        ConfiguredFeature* vanillaCap = canopyMushroom(&s_redVanillaCanopyMushroom, redCap);
        ConfiguredFeature* smoothCap = canopyMushroom(&s_redSmoothCanopyMushroom, redCap);
        ConfiguredFeature* spheroidCap = canopyMushroom(&s_redSpheroidCanopyMushroom, redCap);
        ConfiguredFeature* flatCap = canopyMushroom(&s_redFlatCanopyMushroom, redCap);
        if (vanillaCap && smoothCap && spheroidCap && flatCap) {
            WeightedListFeatureConfig config;
            config.entries = {
                {inlinePlaced(vanillaCap, "canopy_red_vanilla_mushroom"), 33},
                {inlinePlaced(smoothCap, "canopy_red_smooth_mushroom"), 33},
                {inlinePlaced(spheroidCap, "canopy_red_spheroid_mushroom"), 33},
                {inlinePlaced(flatCap, "canopy_red_flat_mushroom"), 1}};
            RED_CANOPY_MUSHROOM = own(std::make_unique<ConfiguredFeatureImpl<WeightedListFeatureConfig, WeightedListFeature>>(
                &s_weightedListFeature, config));
        }
    }
    // canopy_mushrooms_{dense,sparse}.json: random_selector brown / red,
    // default tree/dummy (minecraft:no_op).
    ConfiguredFeature* dummy = own(std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, NoOpFeature>>(
        &s_noOpFeature, NoneFeatureConfiguration::INSTANCE));
    CANOPY_MUSHROOMS_DENSE = selector({{BROWN_CANOPY_MUSHROOM, 0.675f}, {RED_CANOPY_MUSHROOM, 0.225f}},
                                      dummy, "canopy_mushrooms_dense");
    CANOPY_MUSHROOMS_SPARSE = selector({{BROWN_CANOPY_MUSHROOM, 0.15f}, {RED_CANOPY_MUSHROOM, 0.05f}},
                                       dummy, "canopy_mushrooms_sparse");

    // ---------------------------------------------------------- flora
    auto simpleBlock = [&](std::shared_ptr<BlockStateProvider> provider) -> ConfiguredFeature* {
        if (!provider) return nullptr;
        return own(std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlockFeature, SimpleBlockConfiguration(provider.get(), false)));
    };
    auto simpleOf = [&](const char* name, const char* feature) -> ConfiguredFeature* {
        BlockState* state = block(name, feature);
        return state ? simpleBlock(simpleProvider(state)) : nullptr;
    };
    MUSHGLOOM_CLUSTER    = simpleOf("minecraft:mushgloom", "mushgloom_cluster");
    DARK_MUSHGLOOMS      = simpleOf("minecraft:mushgloom", "dark_mushglooms");
    DARK_BROWN_MUSHROOMS = simpleOf("minecraft:brown_mushroom", "dark_brown_mushrooms");
    DARK_RED_MUSHROOMS   = simpleOf("minecraft:red_mushroom", "dark_red_mushrooms");
    DARK_DEAD_BUSHES     = simpleOf("minecraft:dead_bush", "dark_dead_bushes");
    DARK_PUMPKINS        = simpleOf("minecraft:pumpkin", "dark_pumpkins");
    DARK_GRASS           = simpleOf("minecraft:short_grass", "dark_grass");
    DARK_FERNS           = simpleOf("minecraft:fern", "dark_ferns");
    FIDDLEHEAD           = simpleOf("minecraft:fiddlehead", "fiddlehead");
    MAYAPPLE             = simpleOf("minecraft:mayapple", "mayapple");
    DENSE_FERNS          = simpleOf("minecraft:fern", "dense_ferns");
    DENSE_LARGE_FERNS    = simpleOf("minecraft:large_fern", "dense_large_ferns");   // half=lower default
    GRASS                = simpleOf("minecraft:short_grass", "grass");

    // flower_placer(_alt).json: noise_provider seed 2345, noise (firstOctave
    // 0, amplitudes [1.0]), scale 0.020833334, twelve flowers.
    auto flowerPlacer = [&](const std::vector<const char*>& names, const char* feature) -> ConfiguredFeature* {
        std::vector<BlockState*> states;
        for (const char* name : names) {
            BlockState* state = block(name, feature);
            if (!state) return nullptr;
            states.push_back(state);
        }
        auto provider = std::make_shared<NoiseProvider>(
            2345, NormalNoise::NoiseParameters(0, std::vector<double>{1.0}), 0.020833334f, states);
        s_stateProviders.push_back(provider);
        return simpleBlock(provider);
    };
    FLOWER_PLACER = flowerPlacer({"minecraft:poppy", "minecraft:dandelion", "minecraft:red_tulip",
        "minecraft:orange_tulip", "minecraft:pink_tulip", "minecraft:white_tulip", "minecraft:cornflower",
        "minecraft:lily_of_the_valley", "minecraft:blue_orchid", "minecraft:allium", "minecraft:azure_bluet",
        "minecraft:oxeye_daisy"}, "flower_placer");
    FLOWER_PLACER_ALT = flowerPlacer({"minecraft:white_tulip", "minecraft:pink_tulip", "minecraft:orange_tulip",
        "minecraft:red_tulip", "minecraft:dandelion", "minecraft:poppy", "minecraft:oxeye_daisy",
        "minecraft:azure_bluet", "minecraft:allium", "minecraft:blue_orchid", "minecraft:lily_of_the_valley",
        "minecraft:cornflower"}, "flower_placer_alt");

    // 26.1 VegetationFeatures GRASS_JUNGLE (short_grass:3, fern:1) and
    // TAIGA_GRASS (short_grass:1, fern:4).
    {
        BlockState* shortGrass = block("minecraft:short_grass", "grass patches");
        BlockState* fern = block("minecraft:fern", "grass patches");
        if (shortGrass && fern) {
            GRASS_JUNGLE = simpleBlock(weightedProvider({WeightedStateEntry(shortGrass, 3), WeightedStateEntry(fern, 1)}));
            TAIGA_GRASS = simpleBlock(weightedProvider({WeightedStateEntry(shortGrass, 1), WeightedStateEntry(fern, 4)}));
        }
    }

    // ----------------------------------------------------------- misc
    // torch_berries.json: underground_plants, torchberry_plant
    // (has_torchberries=true is not a property of the engine's plant).
    if (BlockState* torchberry = block("minecraft:torchberry_plant", "torch_berries")) {
        TORCH_BERRIES = own(std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, UndergroundPlantFeature>>(
            &s_undergroundPlantFeature, BlockStateConfiguration(torchberry)));
    }
    if (block("minecraft:fallen_leaves", "fallen_leaves")) {
        FALLEN_LEAVES = own(std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, FallenLeavesFeature>>(
            &s_fallenLeavesFeature, NoneFeatureConfiguration::INSTANCE));
    }
    // water_lake.json / lava_lake.json (barrier stone) / water_frozen.json (ice)
    {
        BlockState* water = block("minecraft:water", "small lakes");
        BlockState* lava = block("minecraft:lava", "lava_lake");
        BlockState* stone = block("minecraft:stone", "lava_lake");
        BlockState* ice = block("minecraft:ice", "water_frozen");
        auto lake = [&](BlockState* fluid, BlockState* barrier, BlockState* iceState) -> ConfiguredFeature* {
            if (!fluid) return nullptr;
            SmallLakeConfiguration config;
            config.fluid = fluid;
            config.barrier = barrier;
            config.ice = iceState;
            return own(std::make_unique<ConfiguredFeatureImpl<SmallLakeConfiguration, TFSmallLakeFeature>>(
                &s_smallLakeFeature, config));
        };
        WATER_LAKE = lake(water, nullptr, nullptr);
        LAVA_LAKE = stone ? lake(lava, stone, nullptr) : nullptr;
        WATER_FROZEN = ice ? lake(water, nullptr, ice) : nullptr;
    }

    // ----------------------------------------------------------- ores
    // minecraft:ore, one target in #minecraft:stone_ore_replaceables, discard 0.
    auto ore = [&](const char* oreBlock, int32_t size) -> ConfiguredFeature* {
        BlockState* state = block(oreBlock, oreBlock);
        if (!state) return nullptr;
        std::shared_ptr<RuleTest> target = std::make_shared<TagMatchTest>("minecraft:stone_ore_replaceables");
        std::vector<OreConfiguration::TargetBlockState> targets = {OreConfiguration::target(target, state)};
        return own(std::make_unique<ConfiguredFeatureImpl<OreConfiguration, OreFeature>>(
            &s_oreFeature, OreConfiguration(targets, size, 0.0f)));
    };
    LEGACY_COAL_ORE     = ore("minecraft:coal_ore", 16);
    LEGACY_IRON_ORE     = ore("minecraft:iron_ore", 9);
    LEGACY_GOLD_ORE     = ore("minecraft:gold_ore", 9);
    LEGACY_REDSTONE_ORE = ore("minecraft:redstone_ore", 8);
    LEGACY_DIAMOND_ORE  = ore("minecraft:diamond_ore", 8);
    LEGACY_LAPIS_ORE    = ore("minecraft:lapis_ore", 7);
    LEGACY_COPPER_ORE   = ore("minecraft:copper_ore", 10);
    SMALL_ANDESITE      = ore("minecraft:andesite", 16);
    SMALL_DIORITE       = ore("minecraft:diorite", 16);
    SMALL_GRANITE       = ore("minecraft:granite", 16);

    // Pass two: every configured feature under its JSON id, for the
    // placed-feature JSON loader (TwilightPlacements). Null ones (a missing
    // block) are skipped by registerConfigured.
    {
        using twilight::registerConfigured;
        registerConfigured("twilightforest:tree/twilight_oak_tree", TWILIGHT_OAK_TREE);
        registerConfigured("twilightforest:tree/large_twilight_oak_tree", LARGE_TWILIGHT_OAK_TREE);
        registerConfigured("twilightforest:tree/swampy_oak_tree", SWAMPY_OAK_TREE);
        registerConfigured("twilightforest:tree/oak_bush", OAK_BUSH);
        registerConfigured("twilightforest:tree/canopy_tree", CANOPY_TREE);
        registerConfigured("twilightforest:tree/firefly_canopy_tree", FIREFLY_CANOPY_TREE);
        registerConfigured("twilightforest:tree/dead_canopy_tree", DEAD_CANOPY_TREE);
        registerConfigured("twilightforest:tree/mangrove_tree", MANGROVE_TREE);
        registerConfigured("twilightforest:tree/darkwood_tree", DARKWOOD_TREE);
        registerConfigured("twilightforest:tree/dark_forest_oak_tree", DARK_FOREST_OAK_TREE);
        registerConfigured("twilightforest:tree/dark_forest_birch_tree", DARK_FOREST_BIRCH_TREE);
        registerConfigured("twilightforest:tree/dark_oak_bush", DARK_OAK_BUSH);
        registerConfigured("twilightforest:tree/vanilla_oak_tree", VANILLA_OAK_TREE);
        registerConfigured("twilightforest:tree/vanilla_birch_tree", VANILLA_BIRCH_TREE);
        registerConfigured("twilightforest:tree/rainbow_oak", RAINBOW_OAK);
        registerConfigured("twilightforest:tree/large_rainbow_oak", LARGE_RAINBOW_OAK);
        registerConfigured("twilightforest:tree/mega_spruce_tree", MEGA_SPRUCE_TREE);
        registerConfigured("twilightforest:tree/snowy_spruce_tree", SNOWY_SPRUCE_TREE);
        registerConfigured("twilightforest:tree/selector/canopy_trees", CANOPY_TREES);
        registerConfigured("twilightforest:tree/selector/dense_canopy_trees", DENSE_CANOPY_TREES);
        registerConfigured("twilightforest:tree/selector/firefly_forest_trees", FIREFLY_FOREST_TREES);
        registerConfigured("twilightforest:tree/selector/enchanted_forest_trees", ENCHANTED_FOREST_TREES);
        registerConfigured("twilightforest:tree/selector/highlands_trees", HIGHLANDS_TREES);
        registerConfigured("twilightforest:tree/selector/snowy_forest_trees", SNOWY_FOREST_TREES);
        registerConfigured("twilightforest:tree/selector/vanilla_trees", VANILLA_TREES);
        registerConfigured("twilightforest:tree/selector/dark_forest_trees", DARK_FOREST_TREES);
        registerConfigured("twilightforest:tree/selector/vanilla/vanilla_mushrooms", VANILLA_MUSHROOMS);
        registerConfigured("twilightforest:mushroom/brown_canopy_mushroom", BROWN_CANOPY_MUSHROOM);
        registerConfigured("twilightforest:mushroom/red_canopy_mushroom", RED_CANOPY_MUSHROOM);
        registerConfigured("twilightforest:mushroom/canopy_mushrooms_dense", CANOPY_MUSHROOMS_DENSE);
        registerConfigured("twilightforest:mushroom/canopy_mushrooms_sparse", CANOPY_MUSHROOMS_SPARSE);
        registerConfigured("twilightforest:mushgloom_cluster", MUSHGLOOM_CLUSTER);
        registerConfigured("twilightforest:dark_mushglooms", DARK_MUSHGLOOMS);
        registerConfigured("twilightforest:dark_brown_mushrooms", DARK_BROWN_MUSHROOMS);
        registerConfigured("twilightforest:dark_red_mushrooms", DARK_RED_MUSHROOMS);
        registerConfigured("twilightforest:dark_dead_bushes", DARK_DEAD_BUSHES);
        registerConfigured("twilightforest:dark_pumpkins", DARK_PUMPKINS);
        registerConfigured("twilightforest:dark_grass", DARK_GRASS);
        registerConfigured("twilightforest:dark_ferns", DARK_FERNS);
        registerConfigured("twilightforest:fiddlehead", FIDDLEHEAD);
        registerConfigured("twilightforest:mayapple", MAYAPPLE);
        registerConfigured("twilightforest:flower_placer", FLOWER_PLACER);
        registerConfigured("twilightforest:flower_placer_alt", FLOWER_PLACER_ALT);
        registerConfigured("twilightforest:dense_ferns", DENSE_FERNS);
        registerConfigured("twilightforest:dense_large_ferns", DENSE_LARGE_FERNS);
        registerConfigured("minecraft:grass", GRASS);
        registerConfigured("minecraft:grass_jungle", GRASS_JUNGLE);
        registerConfigured("minecraft:taiga_grass", TAIGA_GRASS);
        registerConfigured("twilightforest:torch_berries", TORCH_BERRIES);
        registerConfigured("twilightforest:fallen_leaves", FALLEN_LEAVES);
        registerConfigured("twilightforest:water_lake", WATER_LAKE);
        registerConfigured("twilightforest:lava_lake", LAVA_LAKE);
        registerConfigured("twilightforest:water_frozen", WATER_FROZEN);
        registerConfigured("twilightforest:legacy_coal_ore", LEGACY_COAL_ORE);
        registerConfigured("twilightforest:legacy_iron_ore", LEGACY_IRON_ORE);
        registerConfigured("twilightforest:legacy_gold_ore", LEGACY_GOLD_ORE);
        registerConfigured("twilightforest:legacy_redstone_ore", LEGACY_REDSTONE_ORE);
        registerConfigured("twilightforest:legacy_diamond_ore", LEGACY_DIAMOND_ORE);
        registerConfigured("twilightforest:legacy_lapis_ore", LEGACY_LAPIS_ORE);
        registerConfigured("twilightforest:legacy_copper_ore", LEGACY_COPPER_ORE);
        registerConfigured("twilightforest:small_andesite", SMALL_ANDESITE);
        registerConfigured("twilightforest:small_diorite", SMALL_DIORITE);
        registerConfigured("twilightforest:small_granite", SMALL_GRANITE);
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
