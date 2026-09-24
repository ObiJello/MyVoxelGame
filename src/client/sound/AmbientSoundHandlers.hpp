// File: src/client/sound/AmbientSoundHandlers.hpp
//
// MC LocalPlayer's ambientSoundHandlers, and the rest of the local player's
// surroundings that only its own client hears:
//
//   UnderwaterAmbientSoundHandler  the underwater additions (1% / 0.1% /
//                                  0.01% a tick), and — from
//                                  LocalPlayer.updateIsUnderwater — the
//                                  enter / exit sounds and the fading
//                                  underwater loop (UnderLiquidAmbientSound-
//                                  Instance)
//   BubbleColumnAmbientSoundHandler  the whirlpool / upwards rush on entering
//                                  a bubble column
//   BiomeAmbientSoundsHandler      the AMBIENT_SOUNDS attribute where the
//                                  player stands: the biome loop (40-tick
//                                  cross-fades between biomes), the random
//                                  additions, and the "mood" — the cave
//                                  sounds that build in the dark
//   ClientLevel.tickWeatherEffects the rain on the ground around the camera
//                                  (WEATHER_RAIN / _ABOVE) — silent until the
//                                  engine rains (EnvironmentState::RainLevel)
//
// The Hush's stillness (its dimension-wide hush, server/level/HushStillness)
// is heard here as silence: while it holds, StillnessGain eases to 0 over two
// seconds — the biome loop fades out, no additions or mood sound starts, and
// MusicManager fades the music with it — and eases back when it lifts.
//
// Ticked by SoundHost on the unpaused world tick; the local player's own
// movement sounds ride the same tick (LocalPlayerSounds).
#pragma once

#include "client/sound/SoundHost.hpp"

namespace Client::AmbientSounds {

    void Tick(const SoundHost::TickContext& context);
    // A new world / no world: every loop stops, every counter restarts.
    void Reset();

    // MC LocalPlayer.getCurrentMood → BiomeAmbientSoundsHandler.getMoodiness:
    // 0..1, how close the next cave sound is. 0 with no world.
    float GetCurrentMood();

    // 1 normally, easing to 0 while the Hush's stillness holds (see above).
    float StillnessGain();

} // namespace Client::AmbientSounds
