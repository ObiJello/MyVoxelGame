// File: src/common/world/block/entity/CraftingTableBlockEntity.hpp
//
// Engine block entity (`obeycraft:crafting_table`) behind the
// `shared_crafting_tables` rule. Vanilla's crafting table has no block entity:
// MC CraftingMenu owns a TransientCraftingContainer that lives only while the
// screen is open, and CraftingMenu.removed → clearContainer hands the grid
// back to the player. With the rule on, the 3x3 grid lives HERE instead, on
// the block, so
//
//   • closing the screen leaves the items in the table (the shared
//     CraftingMenu skips clearContainer), and they are saved with the chunk;
//   • every player with the table open holds a CraftingMenu over this ONE
//     grid — MC's several-menus-on-one-Container model, as a chest has it.
//     PlayerSession's per-tick broadcastChanges diff sends each viewer every
//     slot another viewer changed, exactly as it does for a chest.
//
// The output square is here too (Result()), shared like the grid. It is
// derived state — the recipe lookup over the grid — and is never saved: the
// menu that changed the grid rewrites it at the end of its click
// (AbstractCraftingMenu::SlotsChanged), so every viewer's next diff sees the
// same output and two players racing for one craft are settled in click
// order on the server thread (the loser's click finds the square empty or
// re-filled and is corrected by the diff).
//
// The entity is attached LAZILY: it is not in BlockEntityTypes' per-block
// table (a crafting table placed or generated gets none, so the chunk mesher,
// pistons and every "has a block entity" check see the vanilla block). The
// session creates it when a table is opened with the rule on, and it removes
// itself (Tick) once its grid is empty and nobody has it open — a table only
// carries one while it holds something or is in use, so a vanilla world stays
// vanilla on disk.
//
// It is deliberately NOT an IContainer itself: hoppers, droppers, comparators
// and copper golems find containers by casting the cell's block entity, and a
// crafting table is none of theirs in vanilla. The grid is a member that is.
//
// Rule turned off with items still stored (lossless, PlayerSession::
// CreateCraftingTableMenu): the stored grid is kept until the table is next
// opened, then moved into the opener's private (vanilla) grid — so the usual
// close hands it to their inventory — and the entity is removed. While
// players who opened it under the rule are still in it, a newcomer joins
// their shared session instead, and the hand-over happens at the first open
// after the last of them leaves. Breaking the table drops the grid either
// way (PreRemoveSideEffects, MC Containers.dropContents).
#pragma once

#include "BlockEntity.hpp"
#include "common/inventory/Container.hpp"
#include "common/inventory/SimpleContainer.hpp"

#include <array>
#include <vector>

namespace Game {

    class CraftingTableBlockEntity : public BlockEntity {
    public:
        static constexpr int GRID_WIDTH  = 3;
        static constexpr int GRID_HEIGHT = 3;
        static constexpr int GRID_SIZE   = GRID_WIDTH * GRID_HEIGHT;

        CraftingTableBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId), m_grid(*this) {}

        // The 3x3 grid (MC CraftingContainer, row-major) and the output
        // square every viewer's CraftingMenu builds its slots over.
        IContainer&       Grid()         { return m_grid; }
        const IContainer& Grid()   const { return m_grid; }
        IContainer&       Result()       { return m_result; }

        bool IsGridEmpty() const { return m_grid.IsEmpty(); }
        // Empties the grid, returning what was in it (empty stacks skipped).
        // The output square goes with it — it has nothing left to show.
        std::vector<ItemStack> TakeGridContents();

        // Self-removal once idle (see the header note). Server only.
        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;

        // MC Containers.dropContents for a broken / exploded / replaced table:
        // every stored stack spills at the block. Server only.
        void PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos, BlockState oldState) override;

        // Binary form (the wire, and anything that round-trips an entity
        // through Save/Load): the grid's nine stacks, the shape
        // BaseContainerBlockEntity uses. Disk is BlockEntityNbt's — an Items
        // list, as a container's.
        void Save(Network::PacketBuffer& out) const override;
        void Load(Network::PacketReader& in) override;

        // MC ContainerOpenersCounter.getPlayersWithContainerOpen: how many
        // players have a shared crafting menu over the table at `pos` in
        // `level`. The server installs the counter (it owns the sessions);
        // with none installed the count is unknown and reported as -1, which
        // every caller treats as "in use".
        using ViewerCounter = int (*)(ILevelWrite& level, const glm::ivec3& pos);
        static void SetViewerCounter(ViewerCounter counter);
        static int  CountViewers(ILevelWrite& level, const glm::ivec3& pos);

    private:
        // The grid as a Container. Every write marks the entity changed so the
        // chunk is saved (BlockEntity dirty bit, drained by the tick walker).
        class GridContainer : public IContainer {
        public:
            explicit GridContainer(CraftingTableBlockEntity& owner) : m_owner(owner) {}
            int GetContainerSize() const override { return GRID_SIZE; }
            ItemStack& GetItem(int index) override {
                static ItemStack scratch{};
                if (index < 0 || index >= GRID_SIZE) { scratch = ItemStack{}; return scratch; }
                return m_items[static_cast<size_t>(index)];
            }
            const ItemStack& GetItem(int index) const override {
                static const ItemStack kEmpty{};
                if (index < 0 || index >= GRID_SIZE) return kEmpty;
                return m_items[static_cast<size_t>(index)];
            }
            void SetItem(int index, const ItemStack& stack) override {
                if (index < 0 || index >= GRID_SIZE) return;
                m_items[static_cast<size_t>(index)] = stack;
                SetChanged();
            }
            void SetChanged() override { m_owner.MarkDirty(); }

        private:
            CraftingTableBlockEntity&           m_owner;
            std::array<ItemStack, GRID_SIZE>    m_items{};
        };

        GridContainer   m_grid;
        SimpleContainer m_result{1};
    };

} // namespace Game
