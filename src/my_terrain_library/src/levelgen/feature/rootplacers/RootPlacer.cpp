#include "levelgen/feature/rootplacers/RootPlacer.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "world/level/block/Blocks.h"
#include "core/Direction.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/feature/rootplacers/*.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace rootplacers {

// ============================================================================
// RootPlacer base class
// Reference: RootPlacer.java
// ============================================================================

void RootPlacer::placeRoot(
    LevelReader& level,
    RootSetter rootSetter,
    WorldgenRandom& random,
    const core::BlockPos& pos,
    std::vector<core::BlockPos>& rootPositions
) {
    // Reference: RootPlacer.java placeRoot() lines 45-56
    if (canPlaceRoot(level, pos)) {
        BlockState* rootState = m_rootProvider->getState(random, pos);
        rootState = getPotentiallyWaterloggedState(level, pos, rootState);
        rootSetter(pos, rootState);
        rootPositions.push_back(pos);

        // Place above root if configured
        if (m_aboveRootPlacement.has_value()) {
            const AboveRootPlacement& abovePlacement = m_aboveRootPlacement.value();
            core::BlockPos above = pos.above();

            if (random.nextFloat() < abovePlacement.aboveRootPlacementChance) {
                bool isAir = level.isStateAtPosition(above, [](BlockState* state) {
                    return state && state->isAir();
                });

                if (isAir) {
                    BlockState* aboveState = abovePlacement.aboveRootProvider->getState(random, above);
                    aboveState = getPotentiallyWaterloggedState(level, above, aboveState);
                    rootSetter(above, aboveState);
                }
            }
        }
    }
}

bool RootPlacer::canPlaceRoot(LevelReader& level, const core::BlockPos& pos) const {
    // Reference: RootPlacer.java canPlaceRoot() lines 41-43 =
    // TreeFeature.validTreePos: air OR #replaceable_by_trees.
    return level.isStateAtPosition(pos, [](BlockState* state) {
        return state && (state->isAir() ||
            minecraft::levelgen::blockpredicates::matchesBlockTagName(
                state, "minecraft:replaceable_by_trees"));
    });
}

BlockState* RootPlacer::getPotentiallyWaterloggedState(
    LevelReader& level,
    const core::BlockPos& pos,
    BlockState* state
) const {
    // Reference: RootPlacer.java getPotentiallyWaterloggedState() lines 59-66:
    // if the state has WATERLOGGED, set it from whether water fluid is at pos.
    using minecraft::world::level::block::state::properties::BlockStateProperties;
    if (state && BlockStateProperties::WATERLOGGED &&
        state->hasProperty(BlockStateProperties::WATERLOGGED)) {
        bool waterlogged = level.isStateAtPosition(pos, [](BlockState* s) {
            return s && s->hasWaterFluid();
        });
        return state->setValue(*BlockStateProperties::WATERLOGGED, waterlogged);
    }
    return state;
}

// ============================================================================
// MangroveRootPlacer
// Reference: MangroveRootPlacer.java
// ============================================================================

bool MangroveRootPlacer::placeRoots(
    LevelReader& level,
    RootSetter rootSetter,
    WorldgenRandom& random,
    const core::BlockPos& origin,
    const core::BlockPos& trunkOrigin,
    std::vector<core::BlockPos>& rootPositions
) {
    // Reference: MangroveRootPlacer.java placeRoots() lines 29-59
    core::BlockPos columnPos = origin;

    // Check column from origin to trunk
    while (columnPos.getY() < trunkOrigin.getY()) {
        if (!canPlaceRoot(level, columnPos)) {
            return false;
        }
        columnPos = columnPos.above();
    }

    std::vector<core::BlockPos> allRootPositions;
    allRootPositions.push_back(trunkOrigin.below());

    // Reference: Direction.Plane.HORIZONTAL order = N,E,S,W; simulateRoots
    // draws RNG per direction so the order is stream-visible.
    static constexpr core::Direction kHorizontal[4] = {
        core::Direction::NORTH, core::Direction::EAST,
        core::Direction::SOUTH, core::Direction::WEST};
    for (int dirIdx = 0; dirIdx < 4; ++dirIdx) {
        core::Direction dir = kHorizontal[dirIdx];
        core::BlockPos pos = trunkOrigin.relative(dir);
        std::vector<core::BlockPos> positionsInDirection;

        if (!simulateRoots(level, random, pos, dir, trunkOrigin, positionsInDirection, 0)) {
            return false;
        }

        allRootPositions.insert(allRootPositions.end(),
                               positionsInDirection.begin(),
                               positionsInDirection.end());
        allRootPositions.push_back(trunkOrigin.relative(dir));
    }

    // Place all roots
    for (const auto& rootPos : allRootPositions) {
        placeRootAtPos(level, rootSetter, random, rootPos, rootPositions);
    }

    return true;
}

bool MangroveRootPlacer::simulateRoots(
    LevelReader& level,
    WorldgenRandom& random,
    const core::BlockPos& rootPos,
    core::Direction dir,
    const core::BlockPos& rootOrigin,
    std::vector<core::BlockPos>& positions,
    int layer
) {
    // Reference: MangroveRootPlacer.java simulateRoots() lines 61-77
    if (layer == m_maxRootLength || positions.size() > static_cast<size_t>(m_maxRootLength)) {
        return false;
    }

    std::vector<core::BlockPos> potentialPositions = potentialRootPositions(rootPos, dir, random, rootOrigin);

    for (const auto& pos : potentialPositions) {
        if (canPlaceRoot(level, pos)) {
            positions.push_back(pos);
            if (!simulateRoots(level, random, pos, dir, rootOrigin, positions, layer + 1)) {
                return false;
            }
        }
    }

    return true;
}

std::vector<core::BlockPos> MangroveRootPlacer::potentialRootPositions(
    const core::BlockPos& pos,
    core::Direction prevDir,
    WorldgenRandom& random,
    const core::BlockPos& rootOrigin
) const {
    // Reference: MangroveRootPlacer.java potentialRootPositions() lines 79-94
    core::BlockPos below = pos.below();
    core::BlockPos nextTo = pos.relative(prevDir);

    int width = pos.distManhattan(rootOrigin);

    if (width > m_maxRootWidth - 3 && width <= m_maxRootWidth) {
        if (random.nextFloat() < m_randomSkewChance) {
            return {below, nextTo.below()};
        }
        return {below};
    } else if (width > m_maxRootWidth) {
        return {below};
    } else if (random.nextFloat() < m_randomSkewChance) {
        return {below};
    } else {
        if (random.nextBoolean()) {
            return {nextTo};
        }
        return {below};
    }
}

bool MangroveRootPlacer::canPlaceRoot(LevelReader& level, const core::BlockPos& pos) const {
    // Reference: MangroveRootPlacer.java canPlaceRoot() lines 96-98
    // First check base implementation
    if (RootPlacer::canPlaceRoot(level, pos)) {
        return true;
    }

    // Then the config's canGrowThrough tag (#mangrove_roots_can_grow_through:
    // mud, muddy_mangrove_roots, mangrove_roots, moss_carpet, vine,
    // mangrove_propagule, snow).
    return level.isStateAtPosition(pos, [](BlockState* state) {
        return state && minecraft::levelgen::blockpredicates::matchesBlockTagName(
            state, "minecraft:mangrove_roots_can_grow_through");
    });
}

void MangroveRootPlacer::placeRootAtPos(
    LevelReader& level,
    RootSetter rootSetter,
    WorldgenRandom& random,
    const core::BlockPos& pos,
    std::vector<core::BlockPos>& rootPositions
) {
    // Reference: MangroveRootPlacer.java placeRoot() lines 100-108.
    // muddyRootsIn = {mud, muddy_mangrove_roots}: an existing muddy root from
    // an earlier tree stays muddy, it must NOT downgrade to plain roots.
    bool isMuddy = level.isStateAtPosition(pos, [](BlockState* state) {
        if (!state) return false;
        const std::string& id = state->getIdentifier();
        return id == "minecraft:mud" || id == "minecraft:muddy_mangrove_roots";
    });

    if (isMuddy) {
        // Place muddy mangrove roots
        BlockState* muddyRoots = static_cast<BlockState*>(
            minecraft::world::level::block::Blocks::getDefaultState("minecraft:muddy_mangrove_roots"));
        muddyRoots = getPotentiallyWaterloggedState(level, pos, muddyRoots);
        rootSetter(pos, muddyRoots);
        rootPositions.push_back(pos);
    } else {
        // Use base implementation
        placeRoot(level, rootSetter, random, pos, rootPositions);
    }
}

void MangroveRootPlacer::placeRootColumn(
    LevelReader& level,
    RootSetter rootSetter,
    WorldgenRandom& random,
    const core::BlockPos& columnStart,
    const core::BlockPos& trunkOrigin,
    std::vector<core::BlockPos>& rootPositions
) {
    // Place roots from column start down to ground
    core::BlockPos pos = columnStart;
    int maxDepth = 10;

    while (maxDepth > 0 && pos.getY() >= trunkOrigin.getY() - 10) {
        if (!canPlaceRoot(level, pos)) {
            break;
        }

        placeRootAtPos(level, rootSetter, random, pos, rootPositions);
        pos = pos.below();
        --maxDepth;
    }
}

} // namespace rootplacers
} // namespace feature
} // namespace levelgen
} // namespace minecraft
