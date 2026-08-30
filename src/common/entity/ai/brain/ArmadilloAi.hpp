// File: src/common/entity/ai/brain/ArmadilloAi.hpp
//
// MC net.minecraft.world.entity.animal.armadillo.ArmadilloAi — the roll-up
// state machine on the real brain: the MobSensor scare detector (sprinting
// players and the undead within the inflated 7x2x7 box, scanned every 5
// ticks into an 80-tick DANGER_DETECTED_RECENTLY), the PANIC activity whose
// sole behaviour is ArmadilloBallUp (the peek cycle timers included), the
// rolling-out core check, and MC's idle set.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Armadillo;
    namespace ArmadilloAi {
        void InitBrain(Armadillo& armadillo, Brain& brain);
        void UpdateActivity(Armadillo& armadillo);
    }
}
