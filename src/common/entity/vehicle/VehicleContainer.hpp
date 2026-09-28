// File: src/common/entity/vehicle/VehicleContainer.hpp
//
// MC net.minecraft.world.entity.vehicle.ContainerEntity — the storage half of
// a chest boat, a chest raft and the chest / hopper minecarts: a fixed run of
// slots, the structure loot table it may carry instead of items (a mineshaft's
// chest minecart), and the rules a menu over it follows (open while the
// vehicle lives and the player is within reach, the contents poured out when
// it breaks).
//
// An IContainer, so the ChestMenu the server opens over it, the hopper
// block that pulls from a minecart parked above it and the comparator behind
// a detector rail all read it as they read a chest.
#pragma once

#include "common/inventory/Container.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Game {

    class Entity;
    class LivingEntity;
    class EntityLevel;

    class VehicleContainer : public IContainer {
    public:
        VehicleContainer(Entity& owner, int size) : m_owner(owner), m_items(static_cast<size_t>(size)) {}
        ~VehicleContainer() override = default;

        // ── IContainer ────────────────────────────────────────────────────
        int GetContainerSize() const override { return static_cast<int>(m_items.size()); }
        // The writable read unpacks the loot table first (MC getChestVehicleItem);
        // the const read does not, so a save never rolls a mineshaft cart.
        ItemStack& GetItem(int index) override;
        const ItemStack& GetItem(int index) const override;
        // MC setChestVehicleItem: unpack, set, limitSize(getMaxStackSize).
        void SetItem(int index, const ItemStack& stack) override;
        ItemStack RemoveItem(int slot, int count) override;
        bool IsEmpty() const override;

        // ── Loot (MC RandomizableContainer via ContainerEntity) ────────────
        void SetContainerLootTable(std::string key, int64_t seed = 0) {
            m_lootTable = std::move(key);
            m_lootTableSeed = seed;
        }
        bool HasContainerLootTable() const { return !m_lootTable.empty(); }
        const std::string& GetContainerLootTable() const { return m_lootTable; }
        int64_t GetContainerLootTableSeed() const { return m_lootTableSeed; }
        // MC unpackChestVehicleLootTable(player): server only; the table is
        // cleared first, then rolled into the slots at the vehicle's origin.
        void UnpackChestVehicleLootTable(float luck = 0.0f);

        // The raw slots (MC getItemStacks) — for saving, loading and the wire.
        std::vector<ItemStack>&       Items()       { return m_items; }
        const std::vector<ItemStack>& Items() const { return m_items; }
        // MC clearItemStacks / clearChestVehicleContent.
        void ClearItemStacks();
        void ClearChestVehicleContent();

        // MC isChestVehicleStillValid(player): the vehicle is alive and the
        // player within reach of its box (entity interaction range + 4).
        bool IsChestVehicleStillValid(const LivingEntity& player) const;
        bool IsChestVehicleStillValidFrom(const glm::dvec3& playerEye) const;

        // MC Containers.dropContents(level, entity, container): every stack
        // popped out at the vehicle, split into handfuls with a hop.
        void DropContents();
        // MC chestVehicleDestroyed(source, level, entity): the contents,
        // when entity drops are on.
        void ChestVehicleDestroyed();

        // MC AbstractContainerMenu.getRedstoneSignalFromContainer.
        int RedstoneSignal() const;

    private:
        Entity&                m_owner;
        std::vector<ItemStack> m_items;
        std::string            m_lootTable;
        int64_t                m_lootTableSeed = 0;
    };

} // namespace Game
