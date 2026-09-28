// File: src/server/level/PatrolSpawner.hpp
//
// MC net.minecraft.world.level.levelgen.PatrolSpawner — the overworld's
// CustomSpawner that sends pillager patrols at the players.
//
// Every 12000..13199 ticks (the countdown starts at 0, so the first attempt
// is on the first tick) while spawn_mobs and spawn_patrols hold and the
// difficulty is not Peaceful: in daylight (Level.isBrightOutside), one time
// in five, a random player of the level (skipped when a spectator, or within
// two sections of a village) gets a patrol 24..47 blocks away on each axis
// (random sign each) — when the 21x21 area around that spot is loaded and
// the CAN_PILLAGER_PATROL_SPAWN environment attribute holds there: false in
// the mushroom fields (the biome's attribute) and, through the overworld's
// early_game timeline, before the overworld clock reaches 120000 ticks
// (the first five days). The patrol is ceil(local effective difficulty) + 1
// pillagers, each on the MOTION_BLOCKING_NO_LEAVES surface and drifting
// nextInt(5) - nextInt(5) on each axis from the last; the first is the
// leader (patrol target within 500 blocks of the world origin — it is rolled
// before the pillager is placed, as MC does) and a leader that cannot stand
// there (isValidEmptySpawnBlock, block light <= 8, a valid floor) cancels
// the whole patrol. Members spawn with SpawnReason::Patrol (patrolling; the
// leader's ominous banner comes from PatrollingMonster.finalizeSpawn).
//
// MC keeps nextTick in memory only (a restart resets it to 0).
#pragma once

#include "common/core/JavaRandom.hpp"

#include <glm/glm.hpp>

namespace Server {

    class ServerLevel;

    class PatrolSpawner {
    public:
        // MC CustomSpawner.tick(level, spawnEnemies) — once per server tick
        // for the overworld, while spawn_mobs holds.
        void Tick(ServerLevel& level);

    private:
        bool SpawnPatrolMember(ServerLevel& level, const glm::ivec3& pos, bool isLeader);

        int m_nextTick = 0;
    };

} // namespace Server
