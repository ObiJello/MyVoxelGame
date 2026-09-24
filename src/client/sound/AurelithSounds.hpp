// File: src/client/sound/AurelithSounds.hpp
//
// What Aurelith, the Lantern City, sounds like from inside it (docs/the-hush.md,
// "City sound"): a structure-scoped ambience that runs beside the biome's
// (AmbientSoundHandlers), because the city is not a biome — it stands on the
// Hush's meadows, steppe or barrens, and must sound the same on all three.
//
//   the Heart's hum    looping MC TickableSoundInstances bound to each nearby
//                      resonance_engine block entity: a deep drone that grows
//                      as you approach (linear attenuation over 40 blocks);
//                      once the Chord is sung a second, brighter layer swells
//                      in (AurelithState::Voice) and the pitch climbs with the
//                      rings' pace; under the Undersong a sour third layer
//                      wobbles against it (AurelithState::Sourness).
//   the gates' thrum   a loop on each nearby voice_beacon's lens, pitched by
//                      its voice (AurelithState::BeaconVoice; the block's
//                      facing for a city with no record), louder once lit.
//   the wind           high on the Conductor's Spire terrace (inside the walls,
//                      more than ~52 blocks over the street): a relative wind
//                      bed that rises with height, and gusts round you.
//   the empty choir    inside the walls, down in the streets: now and then a
//                      faint snatch of the F# Chord — two to four notes on a
//                      flute, glass, a bell — from somewhere 6-14 blocks off,
//                      drifting as it sings. Dormant, sparse and unresolved;
//                      awakened, fuller and resolving home.
//   the music          inside a known city's walls the situational music is
//                      the city's own (CityMusic): a dormant track, silence
//                      while the Chord is being sung, the Unsung's fight, and
//                      the awakened theme — reached by fading, never cutting
//                      (MusicGain feeds MusicManager's fadePlaying).
//
// The river's lapping is a block animateTick sound (BlockAmbientSounds,
// resonant_water), heard only near its surface.
//
// Everything reads the city records in Client::AurelithState on the level's
// game time, so the swells land when the server's awakening does. Nearby
// block entities are found by a bounded chunk walk once a second (the nearest
// kMaxHearts engines and kMaxBeacons beacons keep a loop), so nothing scans per
// frame. The Hush's stillness hushes the choir and the wind and lowers the
// hum (AmbientSounds::StillnessGain).
//
// Main thread; ticked from SoundHost on the unpaused world tick.
#pragma once

#include "client/sound/AudioAttributes.hpp"
#include "client/sound/SoundHost.hpp"

#include <optional>
#include <string>

namespace Client::AurelithSounds {

    void Tick(const SoundHost::TickContext& context);
    // A new world / dimension / session: every loop stops, every timer resets.
    void Reset();

    // The city's music at the camera, when the camera is inside a known
    // city's walls: `inCity` true and `music` the track (none while the Chord
    // is being sung — the city falls silent for it). `inCity` false: the
    // biome decides.
    struct CityMusicChoice {
        bool inCity = false;
        std::optional<AudioAttributes::Music> music;
    };
    CityMusicChoice CityMusic(const SoundHost::TickContext& context);

    // A multiplier on the music volume MusicManager fades toward: 0 when the
    // song now playing does not belong where the camera is (a biome song in
    // an awakened city, anything while the Chord is sung, the fight's music
    // after the fight) so it fades out and the city's track can follow; 1
    // otherwise. `currentEvent` is the playing song's event id.
    float MusicGain(const SoundHost::TickContext& context, const std::string& currentEvent);

} // namespace Client::AurelithSounds
