#include "levelgen/feature/rootplacers/RootPlacer.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "world/MinecraftBlockType.h"
#include "core/Direction.h"
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
    XoroshiroRandomSource& random,
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
    // Reference: RootPlacer.java canPlaceRoot() lines 41-43
    return level.isStateAtPosition(pos, [](BlockState* state) {
        // TODO: Check REPLACEABLE_BY_TREES tag when tags are implemented
        return state && state->isAir();
    });
}

BlockState* RootPlacer::getPotentiallyWaterloggedState(
    LevelReader& level,
    const core::BlockPos& pos,
    BlockState* state
) const {
    // Reference: RootPlacer.java getPotentiallyWaterloggedState() lines 59-66
    // TODO: Implement waterlogged property when property system is complete
    // For now, return the state as-is
    return state;
}

// ============================================================================
// MangroveRootPlacer
// Reference: MangroveRootPlacer.java
// ============================================================================

bool MangroveRootPlacer::placeRoots(
    LevelReader& level,
    RootSetter rootSetter,
    XoroshiroRandomSource& random,
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

    // Simulate roots in each horizontal direction
    for (int dirIdx = 0; dirIdx < 4; ++dirIdx) {
        core::Direction dir = core::fromHorizontalIndex(dirIdx);
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
    XoroshiroRandomSource& random,
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
    XoroshiroRandomSource& random,
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

    // Then check if can grow through (like mud)
    return level.isStateAtPosition(pos, [](BlockState* state) {
        if (!state) return false;
        std::string name = state->getIdentifier();
        return name == "minecraft:mud" || name == "minecraft:muddy_mangrove_roots";
    });
}

void MangroveRootPlacer::placeRootAtPos(
    LevelReader& level,
    RootSetter rootSetter,
    XoroshiroRandomSource& random,
    const core::BlockPos& pos,
    std::vector<core::BlockPos>& rootPositions
) {
    // Reference: MangroveRootPlacer.java placeRoot() lines 100-108
    bool isMuddy = level.isStateAtPosition(pos, [](BlockState* state) {
        return state && state->getIdentifier() == "minecraft:mud";
    });

    if (isMuddy) {
        // Place muddy mangrove roots
        BlockState* muddyRoots = static_cast<BlockState*>(
            ::world::MinecraftBlocks::get("minecraft:muddy_mangrove_roots"));
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
    XoroshiroRandomSource& random,
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
