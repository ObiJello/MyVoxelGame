// File: src/server/world/storage/anvil/PlayerDataStore.cpp
#include "server/world/storage/anvil/PlayerDataStore.hpp"

#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/player/ServerPlayer.hpp"

#include "common/core/Log.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

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

    } // namespace

    // ── Write ───────────────────────────────────────────────────────────────

    bool WritePlayerData(const SaveRoot& root, const Server::ServerPlayer& player,
                         int dataVersion, std::string& error) {
        const auto uuid = OfflinePlayerUuid(player.getName());
        const auto& inventory = player.getInventory();

        Nbt::Writer w;
        w.BeginRootCompound();

        w.Int("DataVersion", dataVersion);

        {
            const glm::dvec3 p = player.getPosition();
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

        const auto& food = player.getFoodData();
        w.Int  ("foodLevel",           food.getFoodLevel());
        w.Float("foodSaturationLevel", food.getSaturationLevel());
        w.Float("foodExhaustionLevel", 0.0f);
        w.Int  ("foodTickTimer",       0);

        const auto& xp = player.getExperience();
        w.Int  ("XpLevel", xp.Level());
        w.Float("XpP",     xp.Progress());
        w.Int  ("XpTotal", xp.Total());
        w.Int  ("XpSeed",  0);
        w.Int  ("Score",   0);

        w.Int   ("playerGameType", static_cast<int>(player.getGameMode()));
        w.String("Dimension", player.getDimensionId() == -1 ? "minecraft:the_nether"
                            : player.getDimensionId() ==  1 ? "minecraft:the_end"
                                                            : "minecraft:overworld");
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

        w.EndRootCompound();
        if (!w.ok()) { error = "NBT writer refused the player data"; return false; }

        std::vector<uint8_t> gz;
        if (!Nbt::GzipCompress(w.Bytes(), gz)) { error = "gzip failed"; return false; }

        std::error_code ec;
        std::filesystem::create_directories(root.PlayerDataDir(), ec);

        const std::string file = UuidToString(uuid);
        const auto target = root.PlayerDataDir() / (file + ".dat");
        auto backup = root.PlayerDataDir() / (file + ".dat_old");
        if (!SafeReplace(target, backup, gz, error)) return false;

        Log::Info("[Anvil] saved player '%s' (%s, %zu bytes)",
                  player.getName().c_str(), file.c_str(), gz.size());
        return true;
    }

    // ── Read ────────────────────────────────────────────────────────────────

    bool ReadPlayerData(const SaveRoot& root, Server::ServerPlayer& player, std::string& error) {
        error.clear();
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

        player.setHealthDirect(data->GetValue<float>("Health", 20.0f));
        player.setOnGround(data->GetValue<int8_t>("OnGround", 1) != 0);
        // The WRITE side stores MC's string key ("minecraft:the_end"); this
        // used to read an int key nothing ever wrote, so a player who logged
        // out in the End always rejoined in the Overworld. The int fallback
        // stays for any file that predates the string.
        {
            const std::string dim = data->GetValue<std::string>("Dimension", "");
            if (dim == "minecraft:the_nether")      player.setDimensionId(-1);
            else if (dim == "minecraft:the_end")    player.setDimensionId(1);
            else if (dim == "minecraft:overworld")  player.setDimensionId(0);
            else {
                player.setDimensionId(data->GetValue<int32_t>(
                    "playerDimensionId", player.getDimensionId()));
            }
        }

        auto& food = player.getFoodData();
        food.setFoodLevel (data->GetValue<int32_t>("foodLevel", 20));
        food.setSaturation(data->GetValue<float>("foodSaturationLevel", 5.0f));

        auto& xp = player.getExperience();
        xp.SetLevel   (data->GetValue<int32_t>("XpLevel", 0));
        xp.SetProgress(data->GetValue<float>("XpP", 0.0f));
        xp.SetTotal   (data->GetValue<int32_t>("XpTotal", 0));

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

        Log::Info("[Anvil] restored player '%s' at (%.1f, %.1f, %.1f)",
                  player.getName().c_str(),
                  player.getPosition().x, player.getPosition().y, player.getPosition().z);
        return true;
    }

} // namespace Game::Anvil
