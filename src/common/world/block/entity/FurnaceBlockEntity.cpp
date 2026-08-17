// File: src/common/world/block/entity/FurnaceBlockEntity.cpp
#include "FurnaceBlockEntity.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/World.hpp"
#include <algorithm>
#include <string>

namespace Game {

    bool FurnaceBlockEntity::CanBurn(const CookingRecipe* recipe) const {
        // MC AbstractFurnaceBlockEntity.canBurn (line 300-ish): no recipe, or
        // an empty input, and nothing can run.
        if (!recipe || GetItem(SLOT_INPUT).IsEmpty()) return false;

        const ItemStack& result  = GetItem(SLOT_RESULT);
        if (result.IsEmpty()) return true;                       // room for anything
        if (result.itemId != recipe->resultItem) return false;   // different item in the way

        const int combined = result.count + recipe->resultCount;
        const int limit = std::min(ItemRegistry::Get(result.itemId).maxStackSize, 64);
        return combined <= limit;
    }

    int FurnaceBlockEntity::TotalCookTime() const {
        // MC getTotalCookTime: 200 ticks when the input matches nothing, which
        // is only ever a placeholder — nothing cooks without a recipe.
        const CookingRecipe* recipe = RecipeManager::FindCooking(m_kind, GetItem(SLOT_INPUT));
        return recipe ? recipe->cookingTime : 200;
    }

    void FurnaceBlockEntity::SetItem(int index, const ItemStack& stack) {
        const ItemStack old = GetItem(index);
        const bool same = !stack.IsEmpty() && IsSameItemSameComponents(old, stack);
        BaseContainerBlockEntity::SetItem(index, stack);
        if (index == SLOT_INPUT && !same) {
            m_cookingTotal = TotalCookTime();
            m_cookingTime  = 0;
            SetChanged();
        }
    }

    void FurnaceBlockEntity::Tick(World* world, float /*deltaTime*/) {
        // Branch-for-branch shape of AbstractFurnaceBlockEntity.serverTick. The
        // order matters: fuel burns down FIRST, then we decide whether to
        // consume more, then progress moves. Reordering it lets a furnace cook
        // a tick for free on the tick its fuel runs out.
        //
        // The nesting matters just as much, and is easy to flatten by mistake.
        // MC has TWO different ways progress goes away, and they are not
        // interchangeable:
        //
        //   • INSTANT reset (the inner `else`) — the furnace is lit, or has
        //     both fuel and an input, but cannot cook what is in there. The
        //     player swapped in something unsmeltable, or the output backed up.
        //     Progress is dropped on the spot.
        //   • SLOW wind-down at 2/tick (the outer `else if`) — the furnace has
        //     gone DARK with nothing to work on. This exists so briefly running
        //     out of fuel doesn't throw away a nearly-finished smelt.
        //
        // Collapsing them into one wind-down makes a swapped input crawl
        // backwards instead of snapping to zero.
        const bool wasLit = IsLit();
        bool changed = false;

        if (IsLit()) {
            --m_litTime;
        }

        ItemStack& fuel = GetItem(SLOT_FUEL);
        const bool hasInput = !GetItem(SLOT_INPUT).IsEmpty();
        const bool hasFuel  = !fuel.IsEmpty();

        if (IsLit() || (hasFuel && hasInput)) {
            const CookingRecipe* recipe =
                hasInput ? RecipeManager::FindCooking(m_kind, GetItem(SLOT_INPUT)) : nullptr;

            // Light up from fuel if there is work to do and we are dark. MC
            // assigns the burn duration unconditionally and re-tests isLit():
            // a non-fuel gives 0, which leaves the furnace dark and consumes
            // nothing, so the check doubles as the "is this actually fuel" test.
            if (!IsLit() && CanBurn(recipe)) {
                m_litTime     = RecipeManager::GetFuelBurnTime(fuel);
                m_litDuration = m_litTime;
                if (IsLit()) {
                    changed = true;
                    if (hasFuel) {
                        // MC consumes one fuel item; a lava bucket leaves its
                        // bucket behind via the crafting remainder.
                        const ItemID remainder = ItemRegistry::Get(fuel.itemId).craftingRemainder;
                        if (--fuel.count <= 0) {
                            fuel = remainder != Items::Air ? ItemStack(remainder, 1) : ItemStack{};
                        }
                    }
                }
            }

            if (IsLit() && CanBurn(recipe)) {
                // MC pins cookingTotalTime in setItem and compares with ==.
                // We use >= and re-arm a zero total, because a stack can reach
                // the input slot by in-place merge through GetItemMut without
                // passing through SetItem — with ==, a total left at 0 would
                // never be hit and the item would cook forever.
                if (m_cookingTotal <= 0) m_cookingTotal = TotalCookTime();

                ++m_cookingTime;
                if (m_cookingTime >= m_cookingTotal) {
                    m_cookingTime  = 0;
                    m_cookingTotal = TotalCookTime();

                    // Produce the result.
                    ItemStack& result = GetItem(SLOT_RESULT);
                    if (result.IsEmpty()) {
                        result = ItemStack(recipe->resultItem, recipe->resultCount);
                    } else {
                        result.count += recipe->resultCount;
                    }

                    // Consume one input.
                    ItemStack& input = GetItem(SLOT_INPUT);
                    if (--input.count <= 0) input = ItemStack{};

                    // Bank the XP; it is paid when the player takes the result.
                    m_storedXp += recipe->experience;
                    changed = true;
                }
            } else {
                m_cookingTime = 0;
            }
        } else if (!IsLit() && m_cookingTime > 0) {
            // MC's Mth.clamp(cookingTimer - 2, 0, cookingTotalTime), spelled as
            // min-then-max rather than std::clamp: std::clamp is UB when lo > hi,
            // and m_cookingTotal is read straight off the wire in Load().
            m_cookingTime = std::max(0, std::min(m_cookingTime - 2, m_cookingTotal));
        }

        // MC AbstractFurnaceBlockEntity.serverTick's tail: crossing the
        // lit/unlit boundary republishes the block with LIT flipped, which is
        // what swaps the front face to its glowing variant.
        //
        // Only ON THE TRANSITION, never every tick — the state is already
        // right in between, and rewriting it each tick would re-broadcast the
        // block and re-dirty the section 20x a second for the whole burn.
        //
        // Safe to call from inside World::TileEntityTick's walk over this
        // chunk's block entities: the block id is unchanged, and SetBlock only
        // creates or destroys a BE when the id itself changes, so the map this
        // loop is iterating is untouched.
        if (wasLit != IsLit()) {
            changed = true;
            if (world) {
                const glm::ivec3 p = GetWorldPos();
                const auto& def = BlockRegistry::GetStateDefinition(GetBlockId());
                const uint8_t cur = world->GetBlockState(p.x, p.y, p.z);
                BlockRegistry::BlockStateDefinition::PropertyMap props;
                props["facing"] = std::string(def.ValueOf(cur, "facing"));
                props["lit"]    = IsLit() ? "true" : "false";
                world->SetBlock(p.x, p.y, p.z, GetBlockId(),
                                World::UpdateFlags::All, def.IndexOf(props));
            }
        }
        // The counters move every tick while lit, and the menu's data diff has
        // to see them, so mark dirty on any motion — not only on item changes.
        if (changed || IsLit() || m_cookingTime > 0) MarkDirty();
    }

    void FurnaceBlockEntity::Save(Network::PacketBuffer& out) const {
        BaseContainerBlockEntity::Save(out);
        out.WriteVarInt(static_cast<uint32_t>(m_litTime));
        out.WriteVarInt(static_cast<uint32_t>(m_litDuration));
        out.WriteVarInt(static_cast<uint32_t>(m_cookingTime));
        out.WriteVarInt(static_cast<uint32_t>(m_cookingTotal));
        out.WriteFloat(m_storedXp);
    }

    void FurnaceBlockEntity::Load(Network::PacketReader& in) {
        BaseContainerBlockEntity::Load(in);
        if (!in.HasMore()) return;
        m_litTime      = static_cast<int>(in.ReadVarInt());
        m_litDuration  = static_cast<int>(in.ReadVarInt());
        m_cookingTime  = static_cast<int>(in.ReadVarInt());
        m_cookingTotal = static_cast<int>(in.ReadVarInt());
        if (in.HasMore()) m_storedXp = in.ReadFloat();
    }

} // namespace Game
