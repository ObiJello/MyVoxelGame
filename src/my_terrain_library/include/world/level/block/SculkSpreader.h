#pragma once

#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/state/BlockState.h"
#include <vector>
#include <set>
#include <optional>
#include <cstdint>

// Reference: net/minecraft/world/level/block/SculkSpreader.java

namespace minecraft {
namespace levelgen {
class WorldGenLevel;
class WorldgenRandom;
}

namespace world {
namespace level {
namespace block {

class ChargeCursor;
class SculkSpreader;

/**
 * SculkBehaviour - interface for sculk block behavior
 * Reference: SculkBehaviour.java
 */
class SculkBehaviour {
public:
    virtual ~SculkBehaviour() = default;

    virtual int8_t getSculkSpreadDelay() const;

    virtual void onDischarged(levelgen::WorldGenLevel* level, state::BlockState* state,
                              const core::BlockPos& pos, levelgen::WorldgenRandom& random) const;

    virtual bool depositCharge(levelgen::WorldGenLevel* level, const core::BlockPos& pos,
                               levelgen::WorldgenRandom& random) const;

    virtual bool attemptSpreadVein(levelgen::WorldGenLevel* level, const core::BlockPos& pos,
                                   state::BlockState* state, const std::set<core::Direction>* facings,
                                   bool postProcess) const;

    virtual bool canChangeBlockStateOnSpread() const;

    virtual int updateDecayDelay(int age) const;

    virtual int attemptUseCharge(ChargeCursor& cursor, levelgen::WorldGenLevel* level,
                                 const core::BlockPos& originPos, levelgen::WorldgenRandom& random,
                                 SculkSpreader& spreader, bool spreadVeins) const = 0;
};

/**
 * ChargeCursor - tracks a single spreading cursor
 * Reference: SculkSpreader.ChargeCursor
 */
class ChargeCursor {
public:
    // Reference: NON_CORNER_NEIGHBOURS - 18 positions (faces + edges, no corners)
    static const std::vector<core::Vec3i>& getNonCornerNeighbours();

    static constexpr int MAX_CURSOR_DECAY_DELAY = 1;

    ChargeCursor(const core::BlockPos& pos, int charge);
    ChargeCursor(const core::BlockPos& pos, int charge, int decayDelay, int updateDelay,
                 const std::optional<std::set<core::Direction>>& facings);

    const core::BlockPos& getPos() const { return m_pos; }
    int getCharge() const { return m_charge; }
    int getDecayDelay() const { return m_decayDelay; }
    const std::set<core::Direction>* getFacingData() const {
        return m_facings.has_value() ? &m_facings.value() : nullptr;
    }

    bool isPosUnreasonable(const core::BlockPos& originPos) const;

    void update(levelgen::WorldGenLevel* level, const core::BlockPos& originPos,
                levelgen::WorldgenRandom& random, SculkSpreader& spreader, bool spreadVeins);

    void mergeWith(ChargeCursor& other);

    void setCharge(int charge) { m_charge = charge; }

private:
    core::BlockPos m_pos;
    int m_charge;
    int m_updateDelay;
    int m_decayDelay;
    std::optional<std::set<core::Direction>> m_facings;

    static std::vector<core::Vec3i> getRandomizedNonCornerNeighbourOffsets(levelgen::WorldgenRandom& random);
    static core::BlockPos* getValidMovementPos(levelgen::WorldGenLevel* level, const core::BlockPos& pos, levelgen::WorldgenRandom& random);
    static bool isMovementUnobstructed(levelgen::WorldGenLevel* level, const core::BlockPos& from, const core::BlockPos& to);
    static bool isUnobstructed(levelgen::WorldGenLevel* level, const core::BlockPos& from, core::Direction direction);
};

/**
 * SculkSpreader - manages sculk spreading for world generation
 * Reference: SculkSpreader.java
 */
class SculkSpreader {
public:
    enum class ReplaceableBlocks {
        SCULK_REPLACEABLE,
        SCULK_REPLACEABLE_WORLD_GEN
    };

    static constexpr int MAX_GROWTH_RATE_RADIUS = 24;
    static constexpr int MAX_CHARGE = 1000;
    static constexpr float MAX_DECAY_FACTOR = 0.5f;
    static constexpr int MAX_CURSORS = 32;
    static constexpr int SHRIEKER_PLACEMENT_RATE = 11;
    static constexpr int MAX_CURSOR_DISTANCE = 1024;

    // Create a spreader for world generation
    // Reference: createWorldGenSpreader()
    static SculkSpreader createWorldGenSpreader();

    // Create a spreader for level spreading (not world gen)
    // Reference: createLevelSpreader()
    static SculkSpreader createLevelSpreader();

    SculkSpreader(bool isWorldGeneration, ReplaceableBlocks replaceableBlocks,
                  int growthSpawnCost, int noGrowthRadius,
                  int chargeDecayRate, int additionalDecayRate);

    bool isWorldGeneration() const { return m_isWorldGeneration; }
    ReplaceableBlocks replaceableBlocks() const { return m_replaceableBlocks; }
    int growthSpawnCost() const { return m_growthSpawnCost; }
    int noGrowthRadius() const { return m_noGrowthRadius; }
    int chargeDecayRate() const { return m_chargeDecayRate; }
    int additionalDecayRate() const { return m_additionalDecayRate; }

    std::vector<ChargeCursor>& getCursors() { return m_cursors; }
    const std::vector<ChargeCursor>& getCursors() const { return m_cursors; }

    void clear();

    // Reference: addCursors() lines 123-130
    void addCursors(const core::BlockPos& startPos, int charge);

    // Reference: updateCursors() lines 138-184
    void updateCursors(levelgen::WorldGenLevel* level, const core::BlockPos& originPos,
                       levelgen::WorldgenRandom& random, bool spreadVeins);

private:
    bool m_isWorldGeneration;
    ReplaceableBlocks m_replaceableBlocks;
    int m_growthSpawnCost;
    int m_noGrowthRadius;
    int m_chargeDecayRate;
    int m_additionalDecayRate;
    std::vector<ChargeCursor> m_cursors;

    void addCursor(ChargeCursor cursor);
};

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
