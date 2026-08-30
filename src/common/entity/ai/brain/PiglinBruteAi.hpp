// File: src/common/entity/ai/brain/PiglinBruteAi.hpp
//
// MC net.minecraft.world.entity.monster.piglin.PiglinBruteAi — the simpler,
// always-hostile brain: no bartering, no hunting, no fleeing. It fights
// whatever it is angry at, any targetable player, or a wither-class nemesis,
// and otherwise patrols around the HOME position it spawned at.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class PiglinBrute;
    struct EntityLevel;
    namespace PiglinBruteAi {
        void InitBrain(PiglinBrute& brute, Brain& brain);

        // MC PiglinBruteAi.initMemories — HOME is wherever it spawned.
        void InitMemories(PiglinBrute& brute);

        void UpdateActivity(PiglinBrute& brute);

        // MC PiglinBruteAi.wasHurtBy — retaliate unless a fellow piglin.
        void WasHurtBy(EntityLevel& level, PiglinBrute& brute, LivingEntity& attacker);
    }
}
