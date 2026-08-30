// File: src/common/core/SoundEvents.cpp
#include "common/core/SoundEvents.hpp"

#include "common/core/Log.hpp"

#include <cmath>

namespace Game {

    void PlaySound(const char* eventName, const glm::ivec3& pos,
                   float volume, float pitch) {
        // TODO(sounds): broadcast SoundEventS2CPacket(eventName, pos, volume,
        //               pitch) to every player within 16 * volume blocks, which
        //               is MC's ServerLevel.playSound range rule.
        Log::Debug("[Sound] %s at (%d,%d,%d) vol=%.2f pitch=%.2f — no sound system yet",
                   eventName, pos.x, pos.y, pos.z, volume, pitch);
    }

    void PlaySound(const char* eventName, const glm::dvec3& pos,
                   float volume, float pitch) {
        PlaySound(eventName,
                  glm::ivec3(static_cast<int>(std::floor(pos.x)),
                             static_cast<int>(std::floor(pos.y)),
                             static_cast<int>(std::floor(pos.z))),
                  volume, pitch);
    }

} // namespace Game
