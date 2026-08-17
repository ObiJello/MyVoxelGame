// File: src/common/world/block/entity/CampfireBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.CampfireBlockEntity — the four
// food slots on top of a campfire.
//
// Unlike the furnace this is NOT a menu container: there is no screen, no fuel,
// and no result slot. You right-click food onto the fire, it cooks in place, and
// it pops back off. MC models that with four INDEPENDENT cook timers rather than
// one shared pair, which is why the counters here are arrays — four different
// foods placed at four different moments each finish on their own schedule.
//
// It still derives from BaseContainerBlockEntity because that is what owns an
// item array in this engine, and it makes the break-spill (MC's
// Containers.dropContents in onRemove) work with no extra wiring. Nothing ever
// binds a menu to it.
//
// Which tick runs is decided by the block's `lit` state, exactly as MC picks
// between two BlockEntityTickers in CampfireBlock.getTicker:
//   lit   -> CookTick     (progress climbs, finished food pops off)
//   unlit -> CooldownTick (progress unwinds at 2/tick, MC's BURN_COOL_SPEED)
#pragma once

#include "BaseContainerBlockEntity.hpp"

namespace Game {

    class CampfireBlockEntity : public BaseContainerBlockEntity {
    public:
        static constexpr int SLOT_COUNT = 4;

        CampfireBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, SLOT_COUNT) {}

        // A campfire ticks whenever it is loaded — that is what lets food
        // finish cooking while the player walks away.
        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        // MC CampfireBlockEntity.placeFood: put ONE of `held` on the first free
        // slot, timed by its campfire recipe. Returns false when the fire is
        // full or the item has no campfire recipe, in which case nothing is
        // consumed. Decrements `held` on success (the caller need not).
        bool PlaceFood(ItemStack& held);

        // Per-slot cook counters, for the renderer and for save/load.
        int CookingProgress(int slot) const {
            return (slot >= 0 && slot < SLOT_COUNT) ? m_cookingProgress[slot] : 0;
        }
        int CookingTime(int slot) const {
            return (slot >= 0 && slot < SLOT_COUNT) ? m_cookingTime[slot] : 0;
        }

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        void CookTick(World* world);
        void CooldownTick();

        // MC's CookingTimes / CookingTotalTimes NBT arrays.
        int m_cookingProgress[SLOT_COUNT]{};
        int m_cookingTime[SLOT_COUNT]{};
    };

} // namespace Game
