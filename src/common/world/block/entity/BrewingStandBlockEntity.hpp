// File: src/common/world/block/entity/BrewingStandBlockEntity.hpp
//
// MC 26.3 net.minecraft.world.level.block.entity.BrewingStandBlockEntity —
// the five-slot brewer:
//
//   0..2  bottles     (PotionSlot: brewing potion inputs, one per slot)
//   3     ingredient  (IngredientsSlot: a brewing reagent)
//   4     fuel        (FuelSlot: anything with BREWING_FUEL — blaze powder)
//
// serverTick, ported: a spent stand (fuel == 0) burns one fuel item for its
// BREWING_FUEL uses (20); a brewable stand with fuel starts a 400-tick brew
// (400 / speedMultiplier, rounded up) that costs one use, remembers the
// ingredient's item, and aborts the moment the stand stops being brewable or
// the ingredient changes; when the timer reaches 0 every bottle with a
// matching recipe is replaced by its result and one ingredient is spent
// (its crafting remainder — dragon's breath's glass bottle — takes its place
// or drops). The HAS_BOTTLE_0..2 block-state flags track the bottle slots.
//
// The four counters are MC's dataAccess (DATA_BREW_TIME, DATA_FUEL_USES,
// DATA_TOTAL_BREW_TIME, DATA_TOTAL_FUEL_USES); BrewingStandMenu publishes
// them straight from here, as FurnaceMenu does for the furnace.
//
// Not modelled: levelEvent 1035 (the brew-finished sound — sound stub) and
// the BREWED_POTION advancement trigger.
#pragma once

#include "BaseContainerBlockEntity.hpp"

namespace Game {

    class BrewingStandBlockEntity : public BaseContainerBlockEntity, public IWorldlyContainer {
    public:
        static constexpr int SLOT_BOTTLE_0   = 0;
        static constexpr int SLOT_INGREDIENT = 3;
        static constexpr int SLOT_FUEL       = 4;
        static constexpr int SLOT_COUNT      = 5;

        static constexpr int DATA_BREW_TIME       = 0;
        static constexpr int DATA_FUEL_USES       = 1;
        static constexpr int DATA_TOTAL_BREW_TIME = 2;
        static constexpr int DATA_TOTAL_FUEL_USES = 3;
        static constexpr int DATA_COUNT           = 4;

        static constexpr int kBrewingTicks = 400;   // BREWING_TIME_SECONDS * 20

        BrewingStandBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, SLOT_COUNT) {}

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        // MC canPlaceItem: fuel slot takes BREWING_FUEL, the ingredient slot a
        // reagent, a bottle slot a potion input — and only when empty.
        bool CanPlaceItem(int slot, const ItemStack& stack) const override;

        // MC WorldlyContainer: the ingredient from above; bottles + ingredient
        // out of the bottom; bottles + fuel from the sides. Only a glass
        // bottle comes back out of the ingredient slot.
        void GetSlotsForFace(Direction face, std::vector<int>& out) const override;
        bool CanPlaceItemThroughFace(int slot, const ItemStack& stack, Direction face,
                                     bool hasDirection) const override;
        bool CanTakeItemThroughFace(int slot, const ItemStack& stack, Direction face) const override;

        // ── dataAccess ────────────────────────────────────────────────────
        int  BrewTime() const       { return m_brewTime; }
        int  Fuel() const           { return m_fuel; }
        int  TotalBrewTime() const  { return m_totalBrewTime; }
        int  TotalFuel() const      { return m_totalFuel; }
        float SpeedMultiplier() const { return m_speedMultiplier; }
        void SetBrewTime(int v)       { m_brewTime = v; }
        void SetFuel(int v)           { m_fuel = v; }
        void SetTotalBrewTime(int v)  { m_totalBrewTime = v; }
        void SetTotalFuel(int v)      { m_totalFuel = v; }
        void SetSpeedMultiplier(float v) { m_speedMultiplier = v; }

        // MC loadAdditional: a stand saved mid-brew remembers the ingredient
        // it was brewing with.
        void RestoreIngredientFromSlot();

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        bool IsBrewable() const;
        void DoBrew(World* world);

        int    m_brewTime        = 0;
        int    m_totalBrewTime   = kBrewingTicks;
        int    m_fuel            = 0;
        int    m_totalFuel       = 20;
        float  m_speedMultiplier = 1.0f;
        ItemID m_ingredient      = Items::Air;
        // MC lastPotionCount — null until the first tick compares.
        bool   m_haveLastBits    = false;
        bool   m_lastBits[3]     = {false, false, false};
    };

} // namespace Game
