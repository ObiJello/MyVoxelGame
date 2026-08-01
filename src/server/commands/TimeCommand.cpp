// File: src/server/commands/TimeCommand.cpp
#include "TimeCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../IntegratedServer.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include <cctype>
#include <optional>
#include <string>

namespace Server {

    void TimeCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("time", TimeCommand::Execute);
    }

    namespace {

        std::string ToLower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // MC TimeCommand's named time points + plain integers.
        std::optional<int64_t> ParseTimeValue(const std::string& raw) {
            const std::string arg = ToLower(raw);
            if (arg == "day")      return 1000;
            if (arg == "noon")     return 6000;
            if (arg == "night")    return 13000;
            if (arg == "midnight") return 18000;
            try {
                size_t consumed = 0;
                int64_t value = std::stoll(arg, &consumed);
                if (consumed == arg.size()) return value;
            } catch (...) {}
            return std::nullopt;
        }

    } // namespace

    void TimeCommand::Execute(ServerPlayer& /*sender*/,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& /*sessionManager*/) {
        Game::World* world = g_integratedServer ? g_integratedServer->GetWorld() : nullptr;
        if (!world) {
            connection.SendChatMessage("Time is unavailable (no world)", 1);
            return;
        }

        if (args.size() < 2) {
            connection.SendChatMessage("Usage: /time <set|add|query> <value>", 1);
            return;
        }

        const std::string action = ToLower(args[0]);

        if (action == "set" || action == "add") {
            auto value = ParseTimeValue(args[1]);
            if (!value) {
                connection.SendChatMessage("Invalid time: " + args[1], 1);
                return;
            }
            const int64_t newTime = (action == "set") ? *value
                                                      : world->GetDayTime() + *value;
            world->SetDayTime(newTime);
            g_integratedServer->ForceTimeSync();
            // MC "commands.time.set" feedback reports the resulting daytime.
            connection.SendChatMessage("Set the time to " + std::to_string(newTime), 1);
            return;
        }

        if (action == "query") {
            const std::string what = ToLower(args[1]);
            const int64_t dayTime = world->GetDayTime();
            int64_t result;
            if (what == "daytime") {
                result = ((dayTime % 24000) + 24000) % 24000;
            } else if (what == "gametime") {
                result = world->GetGameTime();
            } else if (what == "day") {
                result = dayTime / 24000;
            } else {
                connection.SendChatMessage("Usage: /time query <daytime|gametime|day>", 1);
                return;
            }
            connection.SendChatMessage("The time is " + std::to_string(result), 1);
            return;
        }

        connection.SendChatMessage("Usage: /time <set|add|query> <value>", 1);
    }

} // namespace Server
