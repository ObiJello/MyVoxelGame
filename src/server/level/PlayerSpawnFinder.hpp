// File: src/server/level/PlayerSpawnFinder.hpp
//
// Port of net.minecraft.server.level.PlayerSpawnFinder — the part of spawn
// selection that looks at REAL BLOCKS rather than at generator noise.
//
// ── Why this exists ─────────────────────────────────────────────────────────
// The generator's FindSpawnPosition answers "which chunk", using the climate
// SpawnFinder and getBaseHeight(WORLD_SURFACE_WG). That height is a worldgen
// NOISE ESTIMATE: it knows nothing about the blocks actually placed in the
// column afterwards — surface rules (grass/sand/gravel), trees, snow layers,
// or any decoration. Spawning at that Y therefore lands inside a block
// whenever real terrain sits above the estimate, which is exactly what MC
// avoids by validating against the generated chunk.
//
// MC splits it the same way, and so do we:
//   • MinecraftServer.setInitialSpawn  — climate chunk, then an 11x11 chunk
//     spiral asking getSpawnPosInChunk for a real, standable block.
//   • PlayerSpawnFinder.findSpawn      — per-player, spreads arrivals over a
//     RESPAWN_RADIUS square around the world spawn and re-validates each.
//
// Everything here reads the world through IBlockAccess, so the caller must
// have the chunk loaded first — GetBlock returns Air for an unloaded chunk,
// which would look like a bottomless column and yield no spawn. IntegratedServer
// loads each chunk synchronously as it walks the spiral, mirroring the ticket
// MC takes out per candidate (TicketType.SPAWN_SEARCH).
#pragma once

#include "common/world/math/WorldMath.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <optional>

namespace Game { class World; }

namespace Server {

    class PlayerSpawnFinder {
    public:
        // MC GameRules.RULE_SPAWN_RADIUS default.
        static constexpr int DEFAULT_RESPAWN_RADIUS = 10;
        // MC PlayerSpawnFinder.ABSOLUTE_MAX_ATTEMPTS.
        static constexpr int ABSOLUTE_MAX_ATTEMPTS = 1024;

        // MC getOverworldRespawnPos(level, x, z) — the standable block in this
        // column, or nothing.
        //
        // Scans DOWN from the top of the column: the first fluid ends the
        // search (you may not spawn in or on liquid), and the first block that
        // can be stood on yields the position one above it. Returns the BLOCK
        // position; the caller centres within it.
        static std::optional<glm::ivec3> GetOverworldRespawnPos(
            const Game::World& world, int x, int z);

        // MC getSpawnPosInChunk — first standable column in the chunk, scanned
        // x-major then z, exactly as MC does (the order decides which block a
        // given seed spawns on, so it is not arbitrary).
        static std::optional<glm::ivec3> GetSpawnPosInChunk(
            const Game::World& world, Game::Math::ChunkPos chunkPos);

        // MC findSpawn(level, spawnSuggestion) — where a PLAYER appears.
        //
        // Walks up to 1024 candidates in a RESPAWN_RADIUS square around the
        // world spawn in a coprime-stepped order (MC's trick for visiting the
        // square in a scattered but exhaustive sequence, so two players rarely
        // land on the same block), validating each with GetOverworldRespawnPos
        // plus a real player-AABB check. Falls back to FixupSpawnHeight at the
        // suggestion, which cannot return a position inside a block.
        //
        // Returns feet-position world coordinates, centred in the block.
        static glm::vec3 FindSpawn(const Game::World& world,
                                   const glm::ivec3& spawnSuggestion,
                                   int respawnRadius = DEFAULT_RESPAWN_RADIUS,
                                   uint64_t randomSeed = 0);

        // MC fixupSpawnHeight — the guarantee of last resort. Rises until the
        // player fits, drops to the floor, and steps back up. Used when no
        // candidate validated, so a spawn is always produced.
        static glm::vec3 FixupSpawnHeight(const Game::World& world,
                                          const glm::ivec3& spawnPos);

        // MC noCollision(entity, PLAYER_DIMENSIONS.makeBoundingBox(...), true) —
        // does a standing player fit here, clear of blocks AND of liquid?
        static bool NoCollisionNoLiquid(const Game::World& world,
                                        const glm::ivec3& blockPos);

    private:
        // MC getCoprime.
        static int GetCoprime(int candidateCount);
    };

} // namespace Server
