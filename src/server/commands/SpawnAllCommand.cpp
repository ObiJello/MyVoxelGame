// File: src/server/commands/SpawnAllCommand.cpp
#include "SpawnAllCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../session/PlayerSession.hpp"
#include "../IntegratedServer.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace Server {

    void SpawnAllCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("spawnall", SpawnAllCommand::Execute);
    }

    void SpawnAllCommand::Execute(ServerPlayer& sender,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        bool adults = true, babies = true;
        double spacing = 3.0;

        if (!args.empty()) {
            std::string mode = args[0];
            std::transform(mode.begin(), mode.end(), mode.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (mode == "adults")      babies = false;
            else if (mode == "babies") adults = false;
            else if (mode != "both") {
                connection.SendChatMessage("Usage: /spawnall [adults|babies|both] [spacing]", 1);
                return;
            }
        }
        if (args.size() > 1) {
            try {
                size_t used = 0;
                spacing = std::stod(args[1], &used);
                if (used != args[1].size() || spacing < 1.0 || spacing > 64.0) throw std::invalid_argument("range");
            } catch (...) {
                connection.SendChatMessage("Spacing must be a number from 1 to 64: " + args[1], 1);
                return;
            }
        }

        if (!g_integratedServer) {
            connection.SendChatMessage("No server", 1);
            return;
        }
        auto session = sessionManager.GetSession(sender.getPlayerId());
        if (!session) {
            connection.SendChatMessage("No session", 1);
            return;
        }

        const IntegratedServer::LineupResult r =
            g_integratedServer->SpawnMobLineup(*session, sender.getPosition(), spacing, adults, babies);

        connection.SendChatMessage(
            "Lined up " + std::to_string(r.adults) + " adults and " +
            std::to_string(r.babies) + " babies in a 12-wide grid (" +
            std::to_string(r.types) + " types, " +
            std::to_string(static_cast<int>(spacing)) + " apart)", 1);
        Log::Info("[SpawnAllCommand] %s lined up %d adults / %d babies over %d types",
                  sender.getName().c_str(), r.adults, r.babies, r.types);
    }

} // namespace Server
