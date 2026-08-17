// File: src/common/world/spawn/NaturalSpawner.hpp
//
// MC net.minecraft.world.level.NaturalSpawner.
//
// The three rules that decide whether the world feels like Minecraft:
//
//  1. THE CAP IS GLOBAL, NOT PER CHUNK. `maxInstancesPerChunk` is misleadingly
//     named: the real budget is `max * spawnableChunkCount / 289`, where 289 is
//     17² — MC's notion of how many chunks one player keeps spawnable. Treating
//     it as per-chunk gives a world with hundreds of times too many mobs.
//
//  2. THE 24-BLOCK EXCLUSION. Nothing spawns within 24 blocks of a player, so
//     mobs always appear at a distance and walk in. Dropping this is the single
//     change that makes a game feel unfair rather than tense.
//
//  3. PACK SPAWNING. Each attempt places 1..4 of the SAME type, scattered with
//     a +-6 block walk between placements. This is why cows come in herds and
//     zombies in clusters rather than being uniformly sprinkled.
//
// The per-biome weights come from GeneratedMobSpawns, baked from the vanilla
// biome JSON by tools/gen_mob_spawns.py.
#pragma once

#include "common/entity/EntityType.hpp"
#include "common/entity/MobCategory.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "common/world/chunk/Chunk.hpp"

namespace Game {

    class Mob;
    struct EntityLevel;
    class JavaRandom;

    // MC NaturalSpawner constants.
    inline constexpr int kMinSpawnDistance    = 24;    // blocks from any player
    inline constexpr int kSpawnDistanceChunk  = 8;
    inline constexpr int kMagicNumber         = 17 * 17;  // 289
    inline constexpr int kPackAttemptsPerChunk = 3;
    inline constexpr int kPackSpread           = 6;
    // MC Mob.getMaxSpawnClusterSize. Counted across all three attempts, not
    // per attempt — see the comment at the loop.
    inline constexpr int kMaxSpawnClusterSize  = 4;
    // MC Level.getMinY for the overworld. The spawner samples Y from here to
    // the WORLD_SURFACE height, inclusive at both ends.
    inline constexpr int kMinBuildHeight       = -64;
    // CREATURE (passive) only gets a spawn pass every 400 ticks; everything
    // else is eligible every tick. This is why animals are placed mostly at
    // worldgen and top up slowly, while monsters refill continuously.
    inline constexpr int kCreatureSpawnInterval = 400;

    // Everything the spawner needs from its caller, so it stays free of any
    // dependency on the server's chunk or session machinery.
    struct SpawnContext {
        EntityLevel* level = nullptr;

        // Number of chunks currently eligible for spawning, for the cap.
        int spawnableChunkCount = 0;

        // Live count per MobCategory, indexed by the enum.
        const int* categoryCounts = nullptr;

        // Positions of every player, for the distance rules.
        const std::vector<glm::dvec3>* playerPositions = nullptr;

        // Biome slug at a world position — the key into GeneratedMobSpawns.
        std::function<std::string_view(int, int, int)> biomeAt;

        // MC Level.noCollision(type.getSpawnAABB(...)) — does the mob's own box
        // fit here? Without this mobs spawn embedded in walls and immediately
        // suffocate or get pushed out.
        std::function<bool(EntityTypeId, double, double, double)> spawnBoxFree;

        // MC isRightDistanceToPlayerAndSpawnPoint's second half: nothing spawns
        // within 24 blocks of the world respawn point.
        glm::dvec3 worldSpawn{0.0};
        bool       hasWorldSpawn = false;

        // MC ServerLevel.canSpawnEntitiesInChunk — the pack walk drifts up to
        // ±6 blocks per placement and routinely leaves the chunk it started in.
        // MC allows that only into a chunk that is itself entity-ticking;
        // without the test, mobs get placed into chunks nobody is simulating.
        std::function<bool(int, int)> chunkSpawnable;

        // MC LocalMobCapCalculator.canSpawn — the PER-PLAYER cap, distinct from
        // the global one. A chunk may spawn only if some player near it is
        // below maxInstancesPerChunk for the category, which is what stops one
        // player's mob farm from consuming the whole server's budget.
        std::function<bool(MobCategory, int, int)> canSpawnLocal;

        // Construct a mob of the given type. Supplied by the caller so this
        // header does not have to know every concrete class.
        std::function<std::unique_ptr<Mob>(EntityTypeId)> createMob;
    };

    // Run one chunk's worth of spawn attempts. Appends anything created to
    // `out`; the caller owns registering them with its entity manager.
    //
    // `chunk` is the RESOLVED chunk being spawned in, matching MC
    // NaturalSpawner.spawnForChunk(ServerLevel, LevelChunk, ...). The caller
    // has it already, and handing it over means the starting position's
    // heightmap read is a direct `chunk.getHeight(...)` — MC getRandomPosWithin
    // — instead of a lookup back through the level for a chunk we are holding.
    //
    // `gameTime` gates the CREATURE pass.
    void SpawnForChunk(const SpawnContext& ctx, const Chunk& chunk,
                       int chunkX, int chunkZ,
                       int64_t gameTime, JavaRandom& rng,
                       std::vector<std::unique_ptr<Mob>>& out);

    // MC SpawnState.canSpawnForCategoryGlobal.
    bool CanSpawnForCategory(const SpawnContext& ctx, MobCategory category);

} // namespace Game
