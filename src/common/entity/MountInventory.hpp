// File: src/common/entity/MountInventory.hpp
//
// MC AbstractHorse.inventory (and AbstractNautilus.inventory) — the mount's
// chest storage — with AbstractChestedHorse's chest flag (DATA_ID_CHEST)
// folded in, because the two only ever change together: putting a chest on a
// donkey, mule or llama re-creates the inventory at its new size, taking the
// chest off re-creates it empty.
//
// Owned by value by every mount class (AbstractHorse, Llama, Camel,
// AbstractNautilus) and reached generically through Mob::GetMountInventory.
// The storage lives only on the server; the client copy carries the synced
// chest flag (the anim byte) for the renderer, and the open menu keeps its
// own scratch container, as MC's client does.
//
// A re-creation bumps `Generation()`: MC's menus hold the container object
// and close when hasInventoryChanged(container) says it was replaced; here
// the menu remembers the generation it opened with.
#pragma once

#include "common/entity/Item.hpp"
#include "common/inventory/SimpleContainer.hpp"

#include <cstdint>
#include <memory>

namespace Game {

    class Mob;
    class EntityLevel;

    class MountInventory {
    public:
        // `canCarryChest`: an AbstractChestedHorse (donkey, mule, llama,
        // trader llama) — the chest slot (499) and the "ChestedHorse" save
        // field exist for it.
        explicit MountInventory(bool canCarryChest = false) : m_canCarryChest(canCarryChest) {}

        // MC AbstractHorse.createInventory: a new SimpleContainer of `size`
        // slots, the old one's items copied over as far as they fit (what
        // does not fit is gone, as in MC — a chest comes off only after its
        // contents were dropped). Bumps the generation.
        void Create(int size);

        SimpleContainer&       Container()       { return *m_container; }
        const SimpleContainer& Container() const { return *m_container; }
        int      Size() const { return m_container->GetContainerSize(); }
        // Changes whenever Create replaces the container (MC
        // hasInventoryChanged: `this.inventory != oldInventory`).
        uint32_t Generation() const { return m_generation; }

        // ── AbstractChestedHorse ─────────────────────────────────────────
        bool CanCarryChest() const { return m_canCarryChest; }
        // MC hasChest / setChest (DATA_ID_CHEST). The flag alone — callers
        // that change the carried chest re-create the inventory after
        // (MC setChest + createInventory, always in that order).
        bool HasChest() const { return m_hasChest; }
        void SetChest(bool hasChest) { m_hasChest = hasChest; }

        // MC AbstractHorse.dropEquipment (and AbstractChestedHorse's): every
        // stored stack not carrying prevent_equipment_drop (Curse of
        // Vanishing) is spawned at the mob; a chested equine then drops its
        // chest block and loses the flag. Server-side.
        static void DropOnDeath(Mob& mob);

        // MC AbstractChestedHorse.getSlot(499).set(stack): an empty stack
        // takes the chest off (flag cleared, inventory re-created), a chest
        // puts one on; anything else is refused. The mob's columns decide the
        // new size (Mob::GetInventoryColumns). True when accepted.
        static bool SetChestSlot(Mob& mob, const ItemStack& stack);

        // MC AbstractChestedHorse.equipChest: the chest goes on, the chest
        // sound plays (DONKEY_CHEST, LLAMA_CHEST for the llamas, pitched
        // ±0.2), one chest is used up and the inventory is re-created.
        // Server-side (the caller answers the client).
        static void EquipChest(Mob& mob, ItemStack& held, const char* chestSound);

    private:
        std::unique_ptr<SimpleContainer> m_container = std::make_unique<SimpleContainer>(0);
        uint32_t m_generation = 0;
        bool     m_canCarryChest = false;
        bool     m_hasChest = false;
    };

} // namespace Game
