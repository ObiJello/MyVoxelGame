// File: src/server/level/WanderingTraderSpawner.hpp
//
// MC net.minecraft.world.entity.npc.wanderingtrader.WanderingTraderSpawner —
// the overworld's CustomSpawner that sends a wandering trader and two trader
// llamas to a random player.
//
// Every 1200 ticks (while spawn_mobs and spawn_wandering_traders hold) the
// saved spawn delay drops by 1200. When it runs out it resets to 24000 (one
// day) and the spawner rolls nextInt(100) <= chance: the chance starts at
// 25, grows by 25 per missed day to at most 75, and falls back to 25 once a
// trader actually arrives. An attempt then passes MC's 1-in-10 roll, looks
// for a bell (the MEETING point of interest) within 48 blocks of the player
// — else the player's own spot — and places the trader on the ground within
// 48 blocks of that, with room for it (a 2x3x2 box with no collision) and
// outside #without_wandering_trader_spawns biomes. Two trader llamas land
// within 4 blocks and go on its lead; the trader gets 48000 ticks to live, a
// wander target and a 16-block home at the reference position.
//
// The two numbers are MC's WanderingTraderData (spawn_delay, spawn_chance).
// A 26.1-stamped world keeps them in level.dat as WanderingTraderSpawnDelay /
// WanderingTraderSpawnChance (MC's LevelDatToSavedDataPreparationFix moves
// them to data/minecraft/wandering_trader.dat on upgrade); a 26.3 world's
// saved-data file is read when level.dat has none.
#pragma once

#include "common/core/JavaRandom.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>

namespace Server {

    class ServerLevel;

    class WanderingTraderSpawner {
    public:
        static constexpr int kDefaultTickDelay     = 1200;    // DEFAULT_TICK_DELAY
        static constexpr int kDefaultSpawnDelay    = 24000;   // DEFAULT_SPAWN_DELAY
        static constexpr int kMinSpawnChance       = 25;      // MIN_SPAWN_CHANCE
        static constexpr int kMaxSpawnChance       = 75;      // MAX_SPAWN_CHANCE
        static constexpr int kSpawnChanceIncrease  = 25;      // SPAWN_CHANCE_INCREASE
        static constexpr int kSpawnOneInXChance    = 10;      // SPAWN_ONE_IN_X_CHANCE
        static constexpr int kNumberOfSpawnAttempts = 10;     // NUMBER_OF_SPAWN_ATTEMPTS
        static constexpr int kSearchRadius         = 48;      // spawn(): radius
        static constexpr int kLlamaRadius          = 4;       // tryToSpawnLlamaFor radius
        static constexpr int kHomeRadius           = 16;      // setHomeTo(referencePos, 16)

        WanderingTraderSpawner();

        // The saved WanderingTraderData.
        void Load(int spawnDelay, int spawnChance);
        // MC's 26.3 SavedData file (data/minecraft/wandering_trader.dat,
        // {data:{spawn_delay, spawn_chance}}). False when absent/unreadable.
        bool LoadSavedDataFile(const std::filesystem::path& file);
        int  SpawnDelay()  const { return m_spawnDelay; }
        int  SpawnChance() const { return m_spawnChance; }

        // MC CustomSpawner.tick — called once per server tick for the
        // overworld, only while spawn_mobs holds (ServerChunkCache.tickChunks
        // gates tickCustomSpawners on doMobSpawning).
        void Tick(ServerLevel& level);

    private:
        bool Spawn(ServerLevel& level);
        void TryToSpawnLlamaFor(ServerLevel& level, int32_t traderId, const glm::ivec3& traderPos);
        std::optional<glm::ivec3> FindSpawnPositionNear(ServerLevel& level,
                                                        const glm::ivec3& reference, int radius);
        bool HasEnoughSpace(ServerLevel& level, const glm::ivec3& pos) const;

        // MC `RandomSource.create()` — the spawner's own stream, not the
        // level's.
        Game::JavaRandom m_random;
        int m_tickDelay   = kDefaultTickDelay;
        int m_spawnDelay  = kDefaultSpawnDelay;
        int m_spawnChance = kMinSpawnChance;
    };

} // namespace Server
