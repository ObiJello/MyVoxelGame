// File: src/common/world/block/CopperChestBlock.cpp
//
// See CopperChestBlock.hpp. Every function names the MC method it ports.
#include "common/world/block/CopperChestBlock.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/block/entity/DoubleChest.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"

#include <cstdlib>

namespace Game {

    namespace {

        // CopperChestBlock.updateShape: a copper chest whose connected
        // neighbour is a copper chest becomes that neighbour's block with its
        // own properties (`neighbourState.getBlock().withPropertiesOf(state)`)
        // — the second half of a pair follows the first through every
        // oxidation, waxing and scraping.
        bool CopperChestUpdateShape(const IBlockAccess& /*level*/, const glm::ivec3& pos, BlockState state,
                                    Direction toNeighbour, BlockID neighbourId, BlockState& outState,
                                    ScheduledTickAccess* /*ticks*/) {
            if (!IsCopperChestBlock(neighbourId) || neighbourId == state.Block()) return false;
            const auto connected = ChestConnectedCell(state, pos);
            if (!connected) return false;   // TYPE SINGLE
            const glm::ivec3 neighbour(pos.x + StepX(toNeighbour), pos.y + StepY(toNeighbour),
                                       pos.z + StepZ(toNeighbour));
            if (*connected != neighbour) return false;
            outState = ChestWithPropertiesOf(neighbourId, state);
            return true;
        }

        // WeatheringCopperChestBlock.isRandomlyTicking:
        // WeatheringCopper.getNext(block).isPresent().
        bool CopperChestIsRandomlyTicking(BlockState state) {
            return !IsWaxedCopperChest(state.Block()) &&
                   WeatheringCopperNext(state.Block()) != BlockID::Air;
        }

        // ChangeOverTimeBlock.getNextState: every weathering copper block
        // within Manhattan distance 4 votes — one younger than this one
        // vetoes, older ones raise the odds, same-age ones lower them — and
        // the square of that share, times WeatheringCopper.getChanceModifier
        // (0.75 while unaffected), is the chance to age a stage.
        bool ShouldAge(const IBlockAccess& level, const glm::ivec3& pos, int ownAge, JavaRandom& random) {
            int sameAgeCount = 0;
            int olderCount   = 0;
            for (int dx = -4; dx <= 4; ++dx) {
                for (int dy = -4; dy <= 4; ++dy) {
                    for (int dz = -4; dz <= 4; ++dz) {
                        if (std::abs(dx) + std::abs(dy) + std::abs(dz) > 4) continue;
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        const int foundAge =
                            WeatheringCopperAge(level.GetBlock(pos.x + dx, pos.y + dy, pos.z + dz));
                        if (foundAge < 0) continue;
                        if (foundAge < ownAge) return false;   // Continuation.ABORT
                        if (foundAge > ownAge) ++olderCount;
                        else                   ++sameAgeCount;
                    }
                }
            }
            const float chance = static_cast<float>(olderCount + 1) /
                                 static_cast<float>(olderCount + sameAgeCount + 1);
            const float chanceModifier = ownAge == 0 ? 0.75f : 1.0f;
            return random.NextFloat() < chance * chance * chanceModifier;
        }

        // WeatheringCopperChestBlock.randomTick → changeOverTime.
        void CopperChestRandomTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   JavaRandom& random) {
            // The LEFT half (or a single chest) ages the pair; the RIGHT one
            // follows through updateShape.
            if (state.GetValueByName("type") == "right") return;
            // Nobody may be looking into it (getEntitiesWithContainerOpen).
            auto* chest = dynamic_cast<ChestBlockEntity*>(level.GetBlockEntity(pos));
            if (!chest || chest->GetOpenerCount() > 0) return;

            // changeOverTime: eachBlockOncePerDayChance = 0.05688889.
            if (!(random.NextFloat() < 0.05688889f)) return;
            const int age = WeatheringCopperAge(state.Block());
            const BlockID next = WeatheringCopperNext(state.Block());
            if (age < 0 || next == BlockID::Air) return;
            if (!ShouldAge(level, pos, age, random)) return;
            // setBlockAndUpdate(pos, next.withPropertiesOf(state)).
            level.SetBlock(pos.x, pos.y, pos.z, ChestWithPropertiesOf(next, state), World::UpdateFlags::All);
        }

    } // namespace

    void RegisterCopperChestBehaviors(std::array<Block, BlockRegistry::Size>& blocks) {
        for (int waxed = 0; waxed < 2; ++waxed) {
            for (int stage = 0; stage < 4; ++stage) {
                Block& b = blocks[static_cast<size_t>(CopperChestOf(stage, waxed != 0))];
                b.updateShape = &CopperChestUpdateShape;
                if (!waxed && stage < 3) {
                    b.isRandomlyTicking = &CopperChestIsRandomlyTicking;
                    b.randomTick        = &CopperChestRandomTick;
                }
            }
        }
    }

} // namespace Game
