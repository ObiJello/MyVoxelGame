#include "EntityStatsCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../entity/MobManager.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Mob.hpp"

#include <cmath>
#include <limits>

namespace Server {

    void EntityStatsCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("entitystats", EntityStatsCommand::Execute);
    }

    void EntityStatsCommand::Execute(ServerPlayer& /*sender*/,
                                     const std::vector<std::string>& args,
                                     ServerConnection& connection,
                                     PlayerSessionManager& /*sessionManager*/) {
        auto* server = g_integratedServer.get();
        if (!server) return;
        ServerLevel& level = server->Overworld();
        MobManager* mobs = level.Mobs();
        const ChunkTicketManager* tickets = level.Tickets();
        if (!mobs) return;

        const bool tntOnly = args.empty() || args[0] == "tnt";

        int total = 0, removed = 0, notTicking = 0, belowWorld = 0;
        glm::dvec3 lo(std::numeric_limits<double>::infinity());
        glm::dvec3 hi(-std::numeric_limits<double>::infinity());
        double maxSpeed = 0.0;
        int samples = 0;
        std::string sampleText;
        for (const auto& [id, mob] : mobs->All()) {
            if (!mob) continue;
            if (tntOnly && mob->GetType() != Game::EntityTypeId::Tnt) continue;
            ++total;
            if (mob->IsRemoved()) ++removed;
            lo = glm::min(lo, mob->position);
            hi = glm::max(hi, mob->position);
            maxSpeed = std::max(maxSpeed, glm::length(mob->velocity));
            if (mob->position.y < -64.0) ++belowWorld;
            const Game::Math::ChunkPos cp(static_cast<int>(std::floor(mob->position.x)) >> 4,
                                          static_cast<int>(std::floor(mob->position.z)) >> 4);
            const bool ticking = !tickets || tickets->IsEntityTickingAfterUpdates(cp);
            if (!ticking) {
                ++notTicking;
                if (samples < 5) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), " (%.1f,%.1f,%.1f)",
                             mob->position.x, mob->position.y, mob->position.z);
                    sampleText += buf;
                    ++samples;
                }
            }
        }
        char line[512];
        snprintf(line, sizeof(line),
                 "[EntityStats] %s: total=%d removed=%d notTicking=%d belowWorld=%d "
                 "x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f] maxSpeed=%.2f notTickingSamples:%s",
                 tntOnly ? "tnt" : "all", total, removed, notTicking, belowWorld,
                 lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, maxSpeed, sampleText.c_str());
        Log::Info("%s", line);
        connection.SendChatMessage(line, 1);
    }

} // namespace Server
