// File: src/server/commands/DimensionCommand.cpp
#include "common/world/level/ModDimensions.hpp"
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

    std::optional<Game::DimensionId> DimensionCommand::ParseDimension(std::string name) {
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name.rfind("minecraft:", 0) == 0) name.erase(0, 10);
        if (name.rfind("obeycraft:", 0) == 0) name.erase(0, 10);
        if (name.rfind("twilightforest:", 0) == 0) name.erase(0, 15);
        if (name.rfind("aether:", 0) == 0) name.erase(0, 7);
        if (name == "overworld")                        return Game::DimensionId::Overworld;
        if (name == "nether" || name == "the_nether")   return Game::DimensionId::Nether;
        if (name == "end"    || name == "the_end")      return Game::DimensionId::End;
        if (name == "hush"   || name == "the_hush")     return Game::DimensionId::Hush;
        if (name == "twilight" || name == "twilight_forest" || name == "tf") return Game::DimensionId::TwilightForest;
        if (name == "aether" || name == "the_aether")   return Game::DimensionId::Aether;
        return std::nullopt;
    }

    void DimensionCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& sessionManager) {
        ServerPlayer& sender = *source.sender;
        if (args.empty()) {
            connection.SendChatMessage("Usage: /dimension <overworld|nether|end|hush|twilight|aether>", 1);
            return;
        }
        const auto target = ParseDimension(args[0]);
        if (!target) {
            connection.SendChatMessage("Unknown dimension '" + args[0] + "' (overworld, nether, end, hush, twilight, aether)", 1);
            return;
        }
        if (!Game::ModDimensions::Enabled(*target)) {
            connection.SendChatMessage(std::string(Game::DimensionName(*target)) +
                                       " is turned off in this world (/gamerule " +
                                       (*target == Game::DimensionId::Aether ? "aether" : "twilight_forest") +
                                       " true)", 1);
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
