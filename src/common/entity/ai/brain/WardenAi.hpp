// File: src/common/entity/ai/brain/WardenAi.hpp
//
// MC net.minecraft.world.entity.monster.warden.WardenAi and the
// ai/behavior/warden package (Emerging, Digging, Roar, Sniffing, SonicBoom,
// SetRoarTarget, SetWardenLookTarget, TryToSniff) — behaviours in the .cpp.
//
// Seven activities checked in strict order every tick:
// EMERGE → DIG → ROAR → FIGHT → INVESTIGATE → SNIFF → IDLE. Each of the five
// baked clips is owned by exactly one of them: emerge/dig/roar/sniff by the
// pose their activity sets, the sonic boom by entity event 62.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Warden;
    namespace WardenAi {
        // MC WardenAi's public duration constants — Warden.finalizeSpawn and
        // the dig lifecycle read them from outside the Ai.
        inline constexpr int kEmergeDuration = 134;   // Mth.ceil(133.59999F)
        inline constexpr int kRoarDuration   = 84;    // Mth.ceil(84.0F)
        inline constexpr int kDiggingCooldown = 1200;

        void InitBrain(Warden& warden, Brain& brain);
        void UpdateActivity(Warden& warden);

        // MC WardenAi.setDigCooldown — refresh to 1200 IF the memory currently
        // holds a value (the "if present" is load-bearing: a warden whose
        // cooldown lapsed keeps it lapsed until a disturbance re-arms it...
        // except MC re-arms it on every disturbance path through here).
        void SetDigCooldown(Warden& warden);

        // MC WardenAi.setDisturbanceLocation — look at it, sniff-cooldown it,
        // and walk to investigate.
        void SetDisturbanceLocation(Warden& warden, const glm::ivec3& pos);
    }
}
