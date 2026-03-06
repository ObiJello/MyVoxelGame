#include "world/level/block/SculkSpreader.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/SculkVeinBlock.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <iostream>

// Reference: net/minecraft/world/level/block/SculkSpreader.java

namespace minecraft {
namespace world {
namespace level {
namespace block {

using namespace state;
using ::minecraft::core::BlockPos;
using ::minecraft::core::Direction;
using ::minecraft::core::Vec3i;
using XoroshiroRandomSource = ::minecraft::XoroshiroRandomSource;

// ============================================================================
// NON_CORNER_NEIGHBOURS initialization
// Reference: SculkSpreader.ChargeCursor.NON_CORNER_NEIGHBOURS
// 18 positions: faces (6) + edges (12), excluding corners (8) and center
// ============================================================================
static std::vector<Vec3i> initNonCornerNeighbours() {
    std::vector<Vec3i> result;
    result.reserve(18);

    // Reference: BlockPos.betweenClosedStream(-1,-1,-1, 1,1,1)
    // Java iteration order: x changes fastest, y second, z slowest (z-layer-by-layer)
    // This is CRITICAL for shuffle parity!
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                // Skip center
                if (x == 0 && y == 0 && z == 0) continue;

                // Include only if at least one coordinate is 0 (faces + edges)
                // Exclude corners (where all coordinates are non-zero)
                if (x == 0 || y == 0 || z == 0) {
                    result.emplace_back(x, y, z);
                }
            }
        }
    }

    // Expected order (matching Java exactly):
    // z=-1: (0,-1,-1), (-1,0,-1), (0,0,-1), (1,0,-1), (0,1,-1)
    // z=0:  (-1,-1,0), (0,-1,0), (1,-1,0), (-1,0,0), (1,0,0), (-1,1,0), (0,1,0), (1,1,0)
    // z=+1: (0,-1,1), (-1,0,1), (0,0,1), (1,0,1), (0,1,1)

    return result;
}

static const std::vector<Vec3i> s_nonCornerNeighbours = initNonCornerNeighbours();

// Debug counters for sculk growth
static int s_attemptUseChargeCount = 0;
static int s_enteredMainLogic = 0;
static int s_canPlaceGrowthTrue = 0;
static int s_growthPlaced = 0;
static int s_canPlaceGrowthChecks = 0;
static int s_canPlaceGrowthFailAbove = 0;
static int s_canPlaceGrowthPass = 0;
static int s_veinToAirCount = 0;

// Track vein faces during spreading (workaround for block state face tracking)
// Key: packed position (x + y*65536 + z*4294967296), Value: set of faces
static std::unordered_map<int64_t, std::set<Direction>> s_veinFaces;

static int64_t packPos(const BlockPos& pos) {
    return (static_cast<int64_t>(pos.getX()) & 0xFFFFF) |
           ((static_cast<int64_t>(pos.getY()) & 0xFFF) << 20) |
           ((static_cast<int64_t>(pos.getZ()) & 0xFFFFF) << 32);
}

static void addVeinFace(const BlockPos& pos, Direction face) {
    s_veinFaces[packPos(pos)].insert(face);
}

static bool hasVeinFace(const BlockPos& pos, Direction face) {
    auto it = s_veinFaces.find(packPos(pos));
    if (it == s_veinFaces.end()) return false;
    return it->second.find(face) != it->second.end();
}

static void clearVeinFaces() {
    s_veinFaces.clear();
}

const std::vector<Vec3i>& ChargeCursor::getNonCornerNeighbours() {
    return s_nonCornerNeighbours;
}

// ============================================================================
// ChargeCursor implementation
// ============================================================================

ChargeCursor::ChargeCursor(const BlockPos& pos, int charge)
    : m_pos(pos)
    , m_charge(charge)
    , m_updateDelay(0)
    , m_decayDelay(1)
    , m_facings(std::nullopt)
{
}

ChargeCursor::ChargeCursor(const BlockPos& pos, int charge, int decayDelay, int updateDelay,
                           const std::optional<std::set<Direction>>& facings)
    : m_pos(pos)
    , m_charge(charge)
    , m_updateDelay(updateDelay)
    , m_decayDelay(decayDelay)
    , m_facings(facings)
{
}

bool ChargeCursor::isPosUnreasonable(const BlockPos& originPos) const {
    // Reference: distChessboard > 1024
    int dx = std::abs(m_pos.getX() - originPos.getX());
    int dy = std::abs(m_pos.getY() - originPos.getY());
    int dz = std::abs(m_pos.getZ() - originPos.getZ());
    return std::max({dx, dy, dz}) > SculkSpreader::MAX_CURSOR_DISTANCE;
}

std::vector<Vec3i> ChargeCursor::getRandomizedNonCornerNeighbourOffsets(XoroshiroRandomSource& random) {
    // Reference: Util.shuffledCopy(NON_CORNER_NEIGHBOURS, random)
    std::vector<Vec3i> result = s_nonCornerNeighbours;

    // Fisher-Yates shuffle using the random source
    for (int i = static_cast<int>(result.size()) - 1; i > 0; --i) {
        int j = random.nextInt(i + 1);
        std::swap(result[i], result[j]);
    }

    return result;
}

bool ChargeCursor::isUnobstructed(IChunk* level, const BlockPos& from, Direction direction) {
    // Reference: lines 348-350
    BlockPos testPos = from.relative(direction);
    BlockState* state = level->getBlockState(testPos);
    if (!state) return true;

    // Check if face is not sturdy (simplified - just check if not solid)
    return state->isAir() || state->getIdentifier() == "minecraft:water";
}

bool ChargeCursor::isMovementUnobstructed(IChunk* level, const BlockPos& from, const BlockPos& to) {
    // Reference: lines 330-345
    int dist = std::abs(to.getX() - from.getX()) + std::abs(to.getY() - from.getY()) + std::abs(to.getZ() - from.getZ());
    if (dist == 1) {
        return true;
    }

    int dx = to.getX() - from.getX();
    int dy = to.getY() - from.getY();
    int dz = to.getZ() - from.getZ();

    Direction dirX = dx < 0 ? Direction::WEST : Direction::EAST;
    Direction dirY = dy < 0 ? Direction::DOWN : Direction::UP;
    Direction dirZ = dz < 0 ? Direction::NORTH : Direction::SOUTH;

    if (dx == 0) {
        return isUnobstructed(level, from, dirY) || isUnobstructed(level, from, dirZ);
    } else if (dy == 0) {
        return isUnobstructed(level, from, dirX) || isUnobstructed(level, from, dirZ);
    } else {
        return isUnobstructed(level, from, dirX) || isUnobstructed(level, from, dirY);
    }
}

// Helper to check if a sculk_vein block has substrate access
// Reference: SculkVeinBlock.hasSubstrateAccess()
static bool hasSubstrateAccess(IChunk* level, BlockState* state, const BlockPos& pos) {
    if (!state || state->getIdentifier() != "minecraft:sculk_vein") {
        return false;
    }
    // Check all 6 directions for replaceable substrate
    for (int dir = 0; dir < 6; ++dir) {
        Direction direction = static_cast<Direction>(dir);
        BlockPos relPos = pos.relative(direction);
        BlockState* relState = level->getBlockState(relPos);
        if (relState) {
            const std::string& id = relState->getIdentifier();
            // Check if it's in SCULK_REPLACEABLE tag (simplified check)
            if (id == "minecraft:stone" || id == "minecraft:deepslate" ||
                id == "minecraft:granite" || id == "minecraft:diorite" ||
                id == "minecraft:andesite" || id == "minecraft:tuff" ||
                id == "minecraft:calcite" || id == "minecraft:smooth_basalt" ||
                id == "minecraft:dripstone_block" || id == "minecraft:dirt" ||
                id == "minecraft:gravel" || id == "minecraft:sand" ||
                id == "minecraft:clay" || id == "minecraft:mud" ||
                id == "minecraft:terracotta" || id.find("_terracotta") != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

BlockPos* ChargeCursor::getValidMovementPos(IChunk* level, const BlockPos& pos, XoroshiroRandomSource& random) {
    // Reference: SculkSpreader.java lines 312-327
    // Java does NOT break on first valid - it keeps looking for one with substrate access!
    static thread_local BlockPos resultPos;
    resultPos = pos;  // Start with current position (means no valid found yet)

    for (const Vec3i& offset : getRandomizedNonCornerNeighbourOffsets(random)) {
        BlockPos neighbour = pos.offset(offset.getX(), offset.getY(), offset.getZ());
        BlockState* transferee = level->getBlockState(neighbour);

        if (transferee && SculkBehaviour::isSculkBehaviour(transferee) &&
            isMovementUnobstructed(level, pos, neighbour)) {
            resultPos = neighbour;
            // In Java, it only breaks early if hasSubstrateAccess returns true
            // Otherwise it keeps looking (but keeps the last valid position)
            if (hasSubstrateAccess(level, transferee, neighbour)) {
                break;
            }
        }
    }

    if (resultPos.getX() == pos.getX() && resultPos.getY() == pos.getY() && resultPos.getZ() == pos.getZ()) {
        return nullptr;
    }
    return &resultPos;
}

void ChargeCursor::update(IChunk* level, const BlockPos& originPos,
                          XoroshiroRandomSource& random, SculkSpreader& spreader, bool spreadVeins) {
    // Reference: lines 247-287
    if (m_charge <= 0) return;
    if (!spreader.isWorldGeneration()) return; // Only world gen in our case

    if (m_updateDelay > 0) {
        --m_updateDelay;
        return;
    }

    BlockState* currentState = level->getBlockState(m_pos);
    if (!currentState) return;

    // Attempt spread vein
    if (spreadVeins) {
        if (SculkBehaviour::attemptSpreadVein(level, m_pos, currentState, getFacingData(), spreader.isWorldGeneration())) {
            if (SculkBehaviour::canChangeBlockStateOnSpread(currentState)) {
                currentState = level->getBlockState(m_pos);
            }
        }
    }

    // Attempt use charge
    m_charge = SculkBehaviour::attemptUseCharge(*this, level, originPos, random, spreader, spreadVeins);

    if (m_charge <= 0) {
        SculkBehaviour::onDischarged(level, currentState, m_pos, random);
    } else {
        BlockPos* transferPos = getValidMovementPos(level, m_pos, random);
        if (transferPos != nullptr) {
            SculkBehaviour::onDischarged(level, currentState, m_pos, random);
            m_pos = *transferPos;

            // Check distance constraint for world gen
            if (spreader.isWorldGeneration()) {
                int dx = m_pos.getX() - originPos.getX();
                int dz = m_pos.getZ() - originPos.getZ();
                double dist = std::sqrt(dx * dx + dz * dz);
                if (dist > 15.0) {
                    m_charge = 0;
                    return;
                }
            }

            currentState = level->getBlockState(m_pos);
        }

        if (currentState && SculkBehaviour::isSculkBehaviour(currentState)) {
            // Update facings - simplified
            m_facings = std::set<Direction>{Direction::UP, Direction::DOWN, Direction::NORTH,
                                            Direction::SOUTH, Direction::EAST, Direction::WEST};
        }

        m_decayDelay = SculkBehaviour::updateDecayDelay(m_decayDelay);
        m_updateDelay = SculkBehaviour::getSculkSpreadDelay();
    }
}

void ChargeCursor::mergeWith(ChargeCursor& other) {
    // Reference: lines 290-294
    m_charge += other.m_charge;
    other.m_charge = 0;
    m_updateDelay = std::min(m_updateDelay, other.m_updateDelay);
}

// ============================================================================
// SculkSpreader implementation
// ============================================================================

SculkSpreader SculkSpreader::createWorldGenSpreader() {
    // Reference: line 67-68
    // new SculkSpreader(true, BlockTags.SCULK_REPLACEABLE_WORLD_GEN, 50, 1, 5, 10)
    return SculkSpreader(true, 50, 1, 5, 10);
}

SculkSpreader SculkSpreader::createLevelSpreader() {
    // Reference: line 63-64
    // new SculkSpreader(false, BlockTags.SCULK_REPLACEABLE, 10, 4, 10, 5)
    return SculkSpreader(false, 10, 4, 10, 5);
}

SculkSpreader::SculkSpreader(bool isWorldGeneration, int growthSpawnCost, int noGrowthRadius,
                             int chargeDecayRate, int additionalDecayRate)
    : m_isWorldGeneration(isWorldGeneration)
    , m_growthSpawnCost(growthSpawnCost)
    , m_noGrowthRadius(noGrowthRadius)
    , m_chargeDecayRate(chargeDecayRate)
    , m_additionalDecayRate(additionalDecayRate)
{
}

void SculkSpreader::clear() {
    m_cursors.clear();

    // Clear auxiliary vein face tracking
    clearVeinFaces();

    // Print debug counters
    if (s_attemptUseChargeCount > 0) {
        std::cerr << "[SculkDebug] attemptUseCharge=" << s_attemptUseChargeCount
                  << " enteredMainLogic=" << s_enteredMainLogic
                  << " canPlaceGrowthTrue=" << s_canPlaceGrowthTrue
                  << " growthPlaced=" << s_growthPlaced
                  << " canPlaceChecks=" << s_canPlaceGrowthChecks
                  << " canPlaceFailAbove=" << s_canPlaceGrowthFailAbove
                  << " canPlacePass=" << s_canPlaceGrowthPass
                  << " veinToAir=" << s_veinToAirCount << std::endl;
        // Reset counters
        s_attemptUseChargeCount = 0;
        s_enteredMainLogic = 0;
        s_canPlaceGrowthTrue = 0;
        s_growthPlaced = 0;
        s_canPlaceGrowthChecks = 0;
        s_canPlaceGrowthFailAbove = 0;
        s_canPlaceGrowthPass = 0;
        s_veinToAirCount = 0;
    }
}

void SculkSpreader::addCursor(ChargeCursor cursor) {
    // Reference: lines 132-136
    if (m_cursors.size() < MAX_CURSORS) {
        m_cursors.push_back(std::move(cursor));
    }
}

void SculkSpreader::addCursors(const BlockPos& startPos, int charge) {
    // Reference: lines 123-129
    while (charge > 0) {
        int currentCharge = std::min(charge, MAX_CHARGE);
        addCursor(ChargeCursor(startPos, currentCharge));
        charge -= currentCharge;
    }
}

void SculkSpreader::updateCursors(IChunk* level, const BlockPos& originPos,
                                   XoroshiroRandomSource& random, bool spreadVeins) {
    // Reference: lines 138-184
    if (m_cursors.empty()) return;

    std::vector<ChargeCursor> processedCursors;
    std::unordered_map<int64_t, ChargeCursor*> mergeableCursors;

    auto posToKey = [](const BlockPos& pos) -> int64_t {
        return (static_cast<int64_t>(pos.getX()) & 0x3FFFFFF) |
               ((static_cast<int64_t>(pos.getY()) & 0xFFF) << 26) |
               ((static_cast<int64_t>(pos.getZ()) & 0x3FFFFFF) << 38);
    };

    for (ChargeCursor& cursor : m_cursors) {
        if (cursor.isPosUnreasonable(originPos)) continue;

        cursor.update(level, originPos, random, *this, spreadVeins);

        if (cursor.getCharge() <= 0) {
            // Cursor exhausted
            continue;
        }

        int64_t key = posToKey(cursor.getPos());
        auto it = mergeableCursors.find(key);

        if (it == mergeableCursors.end()) {
            processedCursors.push_back(cursor);
            mergeableCursors[key] = &processedCursors.back();
        } else if (!m_isWorldGeneration && cursor.getCharge() + it->second->getCharge() <= MAX_CHARGE) {
            it->second->mergeWith(cursor);
        } else {
            processedCursors.push_back(cursor);
            if (cursor.getCharge() < it->second->getCharge()) {
                mergeableCursors[key] = &processedCursors.back();
            }
        }
    }

    m_cursors = std::move(processedCursors);
}

// ============================================================================
// SculkBehaviour namespace implementation
// ============================================================================

namespace SculkBehaviour {

bool isSculkBehaviour(BlockState* state) {
    // Reference: Only SculkBlock and SculkVeinBlock implement SculkBehaviour in Java!
    // SculkCatalystBlock, SculkSensorBlock, SculkShriekerBlock do NOT implement it.
    // This affects cursor movement - cursors can only move to blocks that implement SculkBehaviour
    if (!state) return false;
    const std::string& id = state->getIdentifier();
    return id == "minecraft:sculk" ||
           id == "minecraft:sculk_vein";
    // NOTE: sculk_catalyst, sculk_sensor, sculk_shrieker are NOT SculkBehaviour!
}

bool canChangeBlockStateOnSpread(BlockState* state) {
    if (!state) return true;
    // SculkBlock returns false, others return true
    return state->getIdentifier() != "minecraft:sculk";
}

int getSculkSpreadDelay() {
    return 1;
}

int updateDecayDelay(int age) {
    // Default behavior: return 1
    // SculkBehaviour.DEFAULT: Math.max(age - 1, 0)
    return std::max(age - 1, 0);
}

void onDischarged(IChunk* level, BlockState* state, const BlockPos& pos,
                  XoroshiroRandomSource& random) {
    // Reference: SculkVeinBlock.onDischarged() lines 69-85
    // For sculk_vein: remove faces pointing to sculk, turn to air if empty
    if (!state) return;

    if (state->getIdentifier() == "minecraft:sculk_vein") {
        // Check each direction - if adjacent block is sculk, remove that face
        std::set<Direction> facesToRemove;
        static const Direction allDirs[] = {
            Direction::DOWN, Direction::UP, Direction::NORTH,
            Direction::SOUTH, Direction::WEST, Direction::EAST
        };

        for (Direction dir : allDirs) {
            bool hasFace = SculkVeinBlock::hasFace(state, dir);
            if (!hasFace) {
                // Also check auxiliary tracking
                hasFace = hasVeinFace(pos, dir);
            }

            if (hasFace) {
                BlockPos adjacentPos = pos.relative(dir);
                BlockState* adjacentState = level->getBlockState(adjacentPos);
                if (adjacentState && adjacentState->getIdentifier() == "minecraft:sculk") {
                    facesToRemove.insert(dir);
                }
            }
        }

        // Remove faces and update state
        BlockState* newState = state;
        for (Direction dir : facesToRemove) {
            // Remove from block state property
            BooleanProperty* prop = SculkVeinBlock::getFaceProperty(dir);
            if (prop && newState->hasProperty(prop)) {
                newState = newState->setValue(*prop, false);
            }
            // Also remove from auxiliary tracking
            auto it = s_veinFaces.find(packPos(pos));
            if (it != s_veinFaces.end()) {
                it->second.erase(dir);
            }
        }

        // Check if any faces remain
        bool hasAnyFace = false;
        for (Direction dir : allDirs) {
            if (SculkVeinBlock::hasFace(newState, dir) || hasVeinFace(pos, dir)) {
                hasAnyFace = true;
                break;
            }
        }

        // Update the block
        int localX = pos.getX() & 15;
        int localZ = pos.getZ() & 15;
        if (!hasAnyFace) {
            // No faces left - replace with air (or water if was waterlogged)
            // Reference: SculkVeinBlock.java lines 78-80
            level->setBlockState(localX, pos.getY(), localZ,
                static_cast<BlockState*>(Blocks::getDefaultState("minecraft:air")),
                false);
            // Clear auxiliary tracking
            s_veinFaces.erase(packPos(pos));
            s_veinToAirCount++;
        } else if (newState != state) {
            // Update with modified state
            level->setBlockState(localX, pos.getY(), localZ, newState, false);
        }
    }
}

// ============================================================================
// MultifaceSpreader implementation for sculk vein spreading
// Reference: MultifaceSpreader.java, SculkVeinBlock.java
// ============================================================================

// SculkVeinSpreaderConfig.stateCanBeReplaced() - Reference: SculkVeinBlock.java lines 153-173
static bool stateCanBeReplaced(IChunk* level, const BlockPos& sourcePos,
                               const BlockPos& placementPos, Direction placementDirection,
                               BlockState* existingState) {
    if (!existingState) return false;

    // Get the block we would attach to
    BlockPos againstPos = placementPos.relative(placementDirection);
    BlockState* againstState = level->getBlockState(againstPos);

    if (againstState) {
        const std::string& againstId = againstState->getIdentifier();
        // Cannot place if attaching to sculk, sculk_catalyst, or moving_piston
        if (againstId == "minecraft:sculk" ||
            againstId == "minecraft:sculk_catalyst" ||
            againstId == "minecraft:moving_piston") {
            return false;
        }
    }

    // If Manhattan distance is 2, check for sturdy face blocking
    // Reference: lines 156-160
    int manhattanDist = std::abs(sourcePos.getX() - placementPos.getX()) +
                        std::abs(sourcePos.getY() - placementPos.getY()) +
                        std::abs(sourcePos.getZ() - placementPos.getZ());
    if (manhattanDist == 2) {
        BlockPos neighbourPos = sourcePos.relative(getOpposite(placementDirection));
        BlockState* neighbourState = level->getBlockState(neighbourPos);
        // Simplified: check if neighbour is a solid block (isFaceSturdy)
        if (neighbourState && !neighbourState->isAir() &&
            neighbourState->getIdentifier() != "minecraft:water") {
            return false;
        }
    }

    const std::string& existingId = existingState->getIdentifier();

    // Check fluid state - only water allowed
    // Reference: lines 163-165
    if (existingId != "minecraft:water" && existingId != "minecraft:air" &&
        existingId != "minecraft:sculk_vein" && existingId != "minecraft:cave_air") {
        // Non-replaceable block
        return false;
    }

    // Fire blocks cannot be replaced
    // Reference: lines 166-167
    if (existingId == "minecraft:fire" || existingId == "minecraft:soul_fire") {
        return false;
    }

    // existingState.canBeReplaced() - air, water, sculk_vein can be replaced
    // Reference: line 169
    return existingState->isAir() ||
           existingId == "minecraft:water" ||
           existingId == "minecraft:sculk_vein" ||
           existingId == "minecraft:cave_air";
}

// Check if vein can be placed (block has sturdy face to attach to)
// Reference: MultifaceBlock.canAttachTo()
static bool canAttachTo(IChunk* level, const BlockPos& pos, Direction attachDirection) {
    BlockPos attachPos = pos.relative(attachDirection);
    BlockState* attachState = level->getBlockState(attachPos);

    if (!attachState) return false;
    if (attachState->isAir()) return false;

    const std::string& id = attachState->getIdentifier();
    if (id == "minecraft:water") return false;

    // Simplified: any solid block can have veins attach to it
    // (In Java, this checks isFaceFull of the collision/support shape)
    return true;
}

// canSpreadInto - Reference: MultifaceSpreader.DefaultSpreaderConfig.canSpreadInto()
static bool canSpreadInto(IChunk* level, const BlockPos& sourcePos,
                          const BlockPos& placementPos, Direction placementDirection) {
    BlockState* existingState = level->getBlockState(placementPos);
    if (!existingState) return false;

    // Check stateCanBeReplaced and isValidStateForPlacement
    return stateCanBeReplaced(level, sourcePos, placementPos, placementDirection, existingState) &&
           canAttachTo(level, placementPos, placementDirection);
}

// getSpreadFromFaceTowardDirection - Reference: MultifaceSpreader.java lines 57-71
// Returns true if found a valid spread position, fills outPos and outFace
static bool getSpreadFromFaceTowardDirection(IChunk* level, const BlockPos& pos, BlockState* state,
                                             Direction fromFace, Direction spreadDir,
                                             bool isOtherBlockSource,
                                             BlockPos& outPos, Direction& outFace) {
    // Can't spread along same axis
    if (getAxis(spreadDir) == getAxis(fromFace)) {
        return false;
    }

    // Check if this source can spread from this face
    // Reference: line 60 - isOtherBlockValidAsSource OR (hasFace AND !hasFace)
    if (!isOtherBlockSource) {
        // For sculk_vein, would need to check hasFace - simplified: skip
        return false;
    }

    // Try each spread type in order: SAME_POSITION, SAME_PLANE, WRAP_AROUND
    // Reference: MultifaceSpreader.SpreadType enum lines 142-157

    // SAME_POSITION: SpreadPos(pos, spreadDir)
    if (canSpreadInto(level, pos, pos, spreadDir)) {
        outPos = pos;
        outFace = spreadDir;
        return true;
    }

    // SAME_PLANE: SpreadPos(pos.relative(spreadDir), fromFace)
    BlockPos samePlanePos = pos.relative(spreadDir);
    if (canSpreadInto(level, pos, samePlanePos, fromFace)) {
        outPos = samePlanePos;
        outFace = fromFace;
        return true;
    }

    // WRAP_AROUND: SpreadPos(pos.relative(spreadDir).relative(fromFace), spreadDir.getOpposite())
    BlockPos wrapPos = pos.relative(spreadDir).relative(fromFace);
    Direction wrapFace = getOpposite(spreadDir);
    if (canSpreadInto(level, pos, wrapPos, wrapFace)) {
        outPos = wrapPos;
        outFace = wrapFace;
        return true;
    }

    return false;
}

// spreadToFace - Actually place the vein block
// Reference: MultifaceSpreader.java lines 74-77
static bool spreadToFace(IChunk* level, const BlockPos& pos, Direction face, bool isWorldGen) {
    int localX = pos.getX() & 15;
    int localZ = pos.getZ() & 15;

    // Get existing block state at position
    BlockState* existingState = level->getBlockState(pos);
    BlockState* newState = nullptr;

    // Reference: MultifaceBlock.getStateForPlacement()
    // If there's already a sculk_vein at this position, add the new face to it
    // Otherwise, create a new state with just this face
    if (existingState && existingState->getIdentifier() == "minecraft:sculk_vein") {
        // Add face to existing vein state
        newState = SculkVeinBlock::getStateWithFace(existingState, face);
    } else {
        // Create new vein with just this face
        BlockState* defaultState = Blocks::SCULK_VEIN->defaultBlockState();
        newState = SculkVeinBlock::getStateWithFace(defaultState, face);
    }

    if (!newState) {
        return false;
    }

    // Place the vein block
    level->setBlockState(localX, pos.getY(), localZ, newState, false);

    // Also track in auxiliary map for consistency during this spreading operation
    addVeinFace(pos, face);

    return true;
}

bool attemptSpreadVein(IChunk* level, const BlockPos& pos, BlockState* state,
                      const std::set<Direction>* facings, bool isWorldGen) {
    // Reference: SculkBehaviour.DEFAULT.attemptSpreadVein() - SculkBehaviour.java lines 14-21
    // Reference: MultifaceSpreader.spreadAll() - MultifaceSpreader.java lines 38-40

    if (!state) return false;

    const std::string& currentBlockId = state->getIdentifier();
    bool isOtherBlockSource = (currentBlockId != "minecraft:sculk_vein");

    long count = 0;

    // spreadAll: for each face direction, try to spread toward all perpendicular directions
    // Reference: MultifaceSpreader.java line 39
    for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
        Direction fromFace = static_cast<Direction>(faceIdx);

        // canSpreadFrom: true if isOtherBlockValidAsSource OR hasFace(state, fromFace)
        // Reference: MultifaceSpreader.SpreadConfig.canSpreadFrom() lines 103-105
        if (!isOtherBlockSource) {
            // For sculk_vein, check if it has this face using proper block state
            // Reference: MultifaceBlock.hasFace() - checks if block has a face property set
            bool hasFaceFromState = SculkVeinBlock::hasFace(state, fromFace);
            // Also check auxiliary tracking for faces added during this spreading operation
            bool hasFaceFromTracking = facings && facings->find(fromFace) != facings->end();
            if (!hasFaceFromState && !hasFaceFromTracking) {
                continue;
            }
        }

        // spreadFromFaceTowardAllDirections - Reference: lines 46-48
        for (int spreadIdx = 0; spreadIdx < 6; ++spreadIdx) {
            Direction spreadDir = static_cast<Direction>(spreadIdx);

            BlockPos outPos;
            Direction outFace;
            // Pass true for isValidSource since we already verified canSpreadFrom in outer loop
            if (getSpreadFromFaceTowardDirection(level, pos, state, fromFace, spreadDir,
                                                 true, outPos, outFace)) {
                if (spreadToFace(level, outPos, outFace, isWorldGen)) {
                    ++count;
                }
            }
        }
    }

    return count > 0;
}

// ============================================================================
// SculkVeinBlock.attemptPlaceSculk() - Converts adjacent blocks to sculk
// Reference: SculkVeinBlock.java lines 96-128
// ============================================================================

// Check if a block is in SCULK_REPLACEABLE_WORLD_GEN tag
static bool isSculkReplaceable(const std::string& id) {
    return id == "minecraft:stone" || id == "minecraft:deepslate" ||
           id == "minecraft:granite" || id == "minecraft:diorite" ||
           id == "minecraft:andesite" || id == "minecraft:tuff" ||
           id == "minecraft:calcite" || id == "minecraft:smooth_basalt" ||
           id == "minecraft:dripstone_block" || id == "minecraft:dirt" ||
           id == "minecraft:gravel" || id == "minecraft:sand" ||
           id == "minecraft:clay" || id == "minecraft:mud" ||
           id == "minecraft:terracotta" || id.find("_terracotta") != std::string::npos ||
           id == "minecraft:sandstone" || id == "minecraft:red_sandstone";
}

// Shuffle 6 directions using random (Fisher-Yates)
static std::vector<Direction> allShuffled(XoroshiroRandomSource& random) {
    std::vector<Direction> dirs = {Direction::DOWN, Direction::UP, Direction::NORTH,
                                   Direction::SOUTH, Direction::WEST, Direction::EAST};
    for (int i = 5; i > 0; --i) {
        int j = random.nextInt(i + 1);
        std::swap(dirs[i], dirs[j]);
    }
    return dirs;
}

// SculkVeinBlock.attemptPlaceSculk() - Reference: lines 96-128
static bool attemptPlaceSculk(IChunk* level, const BlockPos& pos, XoroshiroRandomSource& random,
                              bool isWorldGen) {
    BlockState* state = level->getBlockState(pos);
    if (!state || state->getIdentifier() != "minecraft:sculk_vein") {
        return false;
    }

    // For each shuffled direction, check if vein has face there and can place sculk
    // Reference: SculkVeinBlock.java lines 100-101
    for (Direction support : allShuffled(random)) {
        // hasFace() check: Only process directions where the vein has a face
        // Reference: MultifaceBlock.hasFace(state, direction)
        // First try block state property, fall back to auxiliary tracking map
        bool hasFace = SculkVeinBlock::hasFace(state, support);
        if (!hasFace) {
            // Fall back to auxiliary face tracking (in case block state properties don't work)
            hasFace = hasVeinFace(pos, support);
        }

        if (!hasFace) {
            continue;
        }

        // Check if we can place sculk in this direction
        // Reference: SculkVeinBlock.java lines 103-105
        BlockPos supportPos = pos.relative(support);
        BlockState* supportState = level->getBlockState(supportPos);
        if (supportState) {
            const std::string& supportId = supportState->getIdentifier();
            if (isSculkReplaceable(supportId)) {
                // Place sculk block
                // Reference: SculkVeinBlock.java lines 107-109
                int localX = supportPos.getX() & 15;
                int localZ = supportPos.getZ() & 15;
                level->setBlockState(localX, supportPos.getY(), localZ,
                    static_cast<BlockState*>(Blocks::getDefaultState("minecraft:sculk")),
                    false);

                // Reference: SculkVeinBlock.java line 109
                // CRITICAL: Spread veins FROM the new sculk block!
                // This places veins in air adjacent to sculk, which then get
                // discharged and converted to air, creating air above sculk.
                BlockState* sculkState = level->getBlockState(supportPos);
                if (sculkState) {
                    // Spread veins using sculk as source (isOtherBlockSource=true)
                    SculkBehaviour::attemptSpreadVein(level, supportPos, sculkState, nullptr, isWorldGen);
                }

                // Reference: SculkVeinBlock.java lines 112-120
                // Call onDischarged() on adjacent veins (except the direction we came from)
                // This removes face properties pointing to the new sculk and
                // turns veins to air if they have no faces left
                Direction skip = getOpposite(support);
                static const Direction allDirs[] = {
                    Direction::DOWN, Direction::UP, Direction::NORTH,
                    Direction::SOUTH, Direction::WEST, Direction::EAST
                };
                for (Direction veinDir : allDirs) {
                    if (veinDir == skip) continue;

                    BlockPos veinPos = supportPos.relative(veinDir);
                    BlockState* possibleVein = level->getBlockState(veinPos);
                    if (possibleVein && possibleVein->getIdentifier() == "minecraft:sculk_vein") {
                        SculkBehaviour::onDischarged(level, possibleVein, veinPos, random);
                    }
                }

                return true;
            }
        }
    }

    return false;
}

// Helper function to check if growth can be placed
// Reference: SculkBlock.canPlaceGrowth() lines 69-89
static bool canPlaceGrowth(IChunk* level, const BlockPos& pos) {
    s_canPlaceGrowthChecks++;
    BlockPos above = pos.above();
    BlockState* stateAbove = level->getBlockState(above);

    // Reference: line 70-71 - stateAbove.isAir() || (stateAbove.is(Blocks.WATER) && stateAbove.getFluidState().is(Fluids.WATER))
    if (!stateAbove) {
        s_canPlaceGrowthFailAbove++;
        return false;
    }

    bool isAir = stateAbove->isAir();
    bool isWater = stateAbove->getIdentifier() == "minecraft:water";

    if (!isAir && !isWater) {
        s_canPlaceGrowthFailAbove++;
        return false;
    }

    // Check for existing sensors/shriekers in a 9x3x9 area
    int growthCount = 0;
    for (int dx = -4; dx <= 4; ++dx) {
        for (int dy = 0; dy <= 2; ++dy) {
            for (int dz = -4; dz <= 4; ++dz) {
                BlockPos checkPos = pos.offset(dx, dy, dz);
                BlockState* state = level->getBlockState(checkPos);
                if (state) {
                    const std::string& id = state->getIdentifier();
                    if (id == "minecraft:sculk_sensor" || id == "minecraft:sculk_shrieker") {
                        ++growthCount;
                        if (growthCount > 2) {
                            return false;
                        }
                    }
                }
            }
        }
    }

    s_canPlaceGrowthPass++;
    return true;
}

int attemptUseCharge(ChargeCursor& cursor, IChunk* level, const BlockPos& originPos,
                     XoroshiroRandomSource& random, SculkSpreader& spreader, bool spreadVeins) {
    int charge = cursor.getCharge();
    s_attemptUseChargeCount++;

    // Get the block at cursor position to determine which behaviour to use
    // Reference: SculkSpreader.getBlockBehaviour() - only SculkBlock and SculkVeinBlock
    // implement SculkBehaviour; all other blocks use DEFAULT
    BlockState* cursorState = level->getBlockState(cursor.getPos());
    if (!cursorState) {
        // DEFAULT behaviour - return 0 if decayDelay <= 0
        return cursor.getDecayDelay() > 0 ? charge : 0;
    }

    const std::string& blockId = cursorState->getIdentifier();

    // =========================================================================
    // SCULK_VEIN BLOCK - SculkVeinBlock.attemptUseCharge()
    // Reference: SculkVeinBlock.java lines 88-94
    // =========================================================================
    if (blockId == "minecraft:sculk_vein") {
        if (spreadVeins && attemptPlaceSculk(level, cursor.getPos(), random, spreader.isWorldGeneration())) {
            return charge - 1;
        } else {
            // Decay check
            if (random.nextInt(spreader.chargeDecayRate()) == 0) {
                return static_cast<int>(std::floor(static_cast<float>(charge) * 0.5f));
            }
            return charge;
        }
    }

    // =========================================================================
    // SCULK BLOCK - SculkBlock.attemptUseCharge()
    // Reference: SculkBlock.java lines 27-47
    // Only minecraft:sculk uses this logic!
    // =========================================================================
    if (blockId == "minecraft:sculk") {
        // Reference: line 29 - if (charge != 0 && random.nextInt(spreader.chargeDecayRate()) == 0)
        if (charge != 0 && random.nextInt(spreader.chargeDecayRate()) == 0) {
        s_enteredMainLogic++;
        const BlockPos& chargePos = cursor.getPos();

        // Check distance from catalyst - Reference: line 31
        // Java uses closerThan which is: distSqr < radius²  (STRICTLY LESS THAN)
        int dx = chargePos.getX() - originPos.getX();
        int dy = chargePos.getY() - originPos.getY();
        int dz = chargePos.getZ() - originPos.getZ();
        double distSqr = static_cast<double>(dx*dx + dy*dy + dz*dz);
        double radiusSqr = static_cast<double>(spreader.noGrowthRadius()) * static_cast<double>(spreader.noGrowthRadius());
        bool isCloseToCatalyst = distSqr < radiusSqr;

        // Reference: line 32 - if (!isCloseToCatalyst && canPlaceGrowth(level, chargePos))
        if (!isCloseToCatalyst && canPlaceGrowth(level, chargePos)) {
            s_canPlaceGrowthTrue++;
            int xpPerGrowthSpawn = spreader.growthSpawnCost();

            // Reference: line 34 - if (random.nextInt(xpPerGrowthSpawn) < charge)
            if (random.nextInt(xpPerGrowthSpawn) < charge) {
                s_growthPlaced++;
                BlockPos growthPos = chargePos.above();

                // Get random growth state - Reference: getRandomGrowthState() lines 58-67
                // 1/11 chance for shrieker, otherwise sensor
                int localX = growthPos.getX() & 15;
                int localZ = growthPos.getZ() & 15;

                if (random.nextInt(SculkSpreader::SHRIEKER_PLACEMENT_RATE) == 0) {
                    // SCULK_SHRIEKER with CAN_SUMMON = true for world gen
                    level->setBlockState(localX, growthPos.getY(), localZ,
                        static_cast<BlockState*>(Blocks::getDefaultState("minecraft:sculk_shrieker")),
                        false);
                } else {
                    // SCULK_SENSOR
                    level->setBlockState(localX, growthPos.getY(), localZ,
                        static_cast<BlockState*>(Blocks::getDefaultState("minecraft:sculk_sensor")),
                        false);
                }
            }

            // Reference: line 41 - return Math.max(0, charge - xpPerGrowthSpawn);
            // This ALWAYS happens if canPlaceGrowth was true, regardless of whether growth was placed
            return std::max(0, charge - xpPerGrowthSpawn);
        } else {
            // Reference: lines 42-43 - decay path
            if (random.nextInt(spreader.additionalDecayRate()) != 0) {
                return charge;
            }

            if (isCloseToCatalyst) {
                return charge - 1;
            }

            // getDecayPenalty - Reference: SculkBlock.java lines 50-55
            // Java: float outerDistanceSquared = Mth.square((float)Math.sqrt(pos.distSqr(originPos)) - (float)noGrowthRadius);
            //       int maxReachSquared = Mth.square(24 - noGrowthRadius);
            //       float distanceFactor = Math.min(1.0F, outerDistanceSquared / (float)maxReachSquared);
            //       return Math.max(1, (int)((float)charge * distanceFactor * 0.5F));
            int noGrowthRadius = spreader.noGrowthRadius();
            float dist = static_cast<float>(std::sqrt(static_cast<double>(dx*dx + dy*dy + dz*dz)));
            float outerDistanceSquared = (dist - static_cast<float>(noGrowthRadius)) * (dist - static_cast<float>(noGrowthRadius));
            int maxReachSquared = (24 - noGrowthRadius) * (24 - noGrowthRadius);
            float distanceFactor = std::min(1.0f, outerDistanceSquared / static_cast<float>(maxReachSquared));
            int penalty = std::max(1, static_cast<int>(static_cast<float>(charge) * distanceFactor * 0.5f));

            return charge - penalty;
        }
        }

        // Reference: line 46 - return charge (if decay check at line 29 fails)
        return charge;
    }

    // =========================================================================
    // DEFAULT BEHAVIOUR - for sculk_catalyst, sculk_sensor, sculk_shrieker, etc.
    // Reference: SculkBehaviour.java DEFAULT anonymous class lines 24-26
    // These blocks do NOT implement SculkBehaviour, so they use DEFAULT
    // DEFAULT has NO random consumption!
    // =========================================================================
    return cursor.getDecayDelay() > 0 ? charge : 0;
}

} // namespace SculkBehaviour

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
