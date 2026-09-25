// File: src/server/commands/FillBiomeCommand.cpp
#include "FillBiomeCommand.hpp"
#include "CommandCoords.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../level/LocateFinder.hpp"
#include "../network/ServerConnection.hpp"
#include "../world/ChunkProvider.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace Server {

    namespace {
        // MC CommandSourceStack.sendFailure: red text, sender only.
        void SendFailure(ServerConnection& connection, const std::string& text) {
            Network::ChatMessageS2CPacket packet;
            packet.senderId = 0;
            packet.position = 1;
            packet.segments.push_back(Network::ChatSegmentData{
                text, 0xFFFF5555u, Network::ChatClickAction::None, "", ""});
            connection.SendChatMessage(packet);
        }

        // MC QuartPos.toBlock(QuartPos.fromBlock(v)): down to the cell corner.
        int Quantize(int blockCoord) { return (blockCoord >> 2) << 2; }

        // MC Level.isInWorldBounds: the build height, and the 30 000 000
        // horizontal limit (Level.isInSpawnableBounds).
        bool IsInWorldBounds(const Game::World& world, const glm::ivec3& pos) {
            constexpr int kMaxLevelSize = 30000000;
            return world.IsValidPosition(pos.x, pos.y, pos.z) &&
                   pos.x >= -kMaxLevelSize && pos.z >= -kMaxLevelSize &&
                   pos.x < kMaxLevelSize && pos.z < kMaxLevelSize;
        }

        // MC BlockPosArgument.getLoadedBlockPos.
        bool GetLoadedBlockPos(const Game::World& world, const std::string& ax, const std::string& ay,
                               const std::string& az, const CommandSourceStack& source,
                               glm::ivec3& out, std::string& error) {
            if (!ParseBlockPos(ax, ay, az, source, source.rotation, out, error)) return false;
            if (!world.IsChunkLoaded(out.x >> 4, out.z >> 4)) {
                error = "That position is not loaded";           // argument.pos.unloaded
                return false;
            }
            if (!IsInWorldBounds(world, out)) {
                error = "That position is out of this world!";  // argument.pos.outofworld
                return false;
            }
            return true;
        }

        constexpr const char* kBiomeRegistry = "minecraft:worldgen/biome";

        bool IsRegisteredBiome(const std::string& id) {
            const auto& all = AllBiomeIds();
            return std::binary_search(all.begin(), all.end(), id);
        }

        // MC ResourceArgument.getResource over the biome registry.
        bool ResolveBiome(const std::string& typed, ServerLevel& level, Game::BiomeId& out, std::string& error) {
            if (!typed.empty() && typed[0] == '#') {
                error = "Invalid ID";   // argument.id.invalid — an element, never a tag
                return false;
            }
            std::string id = CanonicalWorldgenId("biome", typed, &level);
            // A bare name that only names a tag comes back as "#…": there is
            // no element of that name, which MC reports under minecraft:.
            if (!id.empty() && id[0] == '#') id = "minecraft:" + typed;
            if (!IsRegisteredBiome(id)) {
                error = "Can't find element '" + id + "' of type '" + kBiomeRegistry + "'";
                return false;
            }
            out = Game::BiomeRegistry::FromName(id);
            return true;
        }

        // MC ResourceOrTagArgument.getResourceOrTag over the biome registry,
        // as a predicate indexed by BiomeId.
        bool ResolveBiomeFilter(const std::string& typed, ServerLevel& level, std::vector<bool>& out,
                                std::string& error) {
            const std::string id = CanonicalWorldgenId("biome", typed, &level);
            const bool isTag = !id.empty() && id[0] == '#';
            if (isTag ? !IsWorldgenTag("biome", id.substr(1)) : !IsRegisteredBiome(id)) {
                error = std::string(isTag ? "Can't find tag '" : "Can't find element '") +
                        (isTag ? id.substr(1) : id) + "' of type '" + kBiomeRegistry + "'";
                return false;
            }
            out.assign(Game::BiomeRegistry::Count(), false);
            // FromName answers plains for an id it does not know, so a tag
            // member outside the engine's table must not reach it.
            for (const std::string& member : ResolveBiomeIdOrTag(id)) {
                if (IsRegisteredBiome(member)) out[Game::BiomeRegistry::FromName(member)] = true;
            }
            return true;
        }
    } // namespace

    void FillBiomeCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("fillbiome", FillBiomeCommand::Execute);
    }

    FillBiomeCommand::Result FillBiomeCommand::Fill(ServerLevel& level, const glm::ivec3& rawFrom,
                                                    const glm::ivec3& rawTo, Game::BiomeId biome,
                                                    const std::vector<bool>& filter) {
        Result result;
        Game::World* world = level.World();
        if (!world) { result.message = "That position is not loaded"; return result; }

        // BoundingBox.fromCorners(quantize(from), quantize(to)).
        const glm::ivec3 from(Quantize(rawFrom.x), Quantize(rawFrom.y), Quantize(rawFrom.z));
        const glm::ivec3 to(Quantize(rawTo.x), Quantize(rawTo.y), Quantize(rawTo.z));
        const glm::ivec3 minPos = glm::min(from, to);
        const glm::ivec3 maxPos = glm::max(from, to);

        const int64_t volume = int64_t(maxPos.x - minPos.x + 1) * int64_t(maxPos.y - minPos.y + 1) *
                               int64_t(maxPos.z - minPos.z + 1);
        const int limit = Game::Rules::GetInt(Game::Rules::Id::MaxBlockModifications);
        if (volume > limit) {
            // commands.fillbiome.toobig
            result.message = "Too many blocks in the specified volume (maximum " + std::to_string(limit) +
                             ", specified " + std::to_string(volume) + ")";
            return result;
        }

        // Every chunk the box touches, FULL and resident, or nothing happens.
        std::vector<std::shared_ptr<Game::Chunk>> chunks;
        for (int chunkZ = minPos.z >> 4; chunkZ <= (maxPos.z >> 4); ++chunkZ) {
            for (int chunkX = minPos.x >> 4; chunkX <= (maxPos.x >> 4); ++chunkX) {
                auto chunk = world->GetLoadedChunk(chunkX, chunkZ);
                if (!chunk) {
                    result.message = "That position is not loaded";   // argument.pos.unloaded
                    return result;
                }
                chunks.push_back(std::move(chunk));
            }
        }

        // ChunkAccess.fillBiomesFromNoise with FillBiomeCommand.makeResolver:
        // every section's biome container is rebuilt cell by cell (MC
        // LevelChunkSection.fillBiomesFromNoise recreates it), each cell
        // keeping its biome unless it lies in the box and passes the filter.
        auto* provider = world->GetChunkProvider();
        int changed = 0;
        std::vector<std::shared_ptr<Game::Chunk>> edited;
        for (const auto& chunk : chunks) {
            const int before = changed;
            {
                const auto guard = chunk->LockExclusive();
                const int baseX = chunk->pos.x * Game::Math::CHUNK_SIZE_X;
                const int baseZ = chunk->pos.z * Game::Math::CHUNK_SIZE_Z;
                for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                    Game::ChunkSection* section = chunk->GetSection(sectionY);
                    if (!section) continue;
                    const int baseY = Game::Math::WorldCoordinates::MIN_WORLD_Y + sectionY * Game::Math::SECTION_HEIGHT;
                    Game::PalettedContainer rebuilt = Game::ChunkSection::MakeBiomeContainer();
                    for (int qy = 0; qy < Game::ChunkSection::BIOME_AXIS; ++qy) {
                        for (int qz = 0; qz < Game::ChunkSection::BIOME_AXIS; ++qz) {
                            for (int qx = 0; qx < Game::ChunkSection::BIOME_AXIS; ++qx) {
                                const glm::ivec3 block(baseX + qx * 4, baseY + qy * 4, baseZ + qz * 4);
                                const Game::BiomeId current = section->GetBiome(qx, qy, qz);
                                Game::BiomeId value = current;
                                const bool inside = block.x >= minPos.x && block.x <= maxPos.x &&
                                                    block.y >= minPos.y && block.y <= maxPos.y &&
                                                    block.z >= minPos.z && block.z <= maxPos.z;
                                const bool passes = filter.empty() ||
                                                    (current < filter.size() && filter[current]);
                                if (inside && passes) {
                                    if (current != biome) ++changed;
                                    value = biome;
                                }
                                rebuilt.Set(Game::ChunkSection::BiomeIndex(qx, qy, qz), value);
                            }
                        }
                    }
                    section->AdoptBiomes(std::move(rebuilt));
                }
            }
            if (changed != before) {
                // markUnsaved — and biomes are chunk content: a client's
                // retained copy from before this edit must not be revived.
                chunk->BumpModStamp();
                if (provider) provider->MarkChunkForSave(chunk->pos);
                edited.push_back(chunk);
            }
        }

        if (changed == 0) {
            result.message = "No biome entries were changed";   // commands.fillbiome.no_changes
            return result;
        }
        if (g_integratedServer) g_integratedServer->ResendBiomesForChunks(level.Dimension(), edited);
        result.ok = true;
        result.changed = changed;
        // commands.fillbiome.success.count
        result.message = std::to_string(changed) + " biome entry/entries set between " +
                         std::to_string(minPos.x) + ", " + std::to_string(minPos.y) + ", " + std::to_string(minPos.z) +
                         " and " +
                         std::to_string(maxPos.x) + ", " + std::to_string(maxPos.y) + ", " + std::to_string(maxPos.z);
        return result;
    }

    void FillBiomeCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& /*sessionManager*/) {
        const bool replace = args.size() == 9 && args[7] == "replace";
        if (args.size() != 7 && !replace) {
            connection.SendChatMessage("Usage: /fillbiome <from> <to> <biome> [replace <filter>]", 1);
            return;
        }

        ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
        Game::World* world = level ? level->World() : nullptr;
        if (!world) { SendFailure(connection, "That position is not loaded"); return; }

        // Resolved in MC's order: from, to, biome, filter.
        std::string error;
        glm::ivec3 from, to;
        if (!GetLoadedBlockPos(*world, args[0], args[1], args[2], source, from, error) ||
            !GetLoadedBlockPos(*world, args[3], args[4], args[5], source, to, error)) {
            SendFailure(connection, error);
            return;
        }
        Game::BiomeId biome = Game::kFallbackBiomeId;
        if (!ResolveBiome(args[6], *level, biome, error)) {
            SendFailure(connection, error);
            return;
        }
        std::vector<bool> filter;
        if (replace && !ResolveBiomeFilter(args[8], *level, filter, error)) {
            SendFailure(connection, error);
            return;
        }

        const Result result = Fill(*level, from, to, biome, filter);
        if (result.ok) connection.SendChatMessage(result.message, 1);
        else SendFailure(connection, result.message);
    }

} // namespace Server
