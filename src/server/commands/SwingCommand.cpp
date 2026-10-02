// File: src/server/commands/SwingCommand.cpp
#include "SwingCommand.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"

#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/PlayerSwingS2CPacket.hpp"

namespace Server {

    namespace {

        // LivingEntity.swing(hand, animation, updateSelf = true) for a
        // player: ClientboundAnimatePacket to everyone tracking them AND to
        // themselves (sendToTrackingPlayersAndSelf) — the swinger's client
        // swings its own first-person arm (ClientPacketHandler).
        void SwingPlayer(const SelectedEntity& target, uint8_t hand, PlayerSessionManager& sessions) {
            Network::PlayerSwingS2CPacket swing;
            swing.playerId = target.session ? target.session->GetPlayerId() : static_cast<uint32_t>(target.id);
            swing.hand = hand;
            const auto data = Network::Serialization::Serialize(swing);
            for (const auto& session : sessions.GetAllSessions()) {
                if (!session) continue;
                if (ServerConnection* conn = session->GetConnection()) {
                    conn->SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerSwingS2C), data);
                }
            }
        }

    } // namespace

    void SwingCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("swing", SwingCommand::Execute,
            Cmd::Root().Executes()
                .Then(Cmd::Argument("targets", Cmd::Arg::Entities).Executes()
                    .Then(Cmd::Literals({"mainhand", "offhand"}))));
    }

    void SwingCommand::Execute(const CommandSourceStack& source,
                               const std::vector<std::string>& args,
                               ServerConnection& connection,
                               PlayerSessionManager& sessionManager) {
        using namespace EntityCmd;
        if (args.size() > 2 || (args.size() == 2 && args[1] != "mainhand" && args[1] != "offhand")) {
            Failure(connection, "Incorrect argument for command");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("swing")) {
                Failure(connection, line);
            }
            return;
        }
        std::vector<SelectedEntity> targets;
        std::string error;
        if (args.empty()) {
            // getEntityOrException: the source's entity.
            if (!source.entity) {
                Failure(connection, "An entity is required to run this command here");
                return;
            }
            SelectedEntity self = *source.entity;
            if (!RefreshSelectedEntity(source, self)) {
                Failure(connection, Tr("commands.swing.failed.notliving"));
                return;
            }
            targets.push_back(std::move(self));
        } else if (!ResolveSelector(args[0], SelectorKind::Entities, source, targets, error)) {
            Failure(connection, error);
            return;
        }
        const uint8_t hand = args.size() == 2 && args[1] == "offhand" ? 1 : 0;

        int swung = 0;
        std::string only;
        for (const SelectedEntity& target : targets) {
            if (target.kind == SelectedEntity::Kind::Player) {
                if (!target.player) continue;
                SwingPlayer(target, hand, sessionManager);
            } else if (target.kind == SelectedEntity::Kind::Mob && target.mob) {
                // The mob's arm swing every watcher sees (the swing entity
                // event). Mob models animate the attacking arm only, so the
                // off hand swings the same arm.
                target.mob->Swing();
            } else {
                continue;   // not a LivingEntity
            }
            if (++swung == 1) only = DisplayName(target);
        }
        if (swung == 0) {
            Failure(connection, Tr("commands.swing.failed.notliving"));
            return;
        }
        if (swung == 1) Success(source, connection, /*broadcast=*/true, Tr("commands.swing.success.single", {only}));
        else            Success(source, connection, /*broadcast=*/true, Tr("commands.swing.success.multiple", {std::to_string(swung)}));
    }

} // namespace Server
