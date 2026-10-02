// File: src/server/advancements/ServerAdvancements.cpp
#include "ServerAdvancements.hpp"
#include "CriteriaTriggers.hpp"
#include "PlayerAdvancements.hpp"

#include "server/IntegratedServer.hpp"
#include "server/commands/SnbtParser.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/world/storage/anvil/ItemStackNbt.hpp"
#include "server/world/storage/anvil/PlayerUuid.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"

#include "common/advancements/AdvancementLoader.hpp"
#include "common/core/Log.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/network/PacketTypes.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <unordered_map>

namespace Server::Advancements {

    namespace {

        // ItemStackTemplate through the server's codecs: the icon JSON
        // ({id, count, components}) is SNBT, and the item-stack NBT reader
        // applies the components (an ominous banner's patterns, a decorated
        // pot's sherds).
        Game::ItemStack DecodeIcon(const nlohmann::json& icon) {
            nlohmann::json compound = icon.is_string() ? nlohmann::json{{"id", icon}} : icon;
            if (!compound.is_object()) return {};
            if (!compound.contains("count")) compound["count"] = 1;
            std::string error;
            auto nbt = Snbt::ParseCompound(compound.dump(), error);
            if (!nbt) {
                Log::Warning("[Advancements] icon %s: %s", icon.dump().c_str(), error.c_str());
                return {};
            }
            return Game::Anvil::ReadItemStack(*nbt, &error);
        }

        std::mutex g_mutex;
        std::shared_ptr<const Game::Advancements::Registry> g_registry;
        std::unordered_map<uint32_t, std::unique_ptr<PlayerAdvancements>> g_players;

        // <world>/advancements/<uuid>.json, or empty when the world cannot
        // hold one (no save path, or a root SaveRoot refuses).
        std::filesystem::path FileFor(const ServerPlayer& player) {
            if (!g_integratedServer) return {};
            const std::string& savePath = g_integratedServer->GetConfig().savePath;
            if (savePath.empty()) return {};
            std::string reason;
            auto root = Game::Anvil::SaveRoot::Open(savePath, reason);
            if (!root) return {};
            const std::string uuid = Game::Anvil::UuidToString(Game::Anvil::OfflinePlayerUuid(player.getName()));
            return root->Root() / "advancements" / (uuid + ".json");
        }

        // INVENTORY_CHANGED from the inventory's diff since the last tick
        // (MC's container listener fires per changed slot; the engine's
        // menus write the inventory directly, so the diff is the listener).
        void TickInventory(PlayerAdvancements& advancements, ServerPlayer& player) {
            TrackedState& tracked = advancements.Tracked();
            const Game::Inventory& inventory = player.getInventory();
            constexpr int kFirst = Game::Inventory::ARMOR_BEGIN;
            constexpr int kCount = Game::Inventory::TOTAL_SIZE - kFirst;
            if (!tracked.inventorySnapshotValid) {
                tracked.inventorySnapshot.assign(kCount, Game::ItemStack{});
                tracked.inventorySnapshotValid = true;
            }
            for (int i = 0; i < kCount; ++i) {
                const Game::ItemStack& now = inventory.GetSlot(kFirst + i);
                Game::ItemStack& before = tracked.inventorySnapshot[static_cast<size_t>(i)];
                if (Game::ItemStacksMatch(now, before)) continue;
                before = now;
                CriteriaTriggers::InventoryChanged(player, now);
            }
        }

    } // namespace

    std::shared_ptr<const Game::Advancements::Registry> GetRegistry() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_registry) g_registry = Game::Advancements::LoadRegistry(&DecodeIcon);
        return g_registry;
    }

    bool TriggersEnabled() {
        if (!g_integratedServer) return false;
        return !g_integratedServer->IsAllowCommands() || g_integratedServer->AdvancementsWithCheats();
    }

    PlayerAdvancements* Get(uint32_t playerId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_players.find(playerId);
        return it == g_players.end() ? nullptr : it->second.get();
    }

    PlayerAdvancements* Get(const ServerPlayer& player) { return Get(player.getPlayerId()); }

    void OnPlayerJoined(PlayerSession& session) {
        ServerPlayer* player = session.GetPlayer();
        if (!player) return;
        auto registry = GetRegistry();
        auto advancements = std::make_unique<PlayerAdvancements>(registry, FileFor(*player), player->getPlayerId());
        advancements->Load();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_players[player->getPlayerId()] = std::move(advancements);
    }

    void OnPlayerLeft(uint32_t playerId) {
        std::unique_ptr<PlayerAdvancements> gone;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_players.find(playerId);
            if (it == g_players.end()) return;
            gone = std::move(it->second);
            g_players.erase(it);
        }
        if (g_integratedServer && !g_integratedServer->GetConfig().readOnlyWorld) gone->Save();
    }

    void SavePlayer(const ServerPlayer& player) {
        if (!g_integratedServer || g_integratedServer->GetConfig().readOnlyWorld) return;
        if (PlayerAdvancements* advancements = Get(player)) advancements->Save();
    }

    void SaveAll() {
        if (!g_integratedServer || g_integratedServer->GetConfig().readOnlyWorld) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [id, advancements] : g_players) advancements->Save();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_players.clear();
    }

    void TickPlayer(PlayerSession& session, int64_t serverTick, bool paused) {
        (void)serverTick;
        ServerPlayer* player = session.GetPlayer();
        if (!player) return;
        PlayerAdvancements* advancements = Get(*player);
        if (!advancements) return;
        std::lock_guard<std::recursive_mutex> lock(advancements->Mutex());

        // Awarding switched off (cheats on, advancements_with_cheats off):
        // the inventory trigger forgets what it saw, so the tick awarding
        // comes back on — the rule turned on, or cheats turned off, mid
        // session — finds every held stack new, exactly as MC's first
        // container sync after login does. Without this, a crafting table
        // carried while switched off never earned the Minecraft root, and no
        // tab ever appeared.
        const bool enabled = TriggersEnabled();
        if (!enabled) advancements->Tracked().inventorySnapshotValid = false;

        if (!paused) {
            TrackedState& tracked = advancements->Tracked();
            ++tracked.tickCount;
            // The first unpaused tick with awarding on (the session's first,
            // or the first after it was switched on): the player is evaluated
            // as at login — every undone criterion listening, LOCATION now
            // rather than up to a second later; the inventory diff below
            // fires per held stack (its snapshot is empty).
            const bool justEnabled = enabled && !tracked.triggersWereEnabled;
            tracked.triggersWereEnabled = enabled;
            if (justEnabled) advancements->ReRegisterListeners();
            Game::LivingEntity* view = player->effectEntity();
            const glm::dvec3 position = player->getPosition();
            const bool alive = player->getHealth() > 0.0f;

            // ServerPlayer.doTick: TICK, then LEVITATION while levitating.
            CriteriaTriggers::Tick(*player);
            if (tracked.levitationStartPos) {
                CriteriaTriggers::Levitation(*player, *tracked.levitationStartPos,
                                             tracked.tickCount - tracked.levitationStartTime);
            }

            // trackStartFallingPosition / resetFallDistance: a fall starts
            // when the fall distance leaves 0 and ends when it returns to 0.
            if (player->getFallDistance() > 0.0f) {
                if (!tracked.startingToFallPosition) {
                    tracked.startingToFallPosition = position;
                    if (tracked.currentExplosionImpactPos &&
                        tracked.currentExplosionImpactPos->y <= tracked.startingToFallPosition->y) {
                        Game::Entity* cause = nullptr;
                        if (tracked.hasExplosionCause && view && view->Level()) {
                            cause = view->Level()->ResolveEntityById(tracked.currentExplosionCauseId);
                        }
                        CriteriaTriggers::FallAfterExplosion(*player, *tracked.currentExplosionImpactPos, cause);
                    }
                    tracked.currentExplosionImpactPos.reset();
                    tracked.hasExplosionCause = false;
                }
            } else if (tracked.startingToFallPosition) {
                if (alive) CriteriaTriggers::FallFromHeight(*player, *tracked.startingToFallPosition);
                tracked.startingToFallPosition.reset();
            }

            // trackEnteredOrExitedLavaOnVehicle.
            Game::Entity* vehicle = view ? view->GetVehicle() : nullptr;
            if (vehicle && vehicle->IsInLava()) {
                if (!tracked.enteredLavaOnVehiclePosition) tracked.enteredLavaOnVehiclePosition = position;
                else CriteriaTriggers::RideEntityInLava(*player, *tracked.enteredLavaOnVehiclePosition);
            }
            if (tracked.enteredLavaOnVehiclePosition && (!vehicle || !vehicle->IsInLava())) {
                tracked.enteredLavaOnVehiclePosition.reset();
            }

            // doTick's `tickCount % 20 == 0` LOCATION.
            if (justEnabled || tracked.tickCount % 20 == 0) CriteriaTriggers::Location(*player);

            if (enabled) TickInventory(*advancements, *player);
        }

        advancements->FlushDirty(*player, session.GetConnection(), true);
    }

    void HandleSeenAdvancements(PlayerSession& session, const Network::SeenAdvancementsC2SPacket& packet) {
        if (packet.action != Network::SeenAdvancementsC2SPacket::Action::OpenedTab) return;
        ServerPlayer* player = session.GetPlayer();
        if (!player) return;
        PlayerAdvancements* advancements = Get(*player);
        if (!advancements) return;
        const Game::Advancements::Definition* tab = advancements->GetRegistry().Get(packet.tab);
        if (tab) advancements->SetSelectedTab(tab, session.GetConnection());
    }

} // namespace Server::Advancements
