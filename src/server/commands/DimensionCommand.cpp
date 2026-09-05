// File: src/server/commands/DimensionCommand.cpp
#include "DimensionCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../level/PortalTravel.hpp"
#include "../entity/ServerLevelBridge.hpp"   // PlayerEntityView
#include "common/world/level/DimensionId.hpp"
#include "common/core/Log.hpp"
#include <cctype>
#include <optional>
#include <string>

namespace Server {

    void DimensionCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("dimension", DimensionCommand::Execute);
        dispatcher.RegisterCommand("dim", DimensionCommand::Execute);
    }

    namespace {
        std::optional<Game::DimensionId> ParseDimension(std::string name) {
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.rfind("minecraft:", 0) == 0) name.erase(0, 10);
            if (name == "overworld")                        return Game::DimensionId::Overworld;
            if (name == "nether" || name == "the_nether")   return Game::DimensionId::Nether;
            if (name == "end"    || name == "the_end")      return Game::DimensionId::End;
            return std::nullopt;
        }
    }

    void DimensionCommand::Execute(ServerPlayer& sender,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& sessionManager) {
        if (args.empty()) {
            connection.SendChatMessage("Usage: /dimension <overworld|nether|end>", 1);
            return;
        }
        const auto target = ParseDimension(args[0]);
        if (!target) {
            connection.SendChatMessage("Unknown dimension '" + args[0] + "' (overworld, nether, end)", 1);
            return;
        }
        if (!g_integratedServer) {
            connection.SendChatMessage("Dimensions are unavailable (no server)", 1);
            return;
        }
        auto session = sessionManager.GetSession(sender.getPlayerId());
        if (!session) {
            connection.SendChatMessage("You have no session to travel with", 1);
            return;
        }
        const Game::DimensionId from = Game::DimensionFromRaw(session->GetDimensionId());
        if (from == *target) {
            connection.SendChatMessage("You are already in " + std::string(Game::DimensionName(from)), 1);
            return;
        }
        ServerLevel* fromLevel = g_integratedServer->GetLevel(from);
        Server::PlayerEntityView* view = g_integratedServer->GetPlayerEntityView(sender.getPlayerId());
        if (!fromLevel || !view) {
            connection.SendChatMessage("Cannot travel right now (your level is not ready)", 1);
            return;
        }
        PortalTravel::TravelToDimension(*g_integratedServer, *fromLevel, *view, *target);
        connection.SendChatMessage("Travelled to " + std::string(Game::DimensionName(*target)), 1);
    }

} // namespace Server
