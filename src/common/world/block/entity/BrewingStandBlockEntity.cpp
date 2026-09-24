// File: src/common/world/block/entity/BrewingStandBlockEntity.cpp
#include "BrewingStandBlockEntity.hpp"

#include "common/entity/alchemy/PotionBrewing.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/block/GeneratedBlockStates.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <cmath>

namespace Game {

    bool BrewingStandBlockEntity::CanPlaceItem(int slot, const ItemStack& stack) const {
        if (slot == SLOT_FUEL) return GetBrewingFuelUses(stack) > 0;
        if (slot == SLOT_INGREDIENT) return IsBrewingReagent(stack);
        return IsBrewingPotionInput(stack) && GetItem(slot).IsEmpty();
    }

    void BrewingStandBlockEntity::GetSlotsForFace(Direction face, std::vector<int>& out) const {
        if (face == Direction::Up)        out = {SLOT_INGREDIENT};            // SLOTS_FOR_UP
        else if (face == Direction::Down) out = {0, 1, 2, SLOT_INGREDIENT};   // SLOTS_FOR_DOWN
        else                              out = {0, 1, 2, SLOT_FUEL};         // SLOTS_FOR_SIDES
    }

    bool BrewingStandBlockEntity::CanPlaceItemThroughFace(int slot, const ItemStack& stack,
                                                          Direction, bool) const {
        return CanPlaceItem(slot, stack);
    }

    bool BrewingStandBlockEntity::CanTakeItemThroughFace(int slot, const ItemStack& stack,
                                                         Direction) const {
        return slot == SLOT_INGREDIENT ? stack.itemId == Items::GlassBottle : true;
    }

    bool BrewingStandBlockEntity::IsBrewable() const {
        // MC isBrewable: a reagent in the ingredient slot and at least one
        // bottle a recipe accepts with it.
        const ItemStack& ingredient = GetItem(SLOT_INGREDIENT);
        if (ingredient.IsEmpty() || !IsBrewingReagent(ingredient)) return false;
        for (int dest = 0; dest < 3; ++dest) {
            const ItemStack& bottle = GetItem(dest);
            if (!bottle.IsEmpty() && FindBrewingResult(bottle, ingredient)) return true;
        }
        return false;
    }

    void BrewingStandBlockEntity::DoBrew(World* world) {
        // MC doBrew.
        for (int dest = 0; dest < 3; ++dest) {
            ItemStack& container = GetItem(dest);
            if (auto result = FindBrewingResult(container, GetItem(SLOT_INGREDIENT))) {
                container = *result;
            }
        }

        ItemStack& ingredient = GetItem(SLOT_INGREDIENT);
        const ItemID remainder = ItemRegistry::Get(ingredient.itemId).craftingRemainder;
        if (--ingredient.count <= 0) ingredient.Clear();
        if (remainder != Items::Air) {
            if (ingredient.IsEmpty()) {
                ingredient = ItemStack(remainder, 1);
            } else if (world) {
                // Containers.dropItemStack at the stand.
                DropItemStackNear(world->GetDimension(), GetWorldPos(), ItemStack(remainder, 1));
            }
        }
        // MC doBrew: level.levelEvent(1035, pos, 0) — BREWING_STAND_BREW.
        if (world) {
            PlayLevelEventSound(*world, nullptr, LevelEvent::SOUND_BREWING_STAND_BREW, GetWorldPos(), 0,
                                world->Random());
        }
    }

    void BrewingStandBlockEntity::Tick(World* world, float /*deltaTime*/) {
        // MC serverTick, branch for branch.
        bool changed = false;

        // Refuel a spent stand.
        ItemStack& fuel = GetItem(SLOT_FUEL);
        const int uses = GetBrewingFuelUses(fuel);
        if (m_fuel <= 0 && uses > 0) {
            m_fuel = uses;
            m_totalFuel = m_fuel;
            m_speedMultiplier = GetBrewingFuelSpeedMultiplier(fuel);
            const ItemID remainder = ItemRegistry::Get(fuel.itemId).craftingRemainder;
            if (--fuel.count <= 0) fuel.Clear();
            if (remainder != Items::Air) {
                if (fuel.IsEmpty()) {
                    fuel = ItemStack(remainder, 1);
                } else if (world) {
                    DropItemStackNear(world->GetDimension(), GetWorldPos(), ItemStack(remainder, 1));
                }
            }
            changed = true;
        }

        const bool brewable  = IsBrewable();
        const bool isBrewing = m_brewTime > 0;
        const ItemStack& ingredient = GetItem(SLOT_INGREDIENT);
        if (isBrewing) {
            --m_brewTime;
            const bool isDoneBrewing = m_brewTime == 0;
            if (isDoneBrewing && brewable) {
                DoBrew(world);
            } else if (!brewable || ingredient.itemId != m_ingredient) {
                m_brewTime = 0;
            }
            changed = true;
        } else if (brewable && m_fuel > 0) {
            const float speed = m_speedMultiplier > 0.0f ? m_speedMultiplier : 1.0f;
            --m_fuel;
            m_brewTime = static_cast<int>(std::ceil(static_cast<double>(400.0f / speed)));
            m_totalBrewTime = m_brewTime;
            m_ingredient = ingredient.itemId;
            changed = true;
        }

        // HAS_BOTTLE_0..2 follow the bottle slots (flag 2 — clients only).
        bool bits[3];
        for (int i = 0; i < 3; ++i) bits[i] = !GetItem(i).IsEmpty();
        if (!m_haveLastBits || bits[0] != m_lastBits[0] || bits[1] != m_lastBits[1] ||
            bits[2] != m_lastBits[2]) {
            m_haveLastBits = true;
            for (int i = 0; i < 3; ++i) m_lastBits[i] = bits[i];
            if (world) {
                const glm::ivec3 p = GetWorldPos();
                BlockState state = world->GetBlockState(p.x, p.y, p.z);
                if (state.Block() == GetBlockId()) {
                    static constexpr PropertyId kHasBottle[3] = {
                        PropertyId::HAS_BOTTLE_0, PropertyId::HAS_BOTTLE_1, PropertyId::HAS_BOTTLE_2,
                    };
                    BlockState updated = state;
                    for (int i = 0; i < 3; ++i) updated = WithBool(updated, kHasBottle[i], bits[i]);
                    if (updated != state) {
                        world->SetBlock(p.x, p.y, p.z, updated, World::UpdateFlags::UpdateClients);
                    }
                }
            }
        }

        // setChanged — and the menu's data diff needs every timer step.
        if (changed || m_brewTime > 0) {
            SetChanged();
            MarkDirty();
        }
    }

    void BrewingStandBlockEntity::RestoreIngredientFromSlot() {
        if (m_brewTime > 0) m_ingredient = GetItem(SLOT_INGREDIENT).itemId;
    }

    void BrewingStandBlockEntity::Save(Network::PacketBuffer& out) const {
        BaseContainerBlockEntity::Save(out);
        out.WriteVarInt(static_cast<uint32_t>(m_brewTime));
        out.WriteVarInt(static_cast<uint32_t>(m_totalBrewTime));
        out.WriteVarInt(static_cast<uint32_t>(m_fuel));
        out.WriteVarInt(static_cast<uint32_t>(m_totalFuel));
        out.WriteFloat(m_speedMultiplier);
    }

    void BrewingStandBlockEntity::Load(Network::PacketReader& in) {
        BaseContainerBlockEntity::Load(in);
        if (!in.HasMore()) return;
        m_brewTime      = static_cast<int>(in.ReadVarInt());
        m_totalBrewTime = static_cast<int>(in.ReadVarInt());
        m_fuel          = static_cast<int>(in.ReadVarInt());
        m_totalFuel     = static_cast<int>(in.ReadVarInt());
        if (in.HasMore()) m_speedMultiplier = in.ReadFloat();
        RestoreIngredientFromSlot();
    }

} // namespace Game
