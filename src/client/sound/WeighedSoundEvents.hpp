// File: src/client/sound/WeighedSoundEvents.hpp
//
// MC client.resources.sounds.Sound + client.sounds.Weighted +
// client.sounds.WeighedSoundEvents: what sounds.json says an event is.
//
//   Sound               one entry of an event's "sounds" list — a file (or,
//                       with "type": "event", another event), its volume,
//                       pitch, weight, stream / preload flags and
//                       attenuation_distance.
//   WeighedSoundEvents  the event: its entries, picked by weight. An "event"
//                       entry weighs as much as the whole event it names and,
//                       when picked, resolves to one of that event's files
//                       with the two volumes and pitches multiplied (MC
//                       SoundManager.Preparations's SOUND_EVENT Weighted).
//
// MC's volume/pitch are SampledFloats; sounds.json only ever writes
// constants (ConstantFloat) and an event reference multiplies two of them
// (MultipliedFloats of constants), so a float is the whole value here.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game { class JavaRandom; }

namespace Client {

    class SoundEngine;

    struct Sound {
        enum class Type : uint8_t { File, SoundEvent };

        std::string location;               // "minecraft:block/stone/break1" (a file) or an event id
        std::string path;                   // the resolved .ogg to open (files only)
        float       volume = 1.0f;
        float       pitch  = 1.0f;
        int         weight = 1;
        Type        type   = Type::File;
        bool        stream = false;
        bool        preload = false;
        int         attenuationDistance = 16;

        // MC Sound.getAttenuationDistance(volume): louder sounds carry further.
        float GetAttenuationDistance(float instanceVolume) const {
            return (instanceVolume > 1.0f ? instanceVolume : 1.0f) * static_cast<float>(attenuationDistance);
        }
    };

    class SoundRegistry;

    class WeighedSoundEvents {
    public:
        WeighedSoundEvents() = default;
        explicit WeighedSoundEvents(std::string subtitle) : m_subtitle(std::move(subtitle)) {}

        // MC getWeight: the sum of the entries' weights (an event reference
        // weighing its whole target event).
        int GetWeight(const SoundRegistry& registry, int depth = 0) const;

        // MC getSound(random): one file into `out`, or false (MC EMPTY_SOUND)
        // when the event has nothing to give.
        bool GetSound(Game::JavaRandom& random, const SoundRegistry& registry, Sound& out,
                      int depth = 0) const;

        void AddSound(Sound sound) { m_list.push_back(std::move(sound)); }

        // MC preloadIfRequired → SoundEngine.requestPreload for `preload: true`.
        void CollectPreloads(const SoundRegistry& registry, std::vector<std::string>& out,
                             int depth = 0) const;

        const std::string& Subtitle() const { return m_subtitle; }
        bool Empty() const { return m_list.empty(); }

    private:
        std::vector<Sound> m_list;
        std::string        m_subtitle;
    };

    // The event map (MC SoundManager.registry): event id → WeighedSoundEvents.
    // Keys are the ids WITHOUT the default namespace ("block.stone.break"),
    // other namespaces kept ("aether:block.aether_portal.ambient").
    class SoundRegistry {
    public:
        const WeighedSoundEvents* Find(const std::string& id) const;
        WeighedSoundEvents* Find(const std::string& id);
        WeighedSoundEvents& Replace(const std::string& id, std::string subtitle);
        WeighedSoundEvents& GetOrCreate(const std::string& id, std::string subtitle);
        size_t Size() const { return m_events.size(); }
        void Clear() { m_events.clear(); }
        const std::unordered_map<std::string, WeighedSoundEvents>& All() const { return m_events; }

        // "minecraft:x" and "x" are one key.
        static std::string Normalize(const std::string& id);

    private:
        std::unordered_map<std::string, WeighedSoundEvents> m_events;
    };

} // namespace Client
