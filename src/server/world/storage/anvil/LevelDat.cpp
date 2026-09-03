// File: src/server/world/storage/anvil/LevelDat.cpp
#include "server/world/storage/anvil/LevelDat.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/nbt/NbtWrite.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace Game::Anvil {

    namespace {

        std::FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#if defined(_WIN32)
            const std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
            std::FILE* f = nullptr;
            if (_wfopen_s(&f, p.c_str(), wmode.c_str()) != 0) return nullptr;
            return f;
#else
            return std::fopen(p.c_str(), mode);
#endif
        }

        // MC Util.safeReplaceFile: write a temp file, rotate the current one to
        // `backup`, then move the temp into place. A crash at any point leaves
        // either the old file or the new one, never a half-written level.dat.
        bool SafeReplace(const std::filesystem::path& target,
                         const std::filesystem::path& backup,
                         const std::vector<uint8_t>& bytes,
                         std::string& error) {
            std::filesystem::path tmp = target;
            tmp += ".tmp";

            std::FILE* f = OpenFile(tmp, "wb");
            if (!f) { error = "cannot create " + tmp.filename().string(); return false; }
            const size_t put = std::fwrite(bytes.data(), 1, bytes.size(), f);
            const bool flushed = (std::fflush(f) == 0);
            std::fclose(f);
            if (put != bytes.size() || !flushed) {
                std::error_code ec; std::filesystem::remove(tmp, ec);
                error = "short write on " + tmp.filename().string();
                return false;
            }

            std::error_code ec;
            if (std::filesystem::exists(target, ec)) {
                std::filesystem::remove(backup, ec);
                std::filesystem::rename(target, backup, ec);
                if (ec) { error = "cannot rotate to " + backup.filename().string(); return false; }
            }
            std::filesystem::rename(tmp, target, ec);
            if (ec) { error = "cannot move " + tmp.filename().string() + " into place"; return false; }
            return true;
        }

        // One dimension entry of WorldGenSettings.dimensions. The generator
        // settings are the vanilla presets — ObeyCraft's terrain library is a
        // port of exactly these, so naming them is truthful and it is what lets
        // Minecraft continue the world at its edges with matching terrain.
        void WriteDimension(Nbt::Writer& w, const char* key, const char* type,
                            const char* noiseSettings, const char* biomePreset) {
            w.BeginCompound(key);
            w.String("type", type);
            w.BeginCompound("generator");
            w.String("type", "minecraft:noise");
            w.String("settings", noiseSettings);
            w.BeginCompound("biome_source");
            w.String("type", "minecraft:multi_noise");
            w.String("preset", biomePreset);
            w.EndCompound();
            w.EndCompound();
            w.EndCompound();
        }

    } // namespace

    bool WriteLevelDat(const SaveRoot& root, const LevelDatData& data,
                       int dataVersion, std::string& error) {
        Nbt::Writer w;
        w.BeginRootCompound();
        w.BeginCompound("Data");

        // Version block. MC compares only Series for compatibility
        // (LevelSummary.isCompatible); anything but "main" is rejected outright.
        w.BeginCompound("Version");
        w.String("Name",     Save::kVersionName);
        w.Int   ("Id",       dataVersion);
        w.Bool  ("Snapshot", Save::kVersionSnapshot);
        w.String("Series",   Save::kVersionSeries);
        w.EndCompound();

        // DataVersion lives INSIDE Data, not at the root.
        w.Int("DataVersion", dataVersion);
        // Anvil storage revision, lowercase, unrelated to DataVersion.
        w.Int("version", Save::kAnvilStorageVersion);

        w.String("LevelName", data.levelName);
        w.Int   ("GameType",  data.gameType);
        w.Bool  ("hardcore",  data.hardcore);
        w.Bool  ("allowCommands", data.allowCommands);
        w.Bool  ("initialized",   true);
        w.Byte  ("Difficulty",    static_cast<int8_t>(data.difficulty));
        w.Bool  ("DifficultyLocked", false);

        w.Long("Time",    data.time);
        w.Long("DayTime", data.dayTime);
        w.Long("LastPlayed", data.lastPlayed != 0
                              ? data.lastPlayed
                              : static_cast<int64_t>(std::time(nullptr)) * 1000);

        // Spawn, written TWICE on purpose.
        //
        // At DataVersion 4548 and above MC reads the `spawn` RespawnData
        // compound and WorldSpawnDataFix has removed the legacy keys; below
        // that it reads SpawnX/Y/Z. Emitting both costs a few bytes and means
        // either reader finds what it expects — which matters because the
        // engine's block set and the user's installed Minecraft are not
        // guaranteed to be the same version.
        w.BeginCompound("spawn");
        w.String("dimension", "minecraft:overworld");
        {
            const int32_t pos[3] = {data.spawnX, data.spawnY, data.spawnZ};
            w.IntArray("pos", pos, 3);
        }
        w.Float("yaw",   data.spawnYaw);
        w.Float("pitch", data.spawnPitch);
        w.EndCompound();
        w.Int  ("SpawnX", data.spawnX);
        w.Int  ("SpawnY", data.spawnY);
        w.Int  ("SpawnZ", data.spawnZ);
        w.Float("SpawnAngle", data.spawnYaw);

        // Weather. The engine has no weather system, so these round-trip as
        // "clear forever" rather than being omitted and defaulted to random.
        w.Int ("clearWeatherTime", 0);
        w.Int ("rainTime",     0);
        w.Bool("raining",      false);
        w.Int ("thunderTime",  0);
        w.Bool("thundering",   false);

        w.BeginCompound("WorldGenSettings");
        w.Long("seed", data.seed);
        w.Bool("generate_features", data.generateStructures);
        w.Bool("bonus_chest",       data.bonusChest);
        w.BeginCompound("dimensions");
        WriteDimension(w, "minecraft:overworld",  "minecraft:overworld",
                       "minecraft:overworld",  "minecraft:overworld");
        WriteDimension(w, "minecraft:the_nether", "minecraft:the_nether",
                       "minecraft:nether",     "minecraft:nether");
        WriteDimension(w, "minecraft:the_end",    "minecraft:the_end",
                       "minecraft:end",        "minecraft:end");
        w.EndCompound();
        w.EndCompound();

        // Every gamerule is stored as a STRING, whatever its type.
        w.BeginCompound("game_rules");
        w.String("doDaylightCycle", data.doDaylightCycle ? "true" : "false");
        w.String("doMobSpawning",   data.doMobSpawning   ? "true" : "false");
        w.String("immersivePortals", data.immersivePortals ? "true" : "false");
        w.String("obeyWorldWrap",    std::to_string(data.worldWrapSize));
        w.String("obeyDimensionStack", data.dimensionStack ? "true" : "false");
        w.String("mobGriefing",     data.mobGriefing     ? "true" : "false");
        w.String("randomTickSpeed", std::to_string(data.randomTickSpeed));
        w.EndCompound();

        w.BeginCompound("DataPacks");
        { auto e = w.BeginList("Enabled",  Nbt::TagType::String);
          w.ListString(e, "vanilla"); w.EndList(e); }
        { auto d = w.BeginList("Disabled", Nbt::TagType::String); w.EndList(d); }
        w.EndCompound();
        { auto f = w.BeginList("enabled_features", Nbt::TagType::String);
          w.ListString(f, "minecraft:vanilla"); w.EndList(f); }
        { auto s = w.BeginList("ScheduledEvents", Nbt::TagType::Compound); w.EndList(s); }

        w.EndCompound();       // Data
        w.EndRootCompound();

        if (!w.ok()) { error = "NBT writer refused level.dat"; return false; }

        std::vector<uint8_t> gz;
        if (!Nbt::GzipCompress(w.Bytes(), gz)) { error = "gzip failed"; return false; }

        if (!SafeReplace(root.LevelDat(), root.LevelDatOld(), gz, error)) return false;
        Log::Info("[Anvil] wrote level.dat for \"%s\" (%zu bytes, DataVersion %d)",
                  data.levelName.c_str(), gz.size(), dataVersion);
        return true;
    }

} // namespace Game::Anvil
