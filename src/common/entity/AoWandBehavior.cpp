// File: src/common/entity/AoWandBehavior.cpp
//
// The occlusion wand: right-click one block, then another, and every block
// in the box between them bakes no ambient occlusion — for every player,
// now and after a relog (IntegratedServer::AddAoRegion keeps the boxes,
// saves them, and sends the dimension's list to every client holding it).
// Shift + right-click a block inside a box removes that box.
#include "Item.hpp"
#include "../core/Log.hpp"
#include "../world/block/BlockInteraction.hpp"
#include "../world/level/DimensionId.hpp"

#include "server/IntegratedServer.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/network/ServerConnection.hpp"

#include <cstdio>
#include <unordered_map>

namespace Game::Wands {

    namespace {
        struct Pending {
            bool       set = false;
            glm::ivec3 cell{0};
            Game::DimensionId dimension = Game::DimensionId::Overworld;
        };
        std::unordered_map<uint32_t, Pending> g_pending;   // by player id

        void Say(uint32_t playerId, const std::string& text) {
            if (!Server::g_integratedServer) return;
            auto* sessions = Server::g_integratedServer->GetSessionManager();
            if (!sessions) return;
            if (auto session = sessions->GetSession(playerId)) {
                if (auto* conn = session->GetConnection()) conn->SendChatMessage(text, 1);
            }
        }
    }

    UseResult OnAoWandUseOn(const UseOnContext& ctx, ItemStack& /*stack*/) {
        if (!ctx.world || !ctx.player || !Server::g_integratedServer) return UseResult::Pass;
        auto* player = static_cast<Server::ServerPlayer*>(ctx.player);
        const uint32_t playerId = player->getPlayerId();
        const glm::ivec3 cell = ctx.hitResult.blockPos;
        const Game::DimensionId dim = Game::DimensionFromRaw(player->getDimensionId());
        Pending& pending = g_pending[playerId];

        if (ctx.player->IsSneaking()) {
            pending = Pending{};
            const size_t removed = Server::g_integratedServer->RemoveAoRegionsAt(dim, cell);
            char buf[96];
            std::snprintf(buf, sizeof(buf), removed ? "Removed %zu occlusion box(es) here." : "No occlusion box here; selection cleared.", removed);
            Say(playerId, buf);
            return UseResult::Success;
        }

        if (!pending.set || pending.dimension != dim) {
            pending.set = true;
            pending.cell = cell;
            pending.dimension = dim;
            Say(playerId, "First corner set. Click the opposite corner of the box.");
            return UseResult::Success;
        }

        const glm::ivec3 a = pending.cell;
        pending = Pending{};
        Server::g_integratedServer->AddAoRegion(dim, a, cell);
        const glm::ivec3 size = glm::abs(cell - a) + glm::ivec3(1);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Ambient occlusion off in a %dx%dx%d box.", size.x, size.y, size.z);
        Say(playerId, buf);
        Log::Info("[AoWand] %s cleared occlusion in (%d,%d,%d)-(%d,%d,%d)", player->getName().c_str(),
                  a.x, a.y, a.z, cell.x, cell.y, cell.z);
        return UseResult::Success;
    }

} // namespace Game::Wands
