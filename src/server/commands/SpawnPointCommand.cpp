// File: src/server/commands/SpawnPointCommand.cpp
#include "SpawnPointCommand.hpp"
#include "BlockCommandUtil.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"

#include "common/core/Mth.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Server {

    void SpawnPointCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("spawnpoint", SpawnPointCommand::Execute,
            Cmd::Root().Executes()
                .Then(Cmd::Argument("targets", Cmd::Arg::Players).Executes()
                    .Then(Cmd::Argument("pos", Cmd::Arg::BlockPos).Executes()
                        .Then(Cmd::Argument("rotation", Cmd::Arg::Rotation).Executes()))));
    }

    void SpawnPointCommand::Execute(const CommandSourceStack& source,
                                    const std::vector<std::string>& args,
                                    ServerConnection& connection,
                                    PlayerSessionManager& /*sessionManager*/) {
        if (args.size() != 0 && args.size() != 1 && args.size() != 4 && args.size() != 6) {
            connection.SendChatMessage("Usage: /spawnpoint [<targets> [<pos> [<yaw> <pitch>]]]", 1);
            return;
        }

        std::string error;
        std::vector<ServerPlayer*> targets;
        if (args.empty()) {
            // Collections.singleton(source.getPlayerOrException()).
            ServerPlayer* self = source.ExecutorPlayer();
            if (!self) { SendCommandFailure(connection, CommandSourceStack::kPlayerRequired); return; }
            targets.push_back(self);
        } else {
            std::vector<SelectedEntity> selected;
            if (!ResolveSelector(args[0], SelectorKind::Players, source, selected, error)) {
                SendCommandFailure(connection, error);
                return;
            }
            for (const SelectedEntity& e : selected) {
                ServerPlayer* p = e.session ? e.session->GetPlayer() : e.player;
                if (p) targets.push_back(p);
            }
        }

        glm::ivec3 pos(static_cast<int>(std::floor(source.position.x)),
                       static_cast<int>(std::floor(source.position.y)),
                       static_cast<int>(std::floor(source.position.z)));
        if (args.size() >= 4 && !GetSpawnablePos(args[1], args[2], args[3], source, pos, error)) {
            SendCommandFailure(connection, error);
            return;
        }
        CommandRotation rotation;   // WorldCoordinates.ZERO_ROTATION
        if (args.size() == 6 && !ParseRotationArgument(args[4], args[5], source, rotation, error)) {
            SendCommandFailure(connection, error);
            return;
        }

        // RespawnData.of(source level, pos, wrapDegrees(yaw), clamp(pitch)),
        // forced = true; setRespawnPosition(config, sendMessage = false).
        ServerPlayer::RespawnConfig config;
        config.dimensionId = Game::DimensionToRaw(source.dimension);
        config.pos         = pos;
        config.yaw         = Game::Mth::WrapDegrees(rotation.yRot);
        config.pitch       = std::clamp(rotation.xRot, -90.0f, 90.0f);
        config.forced      = true;
        for (ServerPlayer* target : targets) target->setRespawnConfig(config);

        // CommandResponseTracker: one name, or the count.
        std::string text = "Set spawn point to " + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " +
                           std::to_string(pos.z) + " [" + JavaFloatString(config.yaw) + ", " +
                           JavaFloatString(config.pitch) + "] in " +
                           std::string(Game::DimensionRegistryName(source.dimension)) + " for ";
        text += targets.size() == 1 ? targets.front()->getName()
                                    : std::to_string(targets.size()) + " players";
        source.SendSuccess(connection, text, /*broadcast=*/true);
    }

} // namespace Server
