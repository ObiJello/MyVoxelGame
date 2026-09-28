// File: src/common/world/block/SculkSpreader.hpp
//
// Port of MC 26.3's sculk growth machinery:
//
//   SculkSpreader (+ ChargeCursor)  — the catalyst's charge, walking through
//                                     sculk and veins and spending itself on
//                                     new sculk, veins, sensors and shriekers
//   SculkBehaviour                  — how each block spends a charge
//                                     (SculkBlock, SculkVeinBlock, DEFAULT)
//   MultifaceSpreader               — the sculk vein's spreader config
//                                     (SculkVeinBlock.SculkVeinSpreaderConfig)
//
// The world-generation spreader (SculkPatchFeature, the ancient cities) is the
// terrain library's own port; this is the LEVEL spreader a catalyst runs.
#pragma once

#include "Blocks.hpp"
#include "BlockState.hpp"
#include "Direction.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Game {

    class ILevelWrite;
    class World;
    class JavaRandom;

    class SculkSpreader {
    public:
        // MC SculkSpreader constants.
        static constexpr int   kMaxGrowthRateRadius = 24;
        static constexpr int   kMaxCharge           = 1000;
        static constexpr float kMaxDecayFactor      = 0.5f;
        static constexpr int   kMaxCursors          = 32;
        static constexpr int   kShriekerPlacementRate = 11;
        static constexpr int   kMaxCursorDistance   = 1024;

        // MC SculkSpreader.ChargeCursor. `facings` is MC's nullable
        // Set<Direction>: nullopt is null, otherwise a MultifaceBlock.pack
        // bitmask (bit = Direction ordinal; 0 = the empty set).
        struct ChargeCursor {
            glm::ivec3             pos{0};
            int                    charge = 0;
            int                    updateDelay = 0;
            int                    decayDelay = 1;
            std::optional<uint8_t> facings;

            ChargeCursor() = default;
            ChargeCursor(const glm::ivec3& p, int c) : pos(p), charge(c) {}
        };

        // MC createLevelSpreader: (false, #sculk_replaceable, 10, 4, 10, 5).
        static SculkSpreader CreateLevelSpreader();
        // MC createWorldGenSpreader: (true, #sculk_replaceable_world_gen, 50, 1, 5, 10).
        static SculkSpreader CreateWorldGenSpreader();

        bool IsWorldGeneration() const { return m_isWorldGeneration; }
        int  GrowthSpawnCost() const { return m_growthSpawnCost; }
        int  NoGrowthRadius() const { return m_noGrowthRadius; }
        int  ChargeDecayRate() const { return m_chargeDecayRate; }
        int  AdditionalDecayRate() const { return m_additionalDecayRate; }
        // replaceableBlocks(): the spreader's #sculk_replaceable(_world_gen).
        bool IsReplaceable(BlockID block) const;

        const std::vector<ChargeCursor>& Cursors() const { return m_cursors; }
        void Clear() { m_cursors.clear(); }

        // MC load: the saved cursors, at most 32 of them.
        void Load(const std::vector<ChargeCursor>& cursors);

        // MC addCursors: `charge` split into cursors of at most 1000.
        void AddCursors(const glm::ivec3& startPos, int charge);

        // MC updateCursors — one step of every cursor, then the merge of the
        // cursors that met, and the 3006 level events that draw the charge.
        void UpdateCursors(World& level, const glm::ivec3& originPos, JavaRandom& random, bool spreadVeins);

    private:
        SculkSpreader(bool worldGen, int growthSpawnCost, int noGrowthRadius, int chargeDecayRate,
                      int additionalDecayRate)
            : m_isWorldGeneration(worldGen), m_growthSpawnCost(growthSpawnCost),
              m_noGrowthRadius(noGrowthRadius), m_chargeDecayRate(chargeDecayRate),
              m_additionalDecayRate(additionalDecayRate) {}

        void AddCursor(const ChargeCursor& cursor) {
            if (m_cursors.size() < static_cast<size_t>(kMaxCursors)) m_cursors.push_back(cursor);
        }

        bool m_isWorldGeneration;
        int  m_growthSpawnCost;
        int  m_noGrowthRadius;
        int  m_chargeDecayRate;
        int  m_additionalDecayRate;
        std::vector<ChargeCursor> m_cursors;
    };

    namespace Sculk {

        // MultifaceBlock.pack / unpack over the six faces (bit = ordinal).
        uint8_t PackFaces(BlockState multifaceState);

        // #minecraft:sculk_replaceable.
        bool IsSculkReplaceable(BlockID block);

        // MC SculkVeinBlock.hasSubstrateAccess.
        bool HasSubstrateAccess(const ILevelWrite& level, BlockState state, const glm::ivec3& pos);

        // MC Block.pushEntitiesUp(oldState, newState=full cube, level, pos):
        // what stood in the part of the cell the new block fills is lifted
        // onto it.
        void PushEntitiesUp(ILevelWrite& level, BlockState oldState, const glm::ivec3& pos);

    } // namespace Sculk

} // namespace Game
