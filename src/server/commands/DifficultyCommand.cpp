#include "DifficultyCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../network/ServerConnection.hpp"
#include "common/core/Log.hpp"

#include <cctype>
#include <optional>
#include <string>

namespace Server {

    void DifficultyCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("difficulty", DifficultyCommand::Execute);
    }

    namespace {
        const char* kNames[4] = { "peaceful", "easy", "normal", "hard" };

        std::optional<int> Parse(std::string arg) {
            for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (int i = 0; i < 4; ++i) {
                if (arg == kNames[i] || arg == std::string(1, kNames[i][0]) || arg == std::to_string(i)) return i;
            }
            return std::nullopt;
        }
    }

    void DifficultyCommand::Execute(ServerPlayer& /*sender*/,
                                    const std::vector<std::string>& args,
                                    ServerConnection& connection,
                                    PlayerSessionManager& /*sessionManager*/) {
        if (!g_integratedServer) return;
        if (args.empty()) {
            connection.SendChatMessage(std::string("The difficulty is ") +
                                       kNames[g_integratedServer->GetDifficulty()], 1);
            return;
        }
        const auto wanted = Parse(args[0]);
        if (!wanted) {
            connection.SendChatMessage("Usage: /difficulty [peaceful|easy|normal|hard]", 1);
            return;
        }
        if (*wanted == g_integratedServer->GetDifficulty()) {
            connection.SendChatMessage(std::string("The difficulty is already ") + kNames[*wanted], 1);
            return;
        }
        g_integratedServer->SetDifficulty(*wanted);
        connection.SendChatMessage(std::string("The difficulty has been set to ") + kNames[*wanted], 1);
        Log::Info("[Difficulty] set to %s", kNames[*wanted]);
    }

} // namespace Server
