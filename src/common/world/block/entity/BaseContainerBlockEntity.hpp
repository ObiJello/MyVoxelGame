// File: src/common/world/block/entity/BaseContainerBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.BaseContainerBlockEntity — a
// block entity that IS a Container. Chests, barrels, shulker boxes, furnaces,
// hoppers, dispensers and droppers all derive from it in MC and here.
//
// Before this, no block entity could hold an item at all (ChestBlockEntity's
// header listed `items[27]` as future work), which is why every container
// screen was blocked on the same thing rather than on its own menu.
//
// Two roles in one object, exactly as MC does it:
//   • BlockEntity — owned by the chunk, ticked, saved, network-synced.
//   • IContainer  — the raw stack store a menu's Slots point at, so
//                   AbstractContainerMenu's click code works over a chest with
//                   no idea it isn't the player's inventory.
//
// SetChanged() marks the BE dirty, which is what gets the contents saved with
// the chunk and pushed to clients. Menus never have to remember to do it: Slot
// calls it through the container on every mutation.
#pragma once

#include "BlockEntity.hpp"
#include "common/inventory/Container.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"
#include <vector>

namespace Game {

    class BaseContainerBlockEntity : public BlockEntity, public IContainer {
    public:
        BaseContainerBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos,
                                 BlockID blockId, int containerSize)
            : BlockEntity(type, worldPos, blockId),
              m_items(static_cast<size_t>(containerSize)) {}

        // ── IContainer ────────────────────────────────────────────────────
        int GetContainerSize() const override { return static_cast<int>(m_items.size()); }

        ItemStack& GetItem(int index) override {
            static ItemStack scratch{};
            if (index < 0 || index >= GetContainerSize()) { scratch = ItemStack{}; return scratch; }
            return m_items[static_cast<size_t>(index)];
        }
        const ItemStack& GetItem(int index) const override {
            static const ItemStack kEmpty{};
            if (index < 0 || index >= GetContainerSize()) return kEmpty;
            return m_items[static_cast<size_t>(index)];
        }
        void SetItem(int index, const ItemStack& stack) override {
            if (index < 0 || index >= GetContainerSize()) return;
            m_items[static_cast<size_t>(index)] = stack;
            SetChanged();
        }

        // MC Container.setChanged → BlockEntity.setChanged. The dirty bit is
        // what the per-tick BE walker drains to save the chunk and broadcast
        // BlockEntityDataS2C, so a container that mutates without it silently
        // loses its contents on reload.
        void SetChanged() override { MarkDirty(); }

        bool IsEmpty() const {
            for (const ItemStack& s : m_items) if (!s.IsEmpty()) return false;
            return true;
        }

        // MC Containers.dropContents — the block was broken, hand everything
        // back. Returns the contents and empties the container; the caller
        // decides where they go (today: into the breaker's inventory, since
        // there are no item entities yet).
        std::vector<ItemStack> TakeAllContents() {
            std::vector<ItemStack> out;
            for (ItemStack& s : m_items) {
                if (!s.IsEmpty()) out.push_back(s);
                s.Clear();
            }
            if (!out.empty()) SetChanged();
            return out;
        }

        // ── Persistence / sync ────────────────────────────────────────────
        // Subclasses with extra state call these first, then write their own —
        // and must Load in the same order.
        void Save(Network::PacketBuffer& out) const override {
            out.WriteVarInt(static_cast<uint32_t>(m_items.size()));
            for (const ItemStack& s : m_items) {
                Network::Serialization::WriteItemStack(out, s);
            }
        }
        void Load(Network::PacketReader& in) override {
            if (!in.HasMore()) return;
            const uint32_t count = in.ReadVarInt();
            for (uint32_t i = 0; i < count; ++i) {
                if (!in.HasMore()) break;
                const ItemStack stack = Network::Serialization::ReadItemStack(in);
                // Tolerate a size change across versions: extra entries are
                // dropped rather than resizing the container out from under
                // the menu slot layout that is built from its declared size.
                if (i < m_items.size()) m_items[i] = stack;
            }
        }

    private:
        std::vector<ItemStack> m_items;
    };

} // namespace Game
