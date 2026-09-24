// File: src/common/entity/npc/GossipContainer.hpp
//
// MC net.minecraft.world.entity.ai.gossip.{GossipContainer, GossipType} — a
// villager's opinion of everyone it has heard about, and the reputation that
// opinion adds up to.
//
// Each target (a UUID — a player, usually) carries up to five counters, one
// per gossip type. Reputation is the weighted sum (MAJOR_NEGATIVE counts -5
// per point, TRADING +1…), and it is what bends a trader's prices: a player a
// village likes pays less, one who hit a villager pays more. Gossip decays
// daily and spreads when two villagers meet (Villager.gossip → transferFrom),
// losing a little on the way.
//
// Saved as MC's GossipContainer.CODEC: a list of {Target:[I;…], Type:"…",
// Value:int} (VillagerNbt.cpp).
#pragma once

#include "common/core/Uuid.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string_view>
#include <vector>

namespace Game {

    class JavaRandom;

    // MC GossipType, declaration order.
    enum class GossipType : uint8_t {
        MajorNegative, MinorNegative, MinorPositive, MajorPositive, Trading,
        Count,
    };
    inline constexpr int kGossipTypeCount = static_cast<int>(GossipType::Count);

    struct GossipTypeInfo {
        const char* id;               // "major_negative"
        int         weight;           // reputation per point
        int         max;              // cap per target
        int         decayPerDay;      // GossipContainer.decay
        int         decayPerTransfer; // lost when passed on
    };
    const GossipTypeInfo& GetGossipTypeInfo(GossipType type);
    bool ParseGossipType(std::string_view id, GossipType& out);

    class GossipContainer {
    public:
        // MC GossipContainer.DISCARD_THRESHOLD — a value below 2 is forgotten.
        static constexpr int kDiscardThreshold = 2;

        // One flattened entry (MC GossipEntry) — the save form.
        struct Entry {
            Uuid       target{};
            GossipType type = GossipType::Trading;
            int        value = 0;
        };

        // MC decay — once a day (Villager.maybeDecayGossip).
        void Decay();
        // MC transferFrom: up to `maxCount` weighted draws from `source`, each
        // landing here less its type's decayPerTransfer. Returns how many were
        // drawn (the villager re-prices on a non-zero return).
        int TransferFrom(const GossipContainer& source, JavaRandom& random, int maxCount);
        // MC getReputation(target, types).
        int GetReputation(const Uuid& target, const std::function<bool(GossipType)>& types = {}) const;
        // MC add / remove.
        void Add(const Uuid& target, GossipType type, int amount);
        void Remove(const Uuid& target, GossipType type, int amount) { Add(target, type, -amount); }
        void Remove(const Uuid& target, GossipType type);
        void Remove(GossipType type);
        void Clear() { m_gossips.clear(); }
        void PutAll(const GossipContainer& other);

        // MC unpack() — every (target, type, value), for saving.
        std::vector<Entry> Unpack() const;
        // The load side (MC's private list constructor).
        void Put(const Entry& entry);

        bool Empty() const { return m_gossips.empty(); }

    private:
        // Per target, one value per type (0 = absent). An ordered map keeps
        // Unpack — and therefore TransferFrom's weighted walk — deterministic.
        using Values = std::array<int, kGossipTypeCount>;
        static bool IsEmpty(const Values& v);
        std::map<Uuid, Values> m_gossips;
    };

} // namespace Game
