#include "levelgen/feature/TreeFeature.h"
#include "levelgen/feature/rootplacers/RootPlacer.h"
#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/feature/TreeFeature.java

namespace minecraft {
namespace levelgen {
namespace feature {

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
    XoroshiroRandomSource& random,
    const core::BlockPos& origin,
    const configurations::TreeConfiguration& config
) {
    // Reference: TreeFeature.java place() lines 108-158
    std::set<core::BlockPos> rootPositions;
    std::set<core::BlockPos> trunks;
    std::set<core::BlockPos> foliage;
    std::set<core::BlockPos> decorations;

    auto rootSetter = [&rootPositions, &level](const core::BlockPos& pos, BlockState* state) {
        rootPositions.insert(pos);
        level.setBlock(pos, state, BLOCK_UPDATE_FLAGS);
    };

    auto trunkSetter = [&trunks, &level](const core::BlockPos& pos, BlockState* state) {
        trunks.insert(pos);
        level.setBlock(pos, state, BLOCK_UPDATE_FLAGS);
    };

    SimpleFoliageSetter foliageSetter(level, foliage);

    bool result = doPlace(level, random, origin, rootSetter, trunkSetter, foliageSetter, config);

    if (result && (!trunks.empty() || !foliage.empty())) {
        // Apply decorators
        if (!config.decorators.empty()) {
            // Convert sets to vectors for decorator context
            std::vector<core::BlockPos> logsList(trunks.begin(), trunks.end());
            std::vector<core::BlockPos> leavesList(foliage.begin(), foliage.end());
            std::vector<core::BlockPos> rootsList(rootPositions.begin(), rootPositions.end());

            auto decorationSetter = [&decorations, &level](const core::BlockPos& pos, BlockState* state) {
                decorations.insert(pos);
                level.setBlock(pos, state, BLOCK_UPDATE_FLAGS);
            };

            auto blockGetter = [&level](const core::BlockPos& pos) {
                return level.getBlockState(pos);
            };

            treedecorators::DecoratorContext context(
                logsList, leavesList, rootsList,
                decorationSetter, blockGetter, &random
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
            // Update leaf distances (simplified - full version would propagate distances)
            return true;
        }
        return false;
    }

    return false;
}

bool TreeFeature::doPlace(
    WorldGenLevel& level,
    XoroshiroRandomSource& random,
    const core::BlockPos& origin,
    std::function<void(const core::BlockPos&, BlockState*)> rootSetter,
    std::function<void(const core::BlockPos&, BlockState*)> trunkSetter,
    foliageplacers::FoliageSetter& foliageSetter,
    const configurations::TreeConfiguration& config
) {
    // Reference: TreeFeature.java doPlace() lines 58-83
    int treeHeight = config.trunkPlacer->getTreeHeight(random);
    int foliageHeight = config.foliagePlacer->foliageHeight(random, treeHeight);
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

    if (clippedTreeHeight < treeHeight &&
        (minClippedHeight.has_value() ? clippedTreeHeight < minClippedHeight.value() : true)) {
        return false;
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

                bool isFree = levelAdapter.isStateAtPosition(blockPos, [](BlockState* state) {
                    if (!state) return false;
                    // TODO: Check REPLACEABLE_BY_TREES tag and log types when tags are implemented
                    return state->isAir() || state->isLeaves();
                });

                if (!isFree || (!config.ignoreVines && isVine(level, blockPos))) {
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
