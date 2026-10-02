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
#include <string_view>
#include <vector>
#include <stdexcept>

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

        // A value is set (a removal marker is not a value).
        bool has(const DataComponentTypeBase& key) const;
        // MC DataComponentPatch's removal (`Optional.empty()`): the item's
        // default for `key` is taken AWAY on this stack — ItemStack::get
        // answers nothing instead of falling back to the prototype
        // (`stone_sword[!max_damage]`). set() replaces the marker; remove()
        // drops it (back to the default).
        void setRemoved(const DataComponentTypeBase& key);
        bool isRemoved(const DataComponentTypeBase& key) const;
        // Every component this map removes, in insertion order.
        std::vector<const DataComponentTypeBase*> removedTypes() const;
        // MC CopyComponentsFunction's per-type copy: the value `src` holds
        // for the component registered as `name` ("custom_name",
        // "banner_patterns" — the id without its namespace) replaces this
        // map's. False (nothing changed) when `src` has none. Values are
        // never mutated in place, so the two maps may share it.
        bool CopyNamed(const DataComponentMap& src, std::string_view name);
        void remove(const DataComponentTypeBase& key);
        bool empty() const { return entries.empty(); }
        size_t size() const { return entries.size(); }

        // Value equality — the component half of MC's
        // ItemStack.isSameItemSameComponents. Order-INDEPENDENT: `entries` is a
        // flat vector, so two maps holding the same components can store them
        // in different orders depending on the order `set()` was called, and a
        // positional compare would report them different (which would stop two
        // otherwise-identical stacks from merging).
        //
        // Values are compared by their serialized bytes rather than a typed
        // operator==, because entries are type-erased `shared_ptr<void>` and
        // only the DataComponentType knows how to interpret them. Every
        // registered component currently has a network codec
        // (DataComponents.cpp), so this is a total comparison; a future
        // codec-less component (networkId == 0) would serialize to nothing and
        // compare equal — see the guard in the .cpp.
        bool Equals(const DataComponentMap& other) const;

        // MC DataComponentExactPredicate.test: every component in THIS map is
        // present on the target with an equal value — the target's own
        // override first, then `targetDefaults` (the item's prototype, MC's
        // PatchedDataComponentMap fallback). A trade's cost ("a water
        // bottle": potion_contents {potion: water}) is the one reader.
        bool IsExactSubsetOf(const DataComponentMap& target,
                             const DataComponentMap* targetDefaults) const;

        // ── Network codec — mirrors MC DataComponentPatch.STREAM_CODEC
        // (DataComponentPatch.java:31-106): VarInt addedCount, VarInt
        // removedCount, then per added entry (VarInt networkId, payload),
        // then per removed entry its VarInt networkId. Only entries whose
        // type has a network codec (networkId != 0) are written.
        void Serialize(Network::PacketBuffer& buffer) const;
        // Throws std::runtime_error on an unknown networkId (protocol error —
        // both endpoints run the same binary).
        static DataComponentMap Deserialize(Network::PacketReader& reader);

    private:
        struct Entry {
            const DataComponentTypeBase* type;
            std::shared_ptr<void>        value; // type-erased; only set/get know how to cast
                                                // null: the component is REMOVED (setRemoved)
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
                if (!e.value) return std::nullopt;   // removed
                return *static_cast<const T*>(e.value.get());
            }
        }
        return std::nullopt;
    }

} // namespace Game
