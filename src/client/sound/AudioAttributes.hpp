// File: src/client/sound/AudioAttributes.hpp
//
// The audio half of MC 26.x's environment attributes (world/attribute/
// EnvironmentAttributes): what a place SOUNDS like.
//
//   minecraft:audio/background_music  BackgroundMusic — the songs for this
//                                     place, per default / creative /
//                                     underwater (MusicManager)
//   minecraft:audio/ambient_sounds    AmbientSounds — a loop, a "mood" (the
//                                     cave sounds) and random additions
//                                     (BiomeAmbientSoundsHandler)
//   minecraft:audio/music_volume      a multiplier the music fades toward
//                                     (the pale garden's 0 — its silence)
//
// Read from the datapack JSON the engine already ships (data/<ns>/worldgen/
// biome/*.json and data/minecraft/dimension_type/*.json), under their
// "attributes" object. MC layers them — the dimension type is the base and a
// biome that sets an attribute overrides it (EnvironmentAttributeSystem,
// OVERRIDE modifier) — and so does this. A value neither sets is the
// attribute's default: no music, no ambience, full music volume.
//
// The dimensions MC does not have: the Twilight Forest and the Aether sound
// like the Overworld's dimension type (their own tracks are not shipped); the
// Hush has no dimension type file, so its base — no music, the cave mood — is
// set here and its biomes carry the rest (see their JSON).
#pragma once

#include "common/world/biome/Biomes.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Client::AudioAttributes {

    // MC sounds.Music.
    struct Music {
        std::string sound;                 // event id, default namespace stripped
        int         minDelay = 0;
        int         maxDelay = 0;
        bool        replaceCurrentMusic = false;
    };

    // MC world.attribute.BackgroundMusic.
    struct BackgroundMusic {
        std::optional<Music> defaultMusic;
        std::optional<Music> creativeMusic;
        std::optional<Music> underwaterMusic;

        // MC select(isCreative, isUnderwater); null = none.
        const Music* Select(bool isCreative, bool isUnderwater) const {
            if (isUnderwater && underwaterMusic) return &*underwaterMusic;
            if (isCreative && creativeMusic) return &*creativeMusic;
            return defaultMusic ? &*defaultMusic : nullptr;
        }
    };

    // MC AmbientMoodSettings / AmbientAdditionsSettings / AmbientSounds.
    struct AmbientMoodSettings {
        std::string sound;
        int         tickDelay = 6000;
        int         blockSearchExtent = 8;
        double      soundPositionOffset = 2.0;
    };
    struct AmbientAdditionsSettings {
        std::string sound;
        double      tickChance = 0.0;
    };
    struct AmbientSounds {
        std::string                           loop;     // "" = none
        std::optional<AmbientMoodSettings>    mood;
        std::vector<AmbientAdditionsSettings> additions;
    };

    // Main thread. Loads on first use; safe to call again.
    void Load();

    // The attribute value at a biome in a dimension (biome over dimension
    // type over the attribute's default).
    const BackgroundMusic& BackgroundMusicAt(Game::DimensionId dimension, Game::BiomeId biome);
    const AmbientSounds&   AmbientSoundsAt(Game::DimensionId dimension, Game::BiomeId biome);
    float                  MusicVolumeAt(Game::DimensionId dimension, Game::BiomeId biome);

    // The biome at a world position in the active client level (MC's
    // attribute probe samples the camera / player position). Plains when no
    // level is loaded.
    Game::BiomeId BiomeAt(const glm::dvec3& position);

} // namespace Client::AudioAttributes
