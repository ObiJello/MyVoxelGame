// File: src/server/commands/GameModeCommand.cpp
#include "GameModeCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include <cctype>
#include <optional>
#include <string>

namespace Server {

    void GameModeCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("gamemode", GameModeCommand::Execute);
    }

    namespace {

        bool CaseInsensitiveEquals(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        }

        // MC GameModeArgument accepts the full names; we also take the
        // classic short forms + numeric ids as a convenience.
        std::optional<GameMode> ParseGameMode(std::string arg) {
            for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (arg == "survival"  || arg == "s"  || arg == "0") return GameMode::SURVIVAL;
            if (arg == "creative"  || arg == "c"  || arg == "1") return GameMode::CREATIVE;
            if (arg == "adventure" || arg == "a"  || arg == "2") return GameMode::ADVENTURE;
            if (arg == "spectator" || arg == "sp" || arg == "3") return GameMode::SPECTATOR;
            return std::nullopt;
        }

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

    void GameModeCommand::Execute(ServerPlayer& sender,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        if (args.empty()) {
            connection.SendChatMessage("Usage: /gamemode <survival|creative|adventure|spectator> [player]", 1);
            return;
        }

        auto mode = ParseGameMode(args[0]);
        if (!mode) {
            connection.SendChatMessage("Unknown game mode: " + args[0], 1);
            return;
        }

        // Resolve target: sender by default, named player with args[1]
        // (PlayerList.getPlayerByName, same walk as KickCommand).
        ServerPlayer*     target     = &sender;
        ServerConnection* targetConn = &connection;
        if (args.size() > 1) {
            std::shared_ptr<PlayerSession> targetSession;
            for (const auto& session : sessionManager.GetAllSessions()) {
                if (session && session->GetPlayer() &&
                    CaseInsensitiveEquals(session->GetPlayer()->getName(), args[1])) {
                    targetSession = session;
                    break;
                }
            }
            if (!targetSession || !targetSession->GetPlayer() || !targetSession->GetConnection()) {
                connection.SendChatMessage("Player not found: " + args[1], 1);
                return;
            }
            target     = targetSession->GetPlayer();
            targetConn = targetSession->GetConnection();
        }

        // MC ServerPlayer.setGameMode returns false when already in that
        // mode and sends nothing; keep the same short-circuit.
        if (target->getGameMode() == *mode) {
            connection.SendChatMessage(
                std::string("Nothing changed. ") + target->getName() +
                " is already in " + GameModeDisplayName(*mode), 1);
            return;
        }

        target->setGameMode(*mode);
        targetConn->SendPlayerAbilities(*target);

        Log::Info("[GameModeCommand] %s set %s to game mode %d",
                  sender.getName().c_str(), target->getName().c_str(),
                  static_cast<int>(*mode));

        // commands.gamemode.success.self / .other
        if (target == &sender) {
            connection.SendChatMessage(
                std::string("Set own game mode to ") + GameModeDisplayName(*mode), 1);
        } else {
            connection.SendChatMessage(
                std::string("Set ") + target->getName() + "'s game mode to " +
                GameModeDisplayName(*mode), 1);
            // gameMode.changed — tell the target too (MC sends this when
            // someone else changes your mode).
            targetConn->SendChatMessage(
                std::string("Your game mode has been updated to ") + GameModeDisplayName(*mode), 1);
        }
    }

} // namespace Server
