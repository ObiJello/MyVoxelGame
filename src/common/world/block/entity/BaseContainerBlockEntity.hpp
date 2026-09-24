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
//
// It is also MC's RandomizableContainerBlockEntity: a container placed by
// worldgen carries a `LootTable` key (and optional `LootTableSeed`) instead
// of items, and rolls its contents the first time anything reads a slot
// (RandomizableContainer.unpackLootTable — hooked on getItem / setItem /
// isEmpty / the menu open). MC keeps that in a subclass between
// BaseContainerBlockEntity and the chests; here every container type,
// including barrels and shulker boxes that have no class of their own,
// derives from this one, so the layer lives here.
#pragma once

#include "BlockEntity.hpp"
#include "common/inventory/Container.hpp"
#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <string>
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

        // The non-const read is the hook MC puts on getItem/removeItem (and
        // IContainer::RemoveItem goes through it), so hoppers, droppers,
        // comparators and menus all unpack. The const read deliberately does
        // NOT: Save() and the Anvil writer use it, and a chunk being saved or
        // synced must not roll every structure chest in range.
        ItemStack& GetItem(int index) override {
            UnpackLootTable();
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
            UnpackLootTable();
            if (index < 0 || index >= GetContainerSize()) return;
            m_items[static_cast<size_t>(index)] = stack;
            SetChanged();
        }

        // ── RandomizableContainer ─────────────────────────────────────────
        // MC setLootTable(lootTable, seed): `key` is the resource key
        // ("minecraft:chests/simple_dungeon"); an empty key clears it.
        void SetLootTable(std::string key, int64_t seed = 0) {
            m_lootTable = std::move(key);
            m_lootTableSeed = seed;
        }
        bool HasLootTable() const { return !m_lootTable.empty(); }
        const std::string& GetLootTable() const { return m_lootTable; }
        int64_t GetLootTableSeed() const { return m_lootTableSeed; }
        // MC RandomizableContainer.unpackLootTable(player): server-side only,
        // clears the key FIRST (fill's setItem re-enters here), then rolls
        // the table into the empty slots. A no-op when no key is set.
        // BlockEntityTypes.cpp — needs ILevelWrite + ChestLoot. MC
        // unpackLootTable(player): `luck` is the opening player's LUCK
        // attribute (player.getLuck()); the no-player paths (a hopper, the
        // block breaking) pass nothing — 0, as MC's null player does.
        void UnpackLootTable(float luck = 0.0f);

        // MC Container.setChanged → BlockEntity.setChanged. The dirty bit is
        // what the per-tick BE walker drains to save the chunk and broadcast
        // BlockEntityDataS2C, so a container that mutates without it silently
        // loses its contents on reload.
        void SetChanged() override;   // BlockEntityTypes.cpp — needs ILevelWrite

        bool IsEmpty() const override {
            // MC RandomizableContainerBlockEntity.isEmpty unpacks too: a
            // hopper asking "anything to pull?" must see the rolled contents.
            const_cast<BaseContainerBlockEntity*>(this)->UnpackLootTable();
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
        std::string m_lootTable;          // "" = none (MC's null)
        int64_t     m_lootTableSeed = 0;  // 0 = use the level's random
    };

} // namespace Game
