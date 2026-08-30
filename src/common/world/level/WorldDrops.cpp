// File: src/common/world/level/WorldDrops.cpp
#include "WorldDrops.hpp"
#include "common/entity/Item.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/level/World.hpp"
#include "common/world/loot/LootTables.hpp"
#include "common/world/block/Direction.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/ItemEntityManager.hpp"

namespace Game {

    bool DropItemStackNear(const glm::ivec3& pos, const ItemStack& stack) {
        if (stack.IsEmpty()) return true;   // nothing to deliver

        auto* server = Server::g_integratedServer.get();
        if (!server) return false;

        auto* items = server->GetItemEntities();
        if (!items) return false;

        items->PopResource(pos, stack);
        return true;
    }

    bool DropItemStackAt(const glm::dvec3& pos, const ItemStack& stack) {
        if (stack.IsEmpty()) return true;

        auto* server = Server::g_integratedServer.get();
        if (!server) return false;

        auto* items = server->GetItemEntities();
        if (!items) return false;

        items->SpawnAtLocation(pos, stack);
        return true;
    }

    bool DropItemStackFromFace(const glm::ivec3& pos, int face, const ItemStack& stack) {
        if (stack.IsEmpty()) return true;

        auto* server = Server::g_integratedServer.get();
        if (!server) return false;

        auto* items = server->GetItemEntities();
        if (!items) return false;

        // Face ordinals match Game::Direction (0=down .. 5=east) by
        // construction — Direction's own comment pins them to MC's enum order
        // so anything round-tripping through a numeric id lines up.
        if (face < 0 || face > 5) return false;
        items->PopResourceFromFace(pos, static_cast<Direction>(face), stack);
        return true;
    }

    void DestroyBlockWithDrops(ILevelWrite& level, const glm::ivec3& pos) {
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        const BlockID    id    = state.Block();
        if (id == BlockID::Air) return;

        // Roll the loot BEFORE clearing — the tables key on the block that is
        // still there, and on its state.
        JavaRandom rng(static_cast<uint64_t>(
            (static_cast<int64_t>(pos.x) * 3129871) ^
            (static_cast<int64_t>(pos.z) * 116129781) ^
             static_cast<int64_t>(pos.y)));
        LootContext ctx;
        ctx.block      = id;
        ctx.blockState = state.Index();
        ctx.pos        = pos;
        ctx.rng        = &rng;
        // location_check conditions want the real world. Null for the client's
        // predicted level, which is correct — those conditions then pass, and
        // the client is not rolling authoritative loot anyway.
        ctx.blocks     = &level;

        for (const ItemStack& drop : LootTables::GetDrops(ctx)) {
            DropItemStackNear(pos, drop);
        }
        level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
    }

} // namespace Game
