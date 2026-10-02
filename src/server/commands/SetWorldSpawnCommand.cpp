// File: src/server/commands/SetWorldSpawnCommand.cpp
#include "SetWorldSpawnCommand.hpp"
#include "BlockCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../network/ServerConnection.hpp"

#include "common/core/Mth.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace Server {

    void SetWorldSpawnCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("setworldspawn", SetWorldSpawnCommand::Execute,
            Cmd::Root().Executes()
                .Then(Cmd::Argument("pos", Cmd::Arg::BlockPos).Executes()
                    .Then(Cmd::Argument("rotation", Cmd::Arg::Rotation).Executes())));
    }

    void SetWorldSpawnCommand::Execute(const CommandSourceStack& source,
                                       const std::vector<std::string>& args,
                                       ServerConnection& connection,
                                       PlayerSessionManager& /*sessionManager*/) {
        if (args.size() != 0 && args.size() != 3 && args.size() != 5) {
            connection.SendChatMessage("Usage: /setworldspawn [<pos> [<yaw> <pitch>]]", 1);
            return;
        }
        if (!g_integratedServer) return;

        std::string error;
        // BlockPos.containing(source.getPosition()) for the bare form.
        glm::ivec3 pos(static_cast<int>(std::floor(source.position.x)),
                       static_cast<int>(std::floor(source.position.y)),
                       static_cast<int>(std::floor(source.position.z)));
        if (args.size() >= 3 && !GetSpawnablePos(args[0], args[1], args[2], source, pos, error)) {
            SendCommandFailure(connection, error);
            return;
        }
        // WorldCoordinates.ZERO_ROTATION unless one is given.
        CommandRotation rotation;
        if (args.size() == 5 && !ParseRotationArgument(args[3], args[4], source, rotation, error)) {
            SendCommandFailure(connection, error);
            return;
        }
        if (source.dimension != Game::DimensionId::Overworld) {
            SendCommandFailure(connection, "Can only set the world spawn for overworld");
            return;
        }

        // LevelData.RespawnData.of: yaw wrapped, pitch clamped.
        const float yaw = Game::Mth::WrapDegrees(rotation.yRot);
        const float pitch = std::clamp(rotation.xRot, -90.0f, 90.0f);
        g_integratedServer->SetWorldSpawn(pos, yaw, pitch);

        // commands.setworldspawn.success (26.3: position, yaw, pitch, dimension).
        source.SendSuccess(connection, "Set the world spawn point to " + std::to_string(pos.x) + ", " +
                                   std::to_string(pos.y) + ", " + std::to_string(pos.z) + " [" +
                                   JavaFloatString(yaw) + ", " + JavaFloatString(pitch) + "] in " +
                                   std::string(Game::DimensionRegistryName(source.dimension)), /*broadcast=*/true);
    }

} // namespace Server
