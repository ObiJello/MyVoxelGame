// File: src/server/level/PhantomSpawner.hpp
//
// MC net.minecraft.world.level.levelgen.PhantomSpawner — the overworld's
// CustomSpawner that sends phantoms at players who have not slept.
//
// While spawn_mobs (the caller's gate), spawnEnemies (not Peaceful) and
// spawn_phantoms hold, a countdown of 1200..2399 ticks (starting at 0, so the
// first attempt is on the first tick) runs; on each attempt, when the sky is
// dark enough (getSkyDarken >= 5) or the dimension has no sky light, every
// non-spectator player of the level who stands at or above sea level under
// open sky (sky-lit dimensions only) is considered: the local difficulty must
// beat random(0..3), and nextInt(max(time_since_rest, 1)) must reach 72000
// (three in-game days awake). Then 1 + nextInt(difficulty id + 1) phantoms
// spawn together 20..34 blocks above the player, -10..10 on each horizontal
// axis, when that cell is a valid empty spawn block for a phantom.
//
// MC keeps nextTick in memory only (a restart resets it to 0).
#pragma once

namespace Server {

    class ServerLevel;

    class PhantomSpawner {
    public:
        // MC CustomSpawner.tick(level, spawnEnemies) — once per server tick
        // for the overworld, while spawn_mobs holds.
        void Tick(ServerLevel& level);

    private:
        int m_nextTick = 0;
    };

} // namespace Server
