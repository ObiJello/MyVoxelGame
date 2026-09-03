// File: src/server/world/storage/anvil/LevelDat.hpp
//
// level.dat — GZIP NBT, unnamed root compound holding exactly one key, "Data".
//
// Minecraft will not list a folder without one, so this is what turns a
// directory of region files into a world. Written through a temp file and two
// renames (MC's Util.safeReplaceFile), leaving the previous generation as
// level.dat_old.
#pragma once

#include "server/world/storage/anvil/SaveRoot.hpp"

#include <cstdint>
#include <string>

namespace Game::Anvil {

    // Everything level.dat carries that ObeyCraft actually knows. Fields
    // vanilla defaults sensibly (DragonFight, CustomBossEvents, the wandering
    // trader) are simply not written.
    struct LevelDatData {
        std::string levelName = "World";
        int64_t     seed      = 0;

        int  gameType      = 0;      // 0 survival, 1 creative, 2 adventure, 3 spectator
        bool hardcore      = false;
        bool allowCommands = true;
        int  difficulty    = 2;      // 0 peaceful .. 3 hard

        bool generateStructures = true;
        bool bonusChest         = false;

        int64_t time       = 0;      // total ticks the world has run
        int64_t dayTime    = 6000;   // 6000 = noon
        int64_t lastPlayed = 0;      // epoch MILLISECONDS; 0 = stamp with now

        int   spawnX = 0, spawnY = 64, spawnZ = 0;
        float spawnYaw = 0.0f, spawnPitch = 0.0f;

        // Gamerules the engine models. Written as strings, which is how the
        // game_rules compound stores every rule regardless of its type.
        bool doDaylightCycle  = false;
        bool doMobSpawning    = true;
        bool immersivePortals = true;   // this engine's rule, not vanilla's
        int  worldWrapSize   = 0;       // engine world option (0 = off)
        bool dimensionStack  = false;   // engine world option
        bool mobGriefing      = true;
        int  randomTickSpeed  = 3;
    };

    // Writes <root>/level.dat, rotating the previous one to level.dat_old.
    // Takes a SaveRoot, so it cannot be aimed at a real Minecraft world.
    bool WriteLevelDat(const SaveRoot& root, const LevelDatData& data,
                       int dataVersion, std::string& error);

} // namespace Game::Anvil
