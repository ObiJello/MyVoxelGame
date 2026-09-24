// File: src/common/sound/SoundEvents.hpp
//
// MC net.minecraft.sounds.SoundEvents + SoundEvent: the registry of sound
// event ids.
//
// A sound event here is its id string ("block.stone.break") — MC's
// SoundEvent record is (location, fixedRange) and no vanilla event in 26.3 has
// a fixed range, so the location is the whole of it. The constants below are
// MC's field names (SoundEvents.TNT_PRIMED == "entity.tnt.primed"), generated
// by tools/gen_sounds.py, so new code can name an event the way the decompile
// does and a typo fails to compile; the existing call sites that pass the id
// string directly are exactly equivalent.
//
// Ids outside this registry are legal: MC's Holder codec carries an unregistered
// event inline (ClientboundSoundPacket, SoundEvent.DIRECT_STREAM_CODEC), and
// that is how the engine's own events — "aether:block.aether_portal.ambient",
// the Hush creatures' — travel. Their sounds come from the sounds.json
// overlays under assets/sound_overlays/ (see client/sound/SoundManager).
//
// The index into the generated list is the event's registry id, which is what
// the sound packets put on the wire. Client and server are one binary, so the
// two always agree.
#pragma once

#include <string_view>

namespace Game::SoundEvents {

#define SOUND_EVENT(NAME, ID) inline constexpr const char* NAME = ID;
#include "common/sound/GeneratedSoundEvents.inc"
#undef SOUND_EVENT

    // MC's default namespace, stripped: "minecraft:block.stone.break" and
    // "block.stone.break" are the same event (Identifier.parse).
    std::string_view StripDefaultNamespace(std::string_view id);

    // The registry id of a vanilla event, or -1 for an id this registry does
    // not hold (an engine or mod event, which travels inline).
    int RegistryId(std::string_view id);

    // The id string of registry entry `registryId`; nullptr when out of range.
    const char* ByRegistryId(int registryId);

    int Count();

    // MC SoundEvent.getRange for a variable-range event (every vanilla one):
    // the distance within which the server sends it and past which the
    // client's linear attenuation has taken it to silence.
    inline float GetRange(float volume) { return volume > 1.0f ? 16.0f * volume : 16.0f; }

} // namespace Game::SoundEvents
