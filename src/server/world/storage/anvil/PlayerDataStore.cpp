// File: src/server/world/storage/anvil/PlayerDataStore.cpp
#include "server/world/storage/anvil/PlayerDataStore.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/entity/ShoulderEntities.hpp"
#include "common/entity/decoration/Cushion.hpp"
#include "common/core/Uuid.hpp"
#include "common/entity/Mob.hpp"

#include "common/core/Log.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#endif
#include <cstring>
#include <filesystem>
#include <fstream>
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

        bool SafeReplace(const std::filesystem::path& target,
                         const std::filesystem::path& backup,
                         const std::vector<uint8_t>& bytes, std::string& error) {
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
            }
            std::filesystem::rename(tmp, target, ec);
            if (ec) { error = "cannot move " + tmp.filename().string() + " into place"; return false; }
            return true;
        }

        // ── Inventory slot mapping ──────────────────────────────────────────
        //
        // The engine's Inventory is 46 wide: craft result 0, craft grid 1..4,
        // ARMOUR 5..8 (helmet, chest, legs, boots), main 9..35, hotbar 36..44,
        // offhand 45. Vanilla's Inventory list is a flat 36 — hotbar 0..8 then
        // main 9..35 — with armour and offhand living elsewhere.
        //
        // We write THREE encodings into the one list so both a modern and an
        // older reader find what they expect:
        //   hotbar/main    -> 0..8 / 9..35        (both)
        //   armour         -> 100..103            (pre-4312 only)
        //   offhand        -> -106                (pre-4312 only)
        // A modern reader filters 100..103 and -106 through
        // ItemStackWithSlot.isValidInContainer(36) and skips them, then reads
        // the same items back out of `equipment`. An old reader ignores
        // `equipment`. Neither sees a duplicate.
        //
        // Craft-grid slots are not written at all — vanilla drops them too.
        constexpr int kVanillaArmourSlot[4] = {103, 102, 101, 100};  // helmet..boots
        constexpr int kVanillaOffhandSlot   = -106;

        const char* kEquipmentKey[4] = {"head", "chest", "legs", "feet"};

        // ── stats/<uuid>.json (MC ServerStatsCounter) ───────────────────────
        //
        // The one statistic the engine keeps is minecraft:custom /
        // minecraft:time_since_rest (the phantoms' insomnia count). The file
        // is read, that entry replaced, and written back, so every other stat
        // a vanilla session recorded in the same file survives.
        constexpr const char* kStatsCustom      = "minecraft:custom";
        constexpr const char* kStatTimeSinceRest = "minecraft:time_since_rest";

        std::filesystem::path StatsFile(const SaveRoot& root, const Game::Uuid& uuid) {
            return root.Root() / "stats" / (UuidToString(uuid) + ".json");
        }

        nlohmann::json ReadStatsJson(const std::filesystem::path& file) {
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) return nlohmann::json::object();
            std::ifstream in(file, std::ios::binary);
            if (!in) return nlohmann::json::object();
            nlohmann::json root = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
            return root.is_object() ? root : nlohmann::json::object();
        }

        bool WritePlayerStats(const SaveRoot& root, const Server::ServerPlayer& player,
                              int dataVersion, std::string& error) {
            const auto file = StatsFile(root, OfflinePlayerUuid(player.getName()));
            nlohmann::json doc = ReadStatsJson(file);
            nlohmann::json& stats = doc["stats"];
            if (!stats.is_object()) stats = nlohmann::json::object();
            nlohmann::json& custom = stats[kStatsCustom];
            if (!custom.is_object()) custom = nlohmann::json::object();
            custom[kStatTimeSinceRest] = player.getTimeSinceRest();
            doc["DataVersion"] = dataVersion;

            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            const std::string text = doc.dump();
            const std::vector<uint8_t> bytes(text.begin(), text.end());
            auto backup = file;
            backup += "_old";
            if (!SafeReplace(file, backup, bytes, error)) return false;
            std::filesystem::remove(backup, ec);   // MC keeps no previous generation
            return true;
        }

        void ReadPlayerStats(const SaveRoot& root, Server::ServerPlayer& player) {
            const nlohmann::json doc = ReadStatsJson(StatsFile(root, OfflinePlayerUuid(player.getName())));
            const auto stats = doc.find("stats");
            if (stats == doc.end() || !stats->is_object()) return;
            const auto custom = stats->find(kStatsCustom);
            if (custom == stats->end() || !custom->is_object()) return;
            const auto value = custom->find(kStatTimeSinceRest);
            if (value != custom->end() && value->is_number_integer()) {
                player.setTimeSinceRest(value->get<int>());
            }
        }

    } // namespace

    // ── Write ───────────────────────────────────────────────────────────────

    bool BuildPlayerNbt(const Server::ServerPlayer& player, int dataVersion, std::vector<uint8_t>& out,
                        std::string& error, const Game::Mob* rootVehicle, const Game::Uuid* attachUuid) {
        const auto uuid = OfflinePlayerUuid(player.getName());
        const auto& inventory = player.getInventory();

        Nbt::Writer w;
        w.BeginRootCompound();

        w.Int("DataVersion", dataVersion);

        {
            // A seated player (a cushion) is saved where getting up puts
            // them — on top of the seat (Entity.getDismountLocationForPassenger)
            // — not at the seat, which sits a third of a block into the block
            // below. MC instead saves the vehicle with the player
            // ("RootVehicle"); this engine leaves the cushion in the world.
            // With a RootVehicle the player is saved in the seat and put
            // back into it on join (MC).
            glm::dvec3 p = player.getPosition();
            if (player.isPassenger() && !rootVehicle) p.y += Game::Cushion::kPlayerVehicleAttachmentY;
            const double pos[3] = {p.x, p.y, p.z};
            auto list = w.BeginList("Pos", Nbt::TagType::Double);
            for (double v : pos) w.ListDouble(list, v);
            w.EndList(list);
        }
        {
            // No velocity is persisted deliberately: rejoining mid-fall with
            // the momentum you left with is a worse experience than landing.
            auto list = w.BeginList("Motion", Nbt::TagType::Double);
            for (int i = 0; i < 3; ++i) w.ListDouble(list, 0.0);
            w.EndList(list);
        }
        {
            auto list = w.BeginList("Rotation", Nbt::TagType::Float);
            w.ListFloat(list, player.getYaw());
            w.ListFloat(list, player.getPitch());
            w.EndList(list);
        }

        // "fall_distance", NOT "FallDistance", and TAG_Double, not Float.
        // Both were checked against real files rather than assumed: at
        // DataVersion 4764 the old spelling does not exist (99/99 entities
        // carry fall_distance, none carry FallDistance) and the tag type in a
        // real playerdata file is Double.
        w.Double("fall_distance", 0.0);
        w.Short("Fire", -20);
        w.Short("Air", 300);
        w.Bool ("OnGround", player.isOnGround());
        w.Bool ("Invulnerable", false);
        w.Int  ("PortalCooldown", 0);
        {
            // Vanilla stores a UUID as an int array of 4, big-endian.
            int32_t words[4];
            for (int i = 0; i < 4; ++i) {
                words[i] = static_cast<int32_t>(
                    (uint32_t(uuid[i * 4 + 0]) << 24) | (uint32_t(uuid[i * 4 + 1]) << 16) |
                    (uint32_t(uuid[i * 4 + 2]) << 8)  |  uint32_t(uuid[i * 4 + 3]));
            }
            w.IntArray("UUID", words, 4);
        }

        w.Float("Health", player.getHealth());
        w.Short("HurtTime", 0);
        w.Short("DeathTime", 0);
        // MC LivingEntity.addAdditionalSaveData: AbsorptionAmount and the
        // active_effects list (MobEffectInstance.CODEC, omitted when empty).
        w.Float("AbsorptionAmount", player.getAbsorptionAmount());
        WriteActiveEffects(w, player.activeEffects());
        // MC LivingEntity.addAdditionalSaveData's "attributes"
        // (AttributeMap.save): every base, and the permanent modifiers
        // (/attribute … modifier add). The worn items', effects' and the
        // step-height rule's are rebuilt, never saved.
        WriteAttributeList(w, player.attributes());

        const auto& food = player.getFoodData();
        w.Int  ("foodLevel",           food.getFoodLevel());
        w.Float("foodSaturationLevel", food.getSaturationLevel());
        w.Float("foodExhaustionLevel", 0.0f);
        w.Int  ("foodTickTimer",       0);

        const auto& xp = player.getExperience();
        w.Int  ("XpLevel", xp.Level());
        w.Float("XpP",     xp.Progress());
        w.Int  ("XpTotal", xp.Total());
        w.Int  ("XpSeed",  player.getEnchantmentSeed());
        w.Int  ("Score",   0);
        // MC Entity.saveWithoutId: the scoreboard tags (/tag).
        WriteEntityTags(w, player.getTags());

        // MC ServerPlayer.addAdditionalSaveData: warden_spawn_tracker
        // (WardenSpawnTracker.CODEC).
        {
            const Game::WardenSpawnTracker& tracker = player.getWardenSpawnTracker();
            w.BeginCompound("warden_spawn_tracker");
            w.Int("ticks_since_last_warning", tracker.GetTicksSinceLastWarning());
            w.Int("warning_level",            tracker.GetWarningLevel());
            w.Int("cooldown_ticks",           tracker.GetCooldownTicks());
            w.EndCompound();
        }

        // The player's name. Vanilla keeps none in this file (it is named by
        // UUID; names live in usercache.json), so a tool reading only the
        // world — the portal-gun owner lookup for OFFLINE players — could
        // not name its owner. Namespaced, so vanilla ignores it.
        w.String("obeycraft:last_known_name", player.getName());
        w.Int   ("playerGameType", static_cast<int>(player.getGameMode()));
        // MC ServerPlayer.addAdditionalSaveData: previousPlayerGameType only
        // when there is one (GameType.createProfileSerializationCodec).
        if (player.getPreviousGameMode() >= 0) {
            w.Int("previousPlayerGameType", player.getPreviousGameMode());
        }
        w.String("Dimension", std::string(Game::DimensionRegistryName(
                                  Game::DimensionFromRaw(player.getDimensionId()))));
        w.Int   ("SelectedItemSlot", inventory.GetSelectedSlot());

        {
            auto items = w.BeginList("Inventory", Nbt::TagType::Compound);

            auto put = [&](int engineSlot, int vanillaSlot) {
                const ItemStack& stack = inventory.GetSlot(engineSlot);
                if (stack.IsEmpty()) return;
                w.ListCompoundBegin(items);
                WriteItemStackBody(w, stack, vanillaSlot);
                w.ListCompoundEnd(items);
            };

            for (int i = 0; i < Game::Inventory::HOTBAR_SIZE; ++i) {
                put(Game::Inventory::HOTBAR_BEGIN + i, i);              // hotbar -> 0..8
            }
            for (int i = 0; i < Game::Inventory::MAIN_SIZE; ++i) {
                put(Game::Inventory::MAIN_BEGIN + i, Game::Inventory::MAIN_BEGIN + i);  // 9..35
            }
            for (int i = 0; i < Game::Inventory::ARMOR_SIZE; ++i) {
                put(Game::Inventory::ARMOR_BEGIN + i, kVanillaArmourSlot[i]);
            }
            put(Game::Inventory::OFFHAND_BEGIN, kVanillaOffhandSlot);

            w.EndList(items);
        }

        {
            // The modern home for armour and offhand. A reader at DataVersion
            // 4764 takes them from here and ignores the 100..103 / -106
            // entries above.
            w.BeginCompound("equipment");
            for (int i = 0; i < Game::Inventory::ARMOR_SIZE; ++i) {
                const ItemStack& stack = inventory.GetSlot(Game::Inventory::ARMOR_BEGIN + i);
                if (stack.IsEmpty()) continue;
                w.BeginCompound(kEquipmentKey[i]);
                WriteItemStackBody(w, stack);
                w.EndCompound();
            }
            const ItemStack& offhand = inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN);
            if (!offhand.IsEmpty()) {
                w.BeginCompound("offhand");
                WriteItemStackBody(w, offhand);
                w.EndCompound();
            }
            w.EndCompound();
        }

        { auto ender = w.BeginList("EnderItems", Nbt::TagType::Compound); w.EndList(ender); }

        w.BeginCompound("abilities");
        w.Bool ("invulnerable", player.getGameMode() == Server::GameMode::CREATIVE);
        w.Bool ("mayfly",       player.getGameMode() == Server::GameMode::CREATIVE);
        w.Bool ("instabuild",   player.getGameMode() == Server::GameMode::CREATIVE);
        w.Bool ("mayBuild",     true);
        w.Bool ("flying",       player.isFlying());
        w.Float("flySpeed",     0.05f);
        w.Float("walkSpeed",    0.1f);
        // Engine-only, inside the vanilla `abilities` compound where it reads
        // as an unknown key Minecraft ignores. There is no vanilla noclip.
        w.Bool ("obey_noclip",  player.isNoclip());
        // The body's size from scaled immersive portals; 1 is vanilla.
        w.Float("obey_scale",   player.getScale());
        w.EndCompound();

        // MC ServerPlayer.addAdditionalSaveData: `respawn` is RespawnConfig's
        // codec — LevelData.RespawnData's map codec (GlobalPos flattened to
        // `dimension` + `pos`, then `yaw`, `pitch`) plus `forced`. Absent when
        // the player has no respawn point, as storeNullable leaves it.
        if (const auto& respawn = player.getRespawnConfig()) {
            w.BeginCompound("respawn");
            w.String("dimension", respawn->dimensionId == -1 ? "minecraft:the_nether"
                                : respawn->dimensionId ==  1 ? "minecraft:the_end"
                                                             : "minecraft:overworld");
            {
                const int32_t pos[3] = {respawn->pos.x, respawn->pos.y, respawn->pos.z};
                w.IntArray("pos", pos, 3);
            }
            w.Float("yaw",    respawn->yaw);
            w.Float("pitch",  respawn->pitch);
            w.Bool ("forced", respawn->forced);
            w.EndCompound();
        }

        // Engine-only (the Hush's recall chime, docs/the-hush.md): where the
        // player last arrived through a hush gate. The dimension is written by
        // registry name so the Hush itself round-trips (the `respawn` writer
        // above only knows the three vanilla ones). Absent when never crossed.
        if (const auto& gate = player.getLastHushGate()) {
            w.BeginCompound("obey_hush_gate");
            w.String("dimension", std::string(Game::DimensionRegistryName(
                                      Game::DimensionFromRaw(gate->dimensionId))));
            w.Double("x", gate->pos.x);
            w.Double("y", gate->pos.y);
            w.Double("z", gate->pos.z);
            w.Float ("yaw", gate->yaw);
            w.EndCompound();
        }

        // MC ServerPlayer.addAdditionalSaveData: ShoulderEntityLeft /
        // ShoulderEntityRight, the riding parrots' whole compounds, only when
        // taken.
        Server::ShoulderEntities::WritePlayerData(w, player);

        // MC ServerPlayer.addAdditionalSaveData: the vehicle the player rides
        // leaves with them ("RootVehicle": the root entity with its whole
        // Passengers tree, and the UUID of the one the player sits on).
        if (rootVehicle && attachUuid) {
            w.BeginCompound("RootVehicle");
            int32_t words[4];
            UuidToIntArray(*attachUuid, words);
            w.IntArray("Attach", words, 4);
            if (!WriteMobCompound(w, "Entity", *rootVehicle)) {
                Log::Warning("[Anvil] player '%s' rides a vehicle that cannot be saved",
                             player.getName().c_str());
            }
            w.EndCompound();
        }

        w.EndRootCompound();
        if (!w.ok()) { error = "NBT writer refused the player data"; return false; }
        out = w.TakeBytes();
        return true;
    }

    bool WritePlayerData(const SaveRoot& root, const Server::ServerPlayer& player,
                         int dataVersion, std::string& error,
                         const Game::Mob* rootVehicle, const Game::Uuid* attachUuid) {
        std::vector<uint8_t> nbt;
        if (!BuildPlayerNbt(player, dataVersion, nbt, error, rootVehicle, attachUuid)) return false;
        const auto uuid = OfflinePlayerUuid(player.getName());

        std::vector<uint8_t> gz;
        if (!Nbt::GzipCompress(nbt, gz)) { error = "gzip failed"; return false; }

        std::error_code ec;
        std::filesystem::create_directories(root.PlayerDataDir(), ec);

        const std::string file = UuidToString(uuid);
        const auto target = root.PlayerDataDir() / (file + ".dat");
        auto backup = root.PlayerDataDir() / (file + ".dat_old");
        if (!SafeReplace(target, backup, gz, error)) return false;

        // The statistics file beside it (non-fatal: the player file is the
        // one that matters).
        std::string statsError;
        if (!WritePlayerStats(root, player, dataVersion, statsError)) {
            Log::Warning("[Anvil] player '%s' stats not saved: %s",
                         player.getName().c_str(), statsError.c_str());
        }

        Log::Info("[Anvil] saved player '%s' (%s, %zu bytes)",
                  player.getName().c_str(), file.c_str(), gz.size());
        return true;
    }

    // ── Read ────────────────────────────────────────────────────────────────

    bool ReadPlayerData(const SaveRoot& root, Server::ServerPlayer& player, std::string& error) {
        error.clear();
        // stats/<uuid>.json is its own file, read whether or not the player
        // file exists.
        ReadPlayerStats(root, player);
        const auto uuid = OfflinePlayerUuid(player.getName());
        auto path = root.PlayerDataDir() / (UuidToString(uuid) + ".dat");

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            // One-time migration. The default offline name used to be
            // "Player"; it is now kDefaultPlayerName ("Notch"). The file is
            // named by the type-3 UUID of "OfflinePlayer:" + the name, so that
            // rename orphans every existing world's player file and the player
            // comes back with an empty inventory at spawn.
            //
            // Gated on the name actually BEING the default: a player called
            // "obey" with no file of their own must not inherit the old
            // default player's inventory. Read-only — the next save writes the
            // new path, and the old file is left alone as a backup.
            if (player.getName() != Server::kDefaultPlayerName) return false;
            const auto legacy =
                root.PlayerDataDir() / (UuidToString(OfflinePlayerUuid("Player")) + ".dat");
            if (!std::filesystem::exists(legacy, ec)) return false;   // first join
            Log::Info("[Anvil] no '%s' player file; migrating from the legacy 'Player' one",
                      player.getName().c_str());
            path = legacy;
        }

        std::ifstream f(path, std::ios::binary);
        if (!f) { error = "cannot open " + path.filename().string(); return false; }
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        if (raw.empty()) { error = "player file is empty"; return false; }

        std::vector<uint8_t> nbt;
        if (!Nbt::GzipDecompress(raw, nbt)) { error = "could not decompress player data"; return false; }

        ::World::NBTTagPtr rootTag;
        try {
            rootTag = ::World::NBTParser::Parse(nbt);
        } catch (const std::exception& e) {
            error = std::string("player NBT parse failed: ") + e.what();
            return false;
        }
        auto data = std::dynamic_pointer_cast<::World::NBTTagCompound>(rootTag);
        if (!data) { error = "player root is not a compound"; return false; }

        if (auto pos = std::dynamic_pointer_cast<::World::NBTTagList>(data->GetTag("Pos"));
            pos && pos->value.size() == 3) {
            auto d = [&](int i) {
                auto t = std::dynamic_pointer_cast<::World::NBTTagDouble>(pos->value[i]);
                return t ? t->value : 0.0;
            };
            // MC Entity.load → setPos: the saved position is placed
            // outright. NOT setPosition(): that is the move-packet setter
            // with the "moved too fast" distance gate, measured from the
            // spawn point the player object is born at — so a player who
            // logged out more than 600 blocks from spawn was "rejected" on
            // every rejoin and woke up at spawn (latest.log: "Player 1
            // moved too fast (3474.2 blocks)" right after login).
            player.teleport(glm::dvec3(d(0), d(1), d(2)));
        }
        if (auto rot = std::dynamic_pointer_cast<::World::NBTTagList>(data->GetTag("Rotation"));
            rot && rot->value.size() == 2) {
            auto fl = [&](int i) {
                auto t = std::dynamic_pointer_cast<::World::NBTTagFloat>(rot->value[i]);
                return t ? t->value : 0.0f;
            };
            player.setRotation(fl(0), fl(1));
        }

        // Game mode BEFORE abilities: ServerPlayer::setGameMode re-derives
        // canFly/instabuild and CLEARS m_flying (MC GameType.updatePlayerAbilities),
        // so restoring flight first would be undone one line later.
        {
            const int32_t mode = data->GetValue<int32_t>("playerGameType", -1);
            if (mode >= 0 && mode <= 3) {
                player.setGameMode(static_cast<Server::GameMode>(mode));
            }
            // Loading is not a change: the previous mode is the saved one
            // (MC readAdditionalSaveData → setGameModeForPlayer(loaded,
            // previousPlayerGameType)), -1 when the file has none.
            player.setPreviousGameMode(data->GetValue<int32_t>("previousPlayerGameType", -1));
        }
        if (auto abilities = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                data->GetTag("abilities"))) {
            // Only when the mode actually permits it — a survival player whose
            // file says flying (mode changed while offline, hand-edited file)
            // must not come back airborne.
            if (player.canFly()) {
                player.setFlying(abilities->GetValue<int8_t>("flying", 0) != 0);
            }
            player.setNoclip(abilities->GetValue<int8_t>("obey_noclip", 0) != 0);
            player.setScale(abilities->GetValue<float>("obey_scale", 1.0f));
        }

        // MC LivingEntity.readAdditionalSaveData's order: absorption, then
        // the effects (they move MAX_HEALTH), THEN Health. Installed
        // directly — the view re-folds their attribute modifiers when it is
        // built, and sends them to the client (PlayerList
        // .sendActivePlayerEffects).
        player.setAbsorptionAmount(data->GetValue<float>("AbsorptionAmount", 0.0f));
        // MC readAdditionalSaveData: the attributes first (MAX_HEALTH
        // among them), onto Player.createAttributes' rows only.
        ReadAttributeList(*data, player.attributes(), /*onlyRegistered=*/true);
        player.activeEffects() = ReadActiveEffects(*data);
        player.setHealthDirect(data->GetValue<float>("Health", 20.0f));
        player.setOnGround(data->GetValue<int8_t>("OnGround", 1) != 0);
        // The WRITE side stores MC's string key ("minecraft:the_end"); this
        // used to read an int key nothing ever wrote, so a player who logged
        // out in the End always rejoined in the Overworld. The int fallback
        // stays for any file that predates the string.
        {
            const std::string dim = data->GetValue<std::string>("Dimension", "");
            if (const auto known = Game::DimensionFromRegistryName(dim)) {
                player.setDimensionId(Game::DimensionToRaw(*known));
            } else {
                player.setDimensionId(data->GetValue<int32_t>(
                    "playerDimensionId", player.getDimensionId()));
            }
        }

        // The respawn point: the modern `respawn` compound (see the writer),
        // with the pre-DataVersion-4548 SpawnX/SpawnY/SpawnZ/SpawnAngle/
        // SpawnDimension/SpawnForced keys as the fallback for a file a real
        // Minecraft wrote before the codec change.
        {
            auto dimensionFromName = [](const std::string& name, int fallback) {
                const auto known = Game::DimensionFromRegistryName(name);
                return known ? Game::DimensionToRaw(*known) : fallback;
            };
            std::optional<Server::ServerPlayer::RespawnConfig> config;
            if (auto respawn = std::dynamic_pointer_cast<::World::NBTTagCompound>(data->GetTag("respawn"))) {
                if (auto pos = std::dynamic_pointer_cast<::World::NBTTagIntArray>(respawn->GetTag("pos"));
                    pos && pos->value.size() == 3) {
                    Server::ServerPlayer::RespawnConfig c;
                    c.dimensionId = dimensionFromName(respawn->GetValue<std::string>("dimension", ""), 0);
                    c.pos    = glm::ivec3(pos->value[0], pos->value[1], pos->value[2]);
                    c.yaw    = respawn->GetValue<float>("yaw", 0.0f);
                    c.pitch  = respawn->GetValue<float>("pitch", 0.0f);
                    c.forced = respawn->GetValue<int8_t>("forced", 0) != 0;
                    config = c;
                }
            } else if (data->GetTag("SpawnX") && data->GetTag("SpawnY") && data->GetTag("SpawnZ")) {
                Server::ServerPlayer::RespawnConfig c;
                c.dimensionId = dimensionFromName(data->GetValue<std::string>("SpawnDimension", ""), 0);
                c.pos    = glm::ivec3(data->GetValue<int32_t>("SpawnX", 0),
                                      data->GetValue<int32_t>("SpawnY", 0),
                                      data->GetValue<int32_t>("SpawnZ", 0));
                c.yaw    = data->GetValue<float>("SpawnAngle", 0.0f);
                c.forced = data->GetValue<int8_t>("SpawnForced", 0) != 0;
                config = c;
            }
            player.setRespawnConfig(config);
        }

        // MC ServerPlayer.readAdditionalSaveData: the shoulder riders (absent
        // → empty shoulders).
        Server::ShoulderEntities::ReadPlayerData(*data, player);

        // The last hush gate crossed (see the writer). An unknown dimension
        // name drops the mark rather than guessing a dimension.
        if (auto gate = std::dynamic_pointer_cast<::World::NBTTagCompound>(data->GetTag("obey_hush_gate"))) {
            if (const auto dim = Game::DimensionFromRegistryName(
                    gate->GetValue<std::string>("dimension", ""))) {
                Server::ServerPlayer::HushGateMark mark;
                mark.dimensionId = Game::DimensionToRaw(*dim);
                mark.pos = glm::dvec3(gate->GetValue<double>("x", 0.0),
                                      gate->GetValue<double>("y", 0.0),
                                      gate->GetValue<double>("z", 0.0));
                mark.yaw = gate->GetValue<float>("yaw", 0.0f);
                player.setLastHushGate(mark);
            }
        } else {
            player.setLastHushGate(std::nullopt);
        }

        auto& food = player.getFoodData();
        food.setFoodLevel (data->GetValue<int32_t>("foodLevel", 20));
        food.setSaturation(data->GetValue<float>("foodSaturationLevel", 5.0f));

        auto& xp = player.getExperience();
        xp.SetLevel   (data->GetValue<int32_t>("XpLevel", 0));
        xp.SetProgress(data->GetValue<float>("XpP", 0.0f));
        xp.SetTotal   (data->GetValue<int32_t>("XpTotal", 0));
        player.setEnchantmentSeed(data->GetValue<int32_t>("XpSeed", 0));
        ReadEntityTags(*data, player.getTags());

        // MC ServerPlayer.readAdditionalSaveData: warden_spawn_tracker, a
        // fresh tracker when absent.
        if (auto tracker = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                data->GetTag("warden_spawn_tracker"))) {
            player.getWardenSpawnTracker() = Game::WardenSpawnTracker(
                tracker->GetValue<int32_t>("ticks_since_last_warning", 0),
                tracker->GetValue<int32_t>("warning_level", 0),
                tracker->GetValue<int32_t>("cooldown_ticks", 0));
        } else {
            player.getWardenSpawnTracker() = Game::WardenSpawnTracker();
        }

        auto& inventory = player.getInventory();
        inventory.Clear();
        if (auto items = std::dynamic_pointer_cast<::World::NBTTagList>(data->GetTag("Inventory"))) {
            for (const auto& element : items->value) {
                auto entry = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                if (!entry) continue;
                // Slot is written unsigned by vanilla, so -106 arrives as 150.
                const int raw8 = entry->GetValue<int8_t>("Slot", -1);
                const ItemStack stack = ReadItemStack(*entry);
                if (stack.IsEmpty()) continue;

                int engineSlot = -1;
                if (raw8 >= 0 && raw8 < Game::Inventory::HOTBAR_SIZE) {
                    engineSlot = Game::Inventory::HOTBAR_BEGIN + raw8;          // 0..8
                } else if (raw8 >= Game::Inventory::MAIN_BEGIN &&
                           raw8 <  Game::Inventory::MAIN_BEGIN + Game::Inventory::MAIN_SIZE) {
                    engineSlot = raw8;                                          // 9..35
                } else if (raw8 >= 100 && raw8 <= 103) {
                    for (int i = 0; i < Game::Inventory::ARMOR_SIZE; ++i) {
                        if (kVanillaArmourSlot[i] == raw8) engineSlot = Game::Inventory::ARMOR_BEGIN + i;
                    }
                } else if (raw8 == kVanillaOffhandSlot) {
                    engineSlot = Game::Inventory::OFFHAND_BEGIN;
                }
                if (engineSlot >= 0) inventory.SetSlotFull(engineSlot, stack);
            }
        }

        // `equipment` wins where both forms are present — it is the modern
        // home, and a world round-tripped through real Minecraft will only
        // have this one.
        if (auto equipment = std::dynamic_pointer_cast<::World::NBTTagCompound>(data->GetTag("equipment"))) {
            for (int i = 0; i < Game::Inventory::ARMOR_SIZE; ++i) {
                if (auto piece = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                        equipment->GetTag(kEquipmentKey[i]))) {
                    inventory.SetSlotFull(Game::Inventory::ARMOR_BEGIN + i, ReadItemStack(*piece));
                }
            }
            if (auto offhand = std::dynamic_pointer_cast<::World::NBTTagCompound>(
                    equipment->GetTag("offhand"))) {
                inventory.SetSlotFull(Game::Inventory::OFFHAND_BEGIN, ReadItemStack(*offhand));
            }
        }

        inventory.SetSelectedSlot(data->GetValue<int32_t>("SelectedItemSlot", 0));

        // MC PlayerList.placeNewPlayer's RootVehicle: restored once the
        // player stands in their level (Server::PlayerRiding::Tick).
        {
            auto rootVehicle = std::dynamic_pointer_cast<::World::NBTTagCompound>(data->GetTag("RootVehicle"));
            player.setPendingRootVehicle(rootVehicle && rootVehicle->HasTag("Entity") ? rootVehicle : nullptr);
        }

        Log::Info("[Anvil] restored player '%s' at (%.1f, %.1f, %.1f)",
                  player.getName().c_str(),
                  player.getPosition().x, player.getPosition().y, player.getPosition().z);
        return true;
    }

} // namespace Game::Anvil
