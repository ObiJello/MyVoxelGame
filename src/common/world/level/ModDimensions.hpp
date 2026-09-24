// File: src/common/world/level/ModDimensions.hpp
//
// Per-world switches for the two ported mod dimensions (/gamerule
// twilight_forest, /gamerule aether). Common code reads them because the
// Aether portal is lit from an item behaviour and the Twilight pool from the
// item-entity tick; the server owns them (IntegratedServer::SetModDimension
// Enabled writes both the flag and the level.dat value).
//
// Off means the dimension cannot be ENTERED: its portal will not light, an
// existing portal of its kind stops sending anyone in, and /dimension
// refuses it. Leaving always works — a player standing in the Aether when
// the rule goes off can still walk back through a portal or fall out.
#pragma once

#include "DimensionId.hpp"

#include <atomic>

namespace Game::ModDimensions {

    namespace detail {
        inline std::atomic<bool> g_twilightForest{true};
        inline std::atomic<bool> g_aether{true};
    }

    // True for every dimension that is not a switchable mod dimension.
    inline bool Enabled(DimensionId d) {
        switch (d) {
            case DimensionId::TwilightForest: return detail::g_twilightForest.load(std::memory_order_relaxed);
            case DimensionId::Aether:         return detail::g_aether.load(std::memory_order_relaxed);
            default:                          return true;
        }
    }

    inline void SetEnabled(DimensionId d, bool on) {
        if (d == DimensionId::TwilightForest) detail::g_twilightForest.store(on, std::memory_order_relaxed);
        if (d == DimensionId::Aether)         detail::g_aether.store(on, std::memory_order_relaxed);
    }

} // namespace Game::ModDimensions
