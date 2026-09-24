// File: src/common/sound/SoundEvents.cpp
#include "common/sound/SoundEvents.hpp"

#include <string>
#include <unordered_map>

namespace Game::SoundEvents {

    namespace {

        constexpr const char* kIds[] = {
#define SOUND_EVENT(NAME, ID) ID,
#include "common/sound/GeneratedSoundEvents.inc"
#undef SOUND_EVENT
        };
        constexpr int kCount = static_cast<int>(sizeof(kIds) / sizeof(kIds[0]));

        // Built once, read-only afterwards: a function-local static is
        // initialised thread-safely, and every thread that plays a sound may
        // be the first to ask.
        const std::unordered_map<std::string_view, int>& Index() {
            static const std::unordered_map<std::string_view, int> index = [] {
                std::unordered_map<std::string_view, int> m;
                m.reserve(static_cast<size_t>(kCount) * 2);
                for (int i = 0; i < kCount; ++i) m.emplace(kIds[i], i);
                return m;
            }();
            return index;
        }

    } // namespace

    std::string_view StripDefaultNamespace(std::string_view id) {
        constexpr std::string_view kPrefix = "minecraft:";
        if (id.substr(0, kPrefix.size()) == kPrefix) id.remove_prefix(kPrefix.size());
        return id;
    }

    int RegistryId(std::string_view id) {
        const auto& index = Index();
        const auto it = index.find(StripDefaultNamespace(id));
        return it == index.end() ? -1 : it->second;
    }

    const char* ByRegistryId(int registryId) {
        return (registryId >= 0 && registryId < kCount) ? kIds[registryId] : nullptr;
    }

    int Count() { return kCount; }

} // namespace Game::SoundEvents
