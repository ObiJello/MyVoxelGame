// File: src/common/entity/ai/brain/AllayAi.hpp
//
// MC net.minecraft.world.entity.animal.allay.AllayAi: CORE (swim, panic,
// look/move sinks, the liked-noteblock and item-pickup cooldowns) and IDLE
// (GoToWantedItem, GoAndGiveItemsToTarget, StayCloseToTarget over the item
// deposit position — the liked noteblock while its cooldown lasts, else the
// liked player — then the glances and the flying wander), plus the
// NEAREST_ITEMS sensor, hearNoteblock and throwItem.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

#include <glm/glm.hpp>

namespace Game {
    class Allay;
    struct EntityLevel;
    namespace AllayAi {
        void InitBrain(Allay& allay, Brain& brain);
        void UpdateActivity(Allay& allay);

        // MC AllayAi.hearNoteblock: the first noteblock heard becomes the
        // liked one; hearing the liked one again refreshes its 600 ticks.
        void HearNoteblock(Allay& allay, const glm::ivec3& pos);

    }
}
