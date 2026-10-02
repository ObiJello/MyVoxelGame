// File: src/server/commands/PlaceCommand.cpp
#include "PlaceCommand.hpp"
#include "BlockCommandUtil.hpp"
#include "../IntegratedServer.hpp"
#include "../level/LiveFeatureLevel.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace Server {

    namespace {

        // The live-world adapter refuses a feature unless every chunk within
        // two of the origin is resident (a tree never half-built across an
        // unloaded chunk); MC's own check is one.
        constexpr int kFeatureChunkMargin = 2;

        void Usage(ServerConnection& connection) {
            connection.SendChatMessage("Usage: /place feature <feature> [<pos>]", 1);
        }

    } // namespace

    void PlaceCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        dispatcher.RegisterCommand("place", PlaceCommand::Execute,
            Cmd::Root().Then(Cmd::Literal("feature")
                .Then(Cmd::Argument("feature", Cmd::Arg::ConfiguredFeature).Executes()
                    .Then(Cmd::Argument("pos", Cmd::Arg::BlockPos).Executes()))));
    }

    void PlaceCommand::Execute(const CommandSourceStack& source,
                               const std::vector<std::string>& args,
                               ServerConnection& connection,
                               PlayerSessionManager& /*sessionManager*/) {
        if (args.empty() || args[0] != "feature" || (args.size() != 2 && args.size() != 5)) {
            Usage(connection);
            return;
        }
        ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
        Game::World* world = level ? level->World() : nullptr;
        if (!world) { SendCommandFailure(connection, "That position is not loaded"); return; }

        // ResourceOrIdArgument.feature: an id (namespace defaulted).
        std::string id = args[1];
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!id.empty() && (id[0] == '{' || id[0] == '[')) {
            SendCommandFailure(connection, "Inline feature definitions are not supported; use a configured feature id");
            return;
        }
        if (id.find(':') == std::string::npos) id = "minecraft:" + id;
        if (!Game::HasLiveFeature(id)) {
            SendCommandFailure(connection, "Can't find element '" + id + "' in registry 'minecraft:worldgen/configured_feature'");
            return;
        }

        std::string error;
        glm::ivec3 pos(static_cast<int>(std::floor(source.position.x)),
                       static_cast<int>(std::floor(source.position.y)),
                       static_cast<int>(std::floor(source.position.z)));
        if (args.size() == 5 && !GetLoadedBlockPos(*world, args[2], args[3], args[4], source, pos, error)) {
            SendCommandFailure(connection, error);
            return;
        }

        // placeFeature's checkLoaded: the 3×3 chunks around the origin.
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                if (!world->IsChunkLoaded((pos.x >> 4) + dx, (pos.z >> 4) + dz)) {
                    SendCommandFailure(connection, "That position is not loaded");
                    return;
                }
            }
        }
        // The adapter's wider ring, brought in so the check above is the
        // only one a player meets.
        const glm::ivec3 margin(kFeatureChunkMargin * 16, 0, kFeatureChunkMargin * 16);
        if (!EnsureChunksLoaded(*level, pos - margin, pos + margin)) {
            SendCommandFailure(connection, "That position is not loaded");
            return;
        }

        // feature.place(level, generator, level.getRandom(), pos).
        Game::JavaRandom* random = world->Random();
        if (!random || !Game::PlaceLiveFeature(*world, id, pos, *random)) {
            SendCommandFailure(connection, "Failed to place feature");
            return;
        }
        source.SendSuccess(connection, "Placed \"" + id + "\" at " + std::to_string(pos.x) + ", " +
                                   std::to_string(pos.y) + ", " + std::to_string(pos.z), /*broadcast=*/true);
    }

} // namespace Server
