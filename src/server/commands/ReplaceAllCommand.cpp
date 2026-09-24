// File: src/server/commands/ReplaceAllCommand.cpp
#include "ReplaceAllCommand.hpp"
#include "BlockStateArgument.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/World.hpp"
#include "../world/ChunkProvider.hpp"   // GetLoadedChunkPositions

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace Server {

    namespace {
        // A whole number: the radius, which is what ends the block list.
        bool IsInteger(const std::string& text) {
            if (text.empty()) return false;
            char* end = nullptr;
            std::strtol(text.c_str(), &end, 10);
            return end != text.c_str() && *end == '\0';
        }

        // Split "a, b,c" on commas outside [...], trimming whitespace and
        // dropping empties (a trailing comma before the radius is harmless).
        std::vector<std::string> SplitBlockList(const std::string& text) {
            std::vector<std::string> out;
            std::string current;
            int depth = 0;
            auto flush = [&] {
                const size_t b = current.find_first_not_of(" \t");
                if (b != std::string::npos) {
                    const size_t e = current.find_last_not_of(" \t");
                    out.push_back(current.substr(b, e - b + 1));
                }
                current.clear();
            };
            for (char ch : text) {
                if (ch == '[') ++depth;
                else if (ch == ']' && depth > 0) --depth;
                if (ch == ',' && depth == 0) { flush(); continue; }
                current += ch;
            }
            flush();
            return out;
        }

        // MC CommandSourceStack.sendFailure: red text, sender only.
        void SendFailure(ServerConnection& connection, const std::string& text) {
            Network::ChatMessageS2CPacket packet;
            packet.senderId = 0;
            packet.position = 1;
            packet.segments.push_back(Network::ChatSegmentData{
                text, 0xFFFF5555u, Network::ChatClickAction::None, "", ""});
            connection.SendChatMessage(packet);
        }
    } // namespace

    void ReplaceAllCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("replaceall", ReplaceAllCommand::Execute);
    }

    void ReplaceAllCommand::Execute(const CommandSourceStack& source,
                                    const std::vector<std::string>& args,
                                    ServerConnection& connection,
                                    PlayerSessionManager& /*sessionManager*/) {
        // The block list runs up to the radius (the first whole-number
        // argument); "grass_block, short_grass" tokenizes as two words, so
        // the words are joined back before the comma split.
        size_t radiusArg = 0;
        while (radiusArg < args.size() && !IsInteger(args[radiusArg])) ++radiusArg;
        if (radiusArg == 0 || radiusArg + 1 >= args.size()) {
            connection.SendChatMessage("Usage: /replaceall <block>[, <block>...] <radius> <newblock>", 1);
            return;
        }
        std::string blockList;
        for (size_t i = 0; i < radiusArg; ++i) {
            if (i) blockList += ' ';
            blockList += args[i];
        }

        std::string error;
        std::vector<BlockPredicate> filters;
        for (const std::string& text : SplitBlockList(blockList)) {
            BlockPredicate filter;
            if (!ParseBlockPredicate(text, filter, error)) {
                SendFailure(connection, error);
                return;
            }
            filters.push_back(std::move(filter));
        }
        if (filters.empty()) {
            SendFailure(connection, "Name at least one block to replace");
            return;
        }

        const std::string& radiusText = args[radiusArg];
        char* end = nullptr;
        const long radiusLong = std::strtol(radiusText.c_str(), &end, 10);
        if (end == radiusText.c_str() || *end != '\0' || radiusLong < 0) {
            SendFailure(connection, "Radius must be a whole number of blocks");
            return;
        }
        // No cap. The sweep below only ever visits loaded chunks, so the
        // radius bounds nothing but the distance test; a value past the
        // loaded world is simply "everything that is loaded".
        const int radius = static_cast<int>(std::min<long>(radiusLong, 1L << 24));

        Game::BlockState replacement;
        if (!ParseBlockState(args[radiusArg + 1], replacement, error)) {
            SendFailure(connection, error);
            return;
        }

        if (!g_integratedServer) { SendFailure(connection, "No server"); return; }
        ServerLevel* level = g_integratedServer->GetLevel(source.dimension);
        Game::World* world = level ? level->World() : nullptr;
        if (!world) { SendFailure(connection, "That dimension is not loaded"); return; }

        // The sphere about the source: the sender's feet, or the position an
        // /execute put on the stack. Cells are tested by their centre.
        const glm::dvec3 centre = source.position;
        const glm::ivec3 origin(static_cast<int>(std::floor(centre.x)),
                                static_cast<int>(std::floor(centre.y)),
                                static_cast<int>(std::floor(centre.z)));
        const double radiusSq = (static_cast<double>(radius) + 0.5) * (static_cast<double>(radius) + 0.5);

        // Walk the LOADED chunks that touch the sphere, not every cell of
        // the sphere: unloaded chunks are left alone either way (as /setblock
        // leaves them — a replace must not summon terrain), and enumerating
        // them is what keeps a huge radius from freezing the tick on empty
        // space.
        Game::ChunkProvider* provider = world->GetChunkProvider();
        if (!provider) { SendFailure(connection, "That dimension has no chunks"); return; }
        const int yLo = std::max(origin.y - radius, Game::World::MIN_Y);
        const int yHi = std::min(origin.y + radius, Game::World::MAX_Y);

        int replaced = 0;
        for (const Game::Math::ChunkPos& cp : provider->GetLoadedChunkPositions()) {
            const int cx0 = cp.x * 16, cz0 = cp.z * 16;
            // Skip a chunk whose nearest cell is already out of range.
            const double ndx = std::max({static_cast<double>(cx0 - origin.x), 0.0, static_cast<double>(origin.x - (cx0 + 15))});
            const double ndz = std::max({static_cast<double>(cz0 - origin.z), 0.0, static_cast<double>(origin.z - (cz0 + 15))});
            if (ndx * ndx + ndz * ndz > radiusSq) continue;
            if (!world->IsChunkLoaded(cp.x, cp.z)) continue;
            for (int y = yLo; y <= yHi; ++y) {
                for (int z = cz0; z < cz0 + 16; ++z) {
                    for (int x = cx0; x < cx0 + 16; ++x) {
                        const double dx = x - origin.x, dy = y - origin.y, dz = z - origin.z;
                        if (dx * dx + dy * dy + dz * dz > radiusSq) continue;
                        if (!world->IsValidPosition(x, y, z)) continue;
                        const Game::BlockState existing = world->GetBlockState(x, y, z);
                        bool matches = false;
                        for (const BlockPredicate& filter : filters) {
                            if (filter.Test(existing)) { matches = true; break; }
                        }
                        if (!matches) continue;
                        if (existing == replacement) continue;
                        if (world->SetBlock(x, y, z, replacement, Game::World::UpdateFlags::All)) {
                            ++replaced;
                        }
                    }
                }
            }
        }

        if (replaced == 0) {
            // commands.fill.failed
            SendFailure(connection, "No blocks were filled");
            return;
        }
        // commands.fill.success
        connection.SendChatMessage("Successfully filled " + std::to_string(replaced) + " block(s)", 1);
    }

} // namespace Server
