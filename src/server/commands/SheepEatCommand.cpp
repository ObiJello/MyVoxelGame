// File: src/server/commands/SheepEatCommand.cpp
#include "SheepEatCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/MobManager.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"

#include <cmath>
#include <string>

namespace Server {

    void SheepEatCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("sheepeat", SheepEatCommand::Execute);
    }

    namespace {

        // The same test EatBlockGoal::CanUse makes after its roll. Duplicated
        // here ONLY to report why a sheep did not start — the goal still makes
        // the real decision, so the two cannot disagree about what happens.
        bool HasEdibleBlock(const Game::IBlockAccess& blocks, const glm::ivec3& p) {
            const Game::BlockID at = blocks.GetBlock(p.x, p.y, p.z);
            if (at == Game::BlockID::ShortGrass || at == Game::BlockID::Fern ||
                at == Game::BlockID::ShortDryGrass || at == Game::BlockID::TallDryGrass) {
                return true;
            }
            return blocks.GetBlock(p.x, p.y - 1, p.z) == Game::BlockID::Grass;
        }

    } // namespace

    void SheepEatCommand::Execute(ServerPlayer& sender,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& /*sessionManager*/) {
        if (!g_integratedServer) {
            connection.SendChatMessage("No server", 1);
            return;
        }

        MobManager* mobs = g_integratedServer->GetMobs();
        Game::World* world = g_integratedServer->GetWorld();
        if (!mobs || !world) {
            connection.SendChatMessage("No world", 1);
            return;
        }

        // Optional radius around the sender. Omitted = every loaded sheep.
        double radiusSq = -1.0;
        if (!args.empty()) {
            try {
                const double r = std::stod(args[0]);
                if (r <= 0.0) {
                    connection.SendChatMessage("Radius must be positive", 1);
                    return;
                }
                radiusSq = r * r;
            } catch (...) {
                connection.SendChatMessage("Usage: /sheepeat [radius]", 1);
                return;
            }
        }

        const glm::dvec3 origin = sender.getPosition();

        int forced = 0;
        int noBlock = 0;
        int total = 0;

        for (const auto& [id, mob] : mobs->All()) {
            auto* sheep = dynamic_cast<Game::Sheep*>(mob.get());
            if (!sheep || !sheep->IsAlive()) continue;

            if (radiusSq >= 0.0) {
                const glm::dvec3 d = sheep->position - origin;
                if (glm::dot(d, d) > radiusSq) continue;
            }
            ++total;

            Game::EatBlockGoal* goal = sheep->GetEatBlockGoal();
            if (!goal) continue;

            // Report the block condition separately so "nothing happened" is
            // distinguishable from "it is standing on dirt it already ate".
            if (!HasEdibleBlock(*world, sheep->BlockPosition())) {
                ++noBlock;
                continue;
            }

            goal->ForceNextUse();
            ++forced;
        }

        if (total == 0) {
            connection.SendChatMessage(
                radiusSq >= 0.0 ? "No sheep in range" : "No sheep loaded", 1);
            return;
        }

        // The goal starts on the sheep's next EVEN tick, so within two ticks.
        std::string msg = "Grazing " + std::to_string(forced) + " of " +
                          std::to_string(total) + " sheep";
        if (noBlock > 0) {
            msg += " (" + std::to_string(noBlock) + " on nothing edible)";
        }
        connection.SendChatMessage(msg, 1);

        Log::Info("[SheepEatCommand] %s forced %d/%d sheep to graze (%d had no block)",
                  sender.getName().c_str(), forced, total, noBlock);
    }

} // namespace Server
