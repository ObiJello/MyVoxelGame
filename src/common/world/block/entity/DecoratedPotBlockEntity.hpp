// File: src/common/world/block/entity/DecoratedPotBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.DecoratedPotBlockEntity — a
// decorated pot's four sides (PotDecorations: the sherd, or brick, on its
// back / left / right / front), the one stack it holds (MC
// ContainerSingleItem.BlockContainerSingleItem) and, for the trial chambers'
// corridor pots, the loot table that stack is rolled from
// (RandomizableContainer: pots/trial_chambers/corridor).
//
// It is a one-slot BaseContainerBlockEntity, so the loot-table layer, the
// hopper/comparator container view and the spill when the pot breaks
// (BlockEntity.preRemoveSideEffects -> Containers.dropContents, which
// unpacks first) all come from the base.
//
// Wobble (triggerEvent 1, WobbleStyle ordinal): the renderer's tilt when a
// stack goes in (POSITIVE) or an empty hand knocks it (NEGATIVE).
//
// Wire (BlockEntityDataS2C — MC getUpdateTag = saveCustomOnly): the stack
// and the four sides. Disk (BlockEntityNbt.cpp): sherds {back, left, right,
// front} (26.3's ItemStackTemplate compound), LootTable / LootTableSeed or
// item.
#pragma once

#include "BaseContainerBlockEntity.hpp"

#include <array>
#include <cstdint>

namespace Game {

    class DecoratedPotBlockEntity : public BaseContainerBlockEntity {
    public:
        // PotDecorations' sides in MC's field order.
        enum Side : int { kBack = 0, kLeft = 1, kRight = 2, kFront = 3, kSideCount = 4 };

        // MC DecoratedPotBlockEntity.WobbleStyle: POSITIVE(7), NEGATIVE(10)
        // ticks.
        enum class WobbleStyle : uint8_t { Positive = 0, Negative = 1 };
        static constexpr int kWobbleDuration[2] = { 7, 10 };

        DecoratedPotBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, 1) {
            m_sides.fill(Items::Air);
        }

        // The item on one side (a pottery sherd, or brick); Air = none (MC's
        // Optional.empty(), which renders as the blank side).
        ItemID GetSide(int side) const {
            return (side >= 0 && side < kSideCount) ? m_sides[static_cast<size_t>(side)] : Items::Air;
        }
        void SetSide(int side, ItemID item) {
            if (side >= 0 && side < kSideCount) m_sides[static_cast<size_t>(side)] = item;
        }
        bool HasDecorations() const {
            for (ItemID id : m_sides) if (id != Items::Air) return true;
            return false;
        }

        // MC wobble(style): the block event that makes every client (and
        // this side) tilt the pot.
        void Wobble(WobbleStyle style);

        // The client's animation clock: when the last wobble started (a
        // steady-clock reading in seconds — client block entities carry no
        // level to read game time from, so the renderer turns elapsed time
        // into ticks at 20 per second) and its style (none yet = -1).
        double WobbleStartedAtSeconds() const { return m_wobbleStartedAt; }
        int    LastWobbleStyle() const { return m_lastWobbleStyle; }

        // MC ContainerSingleItem: Container.getMaxStackSize (99), capped by
        // the item's own through GetMaxStackSize(stack).
        using IContainer::GetMaxStackSize;
        int GetMaxStackSize() const override { return 99; }

        // ── BlockEntity ───────────────────────────────────────────────────
        bool TriggerEvent(int b0, int b1) override;
        // (Its spill when the block goes — Containers.dropContents — is the
        // base's PreRemoveSideEffects.)
        // MC DecoratedPotBlockEntity.applyImplicitComponents /
        // collectImplicitComponents: POT_DECORATIONS and the one stack
        // (CONTAINER's first item).
        void ApplyItemComponents(const DataComponentMap& components) override;
        void CollectComponents(DataComponentMap& out) const override;
        void CarryClientState(const BlockEntity& previous) override;
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

    private:
        std::array<ItemID, kSideCount> m_sides{};
        double  m_wobbleStartedAt = 0.0;
        int     m_lastWobbleStyle = -1;
    };

} // namespace Game
