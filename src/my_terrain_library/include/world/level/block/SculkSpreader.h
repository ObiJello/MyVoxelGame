#pragma once

#include "core/BlockPos.h"
#include "core/Direction.h"
#include "random/XoroshiroRandomSource.h"
#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"
#include <vector>
#include <set>
#include <optional>

// Reference: net/minecraft/world/level/block/SculkSpreader.java

namespace minecraft {

// Forward declare to avoid conflicts with system random()
class XoroshiroRandomSource;

namespace world {
namespace level {
namespace block {

class SculkSpreader;

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

    void update(IChunk* level, const core::BlockPos& originPos,
                ::minecraft::XoroshiroRandomSource& random, SculkSpreader& spreader, bool spreadVeins);

    void mergeWith(ChargeCursor& other);

    void setCharge(int charge) { m_charge = charge; }

private:
    core::BlockPos m_pos;
    int m_charge;
    int m_updateDelay;
    int m_decayDelay;
    std::optional<std::set<core::Direction>> m_facings;

    static std::vector<core::Vec3i> getRandomizedNonCornerNeighbourOffsets(::minecraft::XoroshiroRandomSource& random);
    static core::BlockPos* getValidMovementPos(IChunk* level, const core::BlockPos& pos, ::minecraft::XoroshiroRandomSource& random);
    static bool isMovementUnobstructed(IChunk* level, const core::BlockPos& from, const core::BlockPos& to);
    static bool isUnobstructed(IChunk* level, const core::BlockPos& from, core::Direction direction);
};

/**
 * SculkSpreader - manages sculk spreading for world generation
 * Reference: SculkSpreader.java
 */
class SculkSpreader {
public:
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

    SculkSpreader(bool isWorldGeneration, int growthSpawnCost, int noGrowthRadius,
                  int chargeDecayRate, int additionalDecayRate);

    bool isWorldGeneration() const { return m_isWorldGeneration; }
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
    void updateCursors(IChunk* level, const core::BlockPos& originPos,
                       ::minecraft::XoroshiroRandomSource& random, bool spreadVeins);

private:
    bool m_isWorldGeneration;
    int m_growthSpawnCost;
    int m_noGrowthRadius;
    int m_chargeDecayRate;
    int m_additionalDecayRate;
    std::vector<ChargeCursor> m_cursors;

    void addCursor(ChargeCursor cursor);
};

/**
 * SculkBehaviour - interface for sculk block behavior
 * Reference: SculkBehaviour.java
 */
namespace SculkBehaviour {
    // Reference: attemptUseCharge() - returns remaining charge
    int attemptUseCharge(ChargeCursor& cursor, IChunk* level, const core::BlockPos& originPos,
                         ::minecraft::XoroshiroRandomSource& random, SculkSpreader& spreader, bool spreadVeins);

    // Reference: attemptSpreadVein()
    bool attemptSpreadVein(IChunk* level, const core::BlockPos& pos, state::BlockState* state,
                          const std::set<core::Direction>* facings, bool isWorldGen);

    // Reference: updateDecayDelay()
    int updateDecayDelay(int age);

    // Reference: getSculkSpreadDelay()
    int getSculkSpreadDelay();

    // Reference: onDischarged()
    void onDischarged(IChunk* level, state::BlockState* state, const core::BlockPos& pos,
                      ::minecraft::XoroshiroRandomSource& random);

    // Check if block is sculk behavior
    bool isSculkBehaviour(state::BlockState* state);

    // Check if block can change state on spread
    bool canChangeBlockStateOnSpread(state::BlockState* state);
}

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
