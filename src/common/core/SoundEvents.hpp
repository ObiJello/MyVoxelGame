// File: src/common/core/SoundEvents.hpp
//
// MC Level.playSound, reduced to the one call shape this engine needs.
//
// THERE IS NO SOUND SYSTEM. OpenAL is linked but never called, there is no
// assets/sounds/ directory and there is no sound packet — so this logs and
// returns. It exists anyway, and every MC sound site calls it, for one reason:
// when a sound system does land, the work is implementing this function, not
// rediscovering the two hundred places in MC's source that make a noise.
//
// The event names are vanilla's registry ids ("entity.tnt.primed",
// "block.anvil.land") so that wiring them up later is a table lookup rather
// than a translation exercise.
//
// This used to live in an anonymous namespace inside ItemBehaviors.cpp, which
// meant only item behaviours could reach it; blocks and entities that should
// have been making noise silently were not.
#pragma once

#include <glm/glm.hpp>

namespace Game {

    // `volume` and `pitch` default to MC's usual 1.0/1.0. They are accepted and
    // ignored today, but recording them at the call site is most of the value:
    // an anvil landing is volume 0.3 and a generic explosion is volume 4.0, and
    // those numbers are in the decompile right next to the event name.
    void PlaySound(const char* eventName, const glm::ivec3& pos,
                   float volume = 1.0f, float pitch = 1.0f);

    // Same, at a precise position rather than a block cell — entities.
    void PlaySound(const char* eventName, const glm::dvec3& pos,
                   float volume = 1.0f, float pitch = 1.0f);

} // namespace Game
