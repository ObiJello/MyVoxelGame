#include "data/worldgen/features/TwilightTreeFeatures.h"
#include "data/worldgen/features/TwilightFeatures.h"
#include "data/worldgen/features/TwilightFeatureRegistry.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/feature/TreeFeature.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/Heightmap.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/BushBlock.h"
#include "world/level/block/blocks/MossyCarpetBlock.h"
#include "world/level/block/blocks/VineBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "world/level/block/state/properties/DoubleBlockHalf.h"
#include "util/IntProvider.h"
#include "util/JavaHashSet.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — pass two configured features (see the header).
//
// Blocks resolve through levelgen::twilight_blocks (TwilightBlocks.cpp logs
// every stand-in once). The ones these features lean on while the TF block
// set is incomplete:
//   twilightforest:canopy_wood / twilight_oak_wood -> the matching log (axis y)
//   twilightforest:hollow_*_log_horizontal         -> the matching log (axis
//                                                     kept, variant dropped)
//   twilightforest:mangrove_root                   -> minecraft:mangrove_roots
//   twilightforest:root_strand                     -> minecraft:hanging_roots
//   twilightforest:trollvidr / unripe_trollber     -> minecraft:glow_lichen
// Tags the library has no data for are expanded by hand where they are read
// (#twilightforest:tree_roots_skip, #twilightforest:plants_hang_on,
// #twilightforest:clouds, #minecraft:snow). #minecraft:substrate_overworld is
// read as #minecraft:dirt, as in pass one.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using levelgen::feature::TreeFeature;
using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::SimpleStateProvider;
using levelgen::feature::stateproviders::WeightedStateProvider;
using levelgen::feature::stateproviders::WeightedStateEntry;
using levelgen::feature::treedecorators::DecoratorContext;
using levelgen::blockpredicates::matchesBlockTagName;
using ::minecraft::world::level::block::state::properties::BlockStateProperties;
using ::minecraft::world::level::block::state::properties::DoubleBlockHalf;
using twilight::RootPlacer;
using twilight::TreeBlockSetter;

namespace {

// Block.UPDATE_* (the values the Java passes)
constexpr int kUpdateClients = 2;                       // Block.UPDATE_CLIENTS
constexpr int kUpdateAll = 3;                           // Block.UPDATE_ALL
constexpr int kKnownShapeClients = 16 | 2;              // UPDATE_KNOWN_SHAPE | UPDATE_CLIENTS
constexpr int kKnownShapeAll = 16 | 3;                  // UPDATE_KNOWN_SHAPE | UPDATE_ALL

// ---------------------------------------------------------------------------
// Names and states resolved once at bootstrap
// ---------------------------------------------------------------------------
struct ResolvedNames {
    std::string root;             // twilightforest:root
    std::string liveroot;         // twilightforest:liveroot_block
    std::string mangroveRoot;     // twilightforest:mangrove_root
    std::string timeWood;         // twilightforest:time_wood
    std::string rootStrand;       // twilightforest:root_strand
    std::string trollvidr;        // twilightforest:trollvidr
    std::string trollber;         // twilightforest:trollber
    std::string unripeTrollber;   // twilightforest:unripe_trollber
};
ResolvedNames g_names;

// TFBlocks.FIREFLY (the trunk bugs of the mega trees).
BlockState* g_firefly = nullptr;

bool isNamed(BlockState* state, const std::string& name) {
    return state != nullptr && !name.empty() && state->getIdentifier() == name;
}

bool hasTag(BlockState* state, const char* tag) {
    return state != nullptr && matchesBlockTagName(state, tag);
}

// #minecraft:logs (the engine's logs also carry the isLog flag; TF's own
// #twilightforest:logs families are all registered as logs).
bool isLogs(BlockState* state) {
    return state != nullptr && (state->isLog() || matchesBlockTagName(state, "minecraft:logs"));
}

// FeatureLogic.ROOT_SHOULD_SKIP = #twilightforest:tree_roots_skip:
// #minecraft:logs, root, liveroot_block, mangrove_root, time_wood,
// #minecraft:features_cannot_replace.
bool rootShouldSkip(BlockState* state) {
    if (state == nullptr) return false;
    return isLogs(state)
        || isNamed(state, g_names.root)
        || isNamed(state, g_names.liveroot)
        || isNamed(state, g_names.mangroveRoot)
        || isNamed(state, g_names.timeWood)
        || hasTag(state, "minecraft:features_cannot_replace");
}

// #minecraft:snow = snow, snow_block, powder_snow.
bool isSnowTag(BlockState* state) {
    if (state == nullptr) return false;
    const std::string& id = state->getIdentifier();
    return id == "minecraft:snow" || id == "minecraft:snow_block" || id == "minecraft:powder_snow";
}

// FeatureLogic.IS_REPLACEABLE_AIR: canBeReplaced() || isAir()
bool isReplaceableAir(BlockState* state) {
    return state != nullptr && (state->canBeReplaced() || state->isAir());
}

bool isReplaceableAirAt(WorldGenLevel& level, const core::BlockPos& pos) {
    return isReplaceableAir(level.getBlockState(pos));
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
    return twilight::isReplaceable(state, false);   // FeatureLogic.worldGenReplaceable
}

// FeatureUtil.anyBelowMatch(pos, depth, predicate) = isAnyMatchInArea(
// pos.below(depth), 1, depth + 1, 1, predicate): pos.below(depth) .. pos.
template<typename Pred>
bool anyBelowMatch(const core::BlockPos& pos, int depth, Pred&& predicate) {
    for (int dy = -depth; dy <= 0; ++dy) {
        if (predicate(pos.offset(0, dy, 0))) return true;
    }
    return false;
}

// FeatureUtil.hasAirAround: UP, NORTH, SOUTH, WEST, EAST (isEmptyBlock).
bool hasAirAround(WorldGenLevel& level, const core::BlockPos& pos) {
    static constexpr core::Direction kDirectionsExceptDown[] = {
        core::Direction::UP, core::Direction::NORTH, core::Direction::SOUTH,
        core::Direction::WEST, core::Direction::EAST};
    for (core::Direction direction : kDirectionsExceptDown) {
        if (level.isEmptyBlock(pos.relative(direction))) return true;
    }
    return false;
}

// FeatureLogic.isBlockNotOk: liquid, bedrock, GiantBlock, #twilightforest:clouds,
// hardened_dark_leaves. Exclusions compare the real slugs only — never a
// stand-in, which would exclude the vanilla block standing in.
bool isBlockNotOk(BlockState* state) {
    if (state == nullptr) return true;
    if (state->isFluid()) return true;
    const std::string& id = state->getIdentifier();
    return id == "minecraft:bedrock"
        // GiantBlock subclasses (TFBlocks: giant cobblestone/log/leaves/obsidian)
        || id == "minecraft:giant_cobblestone" || id == "minecraft:giant_log"
        || id == "minecraft:giant_leaves" || id == "minecraft:giant_obsidian"
        // #twilightforest:clouds
        || id == "minecraft:fluffy_cloud" || id == "minecraft:wispy_cloud"
        || id == "minecraft:rainy_cloud" || id == "minecraft:snowy_cloud"
        || id == "minecraft:hardened_dark_leaves";
}

// WorldGenRegion.hasChunkAt: the chunk is inside the region being decorated.
bool hasChunkAt(WorldGenLevel& level, const core::BlockPos& pos) {
    return level.getChunk(pos.getX() >> 4, pos.getZ() >> 4) != nullptr;
}

// FeatureUtil.isAreaSuitable — flat natural ground below, free space above.
bool isAreaSuitable(WorldGenLevel& level, const core::BlockPos& pos, int xWidth, int height,
                    int zWidth, bool underwaterAllowed) {
    for (int cx = 0; cx < xWidth; ++cx) {
        for (int cz = 0; cz < zWidth; ++cz) {
            const core::BlockPos column = pos.offset(cx, 0, cz);
            if (!hasChunkAt(level, column)) return false;

            BlockState* below = level.getBlockState(column.below());
            if (below == nullptr || !below->isSolidRender() || isBlockNotOk(below)) {
                if (underwaterAllowed && below != nullptr && below->isFluid()) continue;
                return false;
            }

            for (int cy = 0; cy < height; ++cy) {
                const core::BlockPos above = column.above(cy);
                if (!level.isEmptyBlock(above)) {
                    BlockState* state = level.getBlockState(above);
                    if (state == nullptr || !state->canBeReplaced()) {
                        if (underwaterAllowed && state != nullptr && state->isFluid()) continue;
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool isOutsideBuildHeight(WorldGenLevel& level, int y) {
    return level.isOutsideBuildHeight(core::BlockPos(0, y, 0));
}

// FeaturePlacers.placeIfValidTreePos (validTreePos includes #minecraft:flowers)
bool placeIfValidTreePos(WorldGenLevel& level, const TreeBlockSetter& placer,
                         WorldgenRandom& random, const core::BlockPos& pos,
                         const BlockStateProvider& provider) {
    if (twilight::validTreePos(level, pos)) {
        placer(pos, provider.getState(random, pos));
        return true;
    }
    return false;
}

// FeaturePlacers.placeProvidedBlock with VALID_TREE_POS (TreeFeature::validTreePos)
void placeProvidedBlock(WorldGenLevel& level, const TreeBlockSetter& placer,
                        const core::BlockPos& pos, const BlockStateProvider& provider,
                        WorldgenRandom& random) {
    if (TreeFeature::validTreePos(level, pos)) {
        placer(pos, provider.getState(random, pos));
    }
}

// FeaturePlacers.drawBresenhamBranch
void drawBresenhamBranch(WorldGenLevel& level, const TreeBlockSetter& placer, WorldgenRandom& random,
                         const core::BlockPos& start, const core::BlockPos& end,
                         const BlockStateProvider& provider) {
    twilight::VoxelBresenhamIterator line(start, end);
    while (line.hasNext()) {
        placeIfValidTreePos(level, placer, random, line.next(), provider);
    }
}

// FeaturePlacers.isWithinCircle
bool isWithinCircle(int x, int z, float radius, bool useLegacyDistance) {
    if (useLegacyDistance) {
        const int absX = std::abs(x);
        const int absZ = std::abs(z);
        int legacyDistance;
        if (absX == 3 && absZ == 3) {
            legacyDistance = 6;
        } else {
            const float half = static_cast<float>(std::min(absX, absZ)) * 0.5f;
            const float sum = static_cast<float>(std::max(absX, absZ)) + half;
            legacyDistance = static_cast<int>(sum);
        }
        return static_cast<float>(legacyDistance) <= radius;
    }
    const float radiusSquared = radius * radius;
    return static_cast<float>(x * x + z * z) <= radiusSquared;
}

// FeaturePlacers.placeCircleOdd (odd-width trunks) with VALID_TREE_POS
void placeCircleOdd(WorldGenLevel& level, const TreeBlockSetter& placer, WorldgenRandom& random,
                    const core::BlockPos& center, float radius, const BlockStateProvider& provider,
                    bool useLegacyDistance) {
    placeProvidedBlock(level, placer, center, provider, random);
    for (int x = 0; static_cast<float>(x) <= radius; ++x) {
        for (int z = 1; static_cast<float>(z) <= radius; ++z) {
            if (isWithinCircle(x, z, radius, useLegacyDistance)) {
                placeProvidedBlock(level, placer, center.offset(x, 0, z), provider, random);
                placeProvidedBlock(level, placer, center.offset(-x, 0, -z), provider, random);
                placeProvidedBlock(level, placer, center.offset(-z, 0, x), provider, random);
                placeProvidedBlock(level, placer, center.offset(z, 0, -x), provider, random);
            }
        }
    }
}

// FeaturePlacers.placeCircleEven (even-width trunks) with VALID_TREE_POS
void placeCircleEven(WorldGenLevel& level, const TreeBlockSetter& placer, WorldgenRandom& random,
                     const core::BlockPos& center, float radius, const BlockStateProvider& provider,
                     bool useLegacyDistance) {
    placeProvidedBlock(level, placer, center, provider, random);
    for (int x = 0; static_cast<float>(x) <= radius; ++x) {
        for (int z = 0; static_cast<float>(z) <= radius; ++z) {
            if (isWithinCircle(x, z, radius, useLegacyDistance)) {
                placeProvidedBlock(level, placer, center.offset(1 + x, 0, 1 + z), provider, random);
                placeProvidedBlock(level, placer, center.offset(-x, 0, -z), provider, random);
                placeProvidedBlock(level, placer, center.offset(-x, 0, 1 + z), provider, random);
                placeProvidedBlock(level, placer, center.offset(1 + x, 0, -z), provider, random);
            }
        }
    }
}

// FeaturePlacers.placeSpheroid — the overload without verticalBias, whose
// edge blocks are each skipped 1 in 3. Float terms are kept in separate
// statements so the compiler cannot fuse them (Java rounds every step).
void placeSpheroid(WorldGenLevel& level, const TreeBlockSetter& placer, WorldgenRandom& random,
                   const core::BlockPos& center, float xzRadius, float yRadius,
                   const BlockStateProvider& provider) {
    const float xzRadiusSquared = xzRadius * xzRadius;
    const float yRadiusSquared = yRadius * yRadius;
    const float superRadiusSquared = xzRadiusSquared * yRadiusSquared;
    placeProvidedBlock(level, placer, center, provider, random);

    for (int y = 0; static_cast<float>(y) <= yRadius; ++y) {
        placeProvidedBlock(level, placer, center.offset(0, y, 0), provider, random);
        placeProvidedBlock(level, placer, center.offset(0, -y, 0), provider, random);
    }

    for (int x = 0; static_cast<float>(x) <= xzRadius; ++x) {
        for (int z = 1; static_cast<float>(z) <= xzRadius; ++z) {
            if (static_cast<float>(x * x + z * z) > xzRadiusSquared) continue;

            placeProvidedBlock(level, placer, center.offset(x, 0, z), provider, random);
            placeProvidedBlock(level, placer, center.offset(-x, 0, -z), provider, random);
            placeProvidedBlock(level, placer, center.offset(-z, 0, x), provider, random);
            placeProvidedBlock(level, placer, center.offset(z, 0, -x), provider, random);

            for (int y = 1; static_cast<float>(y) <= yRadius; ++y) {
                const float ySquare = static_cast<float>(y * y) * xzRadiusSquared;
                const float xzTerm = static_cast<float>(x * x + z * z) * yRadiusSquared;
                const float inside = xzTerm + ySquare;
                if (inside <= superRadiusSquared) {
                    const float xNextTerm = static_cast<float>((x + 1) * (x + 1) + z * z) * yRadiusSquared;
                    const float zNextTerm = static_cast<float>(x * x + (z + 1) * (z + 1)) * yRadiusSquared;
                    const float xNext = xNextTerm + ySquare;
                    const float zNext = zNextTerm + ySquare;
                    if (xNext > superRadiusSquared && zNext > superRadiusSquared) {
                        // randomly skip some blocks on the very edges of the blob
                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(x, y, z), provider, random);
                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(-x, y, -z), provider, random);
                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(-z, y, x), provider, random);
                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(z, y, -x), provider, random);

                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(x, -y, z), provider, random);
                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(-x, -y, -z), provider, random);
                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(-z, -y, x), provider, random);
                        if (random.nextInt(3) != 0) placeProvidedBlock(level, placer, center.offset(z, -y, -x), provider, random);
                        continue;
                    }

                    placeProvidedBlock(level, placer, center.offset(x, y, z), provider, random);
                    placeProvidedBlock(level, placer, center.offset(-x, y, -z), provider, random);
                    placeProvidedBlock(level, placer, center.offset(-z, y, x), provider, random);
                    placeProvidedBlock(level, placer, center.offset(z, y, -x), provider, random);

                    placeProvidedBlock(level, placer, center.offset(x, -y, z), provider, random);
                    placeProvidedBlock(level, placer, center.offset(-x, -y, -z), provider, random);
                    placeProvidedBlock(level, placer, center.offset(-z, -y, x), provider, random);
                    placeProvidedBlock(level, placer, center.offset(z, -y, -x), provider, random);
                }
            }
        }
    }
}

// FeaturePlacers.placeIfValidRootPos
bool placeIfValidRootPos(WorldGenLevel& level, const RootPlacer& placer, WorldgenRandom& random,
                         const core::BlockPos& pos, const BlockStateProvider& provider) {
    if (!anyBelowMatch(pos, placer.rootPenetrability - 1,
                       [&level](const core::BlockPos& p) { return !canRootGrowIn(level, p); })) {
        placer.placer(pos, provider.getState(random, pos));
        return true;
    }
    return false;
}

// FeaturePlacers.buildRoot — stop drawing when the root heads into open air.
void buildRoot(WorldGenLevel& level, const RootPlacer& placer, WorldgenRandom& random,
               const core::BlockPos& start, double offset, int b, const BlockStateProvider& provider) {
    const core::BlockPos dest = twilight::translate(start.below(b + 2), 5.0, 0.3 * b + offset, 0.8);
    twilight::VoxelBresenhamIterator line(start.below(), dest);
    while (line.hasNext()) {
        if (!placeIfValidRootPos(level, placer, random, line.next(), provider)) return;
    }
}

// FeaturePlacers.traceRoot
void traceRoot(WorldGenLevel& level, const RootPlacer& placer, WorldgenRandom& random,
               const BlockStateProvider& dirtRoot, twilight::VoxelBresenhamIterator& tracer) {
    while (tracer.hasNext()) {
        const core::BlockPos rootPos = tracer.next();
        if (anyBelowMatch(rootPos, placer.rootPenetrability - 1, [&level](const core::BlockPos& p) {
                return rootShouldSkip(level.getBlockState(p));
            })) {
            return;   // this block cannot be penetrated
        }
        if (!placeIfValidRootPos(level, placer, random, rootPos, dirtRoot)) {
            return;   // not replaceable, or detached from the ground mass
        }
    }
}

// FeaturePlacers.traceExposedRoot — exposed roots while above ground, then
// the same tracer continues as ground roots once buried.
void traceExposedRoot(WorldGenLevel& level, const RootPlacer& placer, WorldgenRandom& random,
                      const BlockStateProvider& exposedRoot, const BlockStateProvider& dirtRoot,
                      twilight::VoxelBresenhamIterator& tracer) {
    while (tracer.hasNext()) {
        const core::BlockPos exposedPos = tracer.next();
        if (rootShouldSkip(level.getBlockState(exposedPos))) {
            continue;
        }
        if (hasEmptyNeighborExceptBelow(level, exposedPos)) {
            // The predicate draws the exposed state per checked block, as Java.
            if (anyBelowMatch(exposedPos, placer.rootPenetrability - 1, [&](const core::BlockPos& p) {
                    BlockState* state = level.getBlockState(p);
                    return state != nullptr && !twilight::isReplaceable(state, false)
                        && state != exposedRoot.getState(random, exposedPos);
                })) {
                return;   // root must stop
            }
            placer.placer(exposedPos, exposedRoot.getState(random, exposedPos));
        } else {
            if (placeIfValidRootPos(level, placer, random, exposedPos, dirtRoot)) {
                traceRoot(level, placer, random, dirtRoot, tracer);
            }
            return;
        }
    }
}

// Direction.getRandom: VALUES[nextInt(6)] in DOWN, UP, NORTH, SOUTH, WEST, EAST order.
core::Direction randomDirection(WorldgenRandom& random) {
    return core::fromIndex(random.nextInt(6));
}

// MegaCanopyTreeFeature.randomlyOffset
core::BlockPos randomlyOffset(const core::BlockPos& pos, WorldgenRandom& random) {
    switch (random.nextInt(4)) {
        case 0: return pos;
        case 1: return pos.offset(1, 0, 0);
        case 2: return pos.offset(0, 0, 1);
        default: return pos.offset(1, 0, 1);
    }
}

// The trunk bug loop of MegaCanopyTreeFeature/MegaOakTreeFeature.buildTrunk:
// seven 1-in-3 tries at a firefly on a random horizontal face of the 2x2
// trunk (Feature.setBlock = setBlockAndUpdate).
void placeTrunkBugs(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos, int treeHeight) {
    for (int i = 0; i < 7; ++i) {
        if (random.nextInt(3) != 0) continue;
        const core::Direction direction = randomDirection(random);
        const core::Axis axis = core::getAxis(direction);
        if (axis == core::Axis::Y) continue;

        core::BlockPos bugPos = pos.offset(direction == core::Direction::EAST ? 1 : 0,
                                           random.nextInt(treeHeight),
                                           direction == core::Direction::SOUTH ? 1 : 0);
        bugPos = bugPos.relative(direction);
        const int moveX = axis == core::Axis::Z ? random.nextInt(2) : 0;
        const int moveZ = axis == core::Axis::X ? random.nextInt(2) : 0;
        bugPos = bugPos.offset(moveX, 0, moveZ);

        BlockState* there = level.getBlockState(bugPos);
        if (there != nullptr && !there->isSolidRender() && g_firefly != nullptr) {
            BlockState* bug = g_firefly->hasProperty(BlockStateProperties::FACING)
                ? g_firefly->setValue(*BlockStateProperties::FACING, direction) : g_firefly;
            level.setBlock(bugPos, bug, kUpdateAll);
        }
    }
}

// CanopyTreeFeature.makeRoots — a root bulb under the column, then 1-2 roots.
void makeRoots(WorldGenLevel& level, const TreeBlockSetter& trunkPlacer, const RootPlacer& decoPlacer,
               WorldgenRandom& random, const core::BlockPos& pos, const TFTreeFeatureConfig& config) {
    if (hasAirAround(level, pos.below())) {
        placeIfValidTreePos(level, trunkPlacer, random, pos.below(), *config.trunkProvider);
    } else {
        placeIfValidRootPos(level, decoPlacer, random, pos.below(), *config.rootsProvider);
    }

    const int numRoots = 1 + random.nextInt(2);
    const float offset = random.nextFloat();
    for (int b = 0; b < numRoots; ++b) {
        buildRoot(level, decoPlacer, random, pos, static_cast<double>(offset), b, *config.rootsProvider);
    }
}

// ---------------------------------------------------------------------------
// TreeFeature.updateLeaves + StructureTemplate.updateShapeAtEdge — the
// library's TreeFeature keeps its port file-local, so TFTreeFeature.place
// carries the same logic (same JavaHashSet iteration order, same face sweep
// and the same shape-update dispatch as levelgen/feature/TreeFeature.cpp).
// ---------------------------------------------------------------------------
enum : uint8_t { kIsMossyCarpet = 1, kIsVine = 2, kIsBush = 4 };

uint8_t blockClassFlags(::minecraft::world::level::block::Block* block) {
    thread_local std::unordered_map<::minecraft::world::level::block::Block*, uint8_t> memo;
    auto it = memo.find(block);
    if (it != memo.end()) return it->second;
    uint8_t f = 0;
    if (dynamic_cast<::minecraft::world::level::block::MossyCarpetBlock*>(block)) f |= kIsMossyCarpet;
    if (dynamic_cast<::minecraft::world::level::block::VineBlock*>(block)) f |= kIsVine;
    if (dynamic_cast<::minecraft::world::level::block::BushBlock*>(block)) f |= kIsBush;
    memo.emplace(block, f);
    return f;
}

class FilledBoundsShape {
public:
    FilledBoundsShape(int xSize, int ySize, int zSize)
        : m_ySize(ySize), m_zSize(zSize)
        , m_storage(static_cast<size_t>(xSize) * static_cast<size_t>(ySize) * static_cast<size_t>(zSize), 0) {}

    void fill(int x, int y, int z) { m_storage[index(x, y, z)] = 1; }
    bool isFull(int x, int y, int z) const { return m_storage[index(x, y, z)] != 0; }

private:
    size_t index(int x, int y, int z) const {
        return (static_cast<size_t>(x) * static_cast<size_t>(m_ySize) + static_cast<size_t>(y))
            * static_cast<size_t>(m_zSize) + static_cast<size_t>(z);
    }

    int m_ySize;
    int m_zSize;
    std::vector<uint8_t> m_storage;
};

std::optional<int> getOptionalDistanceAt(BlockState* state) {
    if (state == nullptr) return std::nullopt;
    if (state->isLog()) return 0;   // #minecraft:prevents_nearby_leaf_decay
    if (BlockStateProperties::DISTANCE && state->hasProperty(BlockStateProperties::DISTANCE)) {
        return state->getValue(*BlockStateProperties::DISTANCE);
    }
    return std::nullopt;
}

BlockState* airState() {
    return static_cast<BlockState*>(::minecraft::world::level::block::Blocks::AIR->defaultBlockState());
}

// BlockState.updateShape for the block at `pos` reacting to its neighbour at
// pos + direction — the families worldgen can observe (see TreeFeature.cpp).
void shapeUpdateAt(WorldGenLevel& level, const core::BlockPos& pos, core::Direction direction) {
    BlockState* state = level.getBlockState(pos);
    if (state == nullptr || state->isAir()) return;

    const std::string& sid = state->getIdentifier();
    auto ends = [&sid](const char* suffix) {
        const size_t n = std::strlen(suffix);
        return sid.size() > n && sid.compare(sid.size() - n, n, suffix) == 0;
    };
    const bool structureFamily = ends("_wall") || ends("_fence") || ends("_pane")
        || ends("_stairs") || ends("_door") || ends("_trapdoor")
        || ends("_bed") || ends("_wall_sign")
        || (ends("_carpet") && sid != "minecraft:moss_carpet" && sid != "minecraft:pale_moss_carpet")
        || sid == "minecraft:iron_bars" || sid == "minecraft:torch"
        || sid == "minecraft:redstone_torch" || sid == "minecraft:wall_torch"
        || sid == "minecraft:ladder" || sid == "minecraft:rail";
    if (structureFamily) {
        const core::BlockPos neighborPos = pos.relative(direction, 1);
        BlockState* updated = levelgen::structure::TemplateEngine::updateShapeForBlock(
            state, &level, pos, direction, neighborPos, level.getBlockState(neighborPos));
        if (updated != state) level.setBlock(pos, updated, kUpdateClients);
        return;
    }

    const bool isDoublePlant = BlockStateProperties::DOUBLE_BLOCK_HALF
        && state->hasProperty(BlockStateProperties::DOUBLE_BLOCK_HALF)
        && (sid == "minecraft:sunflower" || sid == "minecraft:lilac" || sid == "minecraft:rose_bush"
            || sid == "minecraft:peony" || sid == "minecraft:tall_grass" || sid == "minecraft:large_fern"
            || sid == "minecraft:pitcher_plant" || sid == "minecraft:small_dripleaf");
    if (!isDoublePlant) {
        // MossyCarpetBlock.updateShape: !canSurvive -> air, side recalc, !hasFaces -> air.
        if (blockClassFlags(state->getBlock()) & kIsMossyCarpet) {
            auto* carpet = static_cast<::minecraft::world::level::block::MossyCarpetBlock*>(state->getBlock());
            if (!carpet->canSurvive(state, level, pos)) {
                level.setBlock(pos, airState(), kUpdateClients);
                return;
            }
            BlockState* updated = carpet->getUpdatedStateForShapeUpdate(state, level, pos);
            if (updated && !::minecraft::world::level::block::MossyCarpetBlock::hasFaces(updated)) {
                updated = airState();
            }
            if (updated && updated != state) level.setBlock(pos, updated, kUpdateClients);
            return;
        }
        // VineBlock.updateShape (non-DOWN): prune unsupported faces, air when none remain.
        if (direction != core::Direction::DOWN && (blockClassFlags(state->getBlock()) & kIsVine)) {
            auto* vine = static_cast<::minecraft::world::level::block::VineBlock*>(state->getBlock());
            BlockState* updated = vine->getUpdatedStateForShapeUpdate(state, level, pos);
            if (!updated) updated = airState();
            if (updated != state) level.setBlock(pos, updated, kUpdateClients);
            return;
        }
        // VegetationBlock.updateShape: canSurvive on any neighbour-face update.
        const bool isVegetation = (blockClassFlags(state->getBlock()) & kIsBush) != 0
            || state->getBlock() == ::minecraft::world::level::block::Blocks::MANGROVE_PROPAGULE;
        if (isVegetation && !state->getBlock()->canSurvive(state, level, pos)) {
            level.setBlock(pos, airState(), kUpdateClients);
        }
        return;
    }

    // DoublePlantBlock.updateShape
    const DoubleBlockHalf half = state->getValue(*BlockStateProperties::DOUBLE_BLOCK_HALF);
    const bool lower = half.getValue() == DoubleBlockHalf::LOWER;
    const core::BlockPos neighborPos = pos.relative(direction, 1);
    BlockState* neighborState = level.getBlockState(neighborPos);
    const core::Direction otherHalfDir = lower ? core::Direction::UP : core::Direction::DOWN;
    if (direction == otherHalfDir) {
        const bool matching = neighborState && !neighborState->isAir()
            && neighborState->getIdentifier() == state->getIdentifier()
            && neighborState->hasProperty(BlockStateProperties::DOUBLE_BLOCK_HALF)
            && neighborState->getValue(*BlockStateProperties::DOUBLE_BLOCK_HALF).getValue() != half.getValue();
        if (!matching) level.setBlock(pos, airState(), kUpdateClients);
    } else {
        bool survives;
        BlockState* below = level.getBlockState(pos.below());
        if (lower) {
            survives = below && !below->isAir()
                && (matchesBlockTagName(below, "minecraft:substrate_overworld") || below->getIdentifier() == "minecraft:farmland");
        } else {
            survives = below && !below->isAir()
                && below->getIdentifier() == state->getIdentifier()
                && below->hasProperty(BlockStateProperties::DOUBLE_BLOCK_HALF)
                && below->getValue(*BlockStateProperties::DOUBLE_BLOCK_HALF).getValue() == DoubleBlockHalf::LOWER;
        }
        if (!survives) level.setBlock(pos, airState(), kUpdateClients);
    }
}

void updateLeaves(WorldGenLevel& level, const levelgen::feature::BoundingBox& bounds,
                  const util::JavaHashSet<core::BlockPos>& logs,
                  const util::JavaHashSet<core::BlockPos>& decorationSet,
                  const util::JavaHashSet<core::BlockPos>& rootPositions) {
    FilledBoundsShape shape(bounds.getXSpan(), bounds.getYSpan(), bounds.getZSpan());
    std::array<util::JavaHashSet<core::BlockPos>, 7> toCheck;

    for (const auto& pos : decorationSet) {
        if (bounds.isInside(pos)) {
            shape.fill(pos.getX() - bounds.minX(), pos.getY() - bounds.minY(), pos.getZ() - bounds.minZ());
        }
    }
    for (const auto& pos : rootPositions) {
        if (bounds.isInside(pos)) {
            shape.fill(pos.getX() - bounds.minX(), pos.getY() - bounds.minY(), pos.getZ() - bounds.minZ());
        }
    }

    core::BlockPos::MutableBlockPos neighborPos(0, 0, 0);
    int smallestDistance = 0;
    for (const auto& logPos : logs) toCheck[0].add(logPos);

    while (true) {
        while (smallestDistance < 7 && !toCheck[static_cast<size_t>(smallestDistance)].empty()) {
            auto& bucket = toCheck[static_cast<size_t>(smallestDistance)];
            const core::BlockPos pos = *bucket.begin();
            bucket.remove(pos);

            if (!bounds.isInside(pos)) continue;

            if (smallestDistance != 0) {
                BlockState* state = level.getBlockState(pos);
                if (state && BlockStateProperties::DISTANCE && state->hasProperty(BlockStateProperties::DISTANCE)) {
                    level.setBlock(pos, state->setValue(*BlockStateProperties::DISTANCE, smallestDistance),
                                   TreeFeature::BLOCK_UPDATE_FLAGS);
                }
            }

            shape.fill(pos.getX() - bounds.minX(), pos.getY() - bounds.minY(), pos.getZ() - bounds.minZ());

            for (int directionIndex = 0; directionIndex < 6; ++directionIndex) {
                const core::Direction direction = core::fromIndex(directionIndex);
                neighborPos.setWithOffset(pos, core::getStepX(direction), core::getStepY(direction),
                                          core::getStepZ(direction));
                if (!bounds.isInside(neighborPos)) continue;

                const int xInShape = neighborPos.getX() - bounds.minX();
                const int yInShape = neighborPos.getY() - bounds.minY();
                const int zInShape = neighborPos.getZ() - bounds.minZ();
                if (shape.isFull(xInShape, yInShape, zInShape)) continue;

                const std::optional<int> distance = getOptionalDistanceAt(level.getBlockState(neighborPos));
                if (!distance.has_value()) continue;

                const int newDistance = std::min(distance.value(), smallestDistance + 1);
                if (newDistance < 7) {
                    toCheck[static_cast<size_t>(newDistance)].add(neighborPos.immutable());
                    smallestDistance = std::min(smallestDistance, newDistance);
                }
            }
        }

        if (smallestDistance >= 7) break;
        ++smallestDistance;
    }

    // StructureTemplate.updateShapeAtEdge: DiscreteVoxelShape.forAllFaces —
    // Z faces, then Y faces, then X faces, each scanning fill transitions.
    auto emitFace = [&](core::Direction direction, int sx, int sy, int sz) {
        const core::BlockPos worldPos(bounds.minX() + sx, bounds.minY() + sy, bounds.minZ() + sz);
        const core::BlockPos facePos = worldPos.relative(direction, 1);
        shapeUpdateAt(level, worldPos, direction);
        shapeUpdateAt(level, facePos, core::getOpposite(direction));
    };

    const int sizes[3] = {bounds.getXSpan(), bounds.getYSpan(), bounds.getZSpan()};
    struct AxisPass {
        int aAxis, bAxis, cAxis;
        core::Direction negative, positive;
    };
    const AxisPass passes[3] = {
        {0, 1, 2, core::Direction::NORTH, core::Direction::SOUTH},
        {2, 0, 1, core::Direction::DOWN, core::Direction::UP},
        {1, 2, 0, core::Direction::WEST, core::Direction::EAST},
    };
    for (const AxisPass& pass : passes) {
        const int aSize = sizes[pass.aAxis];
        const int bSize = sizes[pass.bAxis];
        const int cSize = sizes[pass.cAxis];
        for (int a = 0; a < aSize; ++a) {
            for (int b = 0; b < bSize; ++b) {
                bool lastFull = false;
                for (int c = 0; c <= cSize; ++c) {
                    int coords[3];
                    coords[pass.aAxis] = a;
                    coords[pass.bAxis] = b;
                    bool full = false;
                    if (c != cSize) {
                        coords[pass.cAxis] = c;
                        full = shape.isFull(coords[0], coords[1], coords[2]);
                    }
                    if (!lastFull && full) {
                        coords[pass.cAxis] = c;
                        emitFace(pass.negative, coords[0], coords[1], coords[2]);
                    }
                    if (lastFull && !full) {
                        coords[pass.cAxis] = c - 1;
                        emitFace(pass.positive, coords[0], coords[1], coords[2]);
                    }
                    lastFull = full;
                }
            }
        }
    }
}

std::vector<core::BlockPos> sortedByY(const util::JavaHashSet<core::BlockPos>& set) {
    std::vector<core::BlockPos> out(set.begin(), set.end());
    std::stable_sort(out.begin(), out.end(),
                     [](const core::BlockPos& a, const core::BlockPos& b) { return a.getY() < b.getY(); });
    return out;
}

} // namespace

// ============================================================================
// TFTreeFeature — trees/TFTreeFeature.java
// ============================================================================
bool TFTreeFeature::place(FeaturePlaceContext<TFTreeFeatureConfig>& context) {
    WorldGenLevel* levelPtr = context.level();
    if (levelPtr == nullptr) return false;
    WorldGenLevel& level = *levelPtr;
    WorldgenRandom& random = context.random();
    const TFTreeFeatureConfig& config = context.config();

    // set = trunk, set1 = leaves, set2 = roots (the RootPlacer), set3 = decorations
    util::JavaHashSet<core::BlockPos> set;
    util::JavaHashSet<core::BlockPos> set1;
    util::JavaHashSet<core::BlockPos> set2;
    util::JavaHashSet<core::BlockPos> set3;
    WorldGenLevel* world = &level;
    auto tracking = [world](util::JavaHashSet<core::BlockPos>& targetSet) -> TreeBlockSetter {
        util::JavaHashSet<core::BlockPos>* target = &targetSet;
        return [world, target](const core::BlockPos& pos, BlockState* state) {
            target->add(pos);
            if (state != nullptr) world->setBlock(pos, state, kKnownShapeAll);
        };
    };

    Placers placers{tracking(set), tracking(set1), RootPlacer{tracking(set2), 1}};
    const bool flag = generate(level, random, context.origin(), placers, config);
    if (!flag || (set1.empty() && set2.empty())) return false;

    if (!config.decorators.empty()) {
        // new TreeDecorator.Context(level, biconsumer3, random, set1, set2, set):
        // the mod hands its leaves as the context's logs, its roots as the
        // leaves and its trunk as the roots.
        const std::vector<core::BlockPos> contextLogs = sortedByY(set1);
        const std::vector<core::BlockPos> contextLeaves = sortedByY(set2);
        const std::vector<core::BlockPos> contextRoots = sortedByY(set);
        auto decorationSetter = [&set3, &level](const core::BlockPos& pos, BlockState* state) {
            set3.add(pos);
            if (state != nullptr) level.setBlock(pos, state, kKnownShapeAll);
        };
        auto blockGetter = [&level](const core::BlockPos& pos) -> BlockState* { return level.getBlockState(pos); };
        auto heightGetter = [&level](int x, int z) -> int {
            return level.getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z);
        };
        DecoratorContext decoratorContext(contextLogs, contextLeaves, contextRoots, decorationSetter,
                                          blockGetter, heightGetter, &random, &level, context.chunkGenerator());
        for (const auto& decorator : config.decorators) {
            if (decorator) decorator->place(decoratorContext);
        }
    }

    std::vector<core::BlockPos> all;
    all.reserve(set.size() + set1.size() + set2.size() + set3.size());
    all.insert(all.end(), set.begin(), set.end());
    all.insert(all.end(), set1.begin(), set1.end());
    all.insert(all.end(), set2.begin(), set2.end());
    all.insert(all.end(), set3.begin(), set3.end());
    const auto bounds = levelgen::feature::BoundingBox::encapsulatingPositions(all);
    if (!bounds.has_value()) return false;

    // TreeFeature.updateLeaves(level, box, set1, set3, set)
    updateLeaves(level, bounds.value(), set1, set3, set);
    return true;
}

// ============================================================================
// MegaCanopyTreeFeature — trees/MegaCanopyTreeFeature.java
// ============================================================================
bool MegaCanopyTreeFeature::generate(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos,
                                     const Placers& placers, const TFTreeFeatureConfig& config) {
    std::vector<core::BlockPos> leaves;
    int treeHeight = config.minHeight;
    if (random.nextInt(config.chanceAddFiveFirst) == 0) {
        treeHeight += random.nextInt(treeHeight / 2);
        if (random.nextInt(config.chanceAddFiveSecond) == 0) {
            treeHeight += random.nextInt(treeHeight / 4);
        }
    }

    if (isOutsideBuildHeight(level, pos.getY() + treeHeight)) return false;

    // "check if we're on dirt or grass": BlockState.canSustainPlant(...,
    // CANOPY_SAPLING).isFalse(). NeoForge's IBlockExtension.canSustainPlant
    // returns TriState.DEFAULT, and no soil these trees can stand on
    // overrides it (only UberousSoilBlock does, and only for non-vertical
    // facings), so the check never rejects — the placement's
    // would_survive(sapling) filter is what vets the ground.

    leaves.clear();
    buildTrunk(level, leaves, placers, random, pos, treeHeight, config);

    const int numBranches = 6 + random.nextInt(3);
    float bangle = random.nextFloat();
    int offset = 0;
    for (int b = 0; b < numBranches; ++b) {
        const float btilt = 0.25f;
        const core::BlockPos branchBase = randomlyOffset(pos, random);
        const int length = 15 + random.nextInt(4);
        buildBranch(level, leaves, branchBase, placers, treeHeight - 15 - (b + offset),
                    static_cast<double>(length), static_cast<double>(bangle), static_cast<double>(btilt),
                    random, config);

        offset += random.nextInt(2);
        const float step = random.nextFloat() * 0.4f;
        bangle += step;
        if (bangle > 1.0f) bangle -= 1.0f;
    }

    for (const core::BlockPos& leafPos : leaves) {
        makeLeafBlob(level, placers, random, leafPos, config);
    }

    makeRoots(level, placers.trunk, placers.decoration, random, pos, config);
    makeRoots(level, placers.trunk, placers.decoration, random, pos.east(), config);
    makeRoots(level, placers.trunk, placers.decoration, random, pos.south(), config);
    makeRoots(level, placers.trunk, placers.decoration, random, pos.east().south(), config);
    return true;
}

void MegaCanopyTreeFeature::makeLeafBlob(WorldGenLevel& level, const Placers& placers, WorldgenRandom& random,
                                         const core::BlockPos& leafPos, const TFTreeFeatureConfig& config) {
    const BlockStateProvider& branch = *config.branchProvider;
    placeIfValidTreePos(level, placers.trunk, random, leafPos, branch);
    static constexpr core::Direction kDirections[] = {
        core::Direction::NORTH, core::Direction::EAST, core::Direction::SOUTH, core::Direction::WEST};
    for (core::Direction direction : kDirections) {
        const core::Direction clockwise = core::rotateYClockwise(direction);
        const core::Direction counterClockwise = core::rotateYCounterClockwise(direction);
        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 1), branch);
        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 2), branch);
        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 3), branch);
        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 4), branch);

        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 5).relative(clockwise), branch);
        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 5).relative(counterClockwise), branch);

        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 2).relative(clockwise, 1), branch);
        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 3).relative(clockwise, 2), branch);
        placeIfValidTreePos(level, placers.trunk, random, leafPos.relative(direction, 4).relative(clockwise, 3), branch);
    }
    // Circles with legacy distance instead of a spheroid, to match 1.4.7.
    const BlockStateProvider& leavesProvider = *config.leavesProvider;
    placeCircleOdd(level, placers.leaves, random, leafPos.above(2), 3.0f, leavesProvider, true);
    placeCircleOdd(level, placers.leaves, random, leafPos.above(), 6.0f, leavesProvider, true);
    placeCircleOdd(level, placers.leaves, random, leafPos, 8.0f, leavesProvider, true);
    placeCircleOdd(level, placers.leaves, random, leafPos.below(), 7.0f, leavesProvider, true);
    placeCircleOdd(level, placers.leaves, random, leafPos.below(2), 4.5f, leavesProvider, true);
}

void MegaCanopyTreeFeature::buildTrunk(WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                                       const Placers& placers, WorldgenRandom& random, const core::BlockPos& pos,
                                       int treeHeight, const TFTreeFeatureConfig& config) {
    const BlockStateProvider& trunk = *config.trunkProvider;
    for (int dy = 0; dy <= treeHeight; ++dy) {
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(0, dy, 0), trunk);
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(1, dy, 0), trunk);
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(0, dy, 1), trunk);
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(1, dy, 1), trunk);
    }

    placeTrunkBugs(level, random, pos, treeHeight);

    leaves.push_back(randomlyOffset(pos.above(treeHeight), random));
}

void MegaCanopyTreeFeature::buildBranch(WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                                        const core::BlockPos& pos, const Placers& placers, int height,
                                        double length, double angle, double tilt, WorldgenRandom& random,
                                        const TFTreeFeatureConfig& config) {
    const core::BlockPos src = pos.above(height);
    core::BlockPos dest = twilight::translate(src, length, angle, tilt);

    // constrain branch spread
    const int limit = 12;
    if ((dest.getX() - pos.getX()) < -limit) dest = core::BlockPos(pos.getX() - limit, dest.getY(), dest.getZ());
    if ((dest.getX() - pos.getX()) > limit) dest = core::BlockPos(pos.getX() + limit, dest.getY(), dest.getZ());
    if ((dest.getZ() - pos.getZ()) < -limit) dest = core::BlockPos(dest.getX(), dest.getY(), pos.getZ() - limit);
    if ((dest.getZ() - pos.getZ()) > limit) dest = core::BlockPos(dest.getX(), dest.getY(), pos.getZ() + limit);

    // trunk == false for every mega-canopy branch: a doubled branch.
    drawBresenhamBranch(level, placers.trunk, random, src, dest, *config.branchProvider);
    drawBresenhamBranch(level, placers.trunk, random, src.below(), dest.below(), *config.branchProvider);

    leaves.push_back(dest);
}

// ============================================================================
// MegaOakTreeFeature — trees/MegaOakTreeFeature.java
// ============================================================================
bool MegaOakTreeFeature::generate(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos,
                                  const Placers& placers, const TFTreeFeatureConfig& config) {
    std::vector<core::BlockPos> leaves;
    int treeHeight = config.minHeight;
    if (random.nextInt(config.chanceAddFiveFirst) == 0) {
        treeHeight += random.nextInt(treeHeight / 2);
        if (random.nextInt(config.chanceAddFiveSecond) == 0) {
            treeHeight += random.nextInt(5);
        }
    }

    if (isOutsideBuildHeight(level, pos.getY() + treeHeight)) return false;

    // canSustainPlant(..., CANOPY_SAPLING).isFalse(): never rejects (see
    // MegaCanopyTreeFeature::generate).

    leaves.clear();
    buildTrunk(level, leaves, placers, random, pos, treeHeight, config);

    const int numBranches = 12 + random.nextInt(9);
    float bangle = random.nextFloat();
    for (int b = 0; b < numBranches; ++b) {
        const float tiltStep = random.nextFloat() * 0.35f;
        const float btilt = 0.15f + tiltStep;
        buildBranch(level, leaves, pos, placers, treeHeight - 10 + (b / 2), 5.0,
                    static_cast<double>(bangle), static_cast<double>(btilt), random, config);

        const float step = random.nextFloat() * 0.4f;
        bangle += step;
        if (bangle > 1.0f) bangle -= 1.0f;
    }

    // makeLeafBlob: placeSpheroid(leafPos, 2.5, 2.5)
    for (const core::BlockPos& leafPos : leaves) {
        placeSpheroid(level, placers.leaves, random, leafPos, 2.5f, 2.5f, *config.leavesProvider);
    }

    makeRoots(level, placers.trunk, placers.decoration, random, pos, config);
    makeRoots(level, placers.trunk, placers.decoration, random, pos.east(), config);
    makeRoots(level, placers.trunk, placers.decoration, random, pos.south(), config);
    makeRoots(level, placers.trunk, placers.decoration, random, pos.east().south(), config);
    return true;
}

void MegaOakTreeFeature::buildTrunk(WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                                    const Placers& placers, WorldgenRandom& random, const core::BlockPos& pos,
                                    int treeHeight, const TFTreeFeatureConfig& config) {
    const BlockStateProvider& trunk = *config.trunkProvider;
    for (int dy = 0; dy < treeHeight; ++dy) {
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(0, dy, 0), trunk);
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(1, dy, 0), trunk);
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(0, dy, 1), trunk);
        placeIfValidTreePos(level, placers.trunk, random, pos.offset(1, dy, 1), trunk);
    }

    placeTrunkBugs(level, random, pos, treeHeight);

    leaves.push_back(pos.offset(0, treeHeight, 0));
}

void MegaOakTreeFeature::buildBranch(WorldGenLevel& level, std::vector<core::BlockPos>& leaves,
                                     const core::BlockPos& pos, const Placers& placers, int height,
                                     double length, double angle, double tilt, WorldgenRandom& random,
                                     const TFTreeFeatureConfig& config) {
    const core::BlockPos src = pos.above(height);
    core::BlockPos dest = twilight::translate(src, length, angle, tilt);

    // constrain branch spread
    const int limit = 5;
    if ((dest.getX() - pos.getX()) < -limit) dest = core::BlockPos(pos.getX() - limit, dest.getY(), dest.getZ());
    if ((dest.getX() - pos.getX()) > limit) dest = core::BlockPos(pos.getX() + limit, dest.getY(), dest.getZ());
    if ((dest.getZ() - pos.getZ()) < -limit) dest = core::BlockPos(dest.getX(), dest.getY(), pos.getZ() - limit);
    if ((dest.getZ() - pos.getZ()) > limit) dest = core::BlockPos(dest.getX(), dest.getY(), pos.getZ() + limit);

    const BlockStateProvider& branch = *config.branchProvider;
    drawBresenhamBranch(level, placers.trunk, random, src, dest, branch);   // trunk == false

    placeIfValidTreePos(level, placers.trunk, random, dest.east(), branch);
    placeIfValidTreePos(level, placers.trunk, random, dest.west(), branch);
    placeIfValidTreePos(level, placers.trunk, random, dest.north(), branch);
    placeIfValidTreePos(level, placers.trunk, random, dest.south(), branch);

    leaves.push_back(dest);
}

// ============================================================================
// LargeWinterTreeFeature — trees/LargeWinterTreeFeature.java
// ============================================================================
bool LargeWinterTreeFeature::generate(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos,
                                      const Placers& placers, const TFTreeFeatureConfig& config) {
    int treeHeight = 35;
    if (random.nextInt(3) == 0) {
        treeHeight += random.nextInt(10);
        if (random.nextInt(8) == 0) {
            treeHeight += random.nextInt(10);
        }
    }

    if (isOutsideBuildHeight(level, pos.getY() + treeHeight)) return false;

    // SnowTreeFeature.validTreePos: air or #minecraft:replaceable_by_trees.
    if (!TreeFeature::validTreePos(level, pos)) return false;

    buildTrunk(level, placers, random, pos, treeHeight, config);
    makeLeaves(level, placers, random, pos, treeHeight, config);

    const int numRoots = 4 + random.nextInt(3);
    const float offset = random.nextFloat();
    for (int b = 0; b < numRoots; ++b) {
        buildRoot(level, placers.decoration, random, pos, static_cast<double>(offset), b, *config.rootsProvider);
    }
    return true;
}

void LargeWinterTreeFeature::makeLeaves(WorldGenLevel& level, const Placers& placers, WorldgenRandom& random,
                                        const core::BlockPos& pos, int treeHeight,
                                        const TFTreeFeatureConfig& config) {
    const int offGround = 3;
    for (int dy = 0; dy < treeHeight; ++dy) {
        // leafRadius(treeHeight, dy, 1) = (int) (4F * dy / treeHeight + (0.75F * dy % 3))
        const float scaled = 4.0f * static_cast<float>(dy);
        const float fraction = scaled / static_cast<float>(treeHeight);
        const float step = 0.75f * static_cast<float>(dy);
        const float wrapped = std::fmod(step, 3.0f);
        const float sum = fraction + wrapped;
        const int radius = static_cast<int>(sum);

        const core::BlockPos layer = pos.above(offGround + treeHeight - dy);
        placeCircleEven(level, placers.leaves, random, layer, static_cast<float>(radius),
                        *config.leavesProvider, false);
        makePineBranches(level, placers, random, layer, radius, config);
    }
}

void LargeWinterTreeFeature::makePineBranches(WorldGenLevel& level, const Placers& placers, WorldgenRandom& random,
                                              const core::BlockPos& pos, int radius,
                                              const TFTreeFeatureConfig& config) {
    (void)level;
    const int branchLength = radius > 4 ? radius - 1 : radius - 2;

    // placeLogAt: the trunk provider's state turned to the axis, unconditionally.
    auto placeLogAt = [&](const core::BlockPos& at, core::Axis axis) {
        BlockState* state = config.trunkProvider->getState(random, at);
        if (state != nullptr && state->hasProperty(BlockStateProperties::AXIS)) {
            state = state->setValue(*BlockStateProperties::AXIS, axis);
        }
        placers.trunk(at, state);
    };

    // Java int % keeps the dividend's sign: a negative odd y is -1, no branches.
    switch (pos.getY() % 2) {
        case 0:
            for (int i = 1; i <= branchLength; ++i) {
                placeLogAt(pos.offset(-i, 0, 0), core::Axis::X);
                placeLogAt(pos.offset(0, 0, i + 1), core::Axis::Z);
                placeLogAt(pos.offset(i + 1, 0, 1), core::Axis::X);
                placeLogAt(pos.offset(1, 0, -i), core::Axis::Z);
            }
            break;
        case 1:
            for (int i = 1; i <= branchLength; ++i) {
                placeLogAt(pos.offset(-1, 0, 1), core::Axis::X);
                placeLogAt(pos.offset(1, 0, i + 1), core::Axis::Z);
                placeLogAt(pos.offset(i + 1, 0, 0), core::Axis::X);
                placeLogAt(pos.offset(0, 0, -i), core::Axis::Z);
            }
            break;
        default:
            break;
    }
}

void LargeWinterTreeFeature::buildTrunk(WorldGenLevel& level, const Placers& placers, WorldgenRandom& random,
                                        const core::BlockPos& pos, int treeHeight,
                                        const TFTreeFeatureConfig& config) {
    const BlockStateProvider& trunk = *config.trunkProvider;
    static constexpr int kColumns[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    for (int dy = 0; dy < treeHeight; ++dy) {
        for (const auto& column : kColumns) {
            if (placeIfValidTreePos(level, placers.trunk, random, pos.offset(column[0], dy, column[1]), trunk)
                && dy == 0 && m_dirt != nullptr) {
                level.setBlock(pos.offset(column[0], -1, column[1]), m_dirt, kKnownShapeAll);
            }
        }
    }
}

// ============================================================================
// HollowStumpFeature — trees/HollowStumpFeature.java + HollowTreeFeature.java
// ============================================================================
bool HollowStumpFeature::generate(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos,
                                  const Placers& placers, const TFTreeFeatureConfig& config) {
    const int radius = random.nextInt(2) + 2;

    if (!isAreaSuitable(level, pos.offset(-radius, 0, -radius), 2 * radius, 6, 2 * radius, false)) {
        return false;
    }

    buildSmallTrunk(level, placers, random, pos, radius, 6, config);

    // Roots first, so they don't fail placement against the trunk shell.
    // 3-5 roots at the bottom
    buildRootRing(level, random, pos, radius, 3, 2, 6, 0.75, 3, 5, config);
    // several more taproots
    buildRootRing(level, random, pos, radius, 1, 2, 8, 0.9, 3, 5, config);
    return true;
}

void HollowStumpFeature::buildSmallTrunk(WorldGenLevel& level, const Placers& placers, WorldgenRandom& random,
                                         const core::BlockPos& pos, int diameter, int maxHeight,
                                         const TFTreeFeatureConfig& config) {
    (void)maxHeight;   // unused in the mod as well
    const int hollow = diameter >> 1;

    // go down 4 squares and fill in extra trunk as needed, for uneven terrain
    for (int dx = -diameter; dx <= diameter; ++dx) {
        for (int dz = -diameter; dz <= diameter; ++dz) {
            for (int dy = -4; dy < 0; ++dy) {
                const int ax = std::abs(dx);
                const int az = std::abs(dz);
                const int dist = static_cast<int>(static_cast<double>(std::max(ax, az))
                                                  + static_cast<double>(std::min(ax, az)) * 0.5);
                if (dist <= diameter) {
                    const core::BlockPos dPos = pos.offset(dx, dy, dz);
                    if (hasEmptyNeighborExceptBelow(level, dPos)) {
                        const BlockStateProvider& provider = dist > hollow ? *config.trunkProvider
                                                                           : *config.branchProvider;
                        placers.trunk(dPos, provider.getState(random, dPos));
                    } else {
                        placeIfValidRootPos(level, placers.decoration, random, dPos, *config.rootsProvider);
                    }
                }
            }
        }
    }

    // build the trunk upwards
    for (int dx = -diameter; dx <= diameter; ++dx) {
        for (int dz = -diameter; dz <= diameter; ++dz) {
            int height = 2 + random.nextInt(3);
            height += random.nextInt(2);

            for (int dy = 0; dy <= height; ++dy) {
                const int ax = std::abs(dx);
                const int az = std::abs(dz);
                const int dist = std::max(ax, az) + (std::min(ax, az) >> 1);
                if (dist <= diameter && dist > hollow) {
                    placeIfValidTreePos(level, placers.trunk, random, pos.offset(dx, dy, dz), *config.trunkProvider);
                }
            }
        }
    }
}

void HollowStumpFeature::buildRootRing(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos,
                                       int radius, int branchHeight, int heightVar, int length, double tilt,
                                       int minBranches, int maxBranches, const TFTreeFeatureConfig& config) {
    const int numBranches = random.nextInt(maxBranches - minBranches) + minBranches;
    const double branchRotation = 1.0 / (numBranches + 1);
    const double branchOffset = random.nextDouble();

    for (int i = 0; i <= numBranches; ++i) {
        int dHeight;
        if (heightVar > 0) {
            dHeight = branchHeight - heightVar + random.nextInt(2 * heightVar);
        } else {
            dHeight = branchHeight;
        }
        makeRoot(level, random, pos, radius, dHeight, static_cast<double>(length),
                 i * branchRotation + branchOffset, tilt, config);
    }
}

void HollowStumpFeature::makeRoot(WorldGenLevel& level, WorldgenRandom& random, const core::BlockPos& pos,
                                  int diameter, int branchHeight, double length, double angle, double tilt,
                                  const TFTreeFeatureConfig& config) {
    const core::BlockPos src = twilight::translate(pos.above(branchHeight), static_cast<double>(diameter), angle, 0.5);
    const core::BlockPos dest = twilight::translate(src, length, angle, tilt);

    // A two-deep root placer writing straight to the world (UPDATE_ALL).
    WorldGenLevel* world = &level;
    RootPlacer placer{[world](const core::BlockPos& checkedPos, BlockState* state) {
                          if (state == nullptr) return;
                          world->setBlock(checkedPos, state, kUpdateAll);
                          world->setBlock(checkedPos.below(), state, kUpdateAll);
                      }, 2};
    twilight::VoxelBresenhamIterator tracer(src, dest);
    traceExposedRoot(level, placer, random, *config.branchProvider, *config.rootsProvider, tracer);
}

// ============================================================================
// FallenHollowLogFeature — feature/FallenHollowLogFeature.java
// ============================================================================
bool FallenHollowLogFeature::place(FeaturePlaceContext<FallenHollowLogConfig>& context) {
    WorldGenLevel* level = context.level();
    if (level == nullptr) return false;
    WorldgenRandom& random = context.random();
    return random.nextBoolean() ? makeLog4Z(*level, random, context.origin(), context.config())
                                : makeLog4X(*level, random, context.origin(), context.config());
}

bool FallenHollowLogFeature::makeLog4Z(WorldGenLevel& world, WorldgenRandom& rand, const core::BlockPos& pos,
                                       const FallenHollowLogConfig& c) {
    // +Z 4x4 log
    if (!isAreaSuitable(world, pos, 4, 3, 9, false)) return false;

    auto negativeJaggy = [&](int length, int dx, int dy) {
        for (int dz = -length; dz < 0; ++dz) world.setBlock(pos.offset(dx, dy, dz + 3), c.oakLogWithZAxis, kUpdateAll);
    };
    auto positiveJaggy = [&](int length, int dx, int dy) {
        for (int dz = 0; dz < length; ++dz) world.setBlock(pos.offset(dx, dy, dz + 7), c.oakLogWithZAxis, kUpdateAll);
    };

    // jaggy parts
    negativeJaggy(rand.nextInt(3), 0, 0);
    negativeJaggy(rand.nextInt(3), 3, 0);
    negativeJaggy(rand.nextInt(3), 0, 1);
    negativeJaggy(rand.nextInt(3), 3, 1);
    negativeJaggy(rand.nextInt(3), 1, 2);
    negativeJaggy(rand.nextInt(3), 2, 2);

    positiveJaggy(rand.nextInt(3), 0, 0);
    positiveJaggy(rand.nextInt(3), 3, 0);
    positiveJaggy(rand.nextInt(3), 0, 1);
    positiveJaggy(rand.nextInt(3), 3, 1);
    positiveJaggy(rand.nextInt(3), 1, 2);
    positiveJaggy(rand.nextInt(3), 2, 2);

    // center
    for (int dz = 0; dz < 4; ++dz) {
        // floor
        for (int fx = 1; fx <= 2; ++fx) {
            if (rand.nextBoolean()) {
                world.setBlock(pos.offset(fx, -1, dz + 3), c.oakLogWithZAxis, kUpdateAll);
                if (rand.nextBoolean()) {
                    world.setBlock(pos.offset(fx, 0, dz + 3), c.mossPatch, kUpdateAll);
                    markAboveForPostProcessing(&world, pos.offset(fx, -1, dz + 3));
                }
            } else {
                world.setBlock(pos.offset(fx, -1, dz + 3), c.grass, kUpdateAll);
                world.setBlock(pos.offset(fx, 0, dz + 3), c.mossPatch, kUpdateAll);
                markAboveForPostProcessing(&world, pos.offset(fx, -1, dz + 3));
            }
        }

        // log part
        world.setBlock(pos.offset(0, 0, dz + 3), c.oakLogWithZAxis, kUpdateAll);
        world.setBlock(pos.offset(3, 0, dz + 3), c.oakLogWithZAxis, kUpdateAll);
        world.setBlock(pos.offset(0, 1, dz + 3), c.oakLogWithZAxis, kUpdateAll);
        world.setBlock(pos.offset(3, 1, dz + 3), c.oakLogWithZAxis, kUpdateAll);
        world.setBlock(pos.offset(1, 2, dz + 3), c.oakLogWithZAxis, kUpdateAll);
        world.setBlock(pos.offset(2, 2, dz + 3), c.oakLogWithZAxis, kUpdateAll);
        if (rand.nextBoolean()) {
            world.setBlock(pos.offset(1, 3, dz + 3), c.mossPatch, kUpdateAll);
            markAboveForPostProcessing(&world, pos.offset(1, 2, dz + 3));
        }
        if (rand.nextBoolean()) {
            world.setBlock(pos.offset(2, 3, dz + 3), c.mossPatch, kUpdateAll);
            markAboveForPostProcessing(&world, pos.offset(2, 2, dz + 3));
        }
    }

    // a few leaves?
    const int offZ = rand.nextInt(3) + 2;
    const bool plusX = rand.nextBoolean();
    for (int dz = 0; dz < 3; ++dz) {
        if (rand.nextBoolean()) {
            world.setBlock(pos.offset(plusX ? 3 : 0, 2, dz + offZ), c.oakLeaves, kUpdateAll);
            if (rand.nextBoolean()) {
                world.setBlock(pos.offset(plusX ? 3 : 0, 3, dz + offZ), c.oakLeaves, kUpdateAll);
            }
            if (rand.nextBoolean()) {
                world.setBlock(pos.offset(plusX ? 4 : -1, 2, dz + offZ), c.oakLeaves, kUpdateAll);
            }
        }
    }

    // firefly
    const int fireflyZ = rand.nextInt(4) + 3;
    world.setBlock(pos.offset(plusX ? 0 : 3, 2, fireflyZ), c.firefly, kUpdateAll);
    return true;
}

bool FallenHollowLogFeature::makeLog4X(WorldGenLevel& world, WorldgenRandom& rand, const core::BlockPos& pos,
                                       const FallenHollowLogConfig& c) {
    // +X 4x4 log
    if (!isAreaSuitable(world, pos, 9, 3, 4, false)) return false;

    auto negativeJaggy = [&](int length, int dz, int dy) {
        for (int dx = -length; dx < 0; ++dx) world.setBlock(pos.offset(dx + 3, dy, dz), c.oakLogWithXAxis, kUpdateAll);
    };
    auto positiveJaggy = [&](int length, int dz, int dy) {
        for (int dx = 0; dx < length; ++dx) world.setBlock(pos.offset(dx + 7, dy, dz), c.oakLogWithXAxis, kUpdateAll);
    };

    // jaggy parts
    negativeJaggy(rand.nextInt(3), 0, 0);
    negativeJaggy(rand.nextInt(3), 3, 0);
    negativeJaggy(rand.nextInt(3), 0, 1);
    negativeJaggy(rand.nextInt(3), 3, 1);
    negativeJaggy(rand.nextInt(3), 1, 2);
    negativeJaggy(rand.nextInt(3), 2, 2);

    positiveJaggy(rand.nextInt(3), 0, 0);
    positiveJaggy(rand.nextInt(3), 3, 0);
    positiveJaggy(rand.nextInt(3), 0, 1);
    positiveJaggy(rand.nextInt(3), 3, 1);
    positiveJaggy(rand.nextInt(3), 1, 2);
    positiveJaggy(rand.nextInt(3), 2, 2);

    // center
    for (int dx = 0; dx < 4; ++dx) {
        // floor
        for (int fz = 1; fz <= 2; ++fz) {
            if (rand.nextBoolean()) {
                world.setBlock(pos.offset(dx + 3, -1, fz), c.oakLogWithXAxis, kUpdateAll);
                if (rand.nextBoolean()) {
                    world.setBlock(pos.offset(dx + 3, 0, fz), c.mossPatch, kUpdateAll);
                    markAboveForPostProcessing(&world, pos.offset(dx + 3, -1, fz));
                }
            } else {
                world.setBlock(pos.offset(dx + 3, -1, fz), c.grass, kUpdateAll);
                world.setBlock(pos.offset(dx + 3, 0, fz), c.mossPatch, kUpdateAll);
                markAboveForPostProcessing(&world, pos.offset(dx + 3, -1, fz));
            }
        }

        // log part
        world.setBlock(pos.offset(dx + 3, 0, 0), c.oakLogWithXAxis, kUpdateAll);
        world.setBlock(pos.offset(dx + 3, 0, 3), c.oakLogWithXAxis, kUpdateAll);
        world.setBlock(pos.offset(dx + 3, 1, 0), c.oakLogWithXAxis, kUpdateAll);
        world.setBlock(pos.offset(dx + 3, 1, 3), c.oakLogWithXAxis, kUpdateAll);
        world.setBlock(pos.offset(dx + 3, 2, 1), c.oakLogWithXAxis, kUpdateAll);
        world.setBlock(pos.offset(dx + 3, 2, 2), c.oakLogWithXAxis, kUpdateAll);
        if (rand.nextBoolean()) {
            world.setBlock(pos.offset(dx + 3, 3, 1), c.mossPatch, kUpdateAll);
            markAboveForPostProcessing(&world, pos.offset(dx + 3, 2, 1));
        }
        if (rand.nextBoolean()) {
            world.setBlock(pos.offset(dx + 3, 3, 2), c.mossPatch, kUpdateAll);
            markAboveForPostProcessing(&world, pos.offset(dx + 3, 2, 2));
        }
    }

    // a few leaves?
    const int offX = rand.nextInt(3) + 2;
    const bool plusZ = rand.nextBoolean();
    for (int dx = 0; dx < 3; ++dx) {
        if (rand.nextBoolean()) {
            world.setBlock(pos.offset(dx + offX, 2, plusZ ? 3 : 0), c.oakLeaves, kUpdateAll);
            if (rand.nextBoolean()) {
                world.setBlock(pos.offset(dx + offX, 3, plusZ ? 3 : 0), c.oakLeaves, kUpdateAll);
            }
            if (rand.nextBoolean()) {
                world.setBlock(pos.offset(dx + offX, 2, plusZ ? 4 : -1), c.oakLeaves, kUpdateAll);
            }
        }
    }

    // firefly
    const int fireflyX = rand.nextInt(4) + 3;
    world.setBlock(pos.offset(fireflyX, 2, plusZ ? 0 : 3), c.firefly, kUpdateAll);
    return true;
}

// ============================================================================
// SmallFallenLogFeature — feature/SmallFallenLogFeature.java
// ============================================================================
bool SmallFallenLogFeature::place(FeaturePlaceContext<HollowLogConfig>& context) {
    WorldGenLevel* levelPtr = context.level();
    if (levelPtr == nullptr) return false;
    WorldGenLevel& world = *levelPtr;
    WorldgenRandom& rand = context.random();
    const HollowLogConfig& config = context.config();
    core::BlockPos pos = context.origin();

    const bool shouldMakeAllHollow = rand.nextBoolean();
    const bool goingX = rand.nextBoolean();
    const int length = rand.nextInt(4) + 3;

    // check area clear
    if (goingX) {
        if (!isAreaSuitable(world, pos, length, 2, 2, true)) return false;
    } else {
        if (!isAreaSuitable(world, pos, 2, 2, length, true)) return false;
    }

    // sometimes make floating logs
    if (rand.nextInt(5) == 0) {
        BlockState* here = world.getBlockState(pos);
        if (here != nullptr && here->isFluid()) {
            core::BlockPos floatingPos = pos;
            for (int i = 0; i < 10; ++i) {
                BlockState* above = world.getBlockState(floatingPos.above());
                if (above != nullptr && above->isAir()) {
                    pos = floatingPos;
                    break;
                }
                floatingPos = floatingPos.above();
            }
        }
    }

    // determineHollowProperties: water -> WATERLOGGED, #snow -> SNOW,
    // 1 in 5 MOSS_AND_GRASS, 1 in 3 MOSS, else EMPTY.
    auto determineHollowProperties = [&]() -> int {
        BlockState* here = world.getBlockState(pos);
        if (here != nullptr && here->getIdentifier() == "minecraft:water") return HollowLogConfig::WATERLOGGED;
        if (isSnowTag(here)) return HollowLogConfig::SNOW;
        if (rand.nextInt(5) == 0) return HollowLogConfig::MOSS_AND_GRASS;
        if (rand.nextInt(3) == 0) return HollowLogConfig::MOSS;
        return HollowLogConfig::EMPTY;
    };

    // mossOrSeagrass: no moss if we're cold; seagrass in water.
    auto mossOrSeagrass = [&](const core::BlockPos& at) -> BlockState* {
        if (isSnowTag(world.getBlockState(at.below(2)))) return config.air;
        BlockState* here = world.getBlockState(at);
        return (here != nullptr && here->getIdentifier() == "minecraft:water") ? config.seagrass : config.mossPatch;
    };

    // hollowOrNormal draws from the level's own random (WorldGenRegion.getRandom).
    auto hollowOrNormal = [&](BlockState* hollow, BlockState* normal) -> BlockState* {
        const bool chosen = shouldMakeAllHollow || world.getRandom().nextInt(3) == 0;
        return (chosen && hollow != nullptr && !hollow->isAir()) ? hollow : normal;
    };

    BlockState* logState = goingX ? config.normalX : config.normalZ;
    BlockState* branchState = goingX ? config.normalZ : config.normalX;
    BlockState* hollowLogState = nullptr;
    if (config.hasHollow()) {
        const int variant = determineHollowProperties();
        hollowLogState = goingX ? config.hollowX[static_cast<size_t>(variant)]
                                : config.hollowZ[static_cast<size_t>(variant)];
    }

    if (goingX) {
        for (int lx = 0; lx < length; ++lx) {
            world.setBlock(pos.offset(lx, 0, 1), hollowOrNormal(hollowLogState, logState), kUpdateAll);
            if (rand.nextInt(3) > 0) {
                world.setBlock(pos.offset(lx, 1, 1), mossOrSeagrass(pos.offset(lx, 1, 1)), kUpdateAll);
                markAboveForPostProcessing(&world, pos.offset(lx, 0, 1));
            }
        }
    } else {
        for (int lz = 0; lz < length; ++lz) {
            world.setBlock(pos.offset(1, 0, lz), hollowOrNormal(hollowLogState, logState), kUpdateAll);
            if (rand.nextInt(3) > 0) {
                world.setBlock(pos.offset(1, 1, lz), mossOrSeagrass(pos.offset(1, 1, lz)), kUpdateAll);
                markAboveForPostProcessing(&world, pos.offset(1, 0, lz));
            }
        }
    }

    // possibly make branch
    if (rand.nextInt(3) > 0) {
        int bx;
        int bz;
        if (goingX) {
            bx = rand.nextInt(length);
            bz = rand.nextBoolean() ? 2 : 0;
        } else {
            bx = rand.nextBoolean() ? 2 : 0;
            bz = rand.nextInt(length);
        }
        world.setBlock(pos.offset(bx, 0, bz), branchState, kUpdateAll);
        if (rand.nextBoolean()) {
            world.setBlock(pos.offset(bx, 1, bz), mossOrSeagrass(pos.offset(bx, 1, bz)), kUpdateAll);
            markAboveForPostProcessing(&world, pos.offset(bx, 0, bz));
        }
    }
    return true;
}

// ============================================================================
// WoodRootFeature — feature/WoodRootFeature.java
// ============================================================================
bool WoodRootFeature::place(FeaturePlaceContext<RootConfig>& context) {
    WorldGenLevel* levelPtr = context.level();
    const RootConfig& config = context.config();
    if (levelPtr == nullptr || !config.blockRoot || !config.oreRoot) return false;
    WorldGenLevel& world = *levelPtr;
    WorldgenRandom& rand = context.random();
    const core::BlockPos& pos = context.origin();

    // start must be in stone
    if (!isNamed(world.getBlockState(pos), "minecraft:stone")) return false;

    const float first = rand.nextFloat() * 6.0f;
    const float second = rand.nextFloat() * 6.0f;
    float length = first + second;
    length = length + 4.0f;
    if (length > static_cast<float>(pos.getY())) {
        length = static_cast<float>(pos.getY());
    }

    // tilt between 0.6 and 0.9
    const float tiltStep = rand.nextFloat() * 0.3f;
    const float tilt = 0.6f + tiltStep;
    const float angle = rand.nextFloat();
    return drawRoot(world, rand, pos, pos, length, angle, tilt, config);
}

bool WoodRootFeature::drawRoot(WorldGenLevel& world, WorldgenRandom& rand, const core::BlockPos& oPos,
                               const core::BlockPos& pos, float length, float angle, float tilt,
                               const RootConfig& config) {
    // generate a direction and a length
    core::BlockPos dest = twilight::translate(pos, static_cast<double>(length), static_cast<double>(angle),
                                              static_cast<double>(tilt));

    // restrict x and z to within 7
    const int limit = 6;
    if (oPos.getX() + limit < dest.getX()) dest = core::BlockPos(oPos.getX() + limit, dest.getY(), dest.getZ());
    if (oPos.getX() - limit > dest.getX()) dest = core::BlockPos(oPos.getX() - limit, dest.getY(), dest.getZ());
    if (oPos.getZ() + limit < dest.getZ()) dest = core::BlockPos(dest.getX(), dest.getY(), oPos.getZ() + limit);
    if (oPos.getZ() - limit > dest.getZ()) dest = core::BlockPos(dest.getX(), dest.getY(), oPos.getZ() - limit);

    // end must be in stone
    if (!isNamed(world.getBlockState(dest), "minecraft:stone")) return false;

    // if both the start and the end are in stone, put a root there
    WorldGenLevel* worldPtr = &world;
    const RootPlacer placer{[worldPtr](const core::BlockPos& checkedPos, BlockState* state) {
                                if (state != nullptr) worldPtr->setBlock(checkedPos, state, kUpdateAll);
                            }, 1};
    twilight::VoxelBresenhamIterator tracer(pos, dest);
    traceRoot(world, placer, rand, *config.blockRoot, tracer);

    const float halfLength = length / 2.0f;

    // if we are long enough, make either another root or an oreball
    if (length > 8.0f) {
        if (rand.nextInt(3) > 0) {
            // usually split off into another root half as long
            const core::BlockPos nextSrc = twilight::translate(pos, static_cast<double>(halfLength),
                                                               static_cast<double>(angle), static_cast<double>(tilt));
            const float angleStep = rand.nextFloat() * 0.5f;
            float nextAngle = angle + 0.25f;
            nextAngle = nextAngle + angleStep;
            nextAngle = std::fmod(nextAngle, 1.0f);
            const float tiltStep = rand.nextFloat() * 0.3f;
            const float nextTilt = 0.6f + tiltStep;
            drawRoot(world, rand, oPos, nextSrc, halfLength, nextAngle, nextTilt, config);
        }
    }

    if (length > 6.0f) {
        if (rand.nextInt(4) == 0) {
            // potentially make an oreball
            const core::BlockPos ballSrc = twilight::translate(pos, static_cast<double>(halfLength),
                                                               static_cast<double>(angle), static_cast<double>(tilt));
            const float oppositeAngle = std::fmod(angle + 0.5f, 1.0f);
            const core::BlockPos ballDest = twilight::translate(ballSrc, 1.5, static_cast<double>(oppositeAngle), 0.75);

            const BlockStateProvider& ore = *config.oreRoot;
            placeRootBlock(world, ballSrc, ore, rand);
            placeRootBlock(world, core::BlockPos(ballSrc.getX(), ballSrc.getY(), ballDest.getZ()), ore, rand);
            placeRootBlock(world, core::BlockPos(ballDest.getX(), ballSrc.getY(), ballSrc.getZ()), ore, rand);
            placeRootBlock(world, core::BlockPos(ballSrc.getX(), ballSrc.getY(), ballDest.getZ()), ore, rand);
            placeRootBlock(world, core::BlockPos(ballSrc.getX(), ballDest.getY(), ballSrc.getZ()), ore, rand);
            placeRootBlock(world, core::BlockPos(ballSrc.getX(), ballDest.getY(), ballDest.getZ()), ore, rand);
            placeRootBlock(world, core::BlockPos(ballDest.getX(), ballDest.getY(), ballSrc.getZ()), ore, rand);
            placeRootBlock(world, ballDest, ore, rand);
        }
    }
    return true;
}

bool WoodRootFeature::placeRootBlock(WorldGenLevel& world, const core::BlockPos& pos,
                                     const BlockStateProvider& state, WorldgenRandom& random) {
    if (!canRootGrowIn(world, pos)) return false;
    BlockState* placed = state.getState(random, pos);
    return placed != nullptr && world.setBlock(pos, placed, kUpdateAll);
}

// ============================================================================
// UndergroundPlantFeature — feature/UndergroundPlantFeature.java
// ============================================================================
bool TwilightUndergroundPlantFeature::canSurvive(const UndergroundPlantConfig& config, BlockState* state,
                                                 WorldGenLevel& level, const core::BlockPos& pos) {
    switch (config.survival) {
        case UndergroundPlantConfig::Survival::ROOT_STRAND: {
            // RootStrandBlock.canSurvive: TFPlantBlock.canPlaceRootAt (the block
            // above is in #twilightforest:plants_hang_on = #substrate_overworld,
            // moss_block, mangrove_root, root, liveroot_block) or root strand above.
            BlockState* above = level.getBlockState(pos.above());
            return hasTag(above, "minecraft:substrate_overworld")
                || isNamed(above, "minecraft:moss_block")
                || isNamed(above, g_names.mangroveRoot)
                || isNamed(above, g_names.root)
                || isNamed(above, g_names.liveroot)
                || isNamed(above, g_names.rootStrand);
        }
        case UndergroundPlantConfig::Survival::TROLL_ROOT: {
            // TrollRootBlock.canSurvive = canPlaceRootBelow(pos.above()):
            // #base_stone_overworld, trollvidr, trollber or unripe_trollber.
            BlockState* above = level.getBlockState(pos.above());
            return hasTag(above, "minecraft:base_stone_overworld")
                || isNamed(above, g_names.trollvidr)
                || isNamed(above, g_names.trollber)
                || isNamed(above, g_names.unripeTrollber);
        }
        case UndergroundPlantConfig::Survival::VANILLA:
        default:
            return state->canSurvive(level, pos);
    }
}

bool TwilightUndergroundPlantFeature::place(FeaturePlaceContext<UndergroundPlantConfig>& context) {
    WorldGenLevel* worldPtr = context.level();
    const UndergroundPlantConfig& config = context.config();
    if (worldPtr == nullptr || config.state == nullptr) return false;
    WorldGenLevel& world = *worldPtr;
    WorldgenRandom& random = context.random();
    const core::BlockPos& origin = context.origin();

    int x = origin.getX();
    int z = origin.getZ();
    int placed = 0;

    for (int y = origin.getY(); y > world.getMinY(); --y) {
        if (placed >= config.maxCount) break;

        const core::BlockPos pos(x, y, z);
        if (!world.isEmptyBlock(pos) || random.nextInt(6) == 0) {
            // origin + nextInt(4) - nextInt(4), left to right as Java evaluates it
            int dx = random.nextInt(4);
            dx -= random.nextInt(4);
            int dz = random.nextInt(4);
            dz -= random.nextInt(4);
            x = origin.getX() + dx;
            z = origin.getZ() + dz;
            continue;
        }

        BlockState* state = config.state;
        // LandmarkUtil.locateNearestLandmarkStart: the library's WorldGenLevel
        // exposes no structure starts, so the "not inside a landmark" half of
        // the test below always passes (it draws no randomness).
        if (config.isTrollvidr && random.nextInt(10) == 0 && config.unripeTrollber != nullptr) {
            state = config.unripeTrollber;
        }
        if (canSurvive(config, state, world, pos)) {
            world.setBlock(pos, state, kKnownShapeClients);
            ++placed;
        }
    }
    return placed > 0;
}

// ============================================================================
// CheckAbovePatchFeature — feature/CheckAbovePatchFeature.java
// ============================================================================
bool CheckAbovePatchFeature::place(FeaturePlaceContext<CheckAbovePatchConfig>& context) {
    const CheckAbovePatchConfig& config = context.config();
    WorldGenLevel* level = context.level();
    if (level == nullptr || !config.stateProvider || !config.target || !config.radius) return false;
    WorldgenRandom& random = context.random();
    const core::BlockPos& origin = context.origin();

    bool flag = false;
    const int i = origin.getY();
    const int j = i + config.halfHeight;
    const int k = i - config.halfHeight - 1;
    const int l = config.radius->sample(random);

    // BlockPos.betweenClosed(origin - (l, 0, l), origin + (l, 0, l)): x fastest, then z.
    for (int bz = origin.getZ() - l; bz <= origin.getZ() + l; ++bz) {
        for (int bx = origin.getX() - l; bx <= origin.getX() + l; ++bx) {
            const int i1 = bx - origin.getX();
            const int j1 = bz - origin.getZ();
            if (i1 * i1 + j1 * j1 <= l * l) {
                flag |= placeColumn(config, level, random, j, k, bx, bz);
            }
        }
    }
    return flag;
}

bool CheckAbovePatchFeature::placeColumn(const CheckAbovePatchConfig& config, WorldGenLevel* level,
                                         WorldgenRandom& random, int start, int end, int x, int z) {
    bool flag = false;
    for (int y = start; y > end; --y) {
        const core::BlockPos pos(x, y, z);
        if (!config.target->test(*level, pos)) continue;
        BlockState* above = level->getBlockState(pos.above());
        if (above == nullptr || !above->canBeReplaced()) continue;

        BlockState* state = config.stateProvider->getState(random, pos);
        if (state != nullptr) level->setBlock(pos, state, kUpdateClients);
        markAboveForPostProcessing(level, pos);
        flag = true;
    }
    return flag;
}

// ============================================================================
// SnowUnderTreeFeature — trees/SnowUnderTreeFeature.java
// ============================================================================
bool SnowUnderTreeFeature::place(FeaturePlaceContext<SnowUnderTreeConfig>& context) {
    WorldGenLevel* world = context.level();
    BlockState* snow = context.config().snow;
    if (world == nullptr || snow == nullptr) return true;
    const core::BlockPos& pos = context.origin();

    for (int xi = 0; xi < 16; ++xi) {
        for (int zi = 0; zi < 16; ++zi) {
            const int x = pos.getX() + xi;
            const int z = pos.getZ() + zi;
            const core::BlockPos top(x, world->getHeight(Heightmap::Types::MOTION_BLOCKING, x, z) - 1, z);

            BlockState* topState = world->getBlockState(top);
            if (topState == nullptr || !topState->isLeaves()) continue;   // instanceof LeavesBlock

            const core::BlockPos ground(x, world->getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z), z);
            BlockState* state = world->getBlockState(ground);
            if (state == nullptr || !state->isAir()) continue;

            const core::BlockPos below = ground.below();
            BlockState* stateBelow = world->getBlockState(below);
            if (stateBelow != nullptr && stateBelow->isFaceSturdy(*world, below, core::Direction::UP)) {
                world->setBlock(ground, snow, kUpdateClients);
                if (stateBelow->hasProperty(BlockStateProperties::SNOWY)) {
                    world->setBlock(below, stateBelow->setValue(*BlockStateProperties::SNOWY, true), kUpdateClients);
                }
            }
        }
    }
    return true;
}

// ============================================================================
// Registry
// ============================================================================
namespace {

std::mutex s_bootstrapMutex;
bool s_initialized = false;

MegaCanopyTreeFeature s_megaCanopyFeature;
MegaOakTreeFeature s_megaOakFeature;
LargeWinterTreeFeature s_largeWinterTreeFeature;
HollowStumpFeature s_hollowStumpFeature;
FallenHollowLogFeature s_fallenHollowLogFeature;
SmallFallenLogFeature s_smallFallenLogFeature;
WoodRootFeature s_woodRootFeature;
TwilightUndergroundPlantFeature s_undergroundPlantFeature;
CheckAbovePatchFeature s_checkAbovePatchFeature;
SnowUnderTreeFeature s_snowUnderTreeFeature;
RandomSelectorFeature s_randomSelectorFeature;

std::vector<std::unique_ptr<levelgen::placement::PlacedFeature>> s_inlinePlaced;

using StateProperties = std::unordered_map<std::string, std::string>;

// A TF (or vanilla) block through TwilightBlocks; null (logged) when nothing fits.
BlockState* resolveState(const char* name, const StateProperties& properties, const char* feature) {
    BlockState* state = properties.empty() ? levelgen::twilight_blocks::defaultState(name)
                                           : levelgen::twilight_blocks::state(name, properties);
    if (state == nullptr) {
        std::fprintf(stderr, "[TwilightTreeFeatures] %s resolves to no block - %s not registered\n",
                     name, feature);
    }
    return state;
}

std::shared_ptr<BlockStateProvider> simple(BlockState* state) {
    return state ? std::make_shared<SimpleStateProvider>(state) : nullptr;
}

// An inline {"feature": id, "placement": []}.
levelgen::placement::PlacedFeature* inlinePlaced(ConfiguredFeature* feature, const std::string& name) {
    if (feature == nullptr) return nullptr;
    auto placed = std::make_unique<levelgen::placement::PlacedFeature>(feature, std::vector<levelgen::placement::PlacementModifier*>{}, name);
    levelgen::placement::PlacedFeature* raw = placed.get();
    s_inlinePlaced.push_back(std::move(placed));
    return raw;
}

// minecraft:random_selector — entries in file order, then the default.
std::unique_ptr<ConfiguredFeature> randomSelector(
    const std::vector<std::pair<const char*, float>>& entries, const char* defaultId, const std::string& name) {
    ConfiguredFeature* fallback = twilight::findConfigured(defaultId);
    if (fallback == nullptr) {
        std::fprintf(stderr, "[TwilightTreeFeatures] %s: default %s is not registered - skipped\n",
                     name.c_str(), defaultId);
        return nullptr;
    }
    std::vector<WeightedPlacedFeature> weighted;
    int index = 0;
    for (const auto& [id, chance] : entries) {
        ConfiguredFeature* feature = twilight::findConfigured(id);
        if (feature == nullptr) {
            // Kept so its chance still draws; selecting it places nothing.
            std::fprintf(stderr, "[TwilightTreeFeatures] %s: entry %s is not registered\n", name.c_str(), id);
        }
        weighted.emplace_back(inlinePlaced(feature, name + "_" + std::to_string(index++)), chance);
    }
    RandomFeatureConfiguration config(std::move(weighted), inlinePlaced(fallback, name + "_default"));
    return std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
        &s_randomSelectorFeature, config);
}

void registerIfPresent(const char* id, std::unique_ptr<ConfiguredFeature> feature) {
    if (feature) twilight::registerOwned(id, std::move(feature));
}

} // namespace

bool TwilightTreeFeatures::isInitialized() {
    std::lock_guard<std::mutex> lock(s_bootstrapMutex);
    return s_initialized;
}

void TwilightTreeFeatures::bootstrap() {
    std::lock_guard<std::mutex> lock(s_bootstrapMutex);
    if (s_initialized) return;
    if (!TwilightFeatures::isInitialized()) TwilightFeatures::bootstrap();

    namespace tb = levelgen::twilight_blocks;
    g_names.root = tb::resolveName("twilightforest:root");
    g_names.liveroot = tb::resolveName("twilightforest:liveroot_block");
    g_names.mangroveRoot = tb::resolveName("twilightforest:mangrove_root");
    g_names.timeWood = tb::resolveName("twilightforest:time_wood");
    g_names.rootStrand = tb::resolveName("twilightforest:root_strand");
    g_names.trollvidr = tb::resolveName("twilightforest:trollvidr");
    g_names.trollber = tb::resolveName("twilightforest:trollber");
    g_names.unripeTrollber = tb::resolveName("twilightforest:unripe_trollber");
    g_firefly = tb::defaultState("twilightforest:firefly");
    if (g_firefly == nullptr) {
        std::fprintf(stderr, "[TwilightTreeFeatures] twilightforest:firefly resolves to no block - "
                             "the mega trees grow without trunk fireflies\n");
    }

    const StateProperties axisY{{"axis", "y"}};
    const StateProperties leafProps{{"distance", "7"}, {"persistent", "false"}, {"waterlogged", "false"}};

    // ------------------------------------------------------------ trees
    // A TFTreeFeatureConfig from the JSON's four providers (all
    // simple_state_provider in these files); null if any block is missing.
    auto treeConfig = [&](const char* trunk, const char* leaves, const char* branch, const char* roots,
                          int minimumSize, int firstChance, int secondChance, bool hasLeaves, bool checkWater,
                          const char* feature) -> std::optional<TFTreeFeatureConfig> {
        BlockState* trunkState = resolveState(trunk, axisY, feature);
        BlockState* leavesState = resolveState(leaves, leafProps, feature);
        BlockState* branchState = resolveState(branch, axisY, feature);
        BlockState* rootsState = resolveState(roots, {}, feature);
        if (!trunkState || !leavesState || !branchState || !rootsState) return std::nullopt;
        return TFTreeFeatureConfig(simple(trunkState), simple(leavesState), simple(branchState), simple(rootsState),
                                   minimumSize, firstChance, secondChance, hasLeaves, checkWater, {});
    };

    // tree/mega_canopy_tree.json (twilightforest:mega_canopy)
    if (auto config = treeConfig("twilightforest:canopy_log", "twilightforest:canopy_leaves",
                                 "twilightforest:canopy_wood", "twilightforest:root",
                                 30, 2, 3, false, false, "tree/mega_canopy_tree")) {
        registerIfPresent("twilightforest:tree/mega_canopy_tree",
            std::make_unique<ConfiguredFeatureImpl<TFTreeFeatureConfig, MegaCanopyTreeFeature>>(
                &s_megaCanopyFeature, *config));
    }
    // tree/forest_mega_oak_tree.json / tree/savannah_mega_oak_tree.json (twilightforest:mega_oak)
    if (auto config = treeConfig("twilightforest:twilight_oak_log", "twilightforest:twilight_oak_leaves",
                                 "twilightforest:twilight_oak_wood", "twilightforest:root",
                                 24, 1, 1, false, false, "tree/forest_mega_oak_tree")) {
        registerIfPresent("twilightforest:tree/forest_mega_oak_tree",
            std::make_unique<ConfiguredFeatureImpl<TFTreeFeatureConfig, MegaOakTreeFeature>>(
                &s_megaOakFeature, *config));
    }
    if (auto config = treeConfig("twilightforest:twilight_oak_log", "twilightforest:twilight_oak_leaves",
                                 "twilightforest:twilight_oak_wood", "twilightforest:root",
                                 16, 1, 1, false, false, "tree/savannah_mega_oak_tree")) {
        registerIfPresent("twilightforest:tree/savannah_mega_oak_tree",
            std::make_unique<ConfiguredFeatureImpl<TFTreeFeatureConfig, MegaOakTreeFeature>>(
                &s_megaOakFeature, *config));
    }
    // tree/large_winter_tree.json (twilightforest:large_winter_tree)
    s_largeWinterTreeFeature.setDirt(resolveState("minecraft:dirt", {}, "large_winter_tree trunk dirt"));
    if (auto config = treeConfig("minecraft:spruce_log", "minecraft:spruce_leaves", "minecraft:spruce_log",
                                 "twilightforest:root", 0, 1, 1, false, false, "tree/large_winter_tree")) {
        registerIfPresent("twilightforest:tree/large_winter_tree",
            std::make_unique<ConfiguredFeatureImpl<TFTreeFeatureConfig, LargeWinterTreeFeature>>(
                &s_largeWinterTreeFeature, *config));
    }
    // hollow_stump.json (twilightforest:hollow_stump)
    if (auto config = treeConfig("twilightforest:twilight_oak_log", "twilightforest:twilight_oak_leaves",
                                 "twilightforest:twilight_oak_wood", "twilightforest:root",
                                 0, 1, 1, false, false, "hollow_stump")) {
        registerIfPresent("twilightforest:hollow_stump",
            std::make_unique<ConfiguredFeatureImpl<TFTreeFeatureConfig, HollowStumpFeature>>(
                &s_hollowStumpFeature, *config));
    }

    // tree/selector/snowy_forest_trees.json — rebuilt with the real large
    // winter tree (pass one stood the mega spruce in for it).
    if (twilight::findConfigured("twilightforest:tree/large_winter_tree") != nullptr) {
        registerIfPresent("twilightforest:tree/selector/snowy_forest_trees",
            randomSelector({{"twilightforest:tree/mega_spruce_tree", 0.33f},
                            {"twilightforest:tree/large_winter_tree", 0.125f}},
                           "twilightforest:tree/snowy_spruce_tree", "snowy_forest_trees"));
    }

    // ------------------------------------------------------------ logs
    // hollow_log.json (twilightforest:fallen_hollow_log): the feature's own blocks.
    {
        FallenHollowLogConfig config;
        config.mossPatch = resolveState("twilightforest:moss_patch", {}, "hollow_log");
        config.oakLeaves = resolveState("twilightforest:twilight_oak_leaves", {{"persistent", "true"}}, "hollow_log");
        config.oakLogWithZAxis = resolveState("twilightforest:twilight_oak_log", {{"axis", "z"}}, "hollow_log");
        config.oakLogWithXAxis = resolveState("twilightforest:twilight_oak_log", {{"axis", "x"}}, "hollow_log");
        config.grass = resolveState("minecraft:grass_block", {}, "hollow_log");
        config.firefly = g_firefly;
        if (config.mossPatch && config.oakLeaves && config.oakLogWithZAxis && config.oakLogWithXAxis
            && config.grass && config.firefly) {
            registerIfPresent("twilightforest:hollow_log",
                std::make_unique<ConfiguredFeatureImpl<FallenHollowLogConfig, FallenHollowLogFeature>>(
                    &s_fallenHollowLogFeature, config));
        }
    }

    // *_fallen_log.json (twilightforest:fallen_small_log): normal log + hollow
    // horizontal log, resolved per axis and per hollow variant.
    BlockState* mossPatch = resolveState("twilightforest:moss_patch", {}, "fallen logs");
    BlockState* seagrass = resolveState("minecraft:seagrass", {}, "fallen logs");
    BlockState* air = resolveState("minecraft:air", {}, "fallen logs");
    auto fallenLog = [&](const char* id, const char* normal, const char* hollow) {
        if (!mossPatch || !seagrass || !air) return;
        HollowLogConfig config;
        config.normalX = resolveState(normal, {{"axis", "x"}}, id);
        config.normalZ = resolveState(normal, {{"axis", "z"}}, id);
        if (!config.normalX || !config.normalZ) return;
        static constexpr const char* kVariants[5] = {"empty", "moss", "moss_and_grass", "snow", "waterlogged"};
        for (size_t v = 0; v < 5; ++v) {
            config.hollowX[v] = levelgen::twilight_blocks::state(hollow, {{"axis", "x"}, {"variant", kVariants[v]}});
            config.hollowZ[v] = levelgen::twilight_blocks::state(hollow, {{"axis", "z"}, {"variant", kVariants[v]}});
        }
        if (!config.hasHollow()) {
            std::fprintf(stderr, "[TwilightTreeFeatures] %s resolves to no block - %s grows no hollow logs\n",
                         hollow, id);
        }
        config.mossPatch = mossPatch;
        config.seagrass = seagrass;
        config.air = air;
        registerIfPresent(id, std::make_unique<ConfiguredFeatureImpl<HollowLogConfig, SmallFallenLogFeature>>(
                                  &s_smallFallenLogFeature, config));
    };
    fallenLog("twilightforest:canopy_fallen_log", "twilightforest:canopy_log",
              "twilightforest:hollow_canopy_log_horizontal");
    fallenLog("twilightforest:mangrove_fallen_log", "twilightforest:mangrove_log",
              "twilightforest:hollow_mangrove_log_horizontal");
    fallenLog("twilightforest:spruce_fallen_log", "minecraft:spruce_log",
              "twilightforest:hollow_spruce_log_horizontal");
    fallenLog("twilightforest:tf_oak_fallen_log", "twilightforest:twilight_oak_log",
              "twilightforest:hollow_twilight_oak_log_horizontal");
    fallenLog("twilightforest:oak_fallen_log", "minecraft:oak_log",
              "twilightforest:hollow_oak_log_horizontal");
    fallenLog("twilightforest:birch_fallen_log", "minecraft:birch_log",
              "twilightforest:hollow_birch_log_horizontal");

    // default_fallen_logs.json: random_selector birch 0.1, oak 0.2, canopy 0.4,
    // default tf_oak.
    registerIfPresent("twilightforest:default_fallen_logs",
        randomSelector({{"twilightforest:birch_fallen_log", 0.1f},
                        {"twilightforest:oak_fallen_log", 0.2f},
                        {"twilightforest:canopy_fallen_log", 0.4f}},
                       "twilightforest:tf_oak_fallen_log", "default_fallen_logs"));

    // ----------------------------------------------------------- roots
    // ore/wood_roots_spread.json (twilightforest:wood_roots): root_block
    // weighted root:6 / liveroot_block:1, root_ore liveroot_block.
    {
        BlockState* root = resolveState("twilightforest:root", {}, "ore/wood_roots_spread");
        BlockState* liveroot = resolveState("twilightforest:liveroot_block", {}, "ore/wood_roots_spread");
        if (root && liveroot) {
            RootConfig config;
            config.blockRoot = std::make_shared<WeightedStateProvider>(
                std::vector<WeightedStateEntry>{WeightedStateEntry(root, 6), WeightedStateEntry(liveroot, 1)});
            config.oreRoot = simple(liveroot);
            registerIfPresent("twilightforest:ore/wood_roots_spread",
                std::make_unique<ConfiguredFeatureImpl<RootConfig, WoodRootFeature>>(&s_woodRootFeature, config));
        }
    }

    // plant_roots.json: underground_plants, twilightforest:root_strand.
    if (BlockState* rootStrand = resolveState("twilightforest:root_strand", {}, "plant_roots")) {
        UndergroundPlantConfig config;
        config.state = rootStrand;
        config.survival = UndergroundPlantConfig::Survival::ROOT_STRAND;
        registerIfPresent("twilightforest:plant_roots",
            std::make_unique<ConfiguredFeatureImpl<UndergroundPlantConfig, TwilightUndergroundPlantFeature>>(
                &s_undergroundPlantFeature, config));
    }
    // vanilla_roots.json: underground_plants, minecraft:hanging_roots[waterlogged=false].
    if (BlockState* hangingRoots = resolveState("minecraft:hanging_roots", {{"waterlogged", "false"}}, "vanilla_roots")) {
        UndergroundPlantConfig config;
        config.state = hangingRoots;
        config.survival = UndergroundPlantConfig::Survival::VANILLA;
        registerIfPresent("twilightforest:vanilla_roots",
            std::make_unique<ConfiguredFeatureImpl<UndergroundPlantConfig, TwilightUndergroundPlantFeature>>(
                &s_undergroundPlantFeature, config));
    }
    // troll_roots.json: troll_vines (UndergroundPlantFeature(codec, true)),
    // twilightforest:trollvidr; 1 in 10 becomes an unripe trollber.
    if (BlockState* trollvidr = resolveState("twilightforest:trollvidr", {}, "troll_roots")) {
        UndergroundPlantConfig config;
        config.state = trollvidr;
        config.survival = UndergroundPlantConfig::Survival::TROLL_ROOT;
        config.isTrollvidr = true;
        config.unripeTrollber = resolveState("twilightforest:unripe_trollber", {}, "troll_roots berries");
        config.spawnInStructure = true;
        registerIfPresent("twilightforest:troll_roots",
            std::make_unique<ConfiguredFeatureImpl<UndergroundPlantConfig, TwilightUndergroundPlantFeature>>(
                &s_undergroundPlantFeature, config));
    }

    // ---------------------------------------------------- patches, snow
    // mycelium_blob.json (twilightforest:mycelium_blob = CheckAbovePatchFeature):
    // mycelium[snowy=false] over matching_blocks grass_block, radius 4..6, half height 3.
    if (BlockState* mycelium = resolveState("minecraft:mycelium", {{"snowy", "false"}}, "mycelium_blob")) {
        CheckAbovePatchConfig config;
        config.stateProvider = simple(mycelium);
        config.target = levelgen::blockpredicates::BlockPredicate::matchesBlocks("minecraft:grass_block");
        config.radius = std::make_shared<util::UniformInt>(4, 6);
        config.halfHeight = 3;
        registerIfPresent("twilightforest:mycelium_blob",
            std::make_unique<ConfiguredFeatureImpl<CheckAbovePatchConfig, CheckAbovePatchFeature>>(
                &s_checkAbovePatchFeature, config));
    }
    // snow_under_trees.json (twilightforest:snow_under_trees): Blocks.SNOW.
    if (BlockState* snow = resolveState("minecraft:snow", {}, "snow_under_trees")) {
        SnowUnderTreeConfig config;
        config.snow = snow;
        registerIfPresent("twilightforest:snow_under_trees",
            std::make_unique<ConfiguredFeatureImpl<SnowUnderTreeConfig, SnowUnderTreeFeature>>(
                &s_snowUnderTreeFeature, config));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
