// File: src/common/world/block/entity/DecoratedPotBlockEntity.cpp
#include "DecoratedPotBlockEntity.hpp"

#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/data/DataComponents.hpp"

#include <chrono>

namespace Game {

    namespace {
        // MC DecoratedPotBlockEntity.EVENT_POT_WOBBLES.
        constexpr int kEventPotWobbles = 1;

        double NowSeconds() {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    } // namespace

    void DecoratedPotBlockEntity::Wobble(WobbleStyle style) {
        // MC wobble: level.blockEvent(pos, block, 1, style.ordinal()) — the
        // server runs triggerEvent itself and every client is told.
        ILevelWrite* level = GetLevel();
        if (!level || level->IsClientSide()) return;
        level->BlockEvent(GetWorldPos(), GetBlockId(), kEventPotWobbles, static_cast<int>(style));
    }

    bool DecoratedPotBlockEntity::TriggerEvent(int b0, int b1) {
        // MC triggerEvent: the wobble's start and style.
        if (b0 == kEventPotWobbles && b1 >= 0 && b1 < 2) {
            m_wobbleStartedAt = NowSeconds();
            m_lastWobbleStyle = b1;
            return true;
        }
        return BaseContainerBlockEntity::TriggerEvent(b0, b1);
    }

    void DecoratedPotBlockEntity::ApplyItemComponents(const DataComponentMap& components) {
        if (auto decorations = components.get(DataComponents::POT_DECORATIONS)) {
            for (int side = 0; side < kSideCount; ++side) m_sides[static_cast<size_t>(side)] = decorations->sides[static_cast<size_t>(side)];
        }
        // ItemContainerContents.copyOne: the first stack.
        if (auto contents = components.get(DataComponents::CONTAINER)) {
            SetItem(0, contents->items.empty() ? ItemStack{} : contents->items.front());
        }
        SetChanged();
    }

    void DecoratedPotBlockEntity::CollectComponents(DataComponentMap& out) const {
        PotDecorations decorations;
        for (int side = 0; side < kSideCount; ++side) decorations.sides[static_cast<size_t>(side)] = m_sides[static_cast<size_t>(side)];
        if (!decorations.IsEmpty()) out.set(DataComponents::POT_DECORATIONS, decorations);
        const ItemStack& item = BaseContainerBlockEntity::GetItem(0);
        if (!item.IsEmpty()) {
            ItemContainerContents contents;
            contents.items.push_back(item);
            out.set(DataComponents::CONTAINER, std::move(contents));
        }
    }

    void DecoratedPotBlockEntity::CarryClientState(const BlockEntity& previous) {
        // A data update mid-wobble keeps the tilt going.
        if (const auto* pot = dynamic_cast<const DecoratedPotBlockEntity*>(&previous)) {
            m_wobbleStartedAt = pot->m_wobbleStartedAt;
            m_lastWobbleStyle = pot->m_lastWobbleStyle;
        }
    }

    void DecoratedPotBlockEntity::Save(Network::PacketBuffer& out) const {
        // The stack (the base's one slot), then the four sides as item ids
        // (0 = none).
        BaseContainerBlockEntity::Save(out);
        for (ItemID side : m_sides) out.WriteVarInt(static_cast<uint32_t>(side));
    }

    void DecoratedPotBlockEntity::Load(Network::PacketReader& in) {
        BaseContainerBlockEntity::Load(in);
        for (ItemID& side : m_sides) {
            side = in.HasMore() ? static_cast<ItemID>(in.ReadVarInt()) : Items::Air;
        }
    }

} // namespace Game
