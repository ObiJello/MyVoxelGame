// File: src/server/commands/FillBiomeCommand.hpp
//
// MC FillBiomeCommand:
//   /fillbiome <from> <to> <biome> [replace <filter>]
//
// Sets the biome of every 4x4x4 biome cell (MC QuartPos) inside the box, the
// corners snapped down to their cell. `replace` limits it to cells whose
// current biome is the filter — an id or a #tag. Every chunk the box touches
// must be loaded, and the box (in blocks, after snapping) may hold at most
// max_block_modifications blocks. The edited chunks are marked for saving and
// their biome columns resent to the players watching them (ChunksBiomesS2C),
// whose clients re-mesh to the new tint.
//
// Ids take the forms /locate takes (LocateFinder CanonicalWorldgenId):
// minecraft:plains, #minecraft:is_forest, and — beyond MC — a bare name in
// any namespace, the source's dimension first. Messages are
// commands.fillbiome.* verbatim.
#pragma once

#include "CommandDispatcher.hpp"
#include "common/world/biome/Biomes.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Server {

    class ServerLevel;

    class FillBiomeCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);

        // MC FillBiomeCommand.fill(level, rawFrom, rawTo, biome, filter, …):
        // the edit itself, for any caller. `filter` is indexed by BiomeId
        // (empty = every biome). On success `changed` is the number of cells
        // set and `message` the success line; otherwise `message` is the
        // failure.
        struct Result {
            bool        ok = false;
            int         changed = 0;
            std::string message;
        };
        static Result Fill(ServerLevel& level, const glm::ivec3& rawFrom, const glm::ivec3& rawTo,
                           Game::BiomeId biome, const std::vector<bool>& filter);
    };

} // namespace Server
