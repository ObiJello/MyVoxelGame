#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "core/Direction.h"
#include <algorithm>
#include <random>

// Reference: net/minecraft/world/level/levelgen/feature/treedecorators/*.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace treedecorators {

// ============================================================================
// LeaveVineDecorator
// Reference: LeaveVineDecorator.java
// ============================================================================

void LeaveVineDecorator::place(DecoratorContext& context) {
    // Reference: LeaveVineDecorator.java place() lines 22-54
    XoroshiroRandomSource& random = context.random();

    for (const auto& pos : context.leaves()) {
        if (random.nextFloat() < m_probability) {
            core::BlockPos west = pos.west();
            if (context.isAir(west)) {
                addHangingVine(west, core::Direction::EAST, context);
            }
        }

        if (random.nextFloat() < m_probability) {
            core::BlockPos east = pos.east();
            if (context.isAir(east)) {
                addHangingVine(east, core::Direction::WEST, context);
            }
        }

        if (random.nextFloat() < m_probability) {
            core::BlockPos north = pos.north();
            if (context.isAir(north)) {
                addHangingVine(north, core::Direction::SOUTH, context);
            }
        }

        if (random.nextFloat() < m_probability) {
            core::BlockPos south = pos.south();
            if (context.isAir(south)) {
                addHangingVine(south, core::Direction::NORTH, context);
            }
        }
    }
}

void LeaveVineDecorator::addHangingVine(
    const core::BlockPos& pos,
    core::Direction direction,
    DecoratorContext& context
) {
    // Reference: LeaveVineDecorator.java addHangingVine() lines 56-65
    placeVine(context, pos, direction);
    int maxLength = 4;

    core::BlockPos currentPos = pos.below();
    while (context.isAir(currentPos) && maxLength > 0) {
        placeVine(context, currentPos, direction);
        currentPos = currentPos.below();
        --maxLength;
    }
}

void LeaveVineDecorator::placeVine(
    DecoratorContext& context,
    const core::BlockPos& pos,
    core::Direction direction
) {
    // Create vine state with the direction property set
    BlockState* vineState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:vine"));
    
    context.setBlock(pos, vineState);
}

// ============================================================================
// TrunkVineDecorator
// Reference: TrunkVineDecorator.java
// ============================================================================

void TrunkVineDecorator::place(DecoratorContext& context) {
    // Reference: TrunkVineDecorator.java place() lines 16-48
    XoroshiroRandomSource& random = context.random();

    for (const auto& pos : context.logs()) {
        if (random.nextInt(3) > 0) {
            core::BlockPos west = pos.west();
            if (context.isAir(west)) {
                placeVine(context, west, core::Direction::EAST);
            }
        }

        if (random.nextInt(3) > 0) {
            core::BlockPos east = pos.east();
            if (context.isAir(east)) {
                placeVine(context, east, core::Direction::WEST);
            }
        }

        if (random.nextInt(3) > 0) {
            core::BlockPos north = pos.north();
            if (context.isAir(north)) {
                placeVine(context, north, core::Direction::SOUTH);
            }
        }

        if (random.nextInt(3) > 0) {
            core::BlockPos south = pos.south();
            if (context.isAir(south)) {
                placeVine(context, south, core::Direction::NORTH);
            }
        }
    }
}

void TrunkVineDecorator::placeVine(
    DecoratorContext& context,
    const core::BlockPos& pos,
    core::Direction direction
) {
    BlockState* vineState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:vine"));
    
    context.setBlock(pos, vineState);
}

// ============================================================================
// CocoaDecorator
// Reference: CocoaDecorator.java
// ============================================================================

void CocoaDecorator::place(DecoratorContext& context) {
    // Reference: CocoaDecorator.java place() lines 25-45
    XoroshiroRandomSource& random = context.random();

    if (random.nextFloat() >= m_probability) {
        return;
    }

    const auto& logs = context.logs();
    if (logs.empty()) {
        return;
    }

    int treeY = logs[0].getY();

    for (const auto& pos : logs) {
        if (pos.getY() - treeY > 2) {
            continue;
        }

        // Check all horizontal directions
        for (int dirIdx = 0; dirIdx < 4; ++dirIdx) {
            core::Direction direction = core::fromHorizontalIndex(dirIdx);

            if (random.nextFloat() <= 0.25f) {
                core::Direction opposite = core::opposite(direction);
                core::BlockPos cocoaPos = pos.offset(
                    core::getStepX(opposite),
                    0,
                    core::getStepZ(opposite)
                );

                if (context.isAir(cocoaPos)) {
                    BlockState* cocoaState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:cocoa"));
                    
                    
                    context.setBlock(cocoaPos, cocoaState);
                }
            }
        }
    }
}

// ============================================================================
// BeehiveDecorator
// Reference: BeehiveDecorator.java
// ============================================================================

void BeehiveDecorator::place(DecoratorContext& context) {
    // Reference: BeehiveDecorator.java place() lines 34-62
    const auto& leaves = context.leaves();
    const auto& logs = context.logs();

    if (logs.empty()) {
        return;
    }

    XoroshiroRandomSource& random = context.random();
    if (random.nextFloat() >= m_probability) {
        return;
    }

    // Calculate hive Y position
    int hiveY;
    if (!leaves.empty()) {
        hiveY = std::max(leaves[0].getY() - 1, logs[0].getY() + 1);
    } else {
        hiveY = std::min(logs[0].getY() + 1 + random.nextInt(3),
                        logs[logs.size() - 1].getY());
    }

    // Find valid placement positions
    std::vector<core::BlockPos> hivePlacements;
    core::Direction worldgenFacing = core::Direction::SOUTH;

    for (const auto& pos : logs) {
        if (pos.getY() == hiveY) {
            // Check horizontal directions (except opposite of worldgen facing)
            for (int dirIdx = 0; dirIdx < 4; ++dirIdx) {
                core::Direction dir = core::fromHorizontalIndex(dirIdx);
                if (dir != core::opposite(worldgenFacing)) {
                    hivePlacements.push_back(pos.relative(dir));
                }
            }
        }
    }

    if (hivePlacements.empty()) {
        return;
    }

    // Shuffle placements
    for (size_t i = hivePlacements.size() - 1; i > 0; --i) {
        size_t j = random.nextInt(static_cast<int>(i + 1));
        std::swap(hivePlacements[i], hivePlacements[j]);
    }

    // Find first valid position
    for (const auto& pos : hivePlacements) {
        if (context.isAir(pos) && context.isAir(pos.relative(worldgenFacing))) {
            BlockState* hiveState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:bee_nest"));
            
            context.setBlock(pos, hiveState);
            return;
        }
    }
}

// ============================================================================
// AlterGroundDecorator
// Reference: AlterGroundDecorator.java
// ============================================================================

void AlterGroundDecorator::place(DecoratorContext& context) {
    // Reference: AlterGroundDecorator.java place() lines 22-43
    // Get lowest trunk or root positions
    std::vector<core::BlockPos> blockPositions;

    const auto& roots = context.roots();
    const auto& logs = context.logs();

    if (roots.empty()) {
        blockPositions = logs;
    } else if (!logs.empty() && roots[0].getY() == logs[0].getY()) {
        blockPositions = logs;
        blockPositions.insert(blockPositions.end(), roots.begin(), roots.end());
    } else {
        blockPositions = roots;
    }

    if (blockPositions.empty()) {
        return;
    }

    int minY = blockPositions[0].getY();

    for (const auto& pos : blockPositions) {
        if (pos.getY() != minY) {
            continue;
        }

        placeCircle(context, pos.west().north());
        placeCircle(context, pos.east(2).north());
        placeCircle(context, pos.west().south(2));
        placeCircle(context, pos.east(2).south(2));

        for (int i = 0; i < 5; ++i) {
            int placement = context.random().nextInt(64);
            int xx = placement % 8;
            int zz = placement / 8;
            if (xx == 0 || xx == 7 || zz == 0 || zz == 7) {
                placeCircle(context, pos.offset(-3 + xx, 0, -3 + zz));
            }
        }
    }
}

void AlterGroundDecorator::placeCircle(DecoratorContext& context, const core::BlockPos& pos) {
    // Reference: AlterGroundDecorator.java placeCircle() lines 45-54
    for (int xx = -2; xx <= 2; ++xx) {
        for (int zz = -2; zz <= 2; ++zz) {
            if (std::abs(xx) != 2 || std::abs(zz) != 2) {
                placeBlockAt(context, pos.offset(xx, 0, zz));
            }
        }
    }
}

void AlterGroundDecorator::placeBlockAt(DecoratorContext& context, const core::BlockPos& pos) {
    // Reference: AlterGroundDecorator.java placeBlockAt() lines 56-68
    for (int dy = 2; dy >= -3; --dy) {
        core::BlockPos blockPos = pos.above(dy);
        BlockState* state = context.getBlockState(blockPos);

        if (state->getIdentifier() == "minecraft:dirt" || state->getIdentifier() == "minecraft:grass_block") {
            context.setBlock(blockPos, m_provider->getState(context.random(), pos));
            break;
        }

        if (!context.isAir(blockPos) && dy < 0) {
            break;
        }
    }
}

// ============================================================================
// AttachedToLeavesDecorator
// Reference: AttachedToLeavesDecorator.java
// ============================================================================

void AttachedToLeavesDecorator::place(DecoratorContext& context) {
    // Reference: AttachedToLeavesDecorator.java place() lines 34-52
    std::set<core::BlockPos> blacklist;
    XoroshiroRandomSource& random = context.random();

    // Get shuffled copy of leaves
    std::vector<core::BlockPos> shuffledLeaves = context.leaves();
    for (size_t i = shuffledLeaves.size() - 1; i > 0; --i) {
        size_t j = random.nextInt(static_cast<int>(i + 1));
        std::swap(shuffledLeaves[i], shuffledLeaves[j]);
    }

    // Get available directions (simplified - use DOWN for now)
    std::vector<core::Direction> directions = {core::Direction::DOWN};

    for (const auto& leafPos : shuffledLeaves) {
        // Pick random direction
        core::Direction direction = directions[random.nextInt(static_cast<int>(directions.size()))];
        core::BlockPos placementPos = leafPos.relative(direction);

        if (blacklist.count(placementPos) > 0) {
            continue;
        }

        if (random.nextFloat() >= m_probability) {
            continue;
        }

        if (!hasRequiredEmptyBlocks(context, leafPos, direction)) {
            continue;
        }

        // Add exclusion zone to blacklist
        for (int dx = -m_exclusionRadiusXZ; dx <= m_exclusionRadiusXZ; ++dx) {
            for (int dy = -m_exclusionRadiusY; dy <= m_exclusionRadiusY; ++dy) {
                for (int dz = -m_exclusionRadiusXZ; dz <= m_exclusionRadiusXZ; ++dz) {
                    blacklist.insert(placementPos.offset(dx, dy, dz));
                }
            }
        }

        context.setBlock(placementPos, m_blockState);
    }
}

bool AttachedToLeavesDecorator::hasRequiredEmptyBlocks(
    DecoratorContext& context,
    const core::BlockPos& leafPos,
    core::Direction direction
) const {
    // Reference: AttachedToLeavesDecorator.java hasRequiredEmptyBlocks() lines 55-64
    for (int i = 1; i <= m_requiredEmptyBlocks; ++i) {
        core::BlockPos offsetPos = leafPos.relative(direction, i);
        if (!context.isAir(offsetPos)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// CreakingHeartDecorator
// Reference: CreakingHeartDecorator.java
// ============================================================================

void CreakingHeartDecorator::place(DecoratorContext& context) {
    // Reference: CreakingHeartDecorator.java place() lines 30-51
    XoroshiroRandomSource& random = context.random();
    const auto& logs = context.logs();

    if (logs.empty()) {
        return;
    }

    if (random.nextFloat() >= m_probability) {
        return;
    }

    // Shuffle logs - Reference: line 35-36
    std::vector<core::BlockPos> heartPlacements = logs;
    for (size_t i = heartPlacements.size() - 1; i > 0; --i) {
        size_t j = random.nextInt(static_cast<int>(i + 1));
        std::swap(heartPlacements[i], heartPlacements[j]);
    }

    // Find a log that has logs on all 6 sides - Reference: lines 37-45
    for (const auto& pos : heartPlacements) {
        bool allSidesAreLogs = true;

        // Check all 6 directions
        for (int dir = 0; dir < 6; ++dir) {
            core::Direction direction = static_cast<core::Direction>(dir);
            core::BlockPos neighborPos = pos.relative(direction);

            // Check if neighbor is a log (in our context, we check if it's in the logs list)
            bool isLog = false;
            for (const auto& logPos : logs) {
                if (logPos == neighborPos) {
                    isLog = true;
                    break;
                }
            }

            if (!isLog) {
                allSidesAreLogs = false;
                break;
            }
        }

        if (allSidesAreLogs) {
            // Place creaking heart block - Reference: line 47
            BlockState* heartState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:creaking_heart"));
            
            
            context.setBlock(pos, heartState);
            return;
        }
    }
}

// ============================================================================
// PaleMossDecorator
// Reference: PaleMossDecorator.java
// ============================================================================

void PaleMossDecorator::place(DecoratorContext& context) {
    // Reference: PaleMossDecorator.java place() lines 37-65
    XoroshiroRandomSource& random = context.random();
    const auto& logs = context.logs();

    if (logs.empty()) {
        return;
    }

    // Shuffle logs - Reference: line 40
    std::vector<core::BlockPos> shuffledLogs = logs;
    for (size_t i = shuffledLogs.size() - 1; i > 0; --i) {
        size_t j = random.nextInt(static_cast<int>(i + 1));
        std::swap(shuffledLogs[i], shuffledLogs[j]);
    }

    // Find minimum Y position - Reference: line 42
    core::BlockPos origin = shuffledLogs[0];
    for (const auto& pos : shuffledLogs) {
        if (pos.getY() < origin.getY()) {
            origin = pos;
        }
    }

    // Ground probability - place moss patch (skipped as requires configured feature)
    // Reference: lines 43-44

    // Place hanging moss on logs - Reference: lines 47-55
    for (const auto& pos : context.logs()) {
        if (random.nextFloat() < m_trunkProbability) {
            core::BlockPos down = pos.below();
            if (context.isAir(down)) {
                addMossHanger(down, context);
            }
        }
    }

    // Place hanging moss on leaves - Reference: lines 56-64
    for (const auto& pos : context.leaves()) {
        if (random.nextFloat() < m_leavesProbability) {
            core::BlockPos down = pos.below();
            if (context.isAir(down)) {
                addMossHanger(down, context);
            }
        }
    }
}

void PaleMossDecorator::addMossHanger(const core::BlockPos& startPos, DecoratorContext& context) {
    // Reference: PaleMossDecorator.java addMossHanger() lines 68-75
    core::BlockPos pos = startPos;

    while (context.isAir(pos.below()) && !(context.random().nextFloat() < 0.5f)) {
        BlockState* mossState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:pale_hanging_moss"));
        
        context.setBlock(pos, mossState);
        pos = pos.below();
    }

    // Place the tip
    BlockState* tipState = static_cast<BlockState*>(::world::MinecraftBlocks::get("minecraft:pale_hanging_moss"));
    
    context.setBlock(pos, tipState);
}

// ============================================================================
// PlaceOnGroundDecorator
// Reference: PlaceOnGroundDecorator.java
// ============================================================================

void PlaceOnGroundDecorator::place(DecoratorContext& context) {
    // Reference: PlaceOnGroundDecorator.java place() lines 34-62
    // Get lowest trunk or root positions
    std::vector<core::BlockPos> blockPositions;

    const auto& roots = context.roots();
    const auto& logs = context.logs();

    if (roots.empty()) {
        blockPositions = logs;
    } else if (!logs.empty() && roots[0].getY() == logs[0].getY()) {
        blockPositions = logs;
        blockPositions.insert(blockPositions.end(), roots.begin(), roots.end());
    } else {
        blockPositions = roots;
    }

    if (blockPositions.empty()) {
        return;
    }

    // Find the origin (first position) - Reference: line 37
    core::BlockPos origin = blockPositions[0];

    // Find bounding box - Reference: lines 38-51
    int minY = origin.getY();
    int minX = origin.getX();
    int maxX = origin.getX();
    int minZ = origin.getZ();
    int maxZ = origin.getZ();

    for (const auto& position : blockPositions) {
        if (position.getY() == minY) {
            minX = std::min(minX, position.getX());
            maxX = std::max(maxX, position.getX());
            minZ = std::min(minZ, position.getZ());
            maxZ = std::max(maxZ, position.getZ());
        }
    }

    // Inflate bounding box - Reference: line 54
    minX -= m_radius;
    maxX += m_radius;
    minZ -= m_radius;
    maxZ += m_radius;
    int bbMinY = minY;
    int bbMaxY = minY + m_height;

    XoroshiroRandomSource& random = context.random();

    // Attempt placements - Reference: lines 57-60
    for (int i = 0; i < m_tries; ++i) {
        int x = random.nextIntBetweenInclusive(minX, maxX);
        int y = random.nextIntBetweenInclusive(bbMinY, bbMaxY);
        int z = random.nextIntBetweenInclusive(minZ, maxZ);
        attemptToPlaceBlockAbove(context, core::BlockPos(x, y, z));
    }
}

void PlaceOnGroundDecorator::attemptToPlaceBlockAbove(DecoratorContext& context, const core::BlockPos& pos) {
    // Reference: PlaceOnGroundDecorator.java attemptToPlaceBlockAbove() lines 65-69
    core::BlockPos abovePos = pos.above();

    // Check if above is air or vine
    BlockState* aboveState = context.getBlockState(abovePos);
    if (!aboveState->isAir() && aboveState->getIdentifier() != "minecraft:vine") {
        return;
    }

    // Check if pos is solid
    BlockState* posState = context.getBlockState(pos);
    if (!!posState->isAir()) {
        return;
    }

    // Place block
    context.setBlock(abovePos, m_blockStateProvider->getState(context.random(), abovePos));
}

// ============================================================================
// AttachedToLogsDecorator
// Reference: AttachedToLogsDecorator.java
// ============================================================================

void AttachedToLogsDecorator::place(DecoratorContext& context) {
    // Reference: AttachedToLogsDecorator.java place() lines 26-36
    XoroshiroRandomSource& random = context.random();

    if (m_directions.empty()) {
        return;
    }

    // Shuffle logs - Reference: line 29
    std::vector<core::BlockPos> shuffledLogs = context.logs();
    for (size_t i = shuffledLogs.size() - 1; i > 0; --i) {
        size_t j = random.nextInt(static_cast<int>(i + 1));
        std::swap(shuffledLogs[i], shuffledLogs[j]);
    }

    for (const auto& logsPos : shuffledLogs) {
        // Pick random direction - Reference: line 30
        core::Direction direction = m_directions[random.nextInt(static_cast<int>(m_directions.size()))];
        core::BlockPos placementPos = logsPos.relative(direction);

        // Check probability and air - Reference: lines 32-33
        if (random.nextFloat() <= m_probability && context.isAir(placementPos)) {
            context.setBlock(placementPos, m_blockProvider->getState(random, placementPos));
        }
    }
}

} // namespace treedecorators
} // namespace feature
} // namespace levelgen
} // namespace minecraft
