// File: src/common/data/DataComponentMap.hpp
//
// Mirrors net/minecraft/core/component/DataComponentMap.java — a map from
// DataComponentType<T> keys to type-erased values, with type-safe set/get.
//
// In MC this is a Java interface backed by a Reference2ObjectMap. We use a
// flat std::vector<Entry> because the typical per-stack component count is
// 0–3 (most stacks are vanilla and have no overrides; even an enchanted item
// rarely has more than two components). For tiny N, vector-with-linear-scan
// beats hash-map on cache behaviour and avoids per-stack allocations.
//
// MC's actual ItemStack uses PatchedDataComponentMap (base + patch). Phase 1
// keeps it simple with a flat map; a patch impl can swap in later as a
// memory optimization without changing call sites.
#pragma once

#include "DataComponentType.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace Game {

    class DataComponentMap {
    public:
        DataComponentMap() = default;

        // Set the value for the given component key. Replaces any existing entry.
        template<typename T>
        void set(const DataComponentType<T>& key, T value);

        // Get the value for the given component key, or std::nullopt if absent.
        template<typename T>
        std::optional<T> get(const DataComponentType<T>& key) const;

        bool has(const DataComponentTypeBase& key) const;
        void remove(const DataComponentTypeBase& key);
        bool empty() const { return entries.empty(); }
        size_t size() const { return entries.size(); }

    private:
        struct Entry {
            const DataComponentTypeBase* type;
            std::shared_ptr<void>        value; // type-erased; only set/get know how to cast
        };
        std::vector<Entry> entries;
    };

    // ── Templated definitions (kept in header so users don't need a TU per type) ──
    template<typename T>
    void DataComponentMap::set(const DataComponentType<T>& key, T value) {
        for (auto& e : entries) {
            if (e.type == &key) {
                e.value = std::make_shared<T>(std::move(value));
                return;
            }
        }
        entries.push_back({&key, std::make_shared<T>(std::move(value))});
    }

    template<typename T>
    std::optional<T> DataComponentMap::get(const DataComponentType<T>& key) const {
        for (const auto& e : entries) {
            if (e.type == &key) {
                return *static_cast<const T*>(e.value.get());
            }
        }
        return std::nullopt;
    }

} // namespace Game
