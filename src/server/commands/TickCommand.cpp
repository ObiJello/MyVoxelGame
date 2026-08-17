// File: src/server/commands/TickCommand.cpp
#include "TickCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../IntegratedServer.hpp"
#include "../ServerTickRateManager.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>

namespace Server {

    void TickCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("tick", TickCommand::Execute);
    }

    namespace {

        std::string ToLower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // MC TickCommand.MAX_TICKRATE. The LOWER bound lives in the manager
        // (TickRateManager.MIN_TICKRATE), the upper only here — vanilla makes
        // the same split, since it is the command's argument type that carries
        // the range.
        constexpr float kMaxTickRate = 10000.0f;

        // MC commands/arguments/TimeArgument.java. UNITS: d = 24000, s = 20,
        // t = 1, and a bare number is ticks. The result is
        // `Math.round(value * factor)`, and anything below `minimum` is
        // rejected — /tick step and /tick sprint both pass minimum = 1.
        std::optional<int> ParseTime(const std::string& raw, int minimum) {
            if (raw.empty()) return std::nullopt;

            size_t consumed = 0;
            float value = 0.0f;
            try {
                value = std::stof(raw, &consumed);
            } catch (...) {
                return std::nullopt;
            }
            if (consumed == 0) return std::nullopt;

            const std::string unit = ToLower(raw.substr(consumed));
            int factor = 1;
            if (unit.empty() || unit == "t")  factor = 1;
            else if (unit == "s")             factor = 20;
            else if (unit == "d")             factor = 24000;
            else return std::nullopt;   // MC: argument.time.invalid_unit

            const long long ticks = static_cast<long long>(std::lround(value * factor));
            if (ticks < minimum) return std::nullopt;   // argument.time.tick_count_too_low
            if (ticks > 0x7FFFFFFFLL) return std::nullopt;
            return static_cast<int>(ticks);
        }

        std::optional<float> ParseFloat(const std::string& raw) {
            if (raw.empty()) return std::nullopt;
            size_t consumed = 0;
            float value = 0.0f;
            try {
                value = std::stof(raw, &consumed);
            } catch (...) {
                return std::nullopt;
            }
            if (consumed != raw.size()) return std::nullopt;
            return value;
        }

        // MC TickCommand.nanosToMilisString: "%.1f" of nanos / 1e6.
        std::string NanosToMillisString(int64_t nanos) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f",
                          static_cast<double>(nanos) /
                          static_cast<double>(ServerTickRateManager::kNanosPerMillisecond));
            return buf;
        }

        std::string Format1f(float v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
            return buf;
        }

        // MC TickCommand.tickQuery. Emits the status line, the rate line, then
        // the percentile line — three separate messages, in that order.
        void TickQuery(ServerConnection& connection, ServerTickRateManager& manager,
                       IntegratedServer& server) {
            const std::string tickRateString = Format1f(manager.tickrate());
            const int64_t averageNanos = manager.averageTickTimeNanos();
            const std::string busyTime = NanosToMillisString(averageNanos);

            if (manager.isSprinting()) {
                connection.SendChatMessage("The game is sprinting", 1);
                connection.SendChatMessage(
                    "Target tick rate: " + tickRateString +
                    " per second (ignored, reference only).\n"
                    "Average time per tick: " + busyTime + "ms", 1);
            } else {
                if (manager.isFrozen()) {
                    connection.SendChatMessage("The game is frozen", 1);
                } else if (manager.nanosecondsPerTick() < averageNanos) {
                    // MC's exact test: the budget is smaller than what a tick
                    // actually costs, i.e. we cannot keep up.
                    connection.SendChatMessage(
                        "The game is running, but can't keep up with the target tick rate", 1);
                } else {
                    connection.SendChatMessage("The game is running normally", 1);
                }
                connection.SendChatMessage(
                    "Target tick rate: " + tickRateString + " per second.\n"
                    "Average time per tick: " + busyTime + "ms (Target: " +
                    Format1f(manager.millisecondsPerTick()) + "ms)", 1);
            }

            // MC sorts a COPY of the sample ring and indexes it with
            // `len/2`, `(int)(len * 0.95)` and `(int)(len * 0.99)`. Those exact
            // indices are reproduced rather than a "proper" percentile: they
            // are what vanilla prints, and the numbers should match.
            int64_t samples[ServerTickRateManager::kSampleCount];
            manager.copyTickTimes(samples);
            std::sort(std::begin(samples), std::end(samples));
            constexpr int len = ServerTickRateManager::kSampleCount;
            const std::string p50 = NanosToMillisString(samples[len / 2]);
            const std::string p95 = NanosToMillisString(samples[static_cast<int>(len * 0.95)]);
            const std::string p99 = NanosToMillisString(samples[static_cast<int>(len * 0.99)]);
            connection.SendChatMessage(
                "Percentiles: P50: " + p50 + "ms P95: " + p95 + "ms P99: " + p99 +
                "ms, sample: " + std::to_string(len), 1);

            (void)server;
        }

    } // namespace

    void TickCommand::Execute(ServerPlayer& /*sender*/,
                              const std::vector<std::string>& args,
                              ServerConnection& connection,
                              PlayerSessionManager& /*sessionManager*/) {
        IntegratedServer* server = g_integratedServer.get();
        if (!server) {
            connection.SendChatMessage("Tick control is unavailable (no server)", 1);
            return;
        }
        ServerTickRateManager& manager = server->tickRateManager();

        if (args.empty()) {
            // MC's root `/tick` is not executable and Brigadier answers with a
            // usage error. This dispatcher has no usage machinery, so print the
            // subcommands instead — the same information, minus the caret.
            connection.SendChatMessage(
                "Usage: /tick <query|rate|freeze|unfreeze|step|sprint>", 1);
            return;
        }

        const std::string sub = ToLower(args[0]);

        // ── /tick query ─────────────────────────────────────────────────────
        if (sub == "query") {
            TickQuery(connection, manager, *server);
            return;
        }

        // ── /tick rate <rate> ───────────────────────────────────────────────
        if (sub == "rate") {
            if (args.size() < 2) {
                connection.SendChatMessage("Usage: /tick rate <rate>", 1);
                return;
            }
            auto rate = ParseFloat(args[1]);
            if (!rate) {
                connection.SendChatMessage("Value must be a number", 1);
                return;
            }
            // MC's range lives in `FloatArgumentType.floatArg(1.0F, 10000.0F)`,
            // so an out-of-range value is a PARSE error there and never reaches
            // the command body. Same outcome here, spelled out.
            if (*rate < ServerTickRateManager::kMinTickRate || *rate > kMaxTickRate) {
                connection.SendChatMessage(
                    "Rate must be between " + Format1f(ServerTickRateManager::kMinTickRate) +
                    " and " + Format1f(kMaxTickRate), 1);
                return;
            }
            manager.setTickRate(*rate);
            connection.SendChatMessage(
                "Set the target tick rate to " + Format1f(*rate) + " per second", 1);
            return;
        }

        // ── /tick freeze | unfreeze ─────────────────────────────────────────
        if (sub == "freeze" || sub == "unfreeze") {
            const bool freeze = (sub == "freeze");
            // MC setFreeze cancels any sprint and any step FIRST, so the two
            // cannot outlive the state change that contradicts them.
            manager.stopSprinting();
            manager.stopStepping();
            manager.setFrozen(freeze);
            connection.SendChatMessage(
                freeze ? "The game is frozen" : "The game is running normally", 1);
            return;
        }

        // ── /tick step [<time>|stop] ────────────────────────────────────────
        if (sub == "step") {
            if (args.size() >= 2 && ToLower(args[1]) == "stop") {
                connection.SendChatMessage(
                    manager.stopStepping() ? "Interrupted the current tick step"
                                           : "No tick step in progress", 1);
                return;
            }
            // Bare `/tick step` is one tick (MC: `step(source, 1)`).
            int ticks = 1;
            if (args.size() >= 2) {
                auto parsed = ParseTime(args[1], 1);
                if (!parsed) {
                    connection.SendChatMessage(
                        "Invalid time — expected a tick count, optionally suffixed t/s/d", 1);
                    return;
                }
                ticks = *parsed;
            }
            if (manager.stepGameIfPaused(ticks)) {
                connection.SendChatMessage("Stepping " + std::to_string(ticks) + " tick(s)", 1);
            } else {
                connection.SendChatMessage(
                    "Unable to step the game - the game must be frozen first", 1);
            }
            return;
        }

        // ── /tick sprint <time>|stop ────────────────────────────────────────
        if (sub == "sprint") {
            if (args.size() < 2) {
                connection.SendChatMessage("Usage: /tick sprint <time>|stop", 1);
                return;
            }
            if (ToLower(args[1]) == "stop") {
                connection.SendChatMessage(
                    manager.stopSprinting() ? "Interrupted the current tick sprint"
                                            : "No tick sprint in progress", 1);
                return;
            }
            auto parsed = ParseTime(args[1], 1);
            if (!parsed) {
                connection.SendChatMessage(
                    "Invalid time — expected a tick count, optionally suffixed t/s/d", 1);
                return;
            }
            // MC reports the interruption of a previous sprint BEFORE
            // announcing the new one.
            if (manager.requestGameToSprint(*parsed)) {
                connection.SendChatMessage("Interrupted the current tick sprint", 1);
            }
            connection.SendChatMessage("The game is sprinting", 1);
            return;
        }

        connection.SendChatMessage("Unknown /tick subcommand: " + args[0], 1);
    }

} // namespace Server
