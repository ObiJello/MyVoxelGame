// File: src/common/world/block/entity/CraftingTableBlockEntity.cpp
#include "CraftingTableBlockEntity.hpp"

#include "common/network/ItemStackSerialization.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <atomic>

namespace Game {

    namespace {
        std::atomic<CraftingTableBlockEntity::ViewerCounter> s_viewerCounter{nullptr};
    } // namespace

    void CraftingTableBlockEntity::SetViewerCounter(ViewerCounter counter) {
        s_viewerCounter.store(counter, std::memory_order_release);
    }

    int CraftingTableBlockEntity::CountViewers(ILevelWrite& level, const glm::ivec3& pos) {
        const ViewerCounter counter = s_viewerCounter.load(std::memory_order_acquire);
        return counter ? counter(level, pos) : -1;
    }

    std::vector<ItemStack> CraftingTableBlockEntity::TakeGridContents() {
        std::vector<ItemStack> out;
        for (int i = 0; i < GRID_SIZE; ++i) {
            const ItemStack& stack = m_grid.GetItem(i);
            if (stack.IsEmpty()) continue;
            out.push_back(stack);
            m_grid.SetItem(i, ItemStack{});
        }
        m_result.SetItem(0, ItemStack{});
        return out;
    }

    void CraftingTableBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world || world->IsClientSide()) return;
        // Idle = nothing stored and nobody in it. An unknown viewer count
        // (no server counter installed) is never idle. A table holding items
        // keeps its entity whatever the rule says — that is what makes
        // turning the rule off lossless.
        if (!m_grid.IsEmpty()) return;
        const glm::ivec3 pos = GetWorldPos();
        if (CountViewers(*world, pos) != 0) return;
        // Last use of `this`: the removal destroys it. The tick walker
        // re-resolves every entry before ticking it, so a self-removal is
        // safe there (the piston's moving cell does the same).
        world->RemoveBlockEntity(pos);
    }

    void CraftingTableBlockEntity::PreRemoveSideEffects(ILevelWrite& level, const glm::ivec3& pos,
                                                        BlockState /*oldState*/) {
        if (level.IsClientSide()) return;
        // MC Containers.dropContents(level, pos, container): each stack from
        // a random point in the cell, the way a broken chest spills.
        for (ItemStack& stack : TakeGridContents()) {
            DropContainerItemStack(level, glm::dvec3(pos), std::move(stack));
        }
    }

    void CraftingTableBlockEntity::Save(Network::PacketBuffer& out) const {
        out.WriteVarInt(static_cast<uint32_t>(GRID_SIZE));
        for (int i = 0; i < GRID_SIZE; ++i) {
            Network::Serialization::WriteItemStack(out, m_grid.GetItem(i));
        }
    }

    void CraftingTableBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        const uint32_t count = in.ReadVarInt();
        for (uint32_t i = 0; i < count; ++i) {
            if (!in.HasMore()) break;
            const ItemStack stack = Network::Serialization::ReadItemStack(in);
            if (i < static_cast<uint32_t>(GRID_SIZE)) m_grid.SetItem(static_cast<int>(i), stack);
        }
    }

} // namespace Game
