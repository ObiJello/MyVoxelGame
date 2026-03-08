#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "world/level/block/blocks/RotatedPillarBlock.h"
#include "core/Direction.h"
#include <cmath>
#include <optional>

// Reference: net/minecraft/world/level/levelgen/feature/trunkplacers/*.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace trunkplacers {

// ============================================================================
// TrunkPlacer base class
// ============================================================================

bool TrunkPlacer::placeLog(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    const core::BlockPos& pos,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider
) {
    return placeLog(level, trunkSetter, random, pos, trunkProvider, [](BlockState* state) {
        return state;
    });
}

bool TrunkPlacer::placeLog(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    const core::BlockPos& pos,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::function<BlockState*(BlockState*)> stateModifier
) {
    if (validTreePos(level, pos)) {
        BlockState* state = trunkProvider->getState(random, pos);
        trunkSetter(pos, stateModifier(state));
        return true;
    }
    return false;
}

bool TrunkPlacer::validTreePos(LevelReader& level, const core::BlockPos& pos) const {
    return level.isStateAtPosition(pos, [](BlockState* state) {
        return state && (state->isAir() ||
                         ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                             state,
                             "minecraft:replaceable_by_trees"
                         ));
    });
}

void TrunkPlacer::setDirtAt(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    const core::BlockPos& pos,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: TrunkPlacer.java setDirtAt() lines 52-57
    bool isDirt = level.isStateAtPosition(pos, [](BlockState* state) {
        if (!state) return false;
        if (!::minecraft::levelgen::FeatureHelpers::isDirt(state)) {
            return false;
        }

        const std::string& name = state->getIdentifier();
        return name != "minecraft:grass_block" && name != "minecraft:mycelium";
    });

    if (forceDirt || !isDirt) {
        BlockState* state = dirtProvider->getState(random, pos);
        trunkSetter(pos, state);
    }
}

// ============================================================================
// StraightTrunkPlacer
// Reference: StraightTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> StraightTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: StraightTrunkPlacer.java placeTrunk() lines 26-34
    setDirtAt(level, trunkSetter, random, origin.below(), dirtProvider, forceDirt);

    for (int y = 0; y < treeHeight; ++y) {
        placeLog(level, trunkSetter, random, origin.above(y), trunkProvider);
    }

    std::vector<foliageplacers::FoliageAttachment> attachments;
    attachments.emplace_back(origin.above(treeHeight), 0, false);
    return attachments;
}

// ============================================================================
// ForkingTrunkPlacer
// Reference: ForkingTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> ForkingTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: ForkingTrunkPlacer.java placeTrunk() lines 28-83
    setDirtAt(level, trunkSetter, random, origin.below(), dirtProvider, forceDirt);

    std::vector<foliageplacers::FoliageAttachment> attachments;

    // Get random horizontal direction for main lean
    core::Direction leanDirection = core::horizontalRandom(random);
    int leanHeight = treeHeight - random.nextInt(4) - 1;
    int leanSteps = 3 - random.nextInt(3);

    int tx = origin.getX();
    int tz = origin.getZ();
    std::optional<int> ey;

    for (int yo = 0; yo < treeHeight; ++yo) {
        int yy = origin.getY() + yo;
        if (yo >= leanHeight && leanSteps > 0) {
            tx += core::getStepX(leanDirection);
            tz += core::getStepZ(leanDirection);
            --leanSteps;
        }

        core::BlockPos logPos(tx, yy, tz);
        if (placeLog(level, trunkSetter, random, logPos, trunkProvider)) {
            ey = yy + 1;
        }
    }

    if (ey.has_value()) {
        attachments.emplace_back(core::BlockPos(tx, ey.value(), tz), 1, false);
    }

    // Reset for branch
    tx = origin.getX();
    tz = origin.getZ();
    core::Direction branchDirection = core::horizontalRandom(random);

    if (branchDirection != leanDirection) {
        int branchPos = leanHeight - random.nextInt(2) - 1;
        int branchSteps = 1 + random.nextInt(3);
        ey = std::nullopt;

        for (int yo = branchPos; yo < treeHeight && branchSteps > 0; --branchSteps) {
            if (yo >= 1) {
                int yy = origin.getY() + yo;
                tx += core::getStepX(branchDirection);
                tz += core::getStepZ(branchDirection);
                core::BlockPos logPos(tx, yy, tz);
                if (placeLog(level, trunkSetter, random, logPos, trunkProvider)) {
                    ey = yy + 1;
                }
            }
            ++yo;
        }

        if (ey.has_value()) {
            attachments.emplace_back(core::BlockPos(tx, ey.value(), tz), 0, false);
        }
    }

    return attachments;
}

// ============================================================================
// GiantTrunkPlacer
// Reference: GiantTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> GiantTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: GiantTrunkPlacer.java placeTrunk() lines 26-44
    core::BlockPos below = origin.below();
    setDirtAt(level, trunkSetter, random, below, dirtProvider, forceDirt);
    setDirtAt(level, trunkSetter, random, below.east(), dirtProvider, forceDirt);
    setDirtAt(level, trunkSetter, random, below.south(), dirtProvider, forceDirt);
    setDirtAt(level, trunkSetter, random, below.south().east(), dirtProvider, forceDirt);

    for (int hh = 0; hh < treeHeight; ++hh) {
        placeLogIfFreeWithOffset(level, trunkSetter, random, trunkProvider, origin, 0, hh, 0);
        if (hh < treeHeight - 1) {
            placeLogIfFreeWithOffset(level, trunkSetter, random, trunkProvider, origin, 1, hh, 0);
            placeLogIfFreeWithOffset(level, trunkSetter, random, trunkProvider, origin, 1, hh, 1);
            placeLogIfFreeWithOffset(level, trunkSetter, random, trunkProvider, origin, 0, hh, 1);
        }
    }

    std::vector<foliageplacers::FoliageAttachment> attachments;
    attachments.emplace_back(origin.above(treeHeight), 0, true);
    return attachments;
}

void GiantTrunkPlacer::placeLogIfFreeWithOffset(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    const core::BlockPos& treePos,
    int x, int y, int z
) {
    core::BlockPos pos = treePos.offset(x, y, z);
    if (isFree(level, pos)) {
        placeLog(level, trunkSetter, random, pos, trunkProvider);
    }
}

bool GiantTrunkPlacer::isFree(LevelReader& level, const core::BlockPos& pos) const {
    // Reference: GiantTrunkPlacer.java isFree() -> validTreePos || LOGS tag
    return validTreePos(level, pos) || level.isStateAtPosition(pos, [](BlockState* state) {
        return state && state->isLog();
    });
}

// ============================================================================
// MegaJungleTrunkPlacer
// Reference: MegaJungleTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> MegaJungleTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: MegaJungleTrunkPlacer.java placeTrunk() lines 27-47
    std::vector<foliageplacers::FoliageAttachment> attachments;

    // Call parent to place 2x2 trunk
    auto parentAttachments = GiantTrunkPlacer::placeTrunk(
        level, trunkSetter, random, treeHeight, origin, trunkProvider, dirtProvider, forceDirt);
    attachments.insert(attachments.end(), parentAttachments.begin(), parentAttachments.end());

    // Place branches at intervals
    for (int branchHeight = treeHeight - 2 - random.nextInt(4);
         branchHeight > treeHeight / 2;
         branchHeight -= 2 + random.nextInt(4)) {

        float angle = random.nextFloat() * (static_cast<float>(M_PI) * 2.0f);
        int bx = 0;
        int bz = 0;

        for (int b = 0; b < 5; ++b) {
            bx = static_cast<int>(1.5f + std::cos(static_cast<double>(angle)) * static_cast<float>(b));
            bz = static_cast<int>(1.5f + std::sin(static_cast<double>(angle)) * static_cast<float>(b));
            core::BlockPos pos = origin.offset(bx, branchHeight - 3 + b / 2, bz);
            placeLog(level, trunkSetter, random, pos, trunkProvider);
        }

        attachments.emplace_back(origin.offset(bx, branchHeight, bz), -2, false);
    }

    return attachments;
}

// ============================================================================
// DarkOakTrunkPlacer
// Reference: DarkOakTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> DarkOakTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: DarkOakTrunkPlacer.java placeTrunk() lines 28-79
    std::vector<foliageplacers::FoliageAttachment> attachments;

    core::BlockPos below = origin.below();
    setDirtAt(level, trunkSetter, random, below, dirtProvider, forceDirt);
    setDirtAt(level, trunkSetter, random, below.east(), dirtProvider, forceDirt);
    setDirtAt(level, trunkSetter, random, below.south(), dirtProvider, forceDirt);
    setDirtAt(level, trunkSetter, random, below.south().east(), dirtProvider, forceDirt);

    core::Direction leanDirection = core::horizontalRandom(random);
    int leanHeight = treeHeight - random.nextInt(4);
    int leanSteps = 2 - random.nextInt(3);

    int x = origin.getX();
    int y = origin.getY();
    int z = origin.getZ();
    int tx = x;
    int tz = z;
    int ey = y + treeHeight - 1;

    for (int dy = 0; dy < treeHeight; ++dy) {
        if (dy >= leanHeight && leanSteps > 0) {
            tx += core::getStepX(leanDirection);
            tz += core::getStepZ(leanDirection);
            --leanSteps;
        }

        int yy = y + dy;
        core::BlockPos blockPos(tx, yy, tz);

        // isAirOrLeaves check
        if (level.isStateAtPosition(blockPos, [](BlockState* state) {
            return state->isAir() || state->isLeaves();
        })) {
            placeLog(level, trunkSetter, random, blockPos, trunkProvider);
            placeLog(level, trunkSetter, random, blockPos.east(), trunkProvider);
            placeLog(level, trunkSetter, random, blockPos.south(), trunkProvider);
            placeLog(level, trunkSetter, random, blockPos.east().south(), trunkProvider);
        }
    }

    attachments.emplace_back(core::BlockPos(tx, ey, tz), 0, true);

    // Place corner branches
    for (int ox = -1; ox <= 2; ++ox) {
        for (int oz = -1; oz <= 2; ++oz) {
            if ((ox < 0 || ox > 1 || oz < 0 || oz > 1) && random.nextInt(3) <= 0) {
                int length = random.nextInt(3) + 2;

                for (int branchY = 0; branchY < length; ++branchY) {
                    placeLog(level, trunkSetter, random,
                        core::BlockPos(x + ox, ey - branchY - 1, z + oz), trunkProvider);
                }

                attachments.emplace_back(core::BlockPos(x + ox, ey, z + oz), 0, false);
            }
        }
    }

    return attachments;
}

// ============================================================================
// FancyTrunkPlacer
// Reference: FancyTrunkPlacer.java
// ============================================================================

static constexpr double TRUNK_HEIGHT_SCALE = 0.618;
static constexpr double CLUSTER_DENSITY_MAGIC = 1.382;
static constexpr double BRANCH_SLOPE = 0.381;
static constexpr double BRANCH_LENGTH_MAGIC = 0.328;

static float treeShape(int height, int y) {
    // Reference: FancyTrunkPlacer.java treeShape() lines 145-159
    if (static_cast<float>(y) < static_cast<float>(height) * 0.3f) {
        return -1.0f;
    }

    float radius = static_cast<float>(height) / 2.0f;
    float adjacent = radius - static_cast<float>(y);
    float distance = std::sqrt(radius * radius - adjacent * adjacent);

    if (adjacent == 0.0f) {
        distance = radius;
    } else if (std::abs(adjacent) >= radius) {
        return 0.0f;
    }

    return distance * 0.5f;
}

std::vector<foliageplacers::FoliageAttachment> FancyTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: FancyTrunkPlacer.java placeTrunk() lines 34-82
    int height = treeHeight + 2;
    int trunkHeight = static_cast<int>(std::floor(static_cast<double>(height) * TRUNK_HEIGHT_SCALE));
    setDirtAt(level, trunkSetter, random, origin.below(), dirtProvider, forceDirt);

    int clustersPerY = std::min(1, static_cast<int>(std::floor(
        CLUSTER_DENSITY_MAGIC + std::pow(1.0 * static_cast<double>(height) / 13.0, 2.0))));
    int trunkTop = origin.getY() + trunkHeight;
    int relativeY = height - 5;

    std::vector<FoliageCoords> foliageCoords;
    foliageCoords.emplace_back(origin.above(relativeY), trunkTop);

    for (; relativeY >= 0; --relativeY) {
        float treeShapeVal = treeShape(height, relativeY);
        if (treeShapeVal >= 0.0f) {
            for (int i = 0; i < clustersPerY; ++i) {
                double radius = 1.0 * static_cast<double>(treeShapeVal) *
                               (static_cast<double>(random.nextFloat()) + BRANCH_LENGTH_MAGIC);
                double angle = static_cast<double>(random.nextFloat() * 2.0f) * M_PI;
                double xOff = radius * std::sin(angle) + 0.5;
                double zOff = radius * std::cos(angle) + 0.5;

                core::BlockPos checkStart = origin.offset(
                    static_cast<int>(std::floor(xOff)),
                    relativeY - 1,
                    static_cast<int>(std::floor(zOff)));
                core::BlockPos checkEnd = checkStart.above(5);

                if (makeLimb(level, trunkSetter, random, checkStart, checkEnd, false, trunkProvider)) {
                    int dx = origin.getX() - checkStart.getX();
                    int dz = origin.getZ() - checkStart.getZ();
                    double branchHeight = static_cast<double>(checkStart.getY()) -
                                         std::sqrt(static_cast<double>(dx * dx + dz * dz)) * BRANCH_SLOPE;
                    int branchTop = branchHeight > static_cast<double>(trunkTop) ?
                                   trunkTop : static_cast<int>(branchHeight);
                    core::BlockPos checkBranchBase(origin.getX(), branchTop, origin.getZ());

                    if (makeLimb(level, trunkSetter, random, checkBranchBase, checkStart, false, trunkProvider)) {
                        foliageCoords.emplace_back(checkStart, checkBranchBase.getY());
                    }
                }
            }
        }
    }

    // Place main trunk
    makeLimb(level, trunkSetter, random, origin, origin.above(trunkHeight), true, trunkProvider);
    makeBranches(level, trunkSetter, random, height, origin, foliageCoords, trunkProvider);

    std::vector<foliageplacers::FoliageAttachment> attachments;
    for (const auto& coord : foliageCoords) {
        if (trimBranches(height, coord.branchBase - origin.getY())) {
            attachments.push_back(coord.attachment);
        }
    }

    return attachments;
}

bool FancyTrunkPlacer::makeLimb(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    const core::BlockPos& startPos,
    const core::BlockPos& endPos,
    bool doPlace,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider
) {
    // Reference: FancyTrunkPlacer.java makeLimb() lines 84-105
    if (!doPlace && startPos == endPos) {
        return true;
    }

    core::BlockPos delta = endPos.offset(-startPos.getX(), -startPos.getY(), -startPos.getZ());
    int steps = getSteps(delta);
    float dx = static_cast<float>(delta.getX()) / static_cast<float>(steps);
    float dy = static_cast<float>(delta.getY()) / static_cast<float>(steps);
    float dz = static_cast<float>(delta.getZ()) / static_cast<float>(steps);

    for (int i = 0; i <= steps; ++i) {
        core::BlockPos blockPos = startPos.offset(
            static_cast<int>(std::floor(0.5f + static_cast<float>(i) * dx)),
            static_cast<int>(std::floor(0.5f + static_cast<float>(i) * dy)),
            static_cast<int>(std::floor(0.5f + static_cast<float>(i) * dz)));

        if (doPlace) {
            core::Axis axis = getLogAxis(startPos, blockPos);
            placeLog(level, trunkSetter, random, blockPos, trunkProvider, [axis](BlockState* state) {
                return state->trySetValue(*world::level::block::RotatedPillarBlock::AXIS, axis);
            });
        } else if (!isFree(level, blockPos)) {
            return false;
        }
    }

    return true;
}

int FancyTrunkPlacer::getSteps(const core::BlockPos& pos) const {
    int absX = std::abs(pos.getX());
    int absY = std::abs(pos.getY());
    int absZ = std::abs(pos.getZ());
    return std::max(absX, std::max(absY, absZ));
}

core::Axis FancyTrunkPlacer::getLogAxis(
    const core::BlockPos& startPos,
    const core::BlockPos& blockPos
) const {
    // Reference: FancyTrunkPlacer.java getLogAxis() lines 114-128
    core::Axis axis = core::Axis::Y;
    int xdiff = std::abs(blockPos.getX() - startPos.getX());
    int zdiff = std::abs(blockPos.getZ() - startPos.getZ());
    int maxdiff = std::max(xdiff, zdiff);

    if (maxdiff > 0) {
        if (xdiff == maxdiff) {
            axis = core::Axis::X;
        } else {
            axis = core::Axis::Z;
        }
    }

    return axis;
}

bool FancyTrunkPlacer::trimBranches(int height, int localY) const {
    return static_cast<double>(localY) >= static_cast<double>(height) * 0.2;
}

void FancyTrunkPlacer::makeBranches(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int height,
    const core::BlockPos& origin,
    const std::vector<FoliageCoords>& foliageCoords,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider
) {
    // Reference: FancyTrunkPlacer.java makeBranches() lines 134-142
    for (const auto& endCoord : foliageCoords) {
        int branchBase = endCoord.branchBase;
        core::BlockPos baseCoord(origin.getX(), branchBase, origin.getZ());
        if (baseCoord != endCoord.attachment.pos() && trimBranches(height, branchBase - origin.getY())) {
            makeLimb(level, trunkSetter, random, baseCoord, endCoord.attachment.pos(), true, trunkProvider);
        }
    }
}

bool FancyTrunkPlacer::isFree(LevelReader& level, const core::BlockPos& pos) const {
    // Reference: FancyTrunkPlacer.java isFree() -> validTreePos || LOGS tag
    return validTreePos(level, pos) || level.isStateAtPosition(pos, [](BlockState* state) {
        return state && state->isLog();
    });
}

// ============================================================================
// BendingTrunkPlacer
// Reference: BendingTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> BendingTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: BendingTrunkPlacer.java placeTrunk() lines 34-70
    core::Direction direction = core::horizontalRandom(random);
    int logHeight = treeHeight - 1;
    core::BlockPos pos = origin;

    setDirtAt(level, trunkSetter, random, pos.below(), dirtProvider, forceDirt);
    std::vector<foliageplacers::FoliageAttachment> foliagePoints;

    for (int i = 0; i <= logHeight; ++i) {
        if (i + 1 >= logHeight + random.nextInt(2)) {
            pos = pos.relative(direction);
        }

        if (validTreePos(level, pos)) {
            placeLog(level, trunkSetter, random, pos, trunkProvider);
        }

        if (i >= m_minHeightForLeaves) {
            foliagePoints.emplace_back(pos, 0, false);
        }

        pos = pos.above();
    }

    int dirLength = m_bendLength->sample(random);

    for (int i = 0; i <= dirLength; ++i) {
        if (validTreePos(level, pos)) {
            placeLog(level, trunkSetter, random, pos, trunkProvider);
        }

        foliagePoints.emplace_back(pos, 0, false);
        pos = pos.relative(direction);
    }

    return foliagePoints;
}

// ============================================================================
// UpwardsBranchingTrunkPlacer
// Reference: UpwardsBranchingTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> UpwardsBranchingTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: UpwardsBranchingTrunkPlacer.java placeTrunk() lines 41-61
    std::vector<foliageplacers::FoliageAttachment> attachments;
    core::BlockPos logPos = origin;

    for (int heightPos = 0; heightPos < treeHeight; ++heightPos) {
        int currentHeight = origin.getY() + heightPos;
        core::BlockPos currentPos(origin.getX(), currentHeight, origin.getZ());

        if (placeLog(level, trunkSetter, random, currentPos, trunkProvider) &&
            heightPos < treeHeight - 1 &&
            random.nextFloat() < m_placeBranchPerLogProbability) {

            core::Direction branchDir = core::horizontalRandom(random);
            int branchLen = m_extraBranchLength->sample(random);
            int branchPos = std::max(0, branchLen - m_extraBranchLength->sample(random) - 1);
            int branchSteps = m_extraBranchSteps->sample(random);

            placeBranch(level, trunkSetter, random, treeHeight, trunkProvider,
                       attachments, currentPos, currentHeight, branchDir, branchPos, branchSteps);
        }

        if (heightPos == treeHeight - 1) {
            attachments.emplace_back(core::BlockPos(origin.getX(), currentHeight + 1, origin.getZ()), 0, false);
        }
    }

    return attachments;
}

void UpwardsBranchingTrunkPlacer::placeBranch(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::vector<foliageplacers::FoliageAttachment>& attachments,
    const core::BlockPos& logPos,
    int currentHeight,
    core::Direction branchDir,
    int branchPos,
    int branchSteps
) {
    // Reference: UpwardsBranchingTrunkPlacer.java placeBranch() lines 63-90
    int heightAlongBranch = currentHeight + branchPos;
    int logX = logPos.getX();
    int logZ = logPos.getZ();

    for (int branchPlacementIndex = branchPos; branchPlacementIndex < treeHeight && branchSteps > 0; --branchSteps) {
        if (branchPlacementIndex >= 1) {
            int placementHeight = currentHeight + branchPlacementIndex;
            logX += core::getStepX(branchDir);
            logZ += core::getStepZ(branchDir);
            heightAlongBranch = placementHeight;

            core::BlockPos branchLogPos(logX, placementHeight, logZ);
            if (placeLog(level, trunkSetter, random, branchLogPos, trunkProvider)) {
                heightAlongBranch = placementHeight + 1;
            }

            attachments.emplace_back(branchLogPos, 0, false);
        }

        ++branchPlacementIndex;
    }

    if (heightAlongBranch - currentHeight > 1) {
        core::BlockPos foliagePos(logX, heightAlongBranch, logZ);
        attachments.emplace_back(foliagePos, 0, false);
        attachments.emplace_back(foliagePos.below(2), 0, false);
    }
}

// ============================================================================
// CherryTrunkPlacer
// Reference: CherryTrunkPlacer.java
// ============================================================================

std::vector<foliageplacers::FoliageAttachment> CherryTrunkPlacer::placeTrunk(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    std::shared_ptr<stateproviders::BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    // Reference: CherryTrunkPlacer.java placeTrunk() lines 44-82
    setDirtAt(level, trunkSetter, random, origin.below(), dirtProvider, forceDirt);

    // Reference: lines 46-47 - sample branch start offsets
    int firstBranchOffsetFromOrigin = std::max(0, treeHeight - 1 +
        sampleUniform(random, m_branchStartOffsetFromTopMin, m_branchStartOffsetFromTopMax));

    // Reference: line 47 - second branch uses range with max-1
    int secondBranchOffsetFromOrigin = std::max(0, treeHeight - 1 +
        sampleUniform(random, m_branchStartOffsetFromTopMin, m_branchStartOffsetFromTopMax - 1));

    // Reference: lines 48-50 - ensure second branch is different
    if (secondBranchOffsetFromOrigin >= firstBranchOffsetFromOrigin) {
        ++secondBranchOffsetFromOrigin;
    }

    // Reference: lines 52-54
    int branchCount = m_branchCount->sample(random);
    bool hasMiddleBranch = branchCount == 3;
    bool hasBothSideBranches = branchCount >= 2;

    // Reference: lines 55-62 - determine trunk height
    int trunkHeight;
    if (hasMiddleBranch) {
        trunkHeight = treeHeight;
    } else if (hasBothSideBranches) {
        trunkHeight = std::max(firstBranchOffsetFromOrigin, secondBranchOffsetFromOrigin) + 1;
    } else {
        trunkHeight = firstBranchOffsetFromOrigin + 1;
    }

    // Reference: lines 64-66 - place main trunk
    for (int y = 0; y < trunkHeight; ++y) {
        placeLog(level, trunkSetter, random, origin.above(y), trunkProvider);
    }

    std::vector<foliageplacers::FoliageAttachment> attachments;

    // Reference: lines 69-71 - add middle foliage if 3 branches
    if (hasMiddleBranch) {
        attachments.emplace_back(origin.above(trunkHeight), 0, false);
    }

    // Reference: lines 73-78 - generate branches
    core::BlockPos::MutableBlockPos logPos;
    core::Direction treeDirection = core::horizontalRandom(random);

    attachments.push_back(generateBranch(
        level, trunkSetter, random, treeHeight, origin, trunkProvider,
        treeDirection, firstBranchOffsetFromOrigin,
        firstBranchOffsetFromOrigin < trunkHeight - 1, logPos));

    if (hasBothSideBranches) {
        attachments.push_back(generateBranch(
            level, trunkSetter, random, treeHeight, origin, trunkProvider,
            core::opposite(treeDirection), secondBranchOffsetFromOrigin,
            secondBranchOffsetFromOrigin < trunkHeight - 1, logPos));
    }

    return attachments;
}

foliageplacers::FoliageAttachment CherryTrunkPlacer::generateBranch(
    LevelReader& level,
    TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<stateproviders::BlockStateProvider> trunkProvider,
    core::Direction branchDirection,
    int offsetFromOrigin,
    bool middleContinuesUpwards,
    core::BlockPos::MutableBlockPos& logPos
) {
    // Reference: CherryTrunkPlacer.java generateBranch() lines 84-109
    logPos.set(origin);
    logPos.move(core::Direction::UP, offsetFromOrigin);

    // Reference: line 86
    int branchEndPosOffsetFromOrigin = treeHeight - 1 + m_branchEndOffsetFromTop->sample(random);

    // Reference: line 87
    bool extendBranchAwayFromTrunk = middleContinuesUpwards || branchEndPosOffsetFromOrigin < offsetFromOrigin;

    // Reference: line 88
    int distanceToTrunk = m_branchHorizontalLength->sample(random) + (extendBranchAwayFromTrunk ? 1 : 0);

    // Reference: line 89 - calculate branch end position
    core::BlockPos branchEndPos = origin.relative(branchDirection, distanceToTrunk).above(branchEndPosOffsetFromOrigin);

    // Reference: line 90
    int stepsHorizontally = extendBranchAwayFromTrunk ? 2 : 1;

    // Reference: lines 92-94 - place initial horizontal logs with sideways axis
    for (int i = 0; i < stepsHorizontally; ++i) {
        logPos.move(branchDirection);
        // Reference: Java calls this.placeLog() which checks validTreePos()
        placeLog(level, trunkSetter, random, logPos, trunkProvider, [branchDirection](BlockState* state) {
            return state->trySetValue(*world::level::block::RotatedPillarBlock::AXIS, core::getAxis(branchDirection));
        });
    }

    // Reference: line 96
    core::Direction verticalDirection = branchEndPos.getY() > logPos.getY() ? core::Direction::UP : core::Direction::DOWN;

    // Reference: lines 98-108 - trace path to branch end
    while (true) {
        int distance = logPos.distManhattan(branchEndPos);
        if (distance == 0) {
            return foliageplacers::FoliageAttachment(branchEndPos.above(), 0, false);
        }

        // Reference: lines 104-105
        float chanceToGrowVertically = static_cast<float>(std::abs(branchEndPos.getY() - logPos.getY())) /
                                       static_cast<float>(distance);
        bool growVertically = random.nextFloat() < chanceToGrowVertically;

        // Reference: line 106
        logPos.move(growVertically ? verticalDirection : branchDirection);

        // Reference: line 107 - place log with validTreePos check
        placeLog(level, trunkSetter, random, logPos, trunkProvider, [growVertically, branchDirection](BlockState* state) {
            if (growVertically) {
                return state;
            }

            return state->trySetValue(*world::level::block::RotatedPillarBlock::AXIS, core::getAxis(branchDirection));
        });
    }
}

} // namespace trunkplacers
} // namespace feature
} // namespace levelgen
} // namespace minecraft
