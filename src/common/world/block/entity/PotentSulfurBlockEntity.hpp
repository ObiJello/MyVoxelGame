// File: src/common/world/block/entity/PotentSulfurBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.PotentSulfurBlockEntity
// (26.3) — the geyser's clock and its tickers. MC picks a ticker per
// `potent_sulfur_state` (PotentSulfurBlock.getTicker); here the entity always
// ticks and branches on the state it finds, which is the same thing:
//
//              server                                client
//   DRY        —                                     —
//   WET        nausea                                noxious gas cloud
//   DORMANT    countdown, then nausea                noxious gas cloud
//   ERUPTING   launch, then countdown                plume (eruption sound), launch
//   CONTINUOUS launch                                plume (continuous sound), launch
//
// The launch runs on both sides because each side moves what it simulates:
// the server its mobs and dropped items, the client its own player (players
// are client-authoritative, and the server's view of one would turn the
// push into a second, packet-borne one). NeedsTicking is always true: a
// state-only write (DRY -> WET) neither re-creates nor re-registers the
// entity on the client, and an entity that once answered false drops off the
// client's ticking list for good.
//
// DEVIATIONS:
//   * MC's update tag carries nothing for this entity, and neither does the
//     wire payload here (no Save/Load override).
#pragma once

#include "BlockEntity.hpp"

#include <cstdint>

namespace Game {

    class PotentSulfurBlockEntity : public BlockEntity {
    public:
        // MC PotentSulfurBlockEntity constants.
        static constexpr int     kEffectApplicationFrequencyTicks = 10;
        static constexpr int     kEffectDurationTicks             = 80;
        static constexpr float   kEffectRange                     = 3.0f;
        static constexpr int     kParticleFrequencyTicks          = 20;
        static constexpr int     kSoundFrequencyTicks             = 40;
        static constexpr double  kGeyserBaseLaunchSpeed           = 0.30000001192092896;  // 0.3F
        static constexpr double  kGeyserLaunchForce               = 0.20000000298023224;  // 0.2F
        static constexpr int64_t kGeyserSalt                      = -904011478LL;

        PotentSulfurBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;
        void ClientTick(ILevelWrite& level) override;

        // The client rebuilds this entity from every BlockEntityDataS2C; the
        // eruption clock is client state MC keeps across such an update.
        void CarryClientState(const BlockEntity& previous) override;

        // MC resetCountdown: the next countdown tick rolls a fresh one.
        void ResetCountdown() { waitingCountdown = -1; }

        // MC waitingCountdown — seconds (20-tick steps) until the geyser
        // flips between DORMANT and ERUPTING; -1 / 0 = roll a new one.
        int waitingCountdown = -1;
        // MC eruptionTick — the game time of the last block event (the start
        // of the current eruption), which phases the client's plume and its
        // sound. -1 until the entity first meets its level (MC setLevel).
        int64_t eruptionTick = -1;
    };

} // namespace Game
