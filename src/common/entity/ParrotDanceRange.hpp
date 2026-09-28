// File: src/common/entity/ParrotDanceRange.hpp
//
// How near a playing jukebox a parrot dances — one constant for every place
// that asks: the song-start notification radius (the client's
// notifyNearbyEntities), the ground parrot's range check and its on-arrival
// search, and the shoulder parrots (client ShoulderParrots).
//
// DELIBERATE DEVIATION (user request): MC uses 3.46 blocks for the parrot
// (Parrot.aiStep's closerToCenterThan) and inflates the jukebox cell by 3 for
// the notification; here both are 8 blocks.
#pragma once

namespace Game {
    inline constexpr double kParrotDanceRange = 8.0;
}
