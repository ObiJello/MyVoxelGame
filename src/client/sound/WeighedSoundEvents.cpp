// File: src/client/sound/WeighedSoundEvents.cpp
#include "client/sound/WeighedSoundEvents.hpp"

#include "common/core/JavaRandom.hpp"

namespace Client {

    namespace {
        // An event that references itself (directly or round a loop) would
        // recurse forever; MC trusts the resource pack, this does not.
        constexpr int kMaxEventDepth = 16;
    }

    int WeighedSoundEvents::GetWeight(const SoundRegistry& registry, int depth) const {
        if (depth > kMaxEventDepth) return 0;
        int sum = 0;
        for (const Sound& s : m_list) {
            if (s.type == Sound::Type::File) {
                sum += s.weight;
            } else if (const WeighedSoundEvents* target = registry.Find(s.location)) {
                sum += target->GetWeight(registry, depth + 1);
            }
        }
        return sum;
    }

    bool WeighedSoundEvents::GetSound(Game::JavaRandom& random, const SoundRegistry& registry,
                                      Sound& out, int depth) const {
        if (depth > kMaxEventDepth) return false;
        const int weight = GetWeight(registry, depth);
        if (m_list.empty() || weight == 0) return false;
        int index = random.NextInt(weight);
        for (const Sound& s : m_list) {
            const WeighedSoundEvents* target = nullptr;
            int w = 0;
            if (s.type == Sound::Type::File) {
                w = s.weight;
            } else {
                target = registry.Find(s.location);
                w = target ? target->GetWeight(registry, depth + 1) : 0;
            }
            index -= w;
            if (index >= 0) continue;

            if (s.type == Sound::Type::File) {
                out = s;
                return true;
            }
            // MC's SOUND_EVENT Weighted.getSound: the target's pick, with the
            // two volumes / pitches multiplied, this entry's weight, either
            // side's stream flag, and the target's preload / attenuation.
            Sound wrapped;
            if (!target || !target->GetSound(random, registry, wrapped, depth + 1)) return false;
            out = wrapped;
            out.volume = wrapped.volume * s.volume;
            out.pitch  = wrapped.pitch * s.pitch;
            out.weight = s.weight;
            out.type   = Sound::Type::File;
            out.stream = wrapped.stream || s.stream;
            return true;
        }
        return false;
    }

    void WeighedSoundEvents::CollectPreloads(const SoundRegistry& registry, std::vector<std::string>& out,
                                             int depth) const {
        if (depth > kMaxEventDepth) return;
        for (const Sound& s : m_list) {
            if (s.type == Sound::Type::File) {
                if (s.preload && !s.path.empty()) out.push_back(s.path);
            } else if (const WeighedSoundEvents* target = registry.Find(s.location)) {
                target->CollectPreloads(registry, out, depth + 1);
            }
        }
    }

    std::string SoundRegistry::Normalize(const std::string& id) {
        static const std::string kPrefix = "minecraft:";
        if (id.compare(0, kPrefix.size(), kPrefix) == 0) return id.substr(kPrefix.size());
        return id;
    }

    const WeighedSoundEvents* SoundRegistry::Find(const std::string& id) const {
        auto it = m_events.find(id);
        if (it == m_events.end() && id.compare(0, 10, "minecraft:") == 0) it = m_events.find(id.substr(10));
        return it == m_events.end() ? nullptr : &it->second;
    }

    WeighedSoundEvents* SoundRegistry::Find(const std::string& id) {
        return const_cast<WeighedSoundEvents*>(static_cast<const SoundRegistry*>(this)->Find(id));
    }

    WeighedSoundEvents& SoundRegistry::Replace(const std::string& id, std::string subtitle) {
        WeighedSoundEvents& e = m_events[Normalize(id)];
        e = WeighedSoundEvents(std::move(subtitle));
        return e;
    }

    WeighedSoundEvents& SoundRegistry::GetOrCreate(const std::string& id, std::string subtitle) {
        const std::string key = Normalize(id);
        auto it = m_events.find(key);
        if (it != m_events.end()) return it->second;
        return m_events.emplace(key, WeighedSoundEvents(std::move(subtitle))).first->second;
    }

} // namespace Client
