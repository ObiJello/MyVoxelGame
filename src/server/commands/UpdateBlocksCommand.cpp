// File: src/server/commands/UpdateBlocksCommand.cpp
#include "UpdateBlocksCommand.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/level/World.hpp"

#include <cmath>
#include <string>

namespace Server {

    namespace {
        constexpr int kDefaultRadius = 8;
        // 65^3 = 274k cells; every non-air one gets seven notifications and
        // whatever cascade they start, all inside one tick.
        constexpr int kMaxRadius = 32;
    } // namespace

    void UpdateBlocksCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("updateblocks", UpdateBlocksCommand::Execute);
    }

    void UpdateBlocksCommand::Execute(const CommandSourceStack& source,
                                      const std::vector<std::string>& args,
                                      ServerConnection& connection,
                                      PlayerSessionManager& /*sessionManager*/) {
        int radius = kDefaultRadius;
        if (!args.empty()) {
            try {
                radius = std::stoi(args[0]);
            } catch (...) {
                connection.SendChatMessage("Usage: /updateblocks [radius]", 1);
                return;
            }
        }
        if (radius < 0 || radius > kMaxRadius) {
            connection.SendChatMessage("Radius must be between 0 and " + std::to_string(kMaxRadius), 1);
            return;
        }
        if (!g_integratedServer) { connection.SendChatMessage("No server", 1); return; }
        ServerLevel* level = g_integratedServer->GetLevel(source.dimension);
        Game::World* world = level ? level->World() : nullptr;
        if (!world) { connection.SendChatMessage("That dimension is not loaded", 1); return; }

        const glm::ivec3 centre(static_cast<int>(std::floor(source.position.x)),
                                static_cast<int>(std::floor(source.position.y)),
                                static_cast<int>(std::floor(source.position.z)));

        // Every write in the world does these two things for its neighbours
        // (World::SetBlock, steps 8 and 9); this does them for a block that
        // has not been written, which is exactly what a block "waiting for an
        // update" is waiting for.
        //
        // The neighbour notification names the block itself as the source —
        // what a neighbour would see if this cell had been re-placed — and
        // the shape walk hands each block its six neighbours' current states
        // in MC's UPDATE_SHAPE_ORDER.
        constexpr Game::Direction kOrder[6] = {
            Game::Direction::West, Game::Direction::East, Game::Direction::North,
            Game::Direction::South, Game::Direction::Down, Game::Direction::Up
        };
        int updated = 0, skipped = 0;
        for (int y = centre.y - radius; y <= centre.y + radius; ++y) {
            for (int z = centre.z - radius; z <= centre.z + radius; ++z) {
                for (int x = centre.x - radius; x <= centre.x + radius; ++x) {
                    if (!world->IsValidPosition(x, y, z)) continue;
                    if (!world->IsPositionLoaded(x, y, z)) { ++skipped; continue; }
                    const Game::BlockState state = world->GetBlockState(x, y, z);
                    if (state.Block() == Game::BlockID::Air) continue;
                    const glm::ivec3 pos(x, y, z);

                    world->NeighborChanged(state, pos, state.Block(), /*movedByPiston=*/false);
                    for (Game::Direction d : kOrder) {
                        const glm::ivec3 np(x + Game::StepX(d), y + Game::StepY(d), z + Game::StepZ(d));
                        if (!world->IsValidPosition(np.x, np.y, np.z)) continue;
                        world->NeighborShapeChanged(d, pos, np, world->GetBlockState(np.x, np.y, np.z),
                                                    Game::World::UpdateFlags::All,
                                                    Game::World::kUpdateLimit);
                    }
                    ++updated;
                }
            }
        }

        std::string msg = "Updated " + std::to_string(updated) + " block" + (updated == 1 ? "" : "s") +
                          " within " + std::to_string(radius) + " of you";
        if (skipped > 0) msg += " (" + std::to_string(skipped) + " cells not loaded)";
        connection.SendChatMessage(msg, 1);
        Log::Info("[UpdateBlocks] radius %d at (%d,%d,%d): %d updated, %d unloaded",
                  radius, centre.x, centre.y, centre.z, updated, skipped);
    }

} // namespace Server
