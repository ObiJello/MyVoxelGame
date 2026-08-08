#include "levelgen/feature/TreeFeature.h"
#include "levelgen/feature/rootplacers/RootPlacer.h"
#include "world/level/block/blocks/BushBlock.h"
#include "world/level/block/blocks/VineBlock.h"
#include "world/level/block/blocks/MossyCarpetBlock.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/feature/TreeFeature.java

namespace minecraft {
namespace levelgen {
namespace feature {

namespace {

class FilledBoundsShape {
private:
    int m_xSize;
    int m_ySize;
    int m_zSize;
    std::vector<uint8_t> m_storage;

    size_t index(int x, int y, int z) const {
        return static_cast<size_t>((x * m_ySize + y) * m_zSize + z);
    }

public:
    FilledBoundsShape(int xSize, int ySize, int zSize)
        : m_xSize(xSize)
        , m_ySize(ySize)
        , m_zSize(zSize)
        , m_storage(static_cast<size_t>(xSize * ySize * zSize), 0)
    {}

    void fill(int x, int y, int z) {
        m_storage[index(x, y, z)] = 1;
    }

    bool isFull(int x, int y, int z) const {
        return m_storage[index(x, y, z)] != 0;
    }
};

std::optional<int> getOptionalDistanceAt(BlockState* state) {
    if (!state) {
        return std::nullopt;
    }

    using world::level::block::state::properties::BlockStateProperties;

    if (state->isLog()) {
        return 0;
    }

    if (BlockStateProperties::DISTANCE && state->hasProperty(BlockStateProperties::DISTANCE)) {
        return state->getValue(*BlockStateProperties::DISTANCE);
    }

    return std::nullopt;
}

void updateLeaves(
    WorldGenLevel& level,
    const BoundingBox& bounds,
    const util::JavaHashSet<core::BlockPos>& logs,
    const util::JavaHashSet<core::BlockPos>& decorationSet,
    const util::JavaHashSet<core::BlockPos>& rootPositions
) {
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
    for (const auto& logPos : logs) {
        toCheck[0].add(logPos);
    }

    while (true) {
        while (smallestDistance < 7 && !toCheck[smallestDistance].empty()) {
            // Java: iterator.next() + iterator.remove() - unlink the first
            // element in iteration order without touching the table capacity.
            const core::BlockPos pos = *toCheck[smallestDistance].begin();
            toCheck[smallestDistance].remove(pos);

            if (!bounds.isInside(pos)) {
                continue;
            }

            if (smallestDistance != 0) {
                using world::level::block::state::properties::BlockStateProperties;
                BlockState* state = level.getBlockState(pos);
                if (state && BlockStateProperties::DISTANCE && state->hasProperty(BlockStateProperties::DISTANCE)) {
                    level.setBlock(
                        pos,
                        state->setValue(*BlockStateProperties::DISTANCE, smallestDistance),
                        TreeFeature::BLOCK_UPDATE_FLAGS
                    );
                }
            }

            shape.fill(pos.getX() - bounds.minX(), pos.getY() - bounds.minY(), pos.getZ() - bounds.minZ());

            for (int directionIndex = 0; directionIndex < 6; ++directionIndex) {
                core::Direction direction = core::fromIndex(directionIndex);
                neighborPos.setWithOffset(pos, core::getStepX(direction), core::getStepY(direction), core::getStepZ(direction));
                if (!bounds.isInside(neighborPos)) {
                    continue;
                }

                int xInShape = neighborPos.getX() - bounds.minX();
                int yInShape = neighborPos.getY() - bounds.minY();
                int zInShape = neighborPos.getZ() - bounds.minZ();
                if (shape.isFull(xInShape, yInShape, zInShape)) {
                    continue;
                }

                BlockState* currentState = level.getBlockState(neighborPos);
                std::optional<int> distance = getOptionalDistanceAt(currentState);
                if (!distance.has_value()) {
                    continue;
                }

                int newDistance = std::min(distance.value(), smallestDistance + 1);
                if (newDistance < 7) {
                    toCheck[newDistance].add(neighborPos.immutable());
                    smallestDistance = std::min(smallestDistance, newDistance);
                }
            }
        }

        if (smallestDistance >= 7) {
            break;
        }

        ++smallestDistance;
    }

    // StructureTemplate.updateShapeAtEdge equivalent: every exposed face of
    // the placed-tree voxel shape triggers updateShape on the block and its
    // neighbor. During worldgen the observable consequence is DoublePlantBlock
    // halves losing their counterpart (e.g. a leaf overwrote a lilac lower -
    // its upper must become air) and lower halves losing their ground.
    // No cascade: Java's pass writes with neighbor updates suppressed.
    using world::level::block::state::properties::BlockStateProperties;
    using world::level::block::state::properties::DoubleBlockHalf;
    auto isDoublePlant = [](BlockState* s) {
        if (!s || !BlockStateProperties::DOUBLE_BLOCK_HALF ||
            !s->hasProperty(BlockStateProperties::DOUBLE_BLOCK_HALF)) {
            return false;
        }
        const std::string& id = s->getIdentifier();
        return id == "minecraft:sunflower" || id == "minecraft:lilac" ||
               id == "minecraft:rose_bush" || id == "minecraft:peony" ||
               id == "minecraft:tall_grass" || id == "minecraft:large_fern" ||
               id == "minecraft:pitcher_plant" || id == "minecraft:small_dripleaf";
    };
    // DoublePlantBlock.updateShape for the block at `pos` reacting to the
    // neighbor at `pos + direction`; plain VegetationBlock (bushes, mushrooms,
    // flowers) reacts to DOWN with a canSurvive check.
    auto shapeUpdateAt = [&](const core::BlockPos& pos, core::Direction direction) {
        BlockState* state = level.getBlockState(pos);
        if (!isDoublePlant(state)) {
            // MossyCarpetBlock.updateShape recalculates wall sides from the
            // (possibly changed) neighbours.
            if (state && !state->isAir()) {
                if (auto* carpet = dynamic_cast<world::level::block::MossyCarpetBlock*>(state->getBlock())) {
                    // MossyCarpetBlock.updateShape: !canSurvive -> air, then
                    // side recalc, then !hasFaces -> air.
                    if (!carpet->canSurvive(state, level, pos)) {
                        level.setBlock(pos,
                            static_cast<BlockState*>(world::level::block::Blocks::AIR->defaultBlockState()),
                            2);
                        return;
                    }
                    BlockState* updated = carpet->getUpdatedStateForShapeUpdate(state, level, pos);
                    if (updated && !world::level::block::MossyCarpetBlock::hasFaces(updated)) {
                        updated = static_cast<BlockState*>(world::level::block::Blocks::AIR->defaultBlockState());
                    }
                    if (updated && updated != state) {
                        level.setBlock(pos, updated, 2);
                    }
                    return;
                }
            }
            // VineBlock.updateShape (non-DOWN): prune unsupported faces, air
            // when none remain (removes vines orphaned by leaf replacement).
            // DOWN neighbour updates are delegated to super (no-op) in Java.
            if (state && !state->isAir() && direction != core::Direction::DOWN) {
                if (auto* vine = dynamic_cast<world::level::block::VineBlock*>(state->getBlock())) {
                    BlockState* updated = vine->getUpdatedStateForShapeUpdate(state, level, pos);
                    if (!updated) {
                        updated = static_cast<BlockState*>(world::level::block::Blocks::AIR->defaultBlockState());
                    }
                    if (updated != state) {
                        level.setBlock(pos, updated, 2);
                    }
                    return;
                }
            }
            // VegetationBlock.updateShape re-checks canSurvive on ANY
            // neighbour-face update (no direction restriction in 26.1).
            // MangrovePropaguleBlock is a VegetationBlock too (its updateShape
            // is UP&&!canSurvive→air, else super's any-direction check — net
            // effect identical); its C++ Impl derives from plain Block, so
            // gate by name and use the virtual canSurvive (hanging → above
            // must be mangrove_leaves; a later trunk replacing that leaf with
            // a log must orphan the propagule).
            if (state && !state->isAir()) {
                bool isVegetation =
                    dynamic_cast<world::level::block::BushBlock*>(state->getBlock()) != nullptr ||
                    state->getBlockName() == "minecraft:mangrove_propagule";
                if (isVegetation && !state->getBlock()->canSurvive(state, level, pos)) {
                    level.setBlock(pos,
                        static_cast<BlockState*>(world::level::block::Blocks::AIR->defaultBlockState()),
                        2);
                }
            }
            return;
        }
        DoubleBlockHalf half = state->getValue(*BlockStateProperties::DOUBLE_BLOCK_HALF);
        bool lower = half.getValue() == DoubleBlockHalf::LOWER;
        core::BlockPos neighborPos2 = pos.relative(direction, 1);
        BlockState* neighborState = level.getBlockState(neighborPos2);
        core::Direction otherHalfDir = lower ? core::Direction::UP : core::Direction::DOWN;
        if (direction == otherHalfDir) {
            bool matching = neighborState && !neighborState->isAir() &&
                neighborState->getIdentifier() == state->getIdentifier() &&
                neighborState->hasProperty(BlockStateProperties::DOUBLE_BLOCK_HALF) &&
                neighborState->getValue(*BlockStateProperties::DOUBLE_BLOCK_HALF).getValue() != half.getValue();
            if (!matching) {
                level.setBlock(pos,
                    static_cast<BlockState*>(world::level::block::Blocks::AIR->defaultBlockState()),
                    2);
            }
        } else if (lower && direction == core::Direction::DOWN) {
            // VegetationBlock.canSurvive for the lower half: dirt-tag or farmland.
            BlockState* below = level.getBlockState(pos.below());
            bool survives = below && !below->isAir() &&
                (::minecraft::levelgen::blockpredicates::matchesBlockTagName(below, "minecraft:dirt") ||
                 below->getIdentifier() == "minecraft:farmland");
            if (!survives) {
                level.setBlock(pos,
                    static_cast<BlockState*>(world::level::block::Blocks::AIR->defaultBlockState()),
                    2);
            }
        }
    };

    // Reference: DiscreteVoxelShape.forAllFaces - three axis passes
    // (AxisCycle.NONE -> Z faces, FORWARD -> Y faces, BACKWARD -> X faces),
    // each scanning fill-boundary transitions along its axis. The ORDER
    // matters: e.g. a vine pruned in an earlier face-update must already be
    // gone when a lower vine's carried face is re-evaluated.
    // Parity-debug: MC_SHAPE_TRACE=<file> logs every emitted face of every
    // tree shape pass (world pos + direction), to compare which faces touch
    // a given block and in what order.
    static std::FILE* shapeTraceFile = [] {
        const char* p = std::getenv("MC_SHAPE_TRACE");
        return (p && *p) ? std::fopen(p, "w") : nullptr;
    }();

    auto emitFace = [&](core::Direction direction, int sx, int sy, int sz) {
        core::BlockPos worldPos(bounds.minX() + sx, bounds.minY() + sy, bounds.minZ() + sz);
        core::BlockPos facePos = worldPos.relative(direction, 1);
        if (shapeTraceFile) {
            std::fprintf(shapeTraceFile, "FACE %d,%d,%d dir=%d\n",
                         worldPos.getX(), worldPos.getY(), worldPos.getZ(),
                         static_cast<int>(direction));
        }
        shapeUpdateAt(worldPos, direction);
        shapeUpdateAt(facePos, core::getOpposite(direction));
    };

    const int sizes[3] = {bounds.getXSpan(), bounds.getYSpan(), bounds.getZSpan()};
    // Each pass: (aAxis, bAxis, cAxis) world-axis indices (0=x,1=y,2=z) with
    // the scan along cAxis; negative/positive directions along cAxis.
    struct AxisPass {
        int aAxis, bAxis, cAxis;
        core::Direction negative, positive;
    };
    const AxisPass passes[3] = {
        // AxisCycle.NONE: a=x, b=y, scan z -> NORTH/SOUTH
        {0, 1, 2, core::Direction::NORTH, core::Direction::SOUTH},
        // AxisCycle.FORWARD (inverse BACKWARD): a=z, b=x, scan y -> DOWN/UP
        {2, 0, 1, core::Direction::DOWN, core::Direction::UP},
        // AxisCycle.BACKWARD (inverse FORWARD): a=y, b=z, scan x -> WEST/EAST
        {1, 2, 0, core::Direction::WEST, core::Direction::EAST},
    };

    for (const AxisPass& pass : passes) {
        int aSize = sizes[pass.aAxis];
        int bSize = sizes[pass.bAxis];
        int cSize = sizes[pass.cAxis];
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

} // namespace

/**
 * LevelReaderAdapter - Adapts WorldGenLevel to trunkplacers::LevelReader
 */
class LevelReaderAdapter : public trunkplacers::LevelReader {
private:
    WorldGenLevel& m_level;

public:
    explicit LevelReaderAdapter(WorldGenLevel& level) : m_level(level) {}

    bool isStateAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const override {
        return m_level.isStateAtPosition(pos, predicate);
    }

    BlockState* getBlockState(const core::BlockPos& pos) const override {
        return m_level.getBlockState(pos);
    }
};

/**
 * RootLevelReaderAdapter - Adapts WorldGenLevel to rootplacers::LevelReader
 */
class RootLevelReaderAdapter : public rootplacers::LevelReader {
private:
    WorldGenLevel& m_level;

public:
    explicit RootLevelReaderAdapter(WorldGenLevel& level) : m_level(level) {}

    bool isStateAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const override {
        return m_level.isStateAtPosition(pos, predicate);
    }

    BlockState* getBlockState(const core::BlockPos& pos) const override {
        return m_level.getBlockState(pos);
    }
};

bool TreeFeature::place(
    WorldGenLevel& level,
    ChunkGenerator* generator,
    WorldgenRandom& random,
    const core::BlockPos& origin,
    const configurations::TreeConfiguration& config
) {
    if (!config.trunkPlacer || !config.foliagePlacer || !config.minimumSize) {
        return false;
    }

    // Reference: TreeFeature.java place() lines 108-158

    util::JavaHashSet<core::BlockPos> rootPositions;
    util::JavaHashSet<core::BlockPos> trunks;
    util::JavaHashSet<core::BlockPos> foliage;
    util::JavaHashSet<core::BlockPos> decorations;

    auto rootSetter = [&rootPositions, &level](const core::BlockPos& pos, BlockState* state) {
        rootPositions.add(pos);
        level.setBlock(pos, state, BLOCK_UPDATE_FLAGS);
    };

    auto trunkSetter = [&trunks, &level](const core::BlockPos& pos, BlockState* state) {
        trunks.add(pos);
        level.setBlock(pos, state, BLOCK_UPDATE_FLAGS);
    };

    SimpleFoliageSetter foliageSetter(level, foliage);

    bool result = doPlace(level, random, origin, rootSetter, trunkSetter, foliageSetter, config);

    if (result && (!trunks.empty() || !foliage.empty())) {
        // Apply decorators
        if (!config.decorators.empty()) {
            // Convert sets to vectors for decorator context
            // Reference: TreeDecorator.Context constructor sorts by Y
            // Java: this.logs.sort(Comparator.comparingInt(Vec3i::getY))
            std::vector<core::BlockPos> logsList(trunks.begin(), trunks.end());
            std::vector<core::BlockPos> leavesList(foliage.begin(), foliage.end());
            std::vector<core::BlockPos> rootsList(rootPositions.begin(), rootPositions.end());
            auto byY = [](const core::BlockPos& a, const core::BlockPos& b) { return a.getY() < b.getY(); };
            std::stable_sort(logsList.begin(), logsList.end(), byY);
            std::stable_sort(leavesList.begin(), leavesList.end(), byY);
            std::stable_sort(rootsList.begin(), rootsList.end(), byY);

            auto decorationSetter = [&decorations, &level](const core::BlockPos& pos, BlockState* state) {
                decorations.add(pos);
                level.setBlock(pos, state, BLOCK_UPDATE_FLAGS);
            };

            auto blockGetter = [&level](const core::BlockPos& pos) -> BlockState* {
                return level.getBlockState(pos);
            };

            // Heightmap getter for MOTION_BLOCKING_NO_LEAVES
            // Reference: PlaceOnGroundDecorator.java line 68
            auto heightGetter = [&level](int x, int z) -> int {
                return level.getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z);
            };

            treedecorators::DecoratorContext context(
                logsList, leavesList, rootsList,
                decorationSetter, blockGetter, heightGetter, &random, &level, generator
            );

            for (const auto& decorator : config.decorators) {
                decorator->place(context);
            }
        }

        // Combine all positions for bounding box
        std::vector<core::BlockPos> allPositions;
        allPositions.reserve(rootPositions.size() + trunks.size() + foliage.size() + decorations.size());
        allPositions.insert(allPositions.end(), rootPositions.begin(), rootPositions.end());
        allPositions.insert(allPositions.end(), trunks.begin(), trunks.end());
        allPositions.insert(allPositions.end(), foliage.begin(), foliage.end());
        allPositions.insert(allPositions.end(), decorations.begin(), decorations.end());

        auto bounds = BoundingBox::encapsulatingPositions(allPositions);
        if (bounds.has_value()) {
            updateLeaves(level, bounds.value(), trunks, decorations, rootPositions);
            return true;
        }
        return false;
    }

    return false;
}

bool TreeFeature::doPlace(
    WorldGenLevel& level,
    WorldgenRandom& random,
    const core::BlockPos& origin,
    std::function<void(const core::BlockPos&, BlockState*)> rootSetter,
    std::function<void(const core::BlockPos&, BlockState*)> trunkSetter,
    foliageplacers::FoliageSetter& foliageSetter,
    const configurations::TreeConfiguration& config
) {
    // Reference: TreeFeature.java doPlace() lines 58-83
    int treeHeight = config.trunkPlacer->getTreeHeight(random);
    int foliageHeight = config.foliagePlacer->foliageHeight(random, treeHeight, config);
    int trunkHeight = treeHeight - foliageHeight;
    int leafRadius = config.foliagePlacer->foliageRadius(random, trunkHeight);

    // Determine trunk origin (may be offset by root placer)
    core::BlockPos trunkOrigin = origin;
    if (config.rootPlacer.has_value()) {
        trunkOrigin = config.rootPlacer.value()->getTrunkOrigin(origin, random);
    }

    int minY = std::min(origin.getY(), trunkOrigin.getY());
    int maxY = std::max(origin.getY(), trunkOrigin.getY()) + treeHeight + 1;

    // Check world bounds
    if (minY < level.getMinY() + 1 || maxY > level.getMaxY() + 1) {
        return false;
    }

    // Check for obstructions
    std::optional<int> minClippedHeight = config.minimumSize->minClippedHeight();
    int clippedTreeHeight = getMaxFreeTreeHeight(level, treeHeight, trunkOrigin, config);

    if (clippedTreeHeight < treeHeight) {
        bool shouldFail = minClippedHeight.has_value() ? (clippedTreeHeight < minClippedHeight.value()) : true;
        if (shouldFail) {
            return false;
        }
    }

    // Place roots if configured
    if (config.rootPlacer.has_value()) {
        RootLevelReaderAdapter rootLevelAdapter(level);
        std::vector<core::BlockPos> rootPositionsList;
        if (!config.rootPlacer.value()->placeRoots(
            rootLevelAdapter,
            rootSetter,
            random,
            origin,
            trunkOrigin,
            rootPositionsList
        )) {
            return false;
        }
    }

    // Place trunk
    LevelReaderAdapter levelAdapter(level);
    std::vector<foliageplacers::FoliageAttachment> foliageAttachments =
        config.trunkPlacer->placeTrunk(
            levelAdapter,
            trunkSetter,
            random,
            clippedTreeHeight,
            trunkOrigin,
            config.trunkProvider,
            config.dirtProvider,
            config.forceDirt
        );

    // Place foliage at each attachment point
    for (const auto& attachment : foliageAttachments) {
        config.foliagePlacer->createFoliage(
            foliageSetter,
            random,
            config.foliageProvider,
            clippedTreeHeight,
            attachment,
            foliageHeight,
            leafRadius
        );
    }

    return true;
}

int TreeFeature::getMaxFreeTreeHeight(
    WorldGenLevel& level,
    int maxTreeHeight,
    const core::BlockPos& treePos,
    const configurations::TreeConfiguration& config
) {
    // Reference: TreeFeature.java getMaxFreeTreeHeight() lines 85-102
    LevelReaderAdapter levelAdapter(level);

    for (int y = 0; y <= maxTreeHeight + 1; ++y) {
        int r = config.minimumSize->getSizeAtHeight(maxTreeHeight, y);

        for (int x = -r; x <= r; ++x) {
            for (int z = -r; z <= r; ++z) {
                core::BlockPos blockPos = treePos.offset(x, y, z);
                if (!config.trunkPlacer->isFree(levelAdapter, blockPos) ||
                    (!config.ignoreVines && isVine(level, blockPos))) {
                    return y - 2;
                }
            }
        }
    }

    return maxTreeHeight;
}

} // namespace feature
} // namespace levelgen
} // namespace minecraft
