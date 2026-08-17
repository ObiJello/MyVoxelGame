// File: src/common/world/level/WorldDrops.cpp
#include "WorldDrops.hpp"
#include "common/entity/Item.hpp"
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

} // namespace Game
