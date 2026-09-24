// File: src/server/commands/SetBlockCommand.cpp
#include "SetBlockCommand.hpp"
#include "CommandCoords.hpp"
#include "BlockStateArgument.hpp"
#include "EntitySelector.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../entity/ServerLevelBridge.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"

#include <cmath>
#include <string>

namespace Server {

    void SetBlockCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("setblock", SetBlockCommand::Execute);
    }

    void SetBlockCommand::Execute(const CommandSourceStack& source,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& /*sessionManager*/) {
        if (args.size() < 4) {
            connection.SendChatMessage("Usage: /setblock <x> <y> <z> <block> [destroy|keep|replace]", 1);
            return;
        }
        enum class Mode { Destroy, Keep, Replace } mode = Mode::Replace;
        if (args.size() >= 5) {
            if (args[4] == "destroy") mode = Mode::Destroy;
            else if (args[4] == "keep") mode = Mode::Keep;
            else if (args[4] == "replace") mode = Mode::Replace;
            else { connection.SendChatMessage("Unknown mode '" + args[4] + "' (destroy|keep|replace)", 1); return; }
        }

        // MC BlockPosArgument, from the stack's position and level.
        glm::ivec3 pos;
        std::string error;
        if (!ParseBlockPos(args[0], args[1], args[2], source, source.rotation, pos, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        Game::BlockState state;
        if (!ParseBlockState(args[3], state, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        if (!g_integratedServer) { connection.SendChatMessage("No server", 1); return; }
        ServerLevel* level = g_integratedServer->GetLevel(source.dimension);
        Game::World* world = level ? level->World() : nullptr;
        if (!world) { connection.SendChatMessage("Could not set the block", 1); return; }
        if (!world->IsValidPosition(pos.x, pos.y, pos.z) || !world->IsChunkLoaded(pos.x >> 4, pos.z >> 4)) {
            connection.SendChatMessage("That position is not loaded", 1);
            return;
        }

        // MC SetBlockCommand.setBlock.
        const Game::BlockState existing = world->GetBlockState(pos.x, pos.y, pos.z);
        if (mode == Mode::Destroy) {
            if (level->MobLevel()) level->MobLevel()->DestroyBlock(pos, true);
            else world->SetBlock(pos.x, pos.y, pos.z, Game::BlockID::Air, Game::World::UpdateFlags::All);
            if (state.Block() == Game::BlockID::Air && existing.Block() == Game::BlockID::Air) {
                connection.SendChatMessage("Could not set the block", 1);
                return;
            }
        } else if (mode == Mode::Keep && existing.Block() != Game::BlockID::Air) {
            connection.SendChatMessage("Could not set the block", 1);
            return;
        }
        if (existing == state && mode != Mode::Destroy) {
            connection.SendChatMessage("Could not set the block", 1);
            return;
        }
        if (!world->SetBlock(pos.x, pos.y, pos.z, state, Game::World::UpdateFlags::All)) {
            connection.SendChatMessage("Could not set the block", 1);
            return;
        }
        connection.SendChatMessage("Changed the block at " + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z), 1);
    }

} // namespace Server
