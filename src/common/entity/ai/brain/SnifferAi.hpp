// File: src/common/entity/ai/brain/SnifferAi.hpp
//
// MC net.minecraft.world.entity.animal.sniffer.SnifferAi, nested behaviours
// (Scenting, Sniffing, Searching, Digging, FinishedDigging, FeelingHappy)
// included — they are private static classes in MC and anonymous-namespace
// classes in the .cpp.
//
// The idle cycle that makes a sniffer a sniffer: every so often it SCENTS
// (nose up), occasionally it SNIFFS (nose down, 40–80 ticks) — and a sniff
// that runs to completion picks a diggable spot, walks there (SEARCHING),
// DIGs for 160–180 ticks, RISES for 40, drops a seed, and is HAPPY about it.
// Then 9600 ticks of cooldown before it may sniff again.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Sniffer;
    namespace SnifferAi {
        void InitBrain(Sniffer& sniffer, Brain& brain);
        void UpdateActivity(Sniffer& sniffer);
    }
}
