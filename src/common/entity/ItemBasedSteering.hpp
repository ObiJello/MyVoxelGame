// File: src/common/entity/ItemBasedSteering.hpp
//
// MC net.minecraft.world.entity.ItemBasedSteering + ItemSteerable — the
// pig's and the strider's "food on a stick" steering. The rider holds the
// rod (carrot / warped fungus on a stick) and the mount walks where the
// rider looks; using the rod (FoodOnAStickItem.use, server side) calls
// boost(): a burst of 140..980 ticks whose speed factor swells along half a
// sine, 1 → 2.15 → 1.
//
// MC keeps the burst's LENGTH in synched entity data (DATA_BOOST_TIME) and
// the progress (boosting / boostTime) locally on each side: the server rolls
// the length and starts its own copy; a client starts its copy when the new
// length arrives (onSyncedDataUpdated → onSynced). The steering client is
// the one that simulates the mount, so the client copy is what speeds it up.
// Here the length rides the owning mob's synced data int (see Pig /
// Strider: Mob::GetCarriedBlockRaw's SetEntityData field, which the tracker
// resends on change — MC's entityData.set semantics).
#pragma once

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"

#include <cmath>

namespace Game {

    // MC ItemSteerable — a mount the matching rod can boost.
    class ItemSteerable {
    public:
        virtual ~ItemSteerable() = default;
        // MC boost(): start a burst; false while one is already running.
        virtual bool Boost() = 0;
    };

    class ItemBasedSteering {
    public:
        // MC MIN_BOOST_TIME / MAX_BOOST_TIME (boost rolls nextInt(841) + 140,
        // so a burst actually lasts up to 980 ticks — MC's own numbers).
        static constexpr int kMinBoostTime = 140;
        static constexpr int kMaxBoostTime = 700;

        // MC onSynced: a client saw a new burst length.
        void OnSynced() {
            m_boosting = true;
            m_boostTime = 0;
        }

        // MC boost(random) — server side (the rod's use).
        bool Boost(JavaRandom& random) {
            if (m_boosting) return false;
            m_boosting = true;
            m_boostTime = 0;
            m_boostTimeTotal = random.NextInt(841) + kMinBoostTime;
            return true;
        }

        // MC tickBoost — once per ridden tick (tickRidden).
        void TickBoost() {
            if (m_boosting && m_boostTime++ > m_boostTimeTotal) m_boosting = false;
        }

        // MC boostFactor: 1 + 1.15 * sin(boostTime / boostTimeTotal * PI).
        float BoostFactor() const {
            if (!m_boosting || m_boostTimeTotal <= 0) return 1.0f;
            return 1.0f + 1.15f * static_cast<float>(std::sin(static_cast<double>(
                              static_cast<float>(m_boostTime) / static_cast<float>(m_boostTimeTotal) *
                              Mth::kPi)));
        }

        // The synched DATA_BOOST_TIME value.
        int BoostTimeTotal() const { return m_boostTimeTotal; }

        // Client: the synched value arrived. MC fires onSyncedDataUpdated
        // only when the value CHANGED (entityData.set of an equal value is
        // not dirty), so an unchanged copy starts nothing.
        void ApplySyncedBoostTimeTotal(int total) {
            if (total == m_boostTimeTotal) return;
            m_boostTimeTotal = total;
            OnSynced();
        }

        bool IsBoosting() const { return m_boosting; }

    private:
        bool m_boosting = false;
        int  m_boostTime = 0;
        int  m_boostTimeTotal = 0;   // DATA_BOOST_TIME
    };

} // namespace Game
