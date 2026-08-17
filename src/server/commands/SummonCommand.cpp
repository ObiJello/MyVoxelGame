// File: src/server/commands/SummonCommand.cpp
#include "SummonCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "common/entity/EntityType.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace Server {

    void SummonCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("summon", SummonCommand::Execute);
    }

    void SummonCommand::Execute(ServerPlayer& sender,
                                const std::vector<std::string>& args,
                                ServerConnection& connection,
                                PlayerSessionManager& /*sessionManager*/) {
        if (args.empty()) {
            connection.SendChatMessage(
                "Usage: /summon <zombie|skeleton|creeper|spider|cow|pig|sheep|chicken> [count]", 1);
            return;
        }

        // Accept the vanilla "minecraft:" prefix so a command copied from the
        // wiki works unchanged.
        std::string slug = args[0];
        if (slug.rfind("minecraft:", 0) == 0) slug = slug.substr(10);
        std::transform(slug.begin(), slug.end(), slug.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        Game::EntityTypeId type{};
        bool found = false;
        for (uint16_t i = 0; i < static_cast<uint16_t>(Game::EntityTypeId::Count); ++i) {
            const auto candidate = static_cast<Game::EntityTypeId>(i);
            if (Game::GetEntityTypeInfo(candidate).slug == slug) {
                type = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            connection.SendChatMessage("Unknown entity type: " + args[0], 1);
            return;
        }

        int count = 1;
        if (args.size() > 1) {
            try {
                count = std::clamp(std::stoi(args[1]), 1, 64);
            } catch (...) {
                connection.SendChatMessage("Invalid count: " + args[1], 1);
                return;
            }
        }

        if (!g_integratedServer) {
            connection.SendChatMessage("No server", 1);
            return;
        }

        const int spawned = g_integratedServer->SummonMobs(type, sender.getPosition(), count);
        if (spawned == 0) {
            connection.SendChatMessage("Failed to summon " + slug, 1);
            return;
        }

        connection.SendChatMessage("Summoned " + std::to_string(spawned) + " " + slug, 1);
        Log::Info("[SummonCommand] %s summoned %d %s",
                  sender.getName().c_str(), spawned, slug.c_str());
    }

} // namespace Server
