// File: src/server/world/storage/anvil/LevelDat.cpp
#include "server/world/storage/anvil/LevelDat.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#endif
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <vector>

namespace Game::Anvil {

    namespace {

        std::FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#if defined(_WIN32)
            const std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
            // _wfopen_s opens with EXCLUSIVE sharing (_SH_DENYRW) — a second
            // open of the same path, even from this same process, fails with
            // EACCES ("Permission denied"). POSIX fopen, which the rest of
            // this code is written against, shares freely. _wfsopen with
            // _SH_DENYNO restores that behaviour; concurrent world access is
            // guarded by session.lock (SessionLock), not by the file mode.
            return _wfsopen(p.c_str(), wmode.c_str(), _SH_DENYNO);
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

        // game_rules is MC's typed, namespaced map (PrimaryLevelData stores
        // it through GameRules.codec = Codec.dispatchedMap over the
        // GAME_RULE registry): keys are the rule ids as resource locations,
        // booleans are bytes, integers are ints. Only the rules this engine
        // models are written; vanilla defaults the rest.
        w.BeginCompound("game_rules");
        w.Bool("minecraft:advance_time",               data.doDaylightCycle);
        w.Bool("minecraft:spawn_mobs",                 data.doMobSpawning);
        w.Bool("minecraft:mob_griefing",               data.mobGriefing);
        w.Int ("minecraft:random_tick_speed",          data.randomTickSpeed);
        w.Bool("minecraft:tnt_explodes",               data.tntExplodes);
        w.Bool("minecraft:entity_drops",               data.doEntityDrops);
        w.Bool("minecraft:tnt_explosion_drop_decay",   data.tntExplosionDropDecay);
        w.Bool("minecraft:block_explosion_drop_decay", data.blockExplosionDropDecay);
        w.Bool("minecraft:mob_explosion_drop_decay",   data.mobExplosionDropDecay);
        w.EndCompound();

        // Engine-only world settings in their own compound. Minecraft
        // ignores a tag it does not know under Data; a key it does not know
        // INSIDE game_rules would fail the registry-keyed decode above.
        w.BeginCompound("obeycraft");
        w.Bool("immersive_portals", data.immersivePortals);
        w.Int ("world_wrap",        data.worldWrapSize);
        w.Bool("dimension_stack",   data.dimensionStack);
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

    namespace {

        using ::World::NBTTagCompound;
        using ::World::NBTTagPtr;

        std::shared_ptr<NBTTagCompound> Compound(const NBTTagCompound& parent, const char* key) {
            return std::dynamic_pointer_cast<NBTTagCompound>(parent.GetTag(key));
        }

        // A boolean rule under any of the names it may carry: typed byte or
        // legacy "true"/"false" string.
        bool RuleBool(const NBTTagCompound* rules, std::initializer_list<const char*> keys, bool def) {
            if (!rules) return def;
            for (const char* key : keys) {
                const NBTTagPtr tag = rules->GetTag(key);
                if (!tag) continue;
                if (auto b = std::dynamic_pointer_cast<::World::NBTTagByte>(tag))   return b->value != 0;
                if (auto s = std::dynamic_pointer_cast<::World::NBTTagString>(tag)) return s->value == "true";
            }
            return def;
        }
        int RuleInt(const NBTTagCompound* rules, std::initializer_list<const char*> keys, int def) {
            if (!rules) return def;
            for (const char* key : keys) {
                const NBTTagPtr tag = rules->GetTag(key);
                if (!tag) continue;
                if (auto i = std::dynamic_pointer_cast<::World::NBTTagInt>(tag)) return i->value;
                if (auto s = std::dynamic_pointer_cast<::World::NBTTagString>(tag)) {
                    char* end = nullptr;
                    const long v = std::strtol(s->value.c_str(), &end, 10);
                    if (end != s->value.c_str() && *end == '\0') return static_cast<int>(v);
                }
            }
            return def;
        }

    } // namespace

    bool ReadLevelDat(const std::filesystem::path& levelDat, LevelDatData& out, std::string& error) {
        std::ifstream f(levelDat, std::ios::binary);
        if (!f) { error = "cannot open " + levelDat.string(); return false; }
        const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.empty()) { error = levelDat.string() + " is empty"; return false; }

        std::vector<uint8_t> nbt;
        if (!Nbt::GzipDecompress(raw, nbt)) { error = "cannot inflate " + levelDat.string(); return false; }

        const auto root = std::dynamic_pointer_cast<NBTTagCompound>(::World::NBTParser::Parse(nbt));
        if (!root) { error = "level.dat is not an NBT compound"; return false; }
        const auto data = Compound(*root, "Data");
        if (!data) { error = "level.dat has no Data compound"; return false; }

        const std::string name = data->GetValue<std::string>("LevelName", "");
        if (!name.empty()) out.levelName = name;
        out.gameType      = data->GetValue<int32_t>("GameType", out.gameType);
        out.hardcore      = data->GetValue<int8_t>("hardcore", out.hardcore ? 1 : 0) != 0;
        out.allowCommands = data->GetValue<int8_t>("allowCommands", out.allowCommands ? 1 : 0) != 0;
        out.difficulty    = data->GetValue<int8_t>("Difficulty", static_cast<int8_t>(out.difficulty));
        out.time          = data->GetValue<int64_t>("Time", out.time);
        out.dayTime       = data->GetValue<int64_t>("DayTime", out.dayTime);
        out.lastPlayed    = data->GetValue<int64_t>("LastPlayed", out.lastPlayed);
        out.spawnX        = data->GetValue<int32_t>("SpawnX", out.spawnX);
        out.spawnY        = data->GetValue<int32_t>("SpawnY", out.spawnY);
        out.spawnZ        = data->GetValue<int32_t>("SpawnZ", out.spawnZ);
        out.spawnYaw      = data->GetValue<float>("SpawnAngle", out.spawnYaw);

        if (const auto gen = Compound(*data, "WorldGenSettings")) {
            out.seed               = gen->GetValue<int64_t>("seed", out.seed);
            out.generateStructures = gen->GetValue<int8_t>("generate_features", out.generateStructures ? 1 : 0) != 0;
            out.bonusChest         = gen->GetValue<int8_t>("bonus_chest", out.bonusChest ? 1 : 0) != 0;
        }

        const auto rules = Compound(*data, "game_rules");
        const NBTTagCompound* r = rules.get();
        out.doDaylightCycle         = RuleBool(r, {"minecraft:advance_time", "advance_time", "doDaylightCycle"}, out.doDaylightCycle);
        out.doMobSpawning           = RuleBool(r, {"minecraft:spawn_mobs", "spawn_mobs", "doMobSpawning"}, out.doMobSpawning);
        out.mobGriefing             = RuleBool(r, {"minecraft:mob_griefing", "mob_griefing", "mobGriefing"}, out.mobGriefing);
        out.randomTickSpeed         = RuleInt (r, {"minecraft:random_tick_speed", "random_tick_speed", "randomTickSpeed"}, out.randomTickSpeed);
        out.tntExplodes             = RuleBool(r, {"minecraft:tnt_explodes", "tnt_explodes", "tntExplodes"}, out.tntExplodes);
        out.doEntityDrops           = RuleBool(r, {"minecraft:entity_drops", "entity_drops", "doEntityDrops"}, out.doEntityDrops);
        out.tntExplosionDropDecay   = RuleBool(r, {"minecraft:tnt_explosion_drop_decay", "tnt_explosion_drop_decay", "tntExplosionDropDecay"}, out.tntExplosionDropDecay);
        out.blockExplosionDropDecay = RuleBool(r, {"minecraft:block_explosion_drop_decay", "block_explosion_drop_decay", "blockExplosionDropDecay"}, out.blockExplosionDropDecay);
        out.mobExplosionDropDecay   = RuleBool(r, {"minecraft:mob_explosion_drop_decay", "mob_explosion_drop_decay", "mobExplosionDropDecay"}, out.mobExplosionDropDecay);

        // Engine settings: their own compound now, game_rules strings before.
        const auto obey = Compound(*data, "obeycraft");
        out.immersivePortals = RuleBool(obey.get(), {"immersive_portals"}, RuleBool(r, {"immersivePortals"}, out.immersivePortals));
        out.worldWrapSize    = RuleInt (obey.get(), {"world_wrap"},        RuleInt (r, {"obeyWorldWrap"}, out.worldWrapSize));
        out.dimensionStack   = RuleBool(obey.get(), {"dimension_stack"},   RuleBool(r, {"obeyDimensionStack"}, out.dimensionStack));
        return true;
    }

} // namespace Game::Anvil
