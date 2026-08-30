// File: src/common/entity/ai/brain/AllayAi.hpp
//
// MC net.minecraft.world.entity.animal.allay.AllayAi. The honest portable
// core: float (Swim), panic, and the flying idle wander with player glances.
// SKIPPED, commented at their MC slots in the .cpp: the whole item-courier
// loop (GoToWantedItem, GoAndGiveItemsToTarget, StayCloseToTarget over the
// liked player / liked noteblock, both cooldown counters) — items and
// noteblock game events do not exist — and with them the jukebox dance and
// the duplication ritual on the Allay class itself.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Allay;
    namespace AllayAi {
        void InitBrain(Allay& allay, Brain& brain);
        void UpdateActivity(Allay& allay);
    }
}
