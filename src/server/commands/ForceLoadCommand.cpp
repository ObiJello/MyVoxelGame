#include "ForceLoadCommand.hpp"
#include "CommandCoords.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ChunkKeeper.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "common/world/level/DimensionId.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace Server {

    namespace {

        constexpr int kMaxChunks = 256;   // MC ForceLoadCommand.MAX_CHUNK_LIMIT

        std::string DimName(Game::DimensionId dim) {
            const std::string name(Game::DimensionName(dim));
            return name.find(':') == std::string::npos ? "minecraft:" + name : name;
        }

        std::string ChunkText(Game::Math::ChunkPos c) {
            return "[" + std::to_string(c.x) + ", " + std::to_string(c.z) + "]";
        }

        // A column position in blocks -> its chunk.
        bool ParseColumn(const std::string& ax, const std::string& az, const CommandSourceStack& source,
                         Game::Math::ChunkPos& out, std::string& error) {
            double x = 0.0, z = 0.0;
            if (!ParseCoord(ax, source.position.x, false, x) || !ParseCoord(az, source.position.z, false, z)) {
                error = "Expected a column position (x z)";
                return false;
            }
            out = Game::Math::ChunkPos{static_cast<int>(std::floor(x)) >> 4, static_cast<int>(std::floor(z)) >> 4};
            return true;
        }

        void Usage(ServerConnection& connection) {
            connection.SendChatMessage("Usage: /forceload (add|remove) <x> <z> [<x> <z>] | /forceload remove all | /forceload query [<x> <z>]", 1);
        }

    } // namespace

    void ForceLoadCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("forceload", ForceLoadCommand::Execute);
    }

    void ForceLoadCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& /*sessionManager*/) {
        if (args.empty()) { Usage(connection); return; }
        if (!g_integratedServer) { connection.SendChatMessage("No server", 1); return; }
        ServerLevel* level = g_integratedServer->GetLevel(source.dimension);
        ChunkKeeper* keeper = level ? level->Keeper() : nullptr;
        if (!keeper) { connection.SendChatMessage("That dimension is not loaded", 1); return; }
        const std::string dim = DimName(source.dimension);
        const std::string& verb = args[0];

        if (verb == "query") {
            if (args.size() >= 3) {
                Game::Math::ChunkPos c; std::string error;
                if (!ParseColumn(args[1], args[2], source, c, error)) { connection.SendChatMessage(error, 1); return; }
                connection.SendChatMessage("Chunk at " + ChunkText(c) + " in " + dim +
                                           (keeper->IsForced(c) ? " is marked for force loading" : " is not marked for force loading"), 1);
                return;
            }
            const auto forced = keeper->ForcedChunks();
            if (forced.empty()) { connection.SendChatMessage("No force-loaded chunks were found in " + dim, 1); return; }
            std::string list;
            const size_t shown = std::min<size_t>(forced.size(), 32);
            for (size_t i = 0; i < shown; ++i) list += (i ? ", " : "") + ChunkText(forced[i]);
            if (forced.size() > shown) list += ", ... (" + std::to_string(forced.size() - shown) + " more)";
            connection.SendChatMessage(std::to_string(forced.size()) + " force-loaded chunk(s) were found in " + dim + " at: " + list, 1);
            return;
        }

        const bool add = verb == "add";
        if (!add && verb != "remove") { Usage(connection); return; }

        if (!add && args.size() >= 2 && args[1] == "all") {
            keeper->RemoveAllForced();
            keeper->Save();
            connection.SendChatMessage("Unmarked all force-loaded chunks in " + dim, 1);
            return;
        }
        if (args.size() != 3 && args.size() != 5) { Usage(connection); return; }

        Game::Math::ChunkPos from, to; std::string error;
        if (!ParseColumn(args[1], args[2], source, from, error)) { connection.SendChatMessage(error, 1); return; }
        to = from;
        if (args.size() == 5 && !ParseColumn(args[3], args[4], source, to, error)) { connection.SendChatMessage(error, 1); return; }
        const Game::Math::ChunkPos lo{std::min(from.x, to.x), std::min(from.z, to.z)};
        const Game::Math::ChunkPos hi{std::max(from.x, to.x), std::max(from.z, to.z)};
        const long long count = static_cast<long long>(hi.x - lo.x + 1) * static_cast<long long>(hi.z - lo.z + 1);
        if (count > kMaxChunks) {
            connection.SendChatMessage("Too many chunks in the specified area (maximum " + std::to_string(kMaxChunks) +
                                       ", specified " + std::to_string(count) + ")", 1);
            return;
        }

        Game::Math::ChunkPos last{lo.x, lo.z};
        int changed = 0;
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                const Game::Math::ChunkPos c{x, z};
                const bool did = add ? keeper->AddForced(c) : keeper->RemoveForced(c);
                if (did) { ++changed; last = c; }
            }
        }
        keeper->Save();

        if (changed == 0) {
            connection.SendChatMessage(add ? "No chunks were marked for force loading" : "No chunks were removed from force loading", 1);
            return;
        }
        if (changed == 1) {
            connection.SendChatMessage((add ? "Marked chunk " : "Unmarked chunk ") + ChunkText(last) + " in " + dim +
                                       (add ? " to be force-loaded" : " for force loading"), 1);
            return;
        }
        connection.SendChatMessage((add ? "Marked " : "Unmarked ") + std::to_string(changed) + " chunks in " + dim +
                                   " from " + ChunkText(lo) + " to " + ChunkText(hi) +
                                   (add ? " to be force-loaded" : " for force loading"), 1);
    }

} // namespace Server
