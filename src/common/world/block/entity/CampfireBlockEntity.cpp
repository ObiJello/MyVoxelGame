// File: src/common/world/block/entity/CampfireBlockEntity.cpp
#include "CampfireBlockEntity.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include <algorithm>
#include <string_view>

namespace Game {

    namespace {
        // The block's `lit` state, which is what selects the ticker in MC
        // (CampfireBlock.getTicker). Absent the property — an unregistered or
        // replaced block — treat it as unlit so we wind down rather than cook.
        bool IsLitAt(World& world, const glm::ivec3& pos, BlockID id) {
            const auto& def = BlockRegistry::GetStateDefinition(id);
            const uint8_t state = world.GetBlockState(pos.x, pos.y, pos.z);
            return def.ValueOf(state, "lit") == "true";
        }
    }

    bool CampfireBlockEntity::PlaceFood(ItemStack& held) {
        if (held.IsEmpty()) return false;

        for (int slot = 0; slot < SLOT_COUNT; ++slot) {
            if (!GetItem(slot).IsEmpty()) continue;

            // MC checks the recipe INSIDE the empty-slot branch and bails out
            // of the whole call when there isn't one — so an item with no
            // campfire recipe never occupies a slot even if one is free.
            const CookingRecipe* recipe =
                RecipeManager::FindCooking(CookingKind::CampfireCooking, held);
            if (!recipe) return false;

            m_cookingTime[slot]     = recipe->cookingTime;
            m_cookingProgress[slot] = 0;

            // Exactly one item goes on the fire, whatever the stack size.
            ItemStack one = held;
            one.count = 1;
            SetItem(slot, one);
            if (--held.count <= 0) held = ItemStack{};

            SetChanged();
            return true;
        }
        return false;   // all four slots busy
    }

    void CampfireBlockEntity::CookTick(World* world) {
        bool changed = false;

        for (int slot = 0; slot < SLOT_COUNT; ++slot) {
            const ItemStack& cooking = GetItem(slot);
            if (cooking.IsEmpty()) continue;

            changed = true;
            ++m_cookingProgress[slot];
            if (m_cookingProgress[slot] < m_cookingTime[slot]) continue;

            // MC: the recipe result, or the item itself when it has none.
            const CookingRecipe* recipe =
                RecipeManager::FindCooking(CookingKind::CampfireCooking, cooking);
            const ItemStack result = recipe
                ? ItemStack(recipe->resultItem, recipe->resultCount)
                : cooking;

            // MC pops the finished food off as an ItemEntity, which is now
            // exactly what happens. The failure branch is kept because the
            // spawn can still decline with no server behind it: the food is
            // PARKED in the slot rather than voided, so walking back to a
            // campfire you left still gets you the meal. The retry is harmless
            // because a cooked item has no campfire recipe, so the next attempt
            // re-derives the same stack.
            if (DropItemStackNear(GetWorldPos(), result)) {
                SetItem(slot, ItemStack{});
                m_cookingProgress[slot] = 0;
                m_cookingTime[slot]     = 0;
            } else {
                SetItem(slot, result);
                m_cookingProgress[slot] = m_cookingTime[slot];
            }
        }

        if (changed) SetChanged();
        (void)world;
    }

    void CampfireBlockEntity::CooldownTick() {
        bool changed = false;
        for (int slot = 0; slot < SLOT_COUNT; ++slot) {
            if (m_cookingProgress[slot] <= 0) continue;
            changed = true;
            // MC BURN_COOL_SPEED = 2, via Mth.clamp(progress - 2, 0, time).
            // Spelled min-then-max because std::clamp is UB when lo > hi and
            // m_cookingTime is read straight off the wire in Load().
            m_cookingProgress[slot] =
                std::max(0, std::min(m_cookingProgress[slot] - 2, m_cookingTime[slot]));
        }
        if (changed) SetChanged();
    }

    void CampfireBlockEntity::Tick(World* world, float /*deltaTime*/) {
        // MC CampfireBlock.getTicker picks the ticker off the block state, so
        // dousing a fire switches which one runs rather than setting a flag.
        if (world && IsLitAt(*world, GetWorldPos(), GetBlockId())) {
            CookTick(world);
        } else {
            CooldownTick();
        }
    }

    void CampfireBlockEntity::Save(Network::PacketBuffer& out) const {
        BaseContainerBlockEntity::Save(out);
        for (int i = 0; i < SLOT_COUNT; ++i) out.WriteVarInt(static_cast<uint32_t>(m_cookingProgress[i]));
        for (int i = 0; i < SLOT_COUNT; ++i) out.WriteVarInt(static_cast<uint32_t>(m_cookingTime[i]));
    }

    void CampfireBlockEntity::Load(Network::PacketReader& in) {
        BaseContainerBlockEntity::Load(in);
        for (int i = 0; i < SLOT_COUNT; ++i) {
            if (!in.HasMore()) return;
            m_cookingProgress[i] = static_cast<int>(in.ReadVarInt());
        }
        for (int i = 0; i < SLOT_COUNT; ++i) {
            if (!in.HasMore()) return;
            m_cookingTime[i] = static_cast<int>(in.ReadVarInt());
        }
    }

} // namespace Game
