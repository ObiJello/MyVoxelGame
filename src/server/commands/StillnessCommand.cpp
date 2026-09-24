// File: src/server/commands/StillnessCommand.cpp
#include "StillnessCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../level/HushStillness.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/DimensionId.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace Server {

    namespace {

        std::string ToLower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // Seconds, whole or fractional ("12", "8.5"); a trailing "s" is
        // accepted. Nullopt for anything else.
        std::optional<float> ParseSeconds(std::string raw) {
            if (!raw.empty() && (raw.back() == 's' || raw.back() == 'S')) raw.pop_back();
            if (raw.empty()) return std::nullopt;
            size_t consumed = 0;
            float value = 0.0f;
            try {
                value = std::stof(raw, &consumed);
            } catch (...) {
                return std::nullopt;
            }
            if (consumed != raw.size() || !std::isfinite(value)) return std::nullopt;
            return value;
        }

        std::string Seconds(int ticks) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f s", ticks / 20.0);
            return buf;
        }

    } // namespace

    void StillnessCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("stillness", StillnessCommand::Execute);
    }

    void StillnessCommand::Execute(const CommandSourceStack& /*source*/,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& /*sessionManager*/) {
        ServerLevel* hush = g_integratedServer
            ? g_integratedServer->GetLevel(Game::DimensionId::Hush) : nullptr;
        HushStillness* stillness = hush ? hush->Stillness() : nullptr;
        if (!stillness) {
            connection.SendChatMessage("The Hush is not loaded - go through a hush portal first", 1);
            return;
        }

        const std::string sub = args.empty() ? std::string() : ToLower(args[0]);

        if (sub == "stop") {
            if (!stillness->Active()) {
                connection.SendChatMessage("No stillness is on", 1);
                return;
            }
            stillness->Stop();
            connection.SendChatMessage("The stillness is lifted", 1);
            return;
        }

        if (sub == "query") {
            if (stillness->Active()) {
                connection.SendChatMessage("A stillness is on: " + Seconds(stillness->RemainingTicks()) +
                                           " left", 1);
            } else {
                connection.SendChatMessage("No stillness is on; the next falls in " +
                                           Seconds(stillness->TicksUntilNext()), 1);
            }
            return;
        }

        int durationTicks = 0;   // natural length
        if (!sub.empty()) {
            const std::optional<float> seconds = ParseSeconds(sub);
            if (!seconds || *seconds < 1.0f ||
                *seconds * 20.0f > static_cast<float>(HushStillness::kMaxCommandDurationTicks)) {
                connection.SendChatMessage("Usage: /stillness [<seconds 1-60> | stop | query]", 1);
                return;
            }
            durationTicks = static_cast<int>(std::lround(*seconds * 20.0f));
        }
        stillness->Start(durationTicks);
        connection.SendChatMessage("A stillness falls over the Hush (" +
                                   Seconds(stillness->RemainingTicks()) + ")", 1);
        Log::Info("[StillnessCommand] Stillness started for %d ticks", stillness->RemainingTicks());
    }

} // namespace Server
