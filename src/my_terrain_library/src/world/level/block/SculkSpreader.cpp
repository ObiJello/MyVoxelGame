#include "world/level/block/SculkSpreader.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/WorldGenLevel.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/SculkBlock.h"
#include "world/level/block/blocks/SculkVeinBlock.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

// Reference: net/minecraft/world/level/block/SculkSpreader.java

namespace minecraft {
namespace world {
namespace level {
namespace block {

using namespace state;
using ::minecraft::core::BlockPos;
using ::minecraft::core::Direction;
using ::minecraft::core::Vec3i;
using ::minecraft::levelgen::WorldGenLevel;
using ::minecraft::levelgen::WorldgenRandom;

namespace {

static std::vector<Vec3i> initNonCornerNeighbours() {
    std::vector<Vec3i> result;
    result.reserve(18);

    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0 && z == 0) {
                    continue;
                }

                if (x == 0 || y == 0 || z == 0) {
                    result.emplace_back(x, y, z);
                }
            }
        }
    }

    return result;
}

static const std::vector<Vec3i> s_nonCornerNeighbours = initNonCornerNeighbours();

static const std::array<Direction, 6> s_directions = {
    Direction::DOWN, Direction::UP, Direction::NORTH,
    Direction::SOUTH, Direction::WEST, Direction::EAST
};

static bool isWaterSource(BlockState* state) {
    return state && state->getIdentifier() == "minecraft:water";
}

static bool isAirOrWater(BlockState* state) {
    return state && (state->isAir() || isWaterSource(state));
}

static bool canBeReplaced(BlockState* state) {
    if (!state) {
        return false;
    }

    return state->canBeReplaced();
}

static bool isSculkReplaceable(const std::string& id) {
    return id == "minecraft:stone" ||
           id == "minecraft:granite" ||
           id == "minecraft:diorite" ||
           id == "minecraft:andesite" ||
           id == "minecraft:tuff" ||
           id == "minecraft:deepslate" ||
           id == "minecraft:dirt" ||
           id == "minecraft:grass_block" ||
           id == "minecraft:podzol" ||
           id == "minecraft:coarse_dirt" ||
           id == "minecraft:mycelium" ||
           id == "minecraft:rooted_dirt" ||
           id == "minecraft:terracotta" ||
           id == "minecraft:white_terracotta" ||
           id == "minecraft:orange_terracotta" ||
           id == "minecraft:magenta_terracotta" ||
           id == "minecraft:light_blue_terracotta" ||
           id == "minecraft:yellow_terracotta" ||
           id == "minecraft:lime_terracotta" ||
           id == "minecraft:pink_terracotta" ||
           id == "minecraft:gray_terracotta" ||
           id == "minecraft:light_gray_terracotta" ||
           id == "minecraft:cyan_terracotta" ||
           id == "minecraft:purple_terracotta" ||
           id == "minecraft:blue_terracotta" ||
           id == "minecraft:brown_terracotta" ||
           id == "minecraft:green_terracotta" ||
           id == "minecraft:red_terracotta" ||
           id == "minecraft:black_terracotta" ||
           id == "minecraft:crimson_nylium" ||
           id == "minecraft:warped_nylium" ||
           id == "minecraft:netherrack" ||
           id == "minecraft:basalt" ||
           id == "minecraft:blackstone" ||
           id == "minecraft:sand" ||
           id == "minecraft:red_sand" ||
           id == "minecraft:gravel" ||
           id == "minecraft:soul_sand" ||
           id == "minecraft:soul_soil" ||
           id == "minecraft:calcite" ||
           id == "minecraft:smooth_basalt" ||
           id == "minecraft:clay" ||
           id == "minecraft:dripstone_block" ||
           id == "minecraft:end_stone" ||
           id == "minecraft:red_sandstone" ||
           id == "minecraft:sandstone";
}

static bool isSculkReplaceableWorldGen(const std::string& id) {
    return isSculkReplaceable(id) ||
           id == "minecraft:deepslate_bricks" ||
           id == "minecraft:deepslate_tiles" ||
           id == "minecraft:cobbled_deepslate" ||
           id == "minecraft:cracked_deepslate_bricks" ||
           id == "minecraft:cracked_deepslate_tiles" ||
           id == "minecraft:polished_deepslate";
}

static bool isReplaceableForSpreader(const SculkSpreader& spreader, const std::string& id) {
    return spreader.replaceableBlocks() == SculkSpreader::ReplaceableBlocks::SCULK_REPLACEABLE_WORLD_GEN
        ? isSculkReplaceableWorldGen(id)
        : isSculkReplaceable(id);
}

struct SpreadPos {
    BlockPos pos;
    Direction face;
};

enum class SpreadType {
    SAME_POSITION,
    SAME_PLANE,
    WRAP_AROUND,
};

static const std::array<SpreadType, 3> s_defaultSpreadTypes = {
    SpreadType::SAME_POSITION,
    SpreadType::SAME_PLANE,
    SpreadType::WRAP_AROUND,
};

static const std::array<SpreadType, 1> s_samePositionSpreadTypes = {
    SpreadType::SAME_POSITION,
};

static SpreadPos getSpreadPos(const BlockPos& pos, Direction spreadDirection, Direction fromFace, SpreadType type) {
    switch (type) {
        case SpreadType::SAME_POSITION:
            return {pos, spreadDirection};
        case SpreadType::SAME_PLANE:
            return {pos.relative(spreadDirection), fromFace};
        case SpreadType::WRAP_AROUND:
            return {pos.relative(spreadDirection).relative(fromFace), getOpposite(spreadDirection)};
    }

    return {pos, spreadDirection};
}

static bool stateCanBeReplaced(
    WorldGenLevel* level,
    const BlockPos& sourcePos,
    const BlockPos& placementPos,
    Direction placementDirection,
    BlockState* existingState
) {
    if (!existingState) {
        return false;
    }

    BlockState* againstState = level->getBlockState(placementPos.relative(placementDirection));
    if (againstState) {
        const std::string againstId = againstState->getIdentifier();
        if (againstId == "minecraft:sculk" || againstId == "minecraft:sculk_catalyst" || againstId == "minecraft:moving_piston") {
            return false;
        }
    }

    auto defaultStateCanBeReplaced = [](BlockState* state) {
        if (!state) {
            return false;
        }

        const std::string& id = state->getIdentifier();
        return state->isAir() ||
               id == "minecraft:sculk_vein" ||
               (id == "minecraft:water" && isWaterSource(state));
    };

    int manhattanDistance =
        std::abs(sourcePos.getX() - placementPos.getX()) +
        std::abs(sourcePos.getY() - placementPos.getY()) +
        std::abs(sourcePos.getZ() - placementPos.getZ());
    if (manhattanDistance == 2) {
        BlockPos neighbourPos = sourcePos.relative(getOpposite(placementDirection));
        BlockState* neighbourState = level->getBlockState(neighbourPos);
        if (neighbourState && neighbourState->isFaceSturdy(*level, neighbourPos, placementDirection)) {
            return false;
        }
    }

    const std::string& existingId = existingState->getIdentifier();

    if (existingId == "minecraft:water" && !isWaterSource(existingState)) {
        return false;
    }

    if (existingId != "minecraft:air" && existingId != "minecraft:water" && existingId != "minecraft:cave_air" && !existingState->canBeReplaced() && existingId != "minecraft:sculk_vein") {
        return false;
    }

    if (existingId == "minecraft:fire" || existingId == "minecraft:soul_fire") {
        return false;
    }

    return canBeReplaced(existingState) || defaultStateCanBeReplaced(existingState);
}

template <size_t N>
static bool getSpreadFromFaceTowardDirection(
    WorldGenLevel* level,
    BlockState* state,
    const BlockPos& pos,
    Direction startingFace,
    Direction spreadDirection,
    bool isOtherBlockSource,
    const std::array<SpreadType, N>& spreadTypes,
    SpreadPos& outSpreadPos
) {
    if (getAxis(spreadDirection) == getAxis(startingFace)) {
        return false;
    }

    if (!(isOtherBlockSource || (SculkVeinBlock::hasFace(state, startingFace) && !SculkVeinBlock::hasFace(state, spreadDirection)))) {
        return false;
    }

    for (SpreadType type : spreadTypes) {
        SpreadPos spreadPos = getSpreadPos(pos, spreadDirection, startingFace, type);
        BlockState* existingState = level->getBlockState(spreadPos.pos);
        if (stateCanBeReplaced(level, pos, spreadPos.pos, spreadPos.face, existingState) &&
            SculkVeinBlock::canAttachTo(*level, spreadPos.pos, spreadPos.face)) {
            outSpreadPos = spreadPos;
            return true;
        }
    }

    return false;
}

static bool spreadToFace(WorldGenLevel* level, const SpreadPos& spreadPos, bool postProcess) {
    BlockState* oldState = level->getBlockState(spreadPos.pos);
    BlockState* spreadState = Blocks::SCULK_VEIN->getStateForPlacement(oldState, *level, spreadPos.pos, spreadPos.face);
    if (!spreadState) {
        return false;
    }

    if (postProcess) {
        if (::world::IChunk* chunk = level->getChunk(spreadPos.pos.getX() >> 4, spreadPos.pos.getZ() >> 4)) {
            chunk->markPosForPostprocessing(spreadPos.pos);
        }
    }

    return level->setBlock(spreadPos.pos, spreadState, 2);
}

template <size_t N>
static long spreadAllInternal(
    WorldGenLevel* level,
    BlockState* state,
    const BlockPos& pos,
    bool postProcess,
    const std::array<SpreadType, N>& spreadTypes
) {
    if (!state) {
        return 0;
    }

    bool isOtherBlockSource = state->getIdentifier() != "minecraft:sculk_vein";
    long count = 0;

    for (Direction faceDirection : s_directions) {
        if (!(isOtherBlockSource || SculkVeinBlock::hasFace(state, faceDirection))) {
            continue;
        }

        for (Direction spreadDirection : s_directions) {
            SpreadPos spreadPos{pos, spreadDirection};
            if (getSpreadFromFaceTowardDirection(level, state, pos, faceDirection, spreadDirection, isOtherBlockSource, spreadTypes, spreadPos) &&
                spreadToFace(level, spreadPos, postProcess)) {
                ++count;
            }
        }
    }

    return count;
}

template <size_t N>
static bool spreadFromFaceTowardRandomDirectionInternal(
    WorldGenLevel* level,
    BlockState* state,
    const BlockPos& pos,
    Direction startingFace,
    WorldgenRandom& random,
    bool postProcess,
    const std::array<SpreadType, N>& spreadTypes
) {
    bool isOtherBlockSource = state && state->getIdentifier() != "minecraft:sculk_vein";

    std::vector<Direction> shuffledDirections(s_directions.begin(), s_directions.end());
    for (int i = static_cast<int>(shuffledDirections.size()) - 1; i > 0; --i) {
        int j = random.nextInt(i + 1);
        std::swap(shuffledDirections[i], shuffledDirections[j]);
    }

    for (Direction spreadDirection : shuffledDirections) {
        SpreadPos spreadPos{pos, spreadDirection};
        if (getSpreadFromFaceTowardDirection(level, state, pos, startingFace, spreadDirection, isOtherBlockSource, spreadTypes, spreadPos) &&
            spreadToFace(level, spreadPos, postProcess)) {
            return true;
        }
    }

    return false;
}

static std::vector<Direction> allShuffled(WorldgenRandom& random) {
    std::vector<Direction> directions(s_directions.begin(), s_directions.end());
    for (int i = static_cast<int>(directions.size()) - 1; i > 0; --i) {
        int j = random.nextInt(i + 1);
        std::swap(directions[i], directions[j]);
    }
    return directions;
}

static BlockState* getRandomGrowthState(WorldGenLevel* level, const BlockPos& pos, WorldgenRandom& random, bool isWorldGen) {
    BlockState* state = nullptr;
    if (random.nextInt(SculkSpreader::SHRIEKER_PLACEMENT_RATE) == 0) {
        state = Blocks::SCULK_SHRIEKER->defaultBlockState();
        if (state && BlockStateProperties::CAN_SUMMON && state->hasProperty(BlockStateProperties::CAN_SUMMON)) {
            state = state->setValue(*BlockStateProperties::CAN_SUMMON, isWorldGen);
        }
    } else {
        state = Blocks::SCULK_SENSOR->defaultBlockState();
    }

    if (state && BlockStateProperties::WATERLOGGED && state->hasProperty(BlockStateProperties::WATERLOGGED)) {
        BlockState* existingState = level->getBlockState(pos);
        state = state->setValue(*BlockStateProperties::WATERLOGGED, isWaterSource(existingState));
    }

    return state;
}

static bool attemptPlaceSculk(
    SculkSpreader& spreader,
    WorldGenLevel* level,
    const BlockPos& pos,
    WorldgenRandom& random
) {
    BlockState* state = level->getBlockState(pos);
    if (!state || state->getIdentifier() != "minecraft:sculk_vein") {
        return false;
    }

    for (Direction support : allShuffled(random)) {
        if (!SculkVeinBlock::hasFace(state, support)) {
            continue;
        }

        BlockPos supportPos = pos.relative(support);
        BlockState* supportState = level->getBlockState(supportPos);
        if (!supportState || !isReplaceableForSpreader(spreader, supportState->getIdentifier())) {
            continue;
        }

        BlockState* defaultSculk = Blocks::SCULK->defaultBlockState();
        level->setBlock(supportPos, defaultSculk, 3);
        Blocks::SCULK_VEIN->spreadAll(defaultSculk, level, supportPos, spreader.isWorldGeneration());

        Direction skip = getOpposite(support);
        for (Direction veinDirection : s_directions) {
            if (veinDirection == skip) {
                continue;
            }

            BlockPos veinPos = supportPos.relative(veinDirection);
            BlockState* possibleVein = level->getBlockState(veinPos);
            if (possibleVein && possibleVein->getIdentifier() == "minecraft:sculk_vein") {
                Blocks::SCULK_VEIN->onDischarged(level, possibleVein, veinPos, random);
            }
        }

        return true;
    }

    return false;
}

static bool canPlaceGrowth(WorldGenLevel* level, const BlockPos& pos) {
    BlockState* stateAbove = level->getBlockState(pos.above());
    if (!isAirOrWater(stateAbove)) {
        return false;
    }

    int growthCount = 0;
    for (int dx = -4; dx <= 4; ++dx) {
        for (int dy = 0; dy <= 2; ++dy) {
            for (int dz = -4; dz <= 4; ++dz) {
                BlockState* state = level->getBlockState(pos.offset(dx, dy, dz));
                if (!state) {
                    continue;
                }

                const std::string id = state->getIdentifier();
                if (id == "minecraft:sculk_sensor" || id == "minecraft:sculk_shrieker") {
                    ++growthCount;
                    if (growthCount > 2) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

class DefaultSculkBehaviour : public SculkBehaviour {
public:
    bool attemptSpreadVein(WorldGenLevel* level, const BlockPos& pos, BlockState* state,
                           const std::set<Direction>* facings, bool postProcess) const override {
        if (facings == nullptr) {
            return Blocks::SCULK_VEIN->spreadSameSpace(level->getBlockState(pos), level, pos, postProcess);
        }

        if (!facings->empty()) {
            if (!state || (!state->isAir() && !isWaterSource(state))) {
                return false;
            }

            return Blocks::SCULK_VEIN->regrow(level, pos, state, *facings);
        }

        return SculkBehaviour::attemptSpreadVein(level, pos, state, facings, postProcess);
    }

    int attemptUseCharge(ChargeCursor& cursor, WorldGenLevel* /*level*/, const BlockPos& /*originPos*/,
                         WorldgenRandom& /*random*/, SculkSpreader& /*spreader*/,
                         bool /*spreadVeins*/) const override {
        return cursor.getDecayDelay() > 0 ? cursor.getCharge() : 0;
    }

    int updateDecayDelay(int age) const override {
        return std::max(age - 1, 0);
    }
};

static const SculkBehaviour& getDefaultSculkBehaviour() {
    static DefaultSculkBehaviour s_defaultSculkBehaviour;
    return s_defaultSculkBehaviour;
}

static const SculkBehaviour& getBlockBehaviour(BlockState* state) {
    if (state) {
        if (auto* behaviour = dynamic_cast<SculkBehaviour*>(state->getBlock())) {
            return *behaviour;
        }
    }

    return getDefaultSculkBehaviour();
}

static bool isSculkBehaviourState(BlockState* state) {
    return state && dynamic_cast<SculkBehaviour*>(state->getBlock()) != nullptr;
}

} // namespace

const std::vector<Vec3i>& ChargeCursor::getNonCornerNeighbours() {
    return s_nonCornerNeighbours;
}

ChargeCursor::ChargeCursor(const BlockPos& pos, int charge)
    : m_pos(pos)
    , m_charge(charge)
    , m_updateDelay(0)
    , m_decayDelay(1)
    , m_facings(std::nullopt)
{
}

ChargeCursor::ChargeCursor(
    const BlockPos& pos,
    int charge,
    int decayDelay,
    int updateDelay,
    const std::optional<std::set<Direction>>& facings
)
    : m_pos(pos)
    , m_charge(charge)
    , m_updateDelay(updateDelay)
    , m_decayDelay(decayDelay)
    , m_facings(facings)
{
}

bool ChargeCursor::isPosUnreasonable(const BlockPos& originPos) const {
    int dx = std::abs(m_pos.getX() - originPos.getX());
    int dy = std::abs(m_pos.getY() - originPos.getY());
    int dz = std::abs(m_pos.getZ() - originPos.getZ());
    return std::max({dx, dy, dz}) > SculkSpreader::MAX_CURSOR_DISTANCE;
}

std::vector<Vec3i> ChargeCursor::getRandomizedNonCornerNeighbourOffsets(WorldgenRandom& random) {
    std::vector<Vec3i> result = s_nonCornerNeighbours;
    for (int i = static_cast<int>(result.size()) - 1; i > 0; --i) {
        int j = random.nextInt(i + 1);
        std::swap(result[i], result[j]);
    }
    return result;
}

bool ChargeCursor::isUnobstructed(WorldGenLevel* level, const BlockPos& from, Direction direction) {
    BlockPos testPos = from.relative(direction);
    BlockState* state = level->getBlockState(testPos);
    return !state || !state->isFaceSturdy(*level, testPos, getOpposite(direction));
}

bool ChargeCursor::isMovementUnobstructed(WorldGenLevel* level, const BlockPos& from, const BlockPos& to) {
    int manhattanDistance =
        std::abs(to.getX() - from.getX()) +
        std::abs(to.getY() - from.getY()) +
        std::abs(to.getZ() - from.getZ());
    if (manhattanDistance == 1) {
        return true;
    }

    int dx = to.getX() - from.getX();
    int dy = to.getY() - from.getY();
    int dz = to.getZ() - from.getZ();

    Direction directionX = dx < 0 ? Direction::WEST : Direction::EAST;
    Direction directionY = dy < 0 ? Direction::DOWN : Direction::UP;
    Direction directionZ = dz < 0 ? Direction::NORTH : Direction::SOUTH;

    if (dx == 0) {
        return isUnobstructed(level, from, directionY) || isUnobstructed(level, from, directionZ);
    }
    if (dy == 0) {
        return isUnobstructed(level, from, directionX) || isUnobstructed(level, from, directionZ);
    }
    return isUnobstructed(level, from, directionX) || isUnobstructed(level, from, directionY);
}

BlockPos* ChargeCursor::getValidMovementPos(WorldGenLevel* level, const BlockPos& pos, WorldgenRandom& random) {
    static thread_local BlockPos resultPos;
    resultPos = pos;

    for (const Vec3i& offset : getRandomizedNonCornerNeighbourOffsets(random)) {
        BlockPos neighbour = pos.offset(offset.getX(), offset.getY(), offset.getZ());
        BlockState* transferee = level->getBlockState(neighbour);
        if (transferee && isSculkBehaviourState(transferee) && isMovementUnobstructed(level, pos, neighbour)) {
            resultPos = neighbour;
            if (SculkVeinBlock::hasSubstrateAccess(level, transferee, neighbour)) {
                break;
            }
        }
    }

    if (resultPos == pos) {
        return nullptr;
    }
    return &resultPos;
}

void ChargeCursor::update(
    WorldGenLevel* level,
    const BlockPos& originPos,
    WorldgenRandom& random,
    SculkSpreader& spreader,
    bool spreadVeins
) {
    if (m_charge <= 0) {
        return;
    }
    if (!spreader.isWorldGeneration()) {
        return;
    }
    if (m_updateDelay > 0) {
        --m_updateDelay;
        return;
    }

    BlockState* currentState = level->getBlockState(m_pos);
    if (!currentState) {
        return;
    }

    const SculkBehaviour* sculkBehaviour = &getBlockBehaviour(currentState);
    BlockState* behaviourState = currentState;

    if (spreadVeins && sculkBehaviour->attemptSpreadVein(level, m_pos, currentState, getFacingData(), spreader.isWorldGeneration())) {
        if (sculkBehaviour->canChangeBlockStateOnSpread()) {
            currentState = level->getBlockState(m_pos);
            behaviourState = currentState;
            sculkBehaviour = &getBlockBehaviour(currentState);
        }
    }

    m_charge = sculkBehaviour->attemptUseCharge(*this, level, originPos, random, spreader, spreadVeins);
    if (m_charge <= 0) {
        sculkBehaviour->onDischarged(level, currentState, m_pos, random);
        return;
    }

    BlockPos* transferPos = getValidMovementPos(level, m_pos, random);
    if (transferPos != nullptr) {
        sculkBehaviour->onDischarged(level, currentState, m_pos, random);
        m_pos = *transferPos;

        if (spreader.isWorldGeneration()) {
            int dx = m_pos.getX() - originPos.getX();
            int dz = m_pos.getZ() - originPos.getZ();
            double distance = std::sqrt(static_cast<double>(dx * dx + dz * dz));
            if (distance >= 15.0) {
                m_charge = 0;
                return;
            }
        }

        currentState = level->getBlockState(m_pos);
    }

    if (currentState && isSculkBehaviourState(currentState)) {
        m_facings = SculkVeinBlock::availableFaces(currentState);
    }

    m_decayDelay = sculkBehaviour->updateDecayDelay(m_decayDelay);
    m_updateDelay = sculkBehaviour->getSculkSpreadDelay();
}

void ChargeCursor::mergeWith(ChargeCursor& other) {
    m_charge += other.m_charge;
    other.m_charge = 0;
    m_updateDelay = std::min(m_updateDelay, other.m_updateDelay);
}

SculkSpreader SculkSpreader::createWorldGenSpreader() {
    return SculkSpreader(true, ReplaceableBlocks::SCULK_REPLACEABLE_WORLD_GEN, 50, 1, 5, 10);
}

SculkSpreader SculkSpreader::createLevelSpreader() {
    return SculkSpreader(false, ReplaceableBlocks::SCULK_REPLACEABLE, 10, 4, 10, 5);
}

SculkSpreader::SculkSpreader(
    bool isWorldGeneration,
    ReplaceableBlocks replaceableBlocks,
    int growthSpawnCost,
    int noGrowthRadius,
    int chargeDecayRate,
    int additionalDecayRate
)
    : m_isWorldGeneration(isWorldGeneration)
    , m_replaceableBlocks(replaceableBlocks)
    , m_growthSpawnCost(growthSpawnCost)
    , m_noGrowthRadius(noGrowthRadius)
    , m_chargeDecayRate(chargeDecayRate)
    , m_additionalDecayRate(additionalDecayRate)
{
}

void SculkSpreader::clear() {
    m_cursors.clear();
}

void SculkSpreader::addCursor(ChargeCursor cursor) {
    if (m_cursors.size() < MAX_CURSORS) {
        m_cursors.push_back(std::move(cursor));
    }
}

void SculkSpreader::addCursors(const BlockPos& startPos, int charge) {
    while (charge > 0) {
        int currentCharge = std::min(charge, MAX_CHARGE);
        addCursor(ChargeCursor(startPos, currentCharge));
        charge -= currentCharge;
    }
}

void SculkSpreader::updateCursors(WorldGenLevel* level, const BlockPos& originPos, WorldgenRandom& random, bool spreadVeins) {
    if (m_cursors.empty()) {
        return;
    }

    std::vector<ChargeCursor> processedCursors;
    processedCursors.reserve(m_cursors.size());
    std::unordered_map<int64_t, size_t> mergeableCursorIndices;

    auto posToKey = [](const BlockPos& pos) -> int64_t {
        return (static_cast<int64_t>(pos.getX()) & 0x3FFFFFF) |
               ((static_cast<int64_t>(pos.getY()) & 0xFFF) << 26) |
               ((static_cast<int64_t>(pos.getZ()) & 0x3FFFFFF) << 38);
    };

    for (ChargeCursor& cursor : m_cursors) {
        if (cursor.isPosUnreasonable(originPos)) {
            continue;
        }

        cursor.update(level, originPos, random, *this, spreadVeins);
        if (cursor.getCharge() <= 0) {
            continue;
        }

        int64_t key = posToKey(cursor.getPos());
        auto it = mergeableCursorIndices.find(key);
        if (it == mergeableCursorIndices.end()) {
            processedCursors.push_back(cursor);
            mergeableCursorIndices.emplace(key, processedCursors.size() - 1);
        } else if (!m_isWorldGeneration && cursor.getCharge() + processedCursors[it->second].getCharge() <= MAX_CHARGE) {
            processedCursors[it->second].mergeWith(cursor);
        } else {
            processedCursors.push_back(cursor);
            if (cursor.getCharge() < processedCursors[it->second].getCharge()) {
                it->second = processedCursors.size() - 1;
            }
        }
    }

    m_cursors = std::move(processedCursors);
}

int8_t SculkBehaviour::getSculkSpreadDelay() const {
    return 1;
}

void SculkBehaviour::onDischarged(WorldGenLevel* /*level*/, BlockState* /*state*/, const BlockPos& /*pos*/,
                                  WorldgenRandom& /*random*/) const {
}

bool SculkBehaviour::depositCharge(WorldGenLevel* /*level*/, const BlockPos& /*pos*/,
                                   WorldgenRandom& /*random*/) const {
    return false;
}

bool SculkBehaviour::attemptSpreadVein(WorldGenLevel* level, const BlockPos& pos, BlockState* state,
                                       const std::set<Direction>* /*facings*/, bool postProcess) const {
    return Blocks::SCULK_VEIN->spreadAll(state, level, pos, postProcess);
}

bool SculkBehaviour::canChangeBlockStateOnSpread() const {
    return true;
}

int SculkBehaviour::updateDecayDelay(int /*age*/) const {
    return 1;
}

SculkBlock::SculkBlock(const Properties& properties)
    : Block(properties) {
}

int SculkBlock::getDecayPenalty(const SculkSpreader& spreader, const BlockPos& pos,
                                const BlockPos& originPos, int charge) {
    int noGrowthRadius = spreader.noGrowthRadius();
    float outerDistanceSquared = std::pow(static_cast<float>(std::sqrt(pos.distSqr(originPos))) - static_cast<float>(noGrowthRadius), 2.0f);
    int maxReachSquared = (24 - noGrowthRadius) * (24 - noGrowthRadius);
    float distanceFactor = std::min(1.0f, outerDistanceSquared / static_cast<float>(maxReachSquared));
    return std::max(1, static_cast<int>(static_cast<float>(charge) * distanceFactor * 0.5f));
}

BlockState* SculkBlock::getRandomGrowthState(WorldGenLevel* level, const BlockPos& pos,
                                             WorldgenRandom& random, bool isWorldGen) const {
    BlockState* state = nullptr;
    if (random.nextInt(SculkSpreader::SHRIEKER_PLACEMENT_RATE) == 0) {
        state = Blocks::SCULK_SHRIEKER->defaultBlockState();
        if (state && BlockStateProperties::CAN_SUMMON && state->hasProperty(BlockStateProperties::CAN_SUMMON)) {
            state = state->setValue(*BlockStateProperties::CAN_SUMMON, isWorldGen);
        }
    } else {
        state = Blocks::SCULK_SENSOR->defaultBlockState();
    }

    if (state && BlockStateProperties::WATERLOGGED && state->hasProperty(BlockStateProperties::WATERLOGGED)) {
        BlockState* existingState = level->getBlockState(pos);
        state = state->setValue(*BlockStateProperties::WATERLOGGED, existingState && isWaterSource(existingState));
    }

    return state;
}

bool SculkBlock::canPlaceGrowth(WorldGenLevel* level, const BlockPos& pos) {
    BlockState* stateAbove = level->getBlockState(pos.above());
    if (!isAirOrWater(stateAbove)) {
        return false;
    }

    int growthCount = 0;
    for (int dx = -4; dx <= 4; ++dx) {
        for (int dy = 0; dy <= 2; ++dy) {
            for (int dz = -4; dz <= 4; ++dz) {
                BlockState* state = level->getBlockState(pos.offset(dx, dy, dz));
                if (!state) {
                    continue;
                }

                const std::string id = state->getIdentifier();
                if (id == "minecraft:sculk_sensor" || id == "minecraft:sculk_shrieker") {
                    ++growthCount;
                    if (growthCount > 2) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

int SculkBlock::attemptUseCharge(ChargeCursor& cursor, WorldGenLevel* level, const BlockPos& originPos,
                                 WorldgenRandom& random, SculkSpreader& spreader,
                                 bool /*spreadVeins*/) const {
    int charge = cursor.getCharge();
    if (charge != 0 && random.nextInt(spreader.chargeDecayRate()) == 0) {
        const BlockPos& chargePos = cursor.getPos();
        bool isCloseToCatalyst = chargePos.distSqr(originPos) <
            static_cast<double>(spreader.noGrowthRadius()) * static_cast<double>(spreader.noGrowthRadius());

        if (!isCloseToCatalyst && canPlaceGrowth(level, chargePos)) {
            int xpPerGrowthSpawn = spreader.growthSpawnCost();
            if (random.nextInt(xpPerGrowthSpawn) < charge) {
                BlockPos growthPlacement = chargePos.above();
                if (BlockState* growthState = getRandomGrowthState(level, growthPlacement, random, spreader.isWorldGeneration())) {
                    level->setBlock(growthPlacement, growthState, 3);
                }
            }

            return std::max(0, charge - xpPerGrowthSpawn);
        }

        return random.nextInt(spreader.additionalDecayRate()) != 0
            ? charge
            : charge - (isCloseToCatalyst ? 1 : getDecayPenalty(spreader, chargePos, originPos, charge));
    }

    return charge;
}

bool SculkBlock::canChangeBlockStateOnSpread() const {
    return false;
}

bool SculkVeinBlock::hasSubstrateAccess(WorldGenLevel* level, BlockState* state, const BlockPos& pos) {
    if (!state || state->getIdentifier() != "minecraft:sculk_vein") {
        return false;
    }

    for (Direction direction : s_directions) {
        if (!hasFace(state, direction)) {
            continue;
        }

        BlockState* adjacent = level->getBlockState(pos.relative(direction));
        if (adjacent && isSculkReplaceable(adjacent->getIdentifier())) {
            return true;
        }
    }

    return false;
}

bool SculkVeinBlock::spreadAll(BlockState* state, WorldGenLevel* level, const BlockPos& pos, bool postProcess) const {
    return spreadAllInternal(level, state, pos, postProcess, s_defaultSpreadTypes) > 0;
}

bool SculkVeinBlock::spreadFromFaceTowardRandomDirection(BlockState* state, WorldGenLevel* level, const BlockPos& pos,
                                                         Direction startingFace, WorldgenRandom& random,
                                                         bool postProcess) const {
    if (!state) {
        return false;
    }

    return spreadFromFaceTowardRandomDirectionInternal(level, state, pos, startingFace, random, postProcess, s_defaultSpreadTypes);
}

bool SculkVeinBlock::spreadSameSpace(BlockState* state, WorldGenLevel* level, const BlockPos& pos, bool postProcess) const {
    return spreadAllInternal(level, state, pos, postProcess, s_samePositionSpreadTypes) > 0;
}

void SculkVeinBlock::onDischarged(WorldGenLevel* level, BlockState* state, const BlockPos& pos,
                                  WorldgenRandom& /*random*/) const {
    if (!state || !state->is(this)) {
        return;
    }

    BlockState* newState = state;
    for (Direction direction : s_directions) {
        BooleanProperty* faceProperty = getFaceProperty(direction);
        if (!faceProperty || !newState->hasProperty(faceProperty)) {
            continue;
        }

        BlockState* adjacentState = level->getBlockState(pos.relative(direction));
        if (newState->getValueOrElse(*faceProperty, false) && adjacentState && adjacentState->is(Blocks::SCULK)) {
            newState = newState->setValue(*faceProperty, false);
        }
    }

    if (!hasAnyFace(newState)) {
        newState = level->isWaterAt(pos)
            ? Blocks::WATER->defaultBlockState()
            : Blocks::AIR->defaultBlockState();
    }

    level->setBlock(pos, newState, 3);
}

int SculkVeinBlock::attemptUseCharge(ChargeCursor& cursor, WorldGenLevel* level, const BlockPos& /*originPos*/,
                                     WorldgenRandom& random, SculkSpreader& spreader,
                                     bool spreadVeins) const {
    if (spreadVeins && attemptPlaceSculk(spreader, level, cursor.getPos(), random)) {
        return cursor.getCharge() - 1;
    }

    return random.nextInt(spreader.chargeDecayRate()) == 0
        ? static_cast<int>(std::floor(static_cast<float>(cursor.getCharge()) * 0.5f))
        : cursor.getCharge();
}

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
