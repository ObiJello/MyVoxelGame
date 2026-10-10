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
#include <filesystem>
#include <map>
#include <string>

namespace Game::Anvil {

    // Everything level.dat carries that ObeyCraft actually knows. Fields
    // vanilla defaults sensibly (DragonFight, CustomBossEvents) are simply
    // not written.
    struct LevelDatData {
        std::string levelName = "World";
        int64_t     seed      = 0;

        int  gameType      = 0;      // 0 survival, 1 creative, 2 adventure, 3 spectator
        bool hardcore      = false;
        bool allowCommands = true;
        int  difficulty    = 2;      // 0 peaceful .. 3 hard
        bool difficultyLocked = false;   // MC DifficultyLocked (one-way, World Options' lock)

        bool generateStructures = true;
        bool bonusChest         = false;

        int64_t time       = 0;      // total ticks the world has run
        int64_t dayTime    = 6000;   // 6000 = noon
        int64_t lastPlayed = 0;      // epoch MILLISECONDS; 0 = stamp with now

        // MC WanderingTraderSpawner's saved clock (WanderingTraderData's
        // spawn_delay / spawn_chance; level.dat's WanderingTraderSpawnDelay /
        // WanderingTraderSpawnChance at this save's DataVersion). The flag
        // says the file carried them.
        int  wanderingTraderSpawnDelay  = 24000;
        int  wanderingTraderSpawnChance = 25;
        bool hasWanderingTraderData     = false;

        // MC WeatherData (Server::ServerWeather): at this save's DataVersion
        // the five keys sit in Data (clearWeatherTime, rainTime, raining,
        // thunderTime, thundering — PrimaryLevelData); 26.3's
        // LevelDatToSavedDataPreparationFix moves them to weather.dat.
        // Absent keys read as MC's defaults (all zero / false: a new world's
        // first cycle rolls the delays).
        int  clearWeatherTime = 0;
        int  rainTime         = 0;
        bool raining          = false;
        int  thunderTime      = 0;
        bool thundering       = false;
        // The file carried them (else a 26.3 world's data/minecraft/
        // weather.dat is read — ServerWeather::LoadSavedDataFile).
        bool hasWeatherData   = false;

        int   spawnX = 0, spawnY = 64, spawnZ = 0;
        float spawnYaw = 0.0f, spawnPitch = 0.0f;
        // The file named a spawn (the `spawn` compound at DataVersion 4548+,
        // or SpawnX/Y/Z before). MC keeps an existing world's spawn
        // (setInitialSpawn runs only for a level that is not yet
        // initialized); a world without one gets the generator's.
        bool  hasSpawn = false;

        // Gamerules the engine models. Written as strings, which is how the
        // game_rules compound stores every rule regardless of its type.
        bool doDaylightCycle  = false;
        bool doMobSpawning    = true;
        bool immersivePortals = true;   // this engine's rule, not vanilla's
        int  worldWrapSize   = 0;       // engine world option (0 = off)
        bool dimensionStack  = false;   // engine world option
        bool redstonePlus    = false;   // engine rule (RedstonePlus.hpp)
        bool redstoneChunks  = false;   // engine rule (ChunkKeeper.hpp)
        int  playerStepHeight = 6;      // engine rule (ServerPlayer::applyStepHeightRule), tenths of a block
        bool sharedVitals    = false;   // engine rule (PlayerSessionManager::ShareVitals)
        bool advancementsWithCheats = false;   // engine rule (server/advancements: progress with cheats on)
        bool sharedCraftingTables = false;   // engine rule (CraftingTableBlockEntity)
        bool pistonsMoveBlockEntities = true;   // engine rule (PistonBlockEntities.hpp); absent = on
        bool portalGunFreePlacement = false;   // engine rule (PortalRegistry::PlacePortal)
        bool twilightForestEnabled = true;   // engine rule (ModDimensions.hpp)
        bool aetherEnabled   = true;    // engine rule (ModDimensions.hpp)
        // World Options "Command Access": may guests use commands. MC keeps
        // this per session (IntegratedServer.guestCommandAccess); this
        // engine saves it with the world so a host does not re-enable it
        // on every launch. On by default: players who join may use commands
        // unless the host turns it off. Stored as obeycraft.guest_commands;
        // the older guest_command_access key was always written with the old
        // default (off), so it cannot tell a host's "off" from "never
        // touched" and is not read back.
        bool guestCommandAccess = true;
        bool mobGriefing      = true;
        int  randomTickSpeed  = 3;
        bool tntExplodes             = true;
        bool doEntityDrops           = true;
        bool tntExplosionDropDecay   = false;   // vanilla's odd one out
        bool blockExplosionDropDecay = true;
        bool mobExplosionDropDecay   = true;

        // EVERY registered game rule (Game::Rules), id -> value (booleans
        // 0/1), as read from the file / to be written. The fields above are
        // the nine rules that predate the registry; the writer takes THEM
        // for those ids and this map for the rest, so a caller that fills
        // only the fields still writes a complete game_rules compound.
        std::map<std::string, int> gameRules;
    };

    // Reads <levelDat> (an existing world's) into `out`, leaving any field
    // the file lacks at its default. Gamerules are accepted in every form
    // this engine and Minecraft have written them: typed under the
    // namespaced id ("minecraft:advance_time": 1b — MC's GameRules.codec
    // and this writer), typed under the bare id, or the old string form
    // under the camelCase name ("doDaylightCycle": "true"). False with
    // `error` set when the file is missing or unreadable.
    bool ReadLevelDat(const std::filesystem::path& levelDat, LevelDatData& out, std::string& error);

    // Writes <root>/level.dat, rotating the previous one to level.dat_old.
    // Takes a SaveRoot, so it cannot be aimed at a real Minecraft world.
    bool WriteLevelDat(const SaveRoot& root, const LevelDatData& data,
                       int dataVersion, std::string& error);

} // namespace Game::Anvil
