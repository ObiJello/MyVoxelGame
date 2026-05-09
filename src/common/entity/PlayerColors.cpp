// File: src/common/entity/PlayerColors.cpp
#include "PlayerColors.hpp"
#include <algorithm>
#include <cctype>

namespace Game {

    const PlayerColorEntry& LookupPlayerColor(PlayerColorId id) {
        const auto idx = static_cast<size_t>(id);
        const auto count = sizeof(kPlayerColorTable) / sizeof(kPlayerColorTable[0]);
        if (idx >= count) return kPlayerColorTable[0]; // fall back to Default
        return kPlayerColorTable[idx];
    }

    PlayerColorId ParsePlayerColorName(const std::string& slug) {
        if (slug.empty()) return PlayerColorId::Default;
        std::string lower;
        lower.reserve(slug.size());
        for (char c : slug) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        for (const auto& e : kPlayerColorTable) {
            if (lower == e.slug) return e.id;
        }
        return PlayerColorId::Default;
    }

} // namespace Game
