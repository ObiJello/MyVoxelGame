// File: src/common/world/block/SculkBlocks.hpp
//
// The block halves of MC 26.3's sculk family:
//
//   SculkSensorBlock / CalibratedSculkSensorBlock — phase ticks, activation,
//     redstone output (power by distance, strong only upward, the calibrated
//     one dark toward its facing), the comparator's frequency reading, the
//     step-on vibration, resonance into #vibration_resonators (amethyst)
//   SculkShriekerBlock — the step-on shriek and the shriek's end
//   SculkCatalystBlock — the bloom's end
//   AmethystBlock      — the chime of a projectile striking it
//
// The listeners, the spreader and the warden summon are the block entities'
// (SculkBlockEntities.hpp); the ambient sensor particles are
// BlockAnimateParticles'.
#pragma once

#include "BlockRegistry.hpp"

#include <array>
#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;
    class Entity;

    namespace SculkSensor {

        // MC SculkSensorPhase, in BlockStateProperties.SCULK_SENSOR_PHASE's
        // value order (inactive, active, cooldown) — the property index.
        enum class Phase : uint8_t { Inactive = 0, Active = 1, Cooldown = 2 };

        // MC SculkSensorBlock.ACTIVE_TICKS / COOLDOWN_TICKS.
        inline constexpr int kActiveTicks = 30;
        inline constexpr int kCalibratedActiveTicks = 10;
        inline constexpr int kCooldownTicks = 10;

        bool IsSculkSensor(BlockID block);   // the sensor or the calibrated one

        Phase PhaseOf(BlockState state);
        BlockState WithPhase(BlockState state, Phase phase);

        // MC SculkSensorBlock.canActivate: INACTIVE.
        bool CanActivate(BlockState state);

        // MC getActiveTicks: 30, or the calibrated sensor's 10.
        int ActiveTicks(BlockID block);

        // MC SculkSensorBlock.activate.
        void Activate(Entity* sourceEntity, ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                      int calculatedPower, int vibrationFrequency);

        // MC SculkSensorBlock.deactivate.
        void Deactivate(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

        // MC SculkSensorBlock.tryResonateVibration: every #vibration_resonators
        // neighbour re-emits the vibration as resonate_<frequency> and chimes.
        void TryResonateVibration(Entity* sourceEntity, ILevelWrite& level, const glm::ivec3& pos,
                                  int vibrationFrequency);

    } // namespace SculkSensor

    // Wires the hooks above onto sculk_sensor, calibrated_sculk_sensor,
    // sculk_shrieker, sculk_catalyst and the amethyst blocks. Called from
    // BlockRegistry_RegisterBehaviors, before the animate-tick chain.
    void RegisterSculkBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
