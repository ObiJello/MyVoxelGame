// File: src/server/level/MobLevelTransfer.cpp
#include "server/level/MobLevelTransfer.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerEntityTracker.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/level/DimensionId.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Server::MobLevelTransfer {

    namespace {

        // The tracker's removal packets for `id`, sent in the old level's
        // name at once — before the new level's tracker announces the entity.
        void UntrackIn(ServerLevel& from, int32_t id) {
            ServerEntityTracker* tracker = from.MobTracker();
            if (!tracker) return;
            std::vector<EntityPacketOut> outgoing;
            tracker->RemoveEntity(id, outgoing);
            IntegratedServer* server = g_integratedServer.get();
            PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
            if (!sessions) return;
            for (const EntityPacketOut& packet : outgoing) {
                auto session = sessions->GetSession(packet.connectionId);
                if (!session || !session->GetConnection()) continue;
                session->GetConnection()->SendPacketIn(from.Dimension(), static_cast<uint8_t>(packet.packetId),
                                                       packet.payload);
            }
        }

        // Every mob in the tree, the root first (depth first, in seat order).
        void CollectMobs(Game::Mob& mob, std::vector<Game::Mob*>& out) {
            out.push_back(&mob);
            for (Game::Entity* p : std::vector<Game::Entity*>(mob.GetPassengers())) {
                if (auto* rider = dynamic_cast<Game::Mob*>(p); rider && !p->IsPlayer()) CollectMobs(*rider, out);
            }
        }

    } // namespace

    bool TransferTree(ServerLevel& from, ServerLevel& to, Game::Mob& root) {
        if (&from == &to) return false;
        MobManager* fromMobs = from.Mobs();
        MobManager* toMobs = to.Mobs();
        ServerLevelBridge* toBridge = to.MobLevel();
        ServerLevelBridge* fromBridge = from.MobLevel();
        if (!fromMobs || !toMobs || !toBridge || !fromBridge) return false;

        std::vector<Game::Mob*> tree;
        CollectMobs(root, tree);

        // Out of the old level: the watchers first, then the ownership.
        std::vector<std::unique_ptr<Game::Mob>> owned;
        owned.reserve(tree.size());
        for (Game::Mob* mob : tree) {
            UntrackIn(from, mob->GetId());
            std::unique_ptr<Game::Mob> extracted = fromMobs->Extract(mob->GetId());
            if (!extracted) {
                Log::Warning("[MobLevelTransfer] Mob #%d was not in its level; left behind", mob->GetId());
                if (mob != &root && mob->IsPassenger()) mob->RemoveVehicle();
                continue;
            }
            owned.push_back(std::move(extracted));
        }
        if (owned.empty() || owned.front().get() != &root) {
            // The root itself could not leave: put back whatever did.
            for (auto& m : owned) fromMobs->AddExisting(std::move(m));
            return false;
        }

        // Nothing of the old level remembered across, either way.
        for (Game::Mob* mob : tree) {
            for (PlayerEntityView* view : fromBridge->PlayerViews()) mob->ClearReferenceTo(view);
            for (const auto& [otherId, other] : fromMobs->All()) {
                if (!other) continue;
                mob->ClearReferenceTo(other.get());
                other->ClearReferenceTo(mob);
            }
        }

        // Into the new level, rebound to its bridge.
        for (auto& m : owned) {
            Game::Mob* raw = m.get();
            const int32_t id = raw->GetId();
            raw->SetLevel(toBridge);
            if (!toMobs->AddExisting(std::move(m))) {
                // Process-wide ids make this impossible; if it happens the
                // mob is lost rather than duplicated.
                Log::Warning("[MobLevelTransfer] Mob #%d lost crossing to %s: id taken", id,
                             std::string(Game::DimensionName(to.Dimension())).c_str());
            }
        }
        return true;
    }

} // namespace Server::MobLevelTransfer
