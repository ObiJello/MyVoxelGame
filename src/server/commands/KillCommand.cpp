// File: src/server/commands/KillCommand.cpp
#include "KillCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/core/Log.hpp"
#include <cctype>

namespace Server {

    void KillCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("kill", KillCommand::Execute);
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
    }

    void KillCommand::Execute(ServerPlayer& sender,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& sessionManager) {
        ServerPlayer* target = &sender;
        if (!args.empty()) {
            target = nullptr;
            for (const auto& session : sessionManager.GetAllSessions()) {
                if (session && session->GetPlayer() &&
                    CaseInsensitiveEquals(session->GetPlayer()->getName(), args[0])) {
                    target = session->GetPlayer();
                    break;
                }
            }
            if (!target) {
                connection.SendChatMessage("Player not found: " + args[0], 1);
                return;
            }
        }

        // MC /kill deals kill() (bypass) — creative dies too. Our damage()
        // is gamemode-gated, so creative targets get an explicit refusal
        // note instead of silently no-oping.
        if (target->getGameMode() == GameMode::CREATIVE ||
            target->getGameMode() == GameMode::SPECTATOR) {
            connection.SendChatMessage(target->getName() + " is invulnerable (creative/spectator)", 1);
            return;
        }

        target->damage(1000.0f, DamageSource::VOID_DAMAGE);
        connection.SendChatMessage("Killed " + target->getName(), 1);
        Log::Info("[KillCommand] %s killed %s",
                  sender.getName().c_str(), target->getName().c_str());
    }

} // namespace Server
