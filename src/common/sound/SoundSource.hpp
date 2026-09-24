// File: src/common/sound/SoundSource.hpp
//
// MC net.minecraft.sounds.SoundSource — the category every sound plays under,
// which is what the Music & Sounds sliders scale. The ordinal is on the wire
// (ClientboundSoundPacket writes it as an enum), and the name is the options.txt
// key suffix (`soundCategory_<name>`) and MC's getName().
#pragma once

#include <cstdint>
#include <string_view>

namespace Game {

    enum class SoundSource : uint8_t {
        Master = 0,
        Music,
        Records,
        Weather,
        Blocks,
        Hostile,
        Neutral,
        Players,
        Ambient,
        Voice,
        Ui,
        Count
    };

    inline constexpr int kSoundSourceCount = static_cast<int>(SoundSource::Count);

    // MC SoundSource.getName — also the options.txt key suffix.
    constexpr std::string_view SoundSourceName(SoundSource source) {
        switch (source) {
            case SoundSource::Master:  return "master";
            case SoundSource::Music:   return "music";
            case SoundSource::Records: return "record";
            case SoundSource::Weather: return "weather";
            case SoundSource::Blocks:  return "block";
            case SoundSource::Hostile: return "hostile";
            case SoundSource::Neutral: return "neutral";
            case SoundSource::Players: return "player";
            case SoundSource::Ambient: return "ambient";
            case SoundSource::Voice:   return "voice";
            case SoundSource::Ui:      return "ui";
            case SoundSource::Count:   break;
        }
        return "master";
    }

    // A wire byte back to a category; anything out of range is Master, which
    // only the master slider scales (the safest reading of a bad byte).
    constexpr SoundSource SoundSourceFromRaw(uint32_t raw) {
        return raw < static_cast<uint32_t>(SoundSource::Count)
            ? static_cast<SoundSource>(raw) : SoundSource::Master;
    }

} // namespace Game
