// File: src/server/commands/GameModeCommand.cpp
#include "GameModeCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "../player/SpectatorMode.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/GameRules.hpp"
#include <cctype>
#include <optional>
#include <string>

namespace Server {

    void GameModeCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // MC GameModeCommand: <gamemode> [<target>] — EntityArgument.players().
        dispatcher.RegisterCommand("gamemode", GameModeCommand::Execute,
            Cmd::Root().Then(Cmd::Argument("gamemode", Cmd::Arg::GameMode).Executes()
                .Then(Cmd::Argument("target", Cmd::Arg::Players).Executes())));
    }

    namespace {

        // MC's gameMode.<name> display strings ("Creative Mode", …).
        const char* GameModeDisplayName(GameMode mode) {
            switch (mode) {
                case GameMode::SURVIVAL:  return "Survival Mode";
                case GameMode::CREATIVE:  return "Creative Mode";
                case GameMode::ADVENTURE: return "Adventure Mode";
                case GameMode::SPECTATOR: return "Spectator Mode";
            }
            return "Unknown Mode";
        }

    } // namespace

    // MC GameModeArgument takes the four serialized names; this engine also
    // keeps the pre-1.13 short forms (s/c/a/sp) and numeric ids (0-3) that
    // players type out of habit, case-insensitively.
    std::optional<GameMode> GameModeCommand::ParseGameMode(const std::string& raw) {
        std::string arg = raw;
        for (char& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (arg == "survival"  || arg == "s"  || arg == "0") return GameMode::SURVIVAL;
        if (arg == "creative"  || arg == "c"  || arg == "1") return GameMode::CREATIVE;
        if (arg == "adventure" || arg == "a"  || arg == "2") return GameMode::ADVENTURE;
        if (arg == "spectator" || arg == "sp" || arg == "3") return GameMode::SPECTATOR;
        return std::nullopt;
    }

    void GameModeCommand::Execute(const CommandSourceStack& source,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        ServerPlayer& sender = *source.sender;
        if (args.empty()) {
            connection.SendChatMessage("Usage: /gamemode <survival|creative|adventure|spectator> [player]", 1);
            return;
        }

        auto mode = ParseGameMode(args[0]);
        if (!mode) {
            // argument.gamemode.invalid
            connection.SendChatMessage("Unknown game mode: " + args[0], 1);
            return;
        }

        // MC GameModeCommand: `targets` is EntityArgument.players(); without
        // it the target is source.getPlayerOrException() — the EXECUTOR, so
        // `/execute as Steve run gamemode creative` changes Steve.
        std::vector<SelectedEntity> targets;
        std::string error;
        if (args.size() > 1) {
            if (!ResolveSelector(args[1], SelectorKind::Players, source, targets, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
        } else if (!ResolveSelector("@s", SelectorKind::Player, source, targets, error)) {
            connection.SendChatMessage("A player is required to run this command here", 1);
            return;
        }

        const ServerPlayer* executor =
            source.entity ? (source.entity->kind == SelectedEntity::Kind::Player ? source.entity->player : nullptr)
                          : &sender;
        int changed = 0;
        for (const SelectedEntity& t : targets) {
            ServerPlayer* target = t.player;
            if (!target) continue;
            std::shared_ptr<PlayerSession> targetSession = t.session ? t.session
                                                                     : sessionManager.GetSession(target->getPlayerId());
            ServerConnection* targetConn = targetSession ? targetSession->GetConnection() : nullptr;

            // MC ServerPlayer.setGameMode returns false when already in that
            // mode and nothing is sent for that player.
            if (target->getGameMode() == *mode) {
                if (targets.size() == 1) {
                    connection.SendChatMessage(
                        std::string("Nothing changed. ") + target->getName() +
                        " is already in " + GameModeDisplayName(*mode), 1);
                }
                continue;
            }

            // MC ServerPlayer.setGameMode: the change plus its consequences —
            // the spectator camera, the abilities packet, UPDATE_GAME_MODE for
            // every tab list.
            if (targetSession) {
                Spectator::ChangeGameMode(*targetSession, *mode);
            } else {
                target->setGameMode(*mode);
                if (targetConn) targetConn->SendPlayerAbilities(*target);
                Spectator::BroadcastGameMode(*target);
            }
            ++changed;

            Log::Info("[GameModeCommand] %s set %s to game mode %d",
                      sender.getName().c_str(), target->getName().c_str(),
                      static_cast<int>(*mode));

            // MC logGamemodeChange: commands.gamemode.success.self when the
            // target is the source's entity, else .other to the source and
            // gameMode.changed to the target. Feedback goes to the player who
            // typed the command.
            if (target == executor) {
                source.SendSuccess(connection,
                    std::string("Set own game mode to ") + GameModeDisplayName(*mode), true);
            } else {
                // MC: gameMode.changed to the target only under
                // send_command_feedback, before the source's success line.
                if (targetConn && targetConn != &connection &&
                    Game::Rules::GetBool(Game::Rules::Id::SendCommandFeedback)) {
                    targetConn->SendChatMessage(
                        std::string("Your game mode has been updated to ") + GameModeDisplayName(*mode), 1);
                }
                source.SendSuccess(connection,
                    std::string("Set ") + target->getName() + "'s game mode to " +
                    GameModeDisplayName(*mode), true);
            }
        }
        (void)changed;
    }

} // namespace Server
