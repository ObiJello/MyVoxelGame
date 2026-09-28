// File: src/common/entity/vehicle/VehicleContainer.cpp
#include "common/entity/vehicle/VehicleContainer.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ItemEntity.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    ItemStack& VehicleContainer::GetItem(int index) {
        UnpackChestVehicleLootTable();
        static thread_local ItemStack scratch{};
        if (index < 0 || index >= GetContainerSize()) { scratch = ItemStack{}; return scratch; }
        return m_items[static_cast<size_t>(index)];
    }

    const ItemStack& VehicleContainer::GetItem(int index) const {
        static const ItemStack kEmpty{};
        if (index < 0 || index >= GetContainerSize()) return kEmpty;
        return m_items[static_cast<size_t>(index)];
    }

    void VehicleContainer::SetItem(int index, const ItemStack& stack) {
        UnpackChestVehicleLootTable();
        if (index < 0 || index >= GetContainerSize()) return;
        ItemStack limited = stack;
        // itemStack.limitSize(getMaxStackSize(itemStack)).
        if (!limited.IsEmpty()) limited.count = std::min(limited.count, GetMaxStackSize(limited));
        m_items[static_cast<size_t>(index)] = limited;
        SetChanged();
    }

    ItemStack VehicleContainer::RemoveItem(int slot, int count) {
        // MC removeChestVehicleItem → ContainerHelper.removeItem.
        UnpackChestVehicleLootTable();
        return IContainer::RemoveItem(slot, count);
    }

    bool VehicleContainer::IsEmpty() const {
        // MC isChestVehicleEmpty reads the raw list; the loot table is
        // unpacked by the reads that matter (a hopper's GetItem).
        const_cast<VehicleContainer*>(this)->UnpackChestVehicleLootTable();
        for (const ItemStack& s : m_items) if (!s.IsEmpty()) return false;
        return true;
    }

    void VehicleContainer::UnpackChestVehicleLootTable(float luck) {
        if (m_lootTable.empty()) return;
        EntityLevel* level = m_owner.Level();
        if (!level || level->IsClientSide()) return;   // the server rolls
        const std::string key = std::move(m_lootTable);
        m_lootTable.clear();                            // before fill (setItem re-enters)
        const int64_t seed = m_lootTableSeed;
        m_lootTableSeed = 0;
        // LootParams: ORIGIN = the vehicle's position.
        ChestLoot::LootLevelContext lootLevel;
        lootLevel.dimensionId = DimensionToRaw(level->Dimension());
        lootLevel.origin = m_owner.position;
        ChestLoot::Fill(*this, key, seed, &level->Random(), luck, &lootLevel);
    }

    void VehicleContainer::ClearItemStacks() {
        for (ItemStack& s : m_items) s.Clear();
    }

    void VehicleContainer::ClearChestVehicleContent() {
        UnpackChestVehicleLootTable();
        ClearItemStacks();
    }

    bool VehicleContainer::IsChestVehicleStillValid(const LivingEntity& player) const {
        return IsChestVehicleStillValidFrom(player.GetEyePosition());
    }

    bool VehicleContainer::IsChestVehicleStillValidFrom(const glm::dvec3& playerEye) const {
        // !isRemoved() && player.isWithinEntityInteractionRange(bb, 4.0):
        // (ENTITY_INTERACTION_RANGE 3 + 4)² against the eye-to-box distance.
        if (m_owner.IsRemoved()) return false;
        constexpr double kRange = 3.0 + 4.0;
        return m_owner.GetAABBd().DistanceToSqr(playerEye) < kRange * kRange;
    }

    void VehicleContainer::DropContents() {
        // MC Containers.dropContents(level, entity, container) →
        // dropItemStack at the entity's position for every slot.
        EntityLevel* level = m_owner.Level();
        if (!level || level->IsClientSide()) return;
        // Each getItem there is getChestVehicleItem, which rolls a pending
        // loot table first.
        UnpackChestVehicleLootTable();
        JavaRandom& random = level->Random();
        const glm::dvec3 at = m_owner.position;
        for (ItemStack& slot : m_items) {
            if (slot.IsEmpty()) continue;
            ItemStack itemStack = slot;
            slot.Clear();
            // Containers.dropItemStack.
            const double size = static_cast<double>(ItemEntity::kWidth);   // EntityType.ITEM.getWidth()
            const double centerRange = 1.0 - size;
            const double halfSize = size / 2.0;
            const double xo = std::floor(at.x) + random.NextDouble() * centerRange + halfSize;
            const double yo = std::floor(at.y) + random.NextDouble() * centerRange;
            const double zo = std::floor(at.z) + random.NextDouble() * centerRange + halfSize;
            while (!itemStack.IsEmpty()) {
                const int splitCount = std::min(itemStack.count, random.NextInt(21) + 10);
                ItemStack part = itemStack;
                part.count = splitCount;
                itemStack.count -= splitCount;
                if (itemStack.count <= 0) itemStack.Clear();
                const glm::dvec3 motion(random.Triangle(0.0, 0.11485000171139836),
                                        random.Triangle(0.2, 0.11485000171139836),
                                        random.Triangle(0.0, 0.11485000171139836));
                level->SpawnThrownItem(glm::dvec3(xo, yo, zo), motion, part, 0);
            }
        }
    }

    void VehicleContainer::ChestVehicleDestroyed() {
        EntityLevel* level = m_owner.Level();
        if (!level || level->IsClientSide() || !level->DoEntityDrops()) return;
        UnpackChestVehicleLootTable();
        DropContents();
        // (PiglinAi.angerNearbyPiglins for a player's blow: piglin anger
        // at container breaking is the piglins' own system.)
    }

    int VehicleContainer::RedstoneSignal() const {
        // MC AbstractContainerMenu.getRedstoneSignalFromContainer.
        float totalPercent = 0.0f;
        const int n = GetContainerSize();
        if (n <= 0) return 0;
        for (int i = 0; i < n; ++i) {
            const ItemStack& stack = GetItem(i);
            if (!stack.IsEmpty()) {
                totalPercent += static_cast<float>(stack.count) / static_cast<float>(GetMaxStackSize(stack));
            }
        }
        totalPercent /= static_cast<float>(n);
        // Mth.lerpDiscrete(totalPercent, 0, 15).
        constexpr int delta = 15;
        return static_cast<int>(std::floor(totalPercent * static_cast<float>(delta - 1))) +
               (totalPercent > 0.0f ? 1 : 0);
    }

} // namespace Game
