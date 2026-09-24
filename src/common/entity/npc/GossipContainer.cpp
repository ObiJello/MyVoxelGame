// File: src/common/entity/npc/GossipContainer.cpp
#include "common/entity/npc/GossipContainer.hpp"

#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace Game {

    namespace {
        // MC GossipType constructor arguments.
        constexpr std::array<GossipTypeInfo, kGossipTypeCount> kTypes = {{
            { "major_negative", -5, 100, 10, 10 },
            { "minor_negative", -1, 200, 20, 20 },
            { "minor_positive",  1,  25,  1,  5 },
            { "major_positive",  5,  20,  0, 20 },
            { "trading",         1,  25,  2, 20 },
        }};
    }

    const GossipTypeInfo& GetGossipTypeInfo(GossipType type) {
        const size_t i = static_cast<size_t>(type);
        return kTypes[i < kTypes.size() ? i : 0];
    }

    bool ParseGossipType(std::string_view id, GossipType& out) {
        for (size_t i = 0; i < kTypes.size(); ++i) {
            if (id == kTypes[i].id) { out = static_cast<GossipType>(i); return true; }
        }
        return false;
    }

    bool GossipContainer::IsEmpty(const Values& v) {
        for (int x : v) if (x != 0) return false;
        return true;
    }

    void GossipContainer::Decay() {
        // MC EntityGossips.decay: each value loses its type's decayPerDay and
        // is forgotten below 2; an emptied target goes with it.
        for (auto it = m_gossips.begin(); it != m_gossips.end();) {
            for (int t = 0; t < kGossipTypeCount; ++t) {
                int& v = it->second[static_cast<size_t>(t)];
                if (v == 0) continue;
                const int next = v - kTypes[static_cast<size_t>(t)].decayPerDay;
                v = next < kDiscardThreshold ? 0 : next;
            }
            if (IsEmpty(it->second)) it = m_gossips.erase(it);
            else ++it;
        }
    }

    std::vector<GossipContainer::Entry> GossipContainer::Unpack() const {
        std::vector<Entry> out;
        for (const auto& [target, values] : m_gossips) {
            for (int t = 0; t < kGossipTypeCount; ++t) {
                const int v = values[static_cast<size_t>(t)];
                if (v != 0) out.push_back(Entry{ target, static_cast<GossipType>(t), v });
            }
        }
        return out;
    }

    void GossipContainer::Put(const Entry& entry) {
        if (entry.value <= 0) return;   // MC ExtraCodecs.POSITIVE_INT
        m_gossips[entry.target][static_cast<size_t>(entry.type)] = entry.value;
    }

    int GossipContainer::TransferFrom(const GossipContainer& source, JavaRandom& random, int maxCount) {
        // MC selectGossipsForTransfer: a weighted pick (by |value * weight|)
        // repeated maxCount times INTO AN IDENTITY SET — a gossip drawn twice
        // is transferred once.
        const std::vector<Entry> entries = source.Unpack();
        if (entries.empty()) return 0;
        std::vector<int> ranges(entries.size());
        int rangesEnd = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            rangesEnd += std::abs(entries[i].value * kTypes[static_cast<size_t>(entries[i].type)].weight);
            ranges[i] = rangesEnd - 1;
        }
        std::set<size_t> picked;
        for (int i = 0; i < maxCount; ++i) {
            const int choice = random.NextInt(rangesEnd);
            // Arrays.binarySearch then the insertion point: the first range
            // whose end is >= choice.
            const auto it = std::lower_bound(ranges.begin(), ranges.end(), choice);
            picked.insert(static_cast<size_t>(it - ranges.begin()));
        }
        for (size_t idx : picked) {
            const Entry& g = entries[idx];
            const int decayed = g.value - kTypes[static_cast<size_t>(g.type)].decayPerTransfer;
            if (decayed >= kDiscardThreshold) {
                // mergeValuesForTransfer: keep the larger.
                int& here = m_gossips[g.target][static_cast<size_t>(g.type)];
                here = std::max(here, decayed);
            }
        }
        return static_cast<int>(picked.size());
    }

    int GossipContainer::GetReputation(const Uuid& target,
                                       const std::function<bool(GossipType)>& types) const {
        auto it = m_gossips.find(target);
        if (it == m_gossips.end()) return 0;
        int sum = 0;
        for (int t = 0; t < kGossipTypeCount; ++t) {
            const GossipType type = static_cast<GossipType>(t);
            if (types && !types(type)) continue;
            sum += it->second[static_cast<size_t>(t)] * kTypes[static_cast<size_t>(t)].weight;
        }
        return sum;
    }

    void GossipContainer::Add(const Uuid& target, GossipType type, int amount) {
        // MC add: mergeValuesForAddition (a sum past max keeps the larger of
        // max and the old value), then makeSureValueIsntTooLowOrTooHigh.
        Values& values = m_gossips[target];
        int& v = values[static_cast<size_t>(type)];
        const int max = kTypes[static_cast<size_t>(type)].max;
        if (v == 0) {
            v = amount;
        } else {
            const int sum = v + amount;
            v = sum > max ? std::max(max, v) : sum;
        }
        if (v > max) v = max;
        if (v < kDiscardThreshold) v = 0;
        if (IsEmpty(values)) m_gossips.erase(target);
    }

    void GossipContainer::Remove(const Uuid& target, GossipType type) {
        auto it = m_gossips.find(target);
        if (it == m_gossips.end()) return;
        it->second[static_cast<size_t>(type)] = 0;
        if (IsEmpty(it->second)) m_gossips.erase(it);
    }

    void GossipContainer::Remove(GossipType type) {
        for (auto it = m_gossips.begin(); it != m_gossips.end();) {
            it->second[static_cast<size_t>(type)] = 0;
            if (IsEmpty(it->second)) it = m_gossips.erase(it);
            else ++it;
        }
    }

    void GossipContainer::PutAll(const GossipContainer& other) {
        for (const auto& [target, values] : other.m_gossips) {
            Values& here = m_gossips[target];
            for (int t = 0; t < kGossipTypeCount; ++t) {
                if (values[static_cast<size_t>(t)] != 0) here[static_cast<size_t>(t)] = values[static_cast<size_t>(t)];
            }
        }
    }

} // namespace Game
