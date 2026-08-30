// File: src/server/level/StrongholdLocate.cpp
//
// References: minecraft_code/decompiled_net/minecraft/world/level/chunk/
// ChunkGenerator.java (findNearestMapStructure / getNearestGeneratedStructure)
// and data/minecraft/worldgen/structure_set/strongholds.json.

#include "StrongholdLocate.hpp"

#include "ServerLevel.hpp"
#include "server/world/MyTerrainGenerator.hpp"

#include "common/core/Log.hpp"
#include "common/world/level/DimensionId.hpp"

#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/structure/StructurePlacement.h"
#include "levelgen/structure/StructureSet.h"

#include <exception>

namespace Server {

    std::optional<glm::ivec3> FindNearestStronghold(const ServerLevel& level,
                                                    const glm::ivec3& around) {
        // Strongholds are an overworld structure set. Asking in the Nether is
        // not an error — MC's tag lookup simply finds nothing there — so this
        // answers empty rather than warning.
        if (level.Dimension() != Game::DimensionId::Overworld) return std::nullopt;

        Game::MyTerrainGenerator* generator = level.TerrainGenerator();
        if (!generator) return std::nullopt;

        auto* state = generator->GetStructureState();
        if (!state) return std::nullopt;

        namespace mls = minecraft::levelgen::structure;

        // byName throws for an unknown set, and a data pack without
        // strongholds is a legitimate configuration rather than a bug — so the
        // throw is caught rather than allowed to take the server thread down.
        const mls::StructureSet* strongholds = nullptr;
        try {
            strongholds = &mls::StructureSets::byName("minecraft:strongholds");
        } catch (const std::exception&) {
            Log::Warning("[StrongholdLocate] No 'minecraft:strongholds' structure set; "
                         "an Eye of Ender will have nothing to point at");
            return std::nullopt;
        }

        const auto* rings = dynamic_cast<const mls::ConcentricRingsStructurePlacement*>(
            strongholds->placement.get());
        if (!rings) {
            Log::Warning("[StrongholdLocate] 'minecraft:strongholds' is not a "
                         "concentric-rings set; locate is not implemented for it");
            return std::nullopt;
        }

        // Precomputed at generator start from the world seed — 128 chunk
        // positions in eight rings. Nothing needs to be generated first.
        const std::vector<std::pair<int32_t, int32_t>>* positions =
            state->getRingPositionsFor(rings);
        if (!positions || positions->empty()) return std::nullopt;

        // MC ChunkGenerator.java:190-216 measures against the chunk's CENTRE
        // block at y=32, not its corner. Reproducing that matters when two
        // strongholds are near-equidistant: the corner would pick the other one.
        std::optional<glm::ivec3> best;
        double bestDistSqr = 0.0;
        for (const auto& [chunkX, chunkZ] : *positions) {
            const glm::ivec3 probe{ chunkX * 16 + 8, 32, chunkZ * 16 + 8 };
            const double dx = static_cast<double>(probe.x - around.x);
            const double dz = static_cast<double>(probe.z - around.z);
            const double distSqr = dx * dx + dz * dz;
            if (!best || distSqr < bestDistSqr) {
                best = glm::ivec3(probe.x, 0, probe.z);   // MC returns y = 0
                bestDistSqr = distSqr;
            }
        }
        return best;
    }

} // namespace Server
