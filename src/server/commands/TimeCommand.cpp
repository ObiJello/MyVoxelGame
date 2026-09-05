// File: src/server/commands/TimeCommand.cpp
#include "TimeCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace Server {

    void TimeCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("time", TimeCommand::Execute);
    }

    namespace {

        // MC TimeArgument.parse: StringReader.readFloat (the characters
        // 0-9 . -), then readUnquotedString as the unit, looked up in
        // UNITS {d=24000, s=20, t=1, ""=1}; ticks = Math.round(value ×
        // factor), rejected below the minimum (0 for /time).
        struct TimeParse {
            bool        ok = false;
            int         ticks = 0;
            std::string error;
        };

        bool IsAllowedNumber(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-'; }
        bool IsAllowedInUnquotedString(char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   c == '_' || c == '-' || c == '.' || c == '+';
        }

        TimeParse ParseTimeArgument(const std::string& raw, int minimum) {
            TimeParse out;
            size_t i = 0;
            while (i < raw.size() && IsAllowedNumber(raw[i])) ++i;
            const std::string number = raw.substr(0, i);
            if (number.empty()) {
                out.error = "Expected float";
                return out;
            }
            char* end = nullptr;
            const float value = std::strtof(number.c_str(), &end);
            if (end == number.c_str() || *end != '\0') {
                out.error = "Invalid float '" + number + "'";
                return out;
            }
            size_t j = i;
            while (j < raw.size() && IsAllowedInUnquotedString(raw[j])) ++j;
            const std::string unit = raw.substr(i, j - i);
            int factor = 0;
            if      (unit.empty() || unit == "t") factor = 1;
            else if (unit == "s")                 factor = 20;
            else if (unit == "d")                 factor = 24000;
            if (factor == 0 || j != raw.size()) {
                out.error = "Invalid unit";
                return out;
            }
            // Java Math.round(float): floor(x + 0.5).
            const long long ticks = static_cast<long long>(std::floor(static_cast<double>(value) * factor + 0.5));
            if (ticks < minimum) {
                out.error = "Tick count must not be less than " + std::to_string(minimum) +
                            ", found " + std::to_string(ticks);
                return out;
            }
            out.ok = true;
            out.ticks = static_cast<int>(ticks);
            return out;
        }

        // TimeCommand.getDayTime: (int)(level.getDayTime() % 24000L), Java
        // remainder (keeps the sign of the dividend).
        int64_t DayTimeOf(const Game::World& world) { return world.GetDayTime() % 24000; }

        // MC applies the time to every level (all share the overworld's
        // clock in vanilla; here each level's World keeps its own copy,
        // and the clients are synced from the overworld's).
        void SetAllLevels(int64_t dayTime) {
            g_integratedServer->ForEachLevel([&](ServerLevel& level) {
                if (Game::World* w = level.World()) w->SetDayTime(dayTime);
            });
        }
        void AddAllLevels(int64_t delta) {
            g_integratedServer->ForEachLevel([&](ServerLevel& level) {
                if (Game::World* w = level.World()) w->SetDayTime(w->GetDayTime() + delta);
            });
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
        const std::string& action = args[0];
        const std::string& arg    = args[1];

        if (action == "set") {
            // The literal branches come before the argument in MC's tree.
            int time = -1;
            if      (arg == "day")      time = 1000;
            else if (arg == "noon")     time = 6000;
            else if (arg == "night")    time = 13000;
            else if (arg == "midnight") time = 18000;
            else {
                const TimeParse parsed = ParseTimeArgument(arg, 0);
                if (!parsed.ok) {
                    connection.SendChatMessage(parsed.error, 1);
                    return;
                }
                time = parsed.ticks;
            }
            SetAllLevels(time);
            g_integratedServer->ForceTimeSync();
            // commands.time.set reports the value that was set.
            connection.SendChatMessage("Set the time to " + std::to_string(time), 1);
            return;
        }

        if (action == "add") {
            const TimeParse parsed = ParseTimeArgument(arg, 0);
            if (!parsed.ok) {
                connection.SendChatMessage(parsed.error, 1);
                return;
            }
            AddAllLevels(parsed.ticks);
            g_integratedServer->ForceTimeSync();
            // addTime reports the resulting time of day (getDayTime).
            connection.SendChatMessage("Set the time to " + std::to_string(DayTimeOf(*world)), 1);
            return;
        }

        if (action == "query") {
            int64_t result;
            if (arg == "daytime") {
                result = DayTimeOf(*world);
            } else if (arg == "gametime") {
                result = world->GetGameTime() % 2147483647LL;
            } else if (arg == "day") {
                result = (world->GetDayTime() / 24000) % 2147483647LL;
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
