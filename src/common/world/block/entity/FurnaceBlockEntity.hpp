// File: src/common/world/block/entity/FurnaceBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.AbstractFurnaceBlockEntity —
// the three-slot cooker behind the furnace, blast furnace and smoker. One class
// covers all three; the only thing that varies is which RecipeType it queries
// and how fast it runs, exactly as MC parameterises AbstractFurnaceBlockEntity
// with a recipeType and BlastingRecipe/SmokingRecipe halve the cook time.
//
// Slots (MC's order — it is what the menu and the wire use):
//   0  input      the thing being cooked
//   1  fuel       coal, planks, a lava bucket…
//   2  result     what came out
//
// The four counters below are the ContainerData the menu publishes, and are
// what animate the flame and the arrow. They live HERE rather than on the menu
// so a furnace keeps burning with nobody watching, and so two players looking
// into the same furnace see one shared state.
#pragma once

#include "BaseContainerBlockEntity.hpp"
#include "common/world/crafting/GeneratedRecipeList.hpp"

namespace Game {

    // RecipeManager.hpp — forward-declared so every block-entity TU doesn't
    // pull the recipe matcher in for one pointer.
    struct CookingRecipe;

    class FurnaceBlockEntity : public BaseContainerBlockEntity {
    public:
        static constexpr int SLOT_INPUT  = 0;
        static constexpr int SLOT_FUEL   = 1;
        static constexpr int SLOT_RESULT = 2;
        static constexpr int SLOT_COUNT  = 3;

        // ContainerData indices — MC AbstractFurnaceBlockEntity.dataAccess.
        static constexpr int DATA_LIT_TIME       = 0;
        static constexpr int DATA_LIT_DURATION   = 1;
        static constexpr int DATA_COOKING_TIME   = 2;
        static constexpr int DATA_COOKING_TOTAL  = 3;
        static constexpr int DATA_COUNT          = 4;

        FurnaceBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId,
                           CookingKind kind)
            : BaseContainerBlockEntity(type, worldPos, blockId, SLOT_COUNT),
              m_kind(kind) {}

        CookingKind Kind() const { return m_kind; }

        // A furnace ticks whenever it is loaded — that is what lets it finish a
        // smelt while the player walks away.
        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        // MC AbstractFurnaceBlockEntity.setItem: swapping the INPUT for a
        // different item throws away the progress made on the old one and
        // re-times for the new recipe.
        //
        // This is the reset that fires the instant the player clicks. The tick
        // has its own reset for "can't cook this" (see Tick), but that one
        // cannot cover swapping one smeltable for another — raw beef for raw
        // chicken still passes canBurn, so only the item-identity check here
        // notices. Count changes are deliberately NOT a swap, matching MC's
        // isSameItemSameComponents test.
        void SetItem(int index, const ItemStack& stack) override;

        // ── The counters the menu publishes ───────────────────────────────
        int  LitTime() const      { return m_litTime; }
        int  LitDuration() const  { return m_litDuration; }
        int  CookingTime() const  { return m_cookingTime; }
        int  CookingTotal() const { return m_cookingTotal; }
        void SetLitTime(int v)      { m_litTime = v; }
        void SetLitDuration(int v)  { m_litDuration = v; }
        void SetCookingTime(int v)  { m_cookingTime = v; }
        void SetCookingTotal(int v) { m_cookingTotal = v; }

        bool IsLit() const { return m_litTime > 0; }

        // XP banked by completed smelts, paid out when the result is taken
        // (MC AbstractFurnaceBlockEntity.recipesUsed → awardUsedRecipes).
        float TakeStoredExperience() { const float xp = m_storedXp; m_storedXp = 0.0f; return xp; }

        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        // MC AbstractFurnaceBlockEntity.canBurn — is there a recipe, is the
        // result slot able to take its output, and does the count fit?
        bool CanBurn(const CookingRecipe* recipe) const;

        // MC AbstractFurnaceBlockEntity.getTotalCookTime — the current input's
        // cook time, or 200 ticks when it has no recipe at all.
        int TotalCookTime() const;

        CookingKind m_kind = CookingKind::Smelting;
        int   m_litTime      = 0;   // ticks of fuel left
        int   m_litDuration  = 0;   // ticks the current fuel gave (for the flame's height)
        int   m_cookingTime  = 0;   // ticks the current item has cooked
        int   m_cookingTotal = 0;   // ticks it needs (for the arrow's width)
        float m_storedXp     = 0.0f;
    };

} // namespace Game
