// File: src/server/commands/SpectateCommand.cpp
#include "SpectateCommand.hpp"
#include "EntityCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../player/ServerPlayer.hpp"
#include "../player/SpectatorMode.hpp"
#include "../session/PlayerSession.hpp"

#include "common/entity/Mob.hpp"

#include <optional>

namespace Server {

    void SpectateCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("spectate", SpectateCommand::Execute,
            Cmd::Root().Executes()
                .Then(Cmd::Argument("target", Cmd::Arg::Entity).Executes()
                    .Then(Cmd::Argument("player", Cmd::Arg::Player).Executes())));
    }

    void SpectateCommand::Execute(const CommandSourceStack& source,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        (void)sessionManager;
        using namespace EntityCmd;
        if (args.size() > 2) {
            Failure(connection, "Incorrect argument for command");
            for (const std::string& line : g_integratedServer->GetCommandDispatcher().GetUsageLines("spectate")) {
                Failure(connection, line);
            }
            return;
        }
        std::string error;

        // The spectating player: the named one, else getPlayerOrException.
        SelectedEntity spectator;
        if (args.size() == 2) {
            std::vector<SelectedEntity> players;
            if (!ResolveSelector(args[1], SelectorKind::Player, source, players, error)) {
                Failure(connection, error);
                return;
            }
            spectator = players.front();
        } else {
            ServerPlayer* self = source.ExecutorPlayer();
            if (!self || !DescribePlayer(*self, source, spectator)) {
                Failure(connection, CommandSourceStack::kPlayerRequired);
                return;
            }
        }
        if (!spectator.player || !spectator.session) return;

        std::optional<SelectedEntity> target;
        if (!args.empty()) {
            std::vector<SelectedEntity> found;
            if (!ResolveSelector(args[0], SelectorKind::Entity, source, found, error)) {
                Failure(connection, error);
                return;
            }
            target = found.front();
        }

        if (target && target->kind == SelectedEntity::Kind::Player && target->player == spectator.player) {
            Failure(connection, Tr("commands.spectate.self"));
            return;
        }
        if (!spectator.player->isSpectator()) {
            Failure(connection, Tr("commands.spectate.not_spectator", {DisplayName(spectator)}));
            return;
        }
        if (target) {
            // EntityType.clientTrackingRange() == 0 cannot be spectated. Here
            // too: what the camera cannot follow — a dropped item or orb (no
            // Entity to ride along with), or anything outside the
            // spectator's own level (the camera follows within one level).
            const bool followable = (target->kind == SelectedEntity::Kind::Mob && target->mob) ||
                                    target->kind == SelectedEntity::Kind::Player;
            if (!followable || target->dimension != spectator.dimension) {
                Failure(connection, Tr("commands.spectate.cannot_spectate", {DisplayName(*target)}));
                return;
            }
            // Spectator::SetCamera takes a mob id or a player's id.
            const int32_t cameraId = target->kind == SelectedEntity::Kind::Player
                ? static_cast<int32_t>(target->session ? target->session->GetPlayerId() : target->id)
                : target->id;
            Spectator::SetCamera(*spectator.session, cameraId);
            Success(source, connection, /*broadcast=*/false, Tr("commands.spectate.success.started", {DisplayName(*target)}));
        } else {
            Spectator::SetCamera(*spectator.session, ServerPlayer::kSelfCamera);
            Success(source, connection, /*broadcast=*/false, Tr("commands.spectate.success.stopped"));
        }
    }

} // namespace Server
