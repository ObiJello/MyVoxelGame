// File: src/common/inventory/ContainerData.hpp
//
// Mirrors net.minecraft.world.inventory.ContainerData — the small array of ints
// a menu publishes alongside its slots so the client can draw state that isn't
// an item: a furnace's burn timer and cook progress, a brewing stand's fuel, an
// enchanting table's three offers.
//
// Why it exists as its own channel rather than riding the slot sync: these
// values change EVERY TICK while a furnace burns, and they belong to the block
// entity, not to any stack. MC keeps them separate for the same reason and
// diffs them in AbstractContainerMenu.broadcastChanges, sending one
// ClientboundContainerSetDataPacket per changed index.
//
// The menu does not own the storage. A furnace menu's data slots read straight
// through to the block entity's live counters (MC does this with an anonymous
// ContainerData in AbstractFurnaceBlockEntity), so the server never has to copy
// state forward and the numbers cannot go stale. SimpleContainerData is the
// fallback for menus with no block entity behind them, and is what the CLIENT
// always uses — it receives values over the wire and has nothing to read from.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace Game {

    class ContainerData {
    public:
        virtual ~ContainerData() = default;

        virtual int  Count() const = 0;
        virtual int  Get(int index) const = 0;
        virtual void Set(int index, int value) = 0;
    };

    // A plain int array. Used by the client for every menu (it only ever
    // receives values) and by server menus with no block entity behind them.
    class SimpleContainerData : public ContainerData {
    public:
        explicit SimpleContainerData(int size) : m_values(static_cast<size_t>(size), 0) {}

        int Count() const override { return static_cast<int>(m_values.size()); }
        int Get(int index) const override {
            return (index >= 0 && index < Count()) ? m_values[static_cast<size_t>(index)] : 0;
        }
        void Set(int index, int value) override {
            if (index >= 0 && index < Count()) m_values[static_cast<size_t>(index)] = value;
        }

    private:
        std::vector<int> m_values;
    };

    // Reads and writes live fields elsewhere — MC's anonymous ContainerData
    // pattern, where each index is a getter/setter pair over the block entity's
    // own counters. Keeps the menu a view rather than a copy.
    class DelegatingContainerData : public ContainerData {
    public:
        using Getter = std::function<int()>;
        using Setter = std::function<void(int)>;

        struct Entry { Getter get; Setter set; };

        explicit DelegatingContainerData(std::vector<Entry> entries)
            : m_entries(std::move(entries)) {}

        int Count() const override { return static_cast<int>(m_entries.size()); }
        int Get(int index) const override {
            if (index < 0 || index >= Count()) return 0;
            const auto& e = m_entries[static_cast<size_t>(index)];
            return e.get ? e.get() : 0;
        }
        void Set(int index, int value) override {
            if (index < 0 || index >= Count()) return;
            const auto& e = m_entries[static_cast<size_t>(index)];
            if (e.set) e.set(value);
        }

    private:
        std::vector<Entry> m_entries;
    };

} // namespace Game
