// File: src/common/entity/MountInventory.cpp
//
// See MountInventory.hpp. MC sources: AbstractHorse.createInventory /
// dropEquipment, AbstractChestedHorse.dropEquipment / getSlot(499) /
// equipChest / playChestEquipsSound.
#include "common/entity/MountInventory.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <algorithm>

namespace Game {

    namespace {
        ItemID ChestItem() { return ItemRegistry::FromBlock(BlockID::Chest); }
    }

    void MountInventory::Create(int size) {
        // MC createInventory: new SimpleContainer(getInventorySize()), then
        // the old one's first min(old, new) slots copied across.
        auto fresh = std::make_unique<SimpleContainer>(std::max(0, size));
        if (m_container) {
            const int keep = std::min(m_container->GetContainerSize(), fresh->GetContainerSize());
            for (int i = 0; i < keep; ++i) {
                const ItemStack& stack = m_container->GetItem(i);
                if (!stack.IsEmpty()) fresh->SetItem(i, stack);
            }
        }
        m_container = std::move(fresh);
        ++m_generation;
    }

    void MountInventory::DropOnDeath(Mob& mob) {
        MountInventory* inventory = mob.GetMountInventory();
        EntityLevel* level = mob.Level();
        if (!inventory || !level || level->IsClientSide()) return;
        const DimensionId dimension = level->Dimension();
        // AbstractHorse.dropEquipment: every stack without
        // prevent_equipment_drop, spawnAtLocation.
        SimpleContainer& items = inventory->Container();
        for (int i = 0; i < items.GetContainerSize(); ++i) {
            const ItemStack stack = items.GetItem(i);
            if (stack.IsEmpty() || EnchantmentHelper::HasPreventEquipmentDrop(stack)) continue;
            DropItemStackAt(dimension, mob.position, stack);
            items.SetItem(i, ItemStack{});
        }
        // AbstractChestedHorse.dropEquipment: the chest block, and the flag
        // goes.
        if (inventory->CanCarryChest() && inventory->HasChest()) {
            DropItemStackAt(dimension, mob.position, ItemStack(ChestItem(), 1));
            inventory->SetChest(false);
        }
    }

    bool MountInventory::SetChestSlot(Mob& mob, const ItemStack& stack) {
        MountInventory* inventory = mob.GetMountInventory();
        if (!inventory || !inventory->CanCarryChest()) return false;
        if (stack.IsEmpty()) {
            if (inventory->HasChest()) {
                inventory->SetChest(false);
                mob.CreateMountInventory();
            }
            return true;
        }
        if (stack.itemId == ChestItem()) {
            if (!inventory->HasChest()) {
                inventory->SetChest(true);
                mob.CreateMountInventory();
            }
            return true;
        }
        return false;
    }

    void MountInventory::EquipChest(Mob& mob, ItemStack& held, const char* chestSound) {
        MountInventory* inventory = mob.GetMountInventory();
        if (!inventory || !inventory->CanCarryChest()) return;
        inventory->SetChest(true);
        // playChestEquipsSound: 1.0, (nextFloat - nextFloat) * 0.2 + 1.0.
        if (EntityLevel* level = mob.Level()) {
            JavaRandom& random = level->Random();
            const float pitch = (random.NextFloat() - random.NextFloat()) * 0.2f + 1.0f;
            mob.PlaySound(chestSound, 1.0f, pitch);
        }
        // itemStack.consume(1, player) — the interaction path hands a
        // creative player's count back (Player.interactOn).
        held.count -= 1;
        if (held.count <= 0) held.Clear();
        mob.CreateMountInventory();
    }

} // namespace Game
