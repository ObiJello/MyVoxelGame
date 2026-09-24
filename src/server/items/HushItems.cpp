// File: src/server/items/HushItems.cpp
//
// Server half of the Hush's "tools of the deep": the common bridges declared
// in common/world/level/HushItems.hpp and the server's own hooks in
// server/items/HushItems.hpp. Design notes live with the declarations and in
// docs/the-hush.md.
//
// References: minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/item/BowItem.java
// (draw, power curve, release), Player.getProjectile (arrow search order),
// CompassItem / CompassAngleState (the needle, client-side in Item.cpp).

#include "server/items/HushItems.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/LocateFinder.hpp"
#include "server/level/PortalTravel.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/ResonanceArrow.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/HushSignalS2CPacket.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/HushItems.hpp"
#include "common/world/level/World.hpp"
#include "common/world/portal/PortalFamily.hpp"
#include "common/world/portal/PortalState.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Server::HushItems {

    namespace {

        using Signal = Network::HushSignalS2CPacket;

        // The module's clock: the last server tick TickPlayer saw. Cooldowns
        // are stamped against it, so they run on server ticks (a paused
        // integrated server pauses them too) and survive a dimension change,
        // which a per-level game time would not guarantee.
        std::atomic<int64_t> g_now{0};

        struct PlayerState {
            int64_t pingReady   = 0;
            int64_t recallReady = 0;
            // The chime's hold ran out this tick (RecallChimeFinish); the
            // teleport itself is TickPlayer's (RingRecall).
            bool    recallPending = false;
            // The echo compass.
            bool       searched = false;
            int        searchDimension = 0;
            glm::dvec3 searchOrigin{0.0};
            bool       found = false;
            glm::dvec2 target{0.0};
            // What the client was last told (so a change is sent once).
            bool       sent = false;
            bool       sentHasTarget = false;
            int        sentDimension = 0;
            glm::dvec2 sentTarget{0.0};
        };

        std::mutex& StateMutex() {
            static std::mutex m;
            return m;
        }
        std::unordered_map<uint32_t, PlayerState>& States() {
            static std::unordered_map<uint32_t, PlayerState> states;
            return states;
        }

        ServerPlayer* AsServerPlayer(Game::IUsePlayer& player) {
            return dynamic_cast<ServerPlayer*>(&player);
        }

        std::shared_ptr<PlayerSession> SessionOf(const ServerPlayer& player) {
            IntegratedServer* server = g_integratedServer.get();
            PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
            return sessions ? sessions->GetSession(player.getPlayerId()) : nullptr;
        }

        void Send(PlayerSession& session, const Signal& packet) {
            if (ServerConnection* conn = session.GetConnection()) {
                conn->SendPacket(static_cast<uint8_t>(Network::PacketId::HushSignalS2C),
                                 Network::Serialization::Serialize(packet));
            }
        }

        int SecondsLeft(int64_t readyAt) {
            return static_cast<int>((std::max<int64_t>(0, readyAt - g_now.load()) + 19) / 20);
        }

        // ── Tuning fork ──────────────────────────────────────────────────

        // Every `*_ore` block and ancient debris — the vanilla ores in both
        // their stone and deepslate forms, the nether's, and the Hush's own —
        // decided once from the registry slugs rather than listed by hand.
        bool IsOre(Game::BlockID id) {
            static const std::vector<uint8_t> table = [] {
                std::vector<uint8_t> t(Game::BlockRegistry::Size, 0);
                for (size_t i = 0; i < t.size(); ++i) {
                    const std::string& slug =
                        Game::BlockRegistry::Get(static_cast<Game::BlockID>(i)).registrySlug;
                    const bool ore = (slug.size() > 4 && slug.compare(slug.size() - 4, 4, "_ore") == 0) ||
                                     slug == "ancient_debris";
                    t[i] = ore ? 1 : 0;
                }
                return t;
            }();
            const size_t i = static_cast<size_t>(id);
            return i < table.size() && table[i] != 0;
        }

        constexpr size_t kMaxOreOutlines = 512;

        void CollectOres(Game::World& world, const glm::ivec3& c, std::vector<Signal::Target>& out) {
            struct Hit { glm::ivec3 p; int d2; bool echo; };
            std::vector<Hit> hits;
            const int r = Game::HushItems::kPingRadius;
            for (int dy = -r; dy <= r; ++dy) {
                const int y = c.y + dy;
                if (!world.IsValidPosition(c.x, y, c.z)) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    for (int dz = -r; dz <= r; ++dz) {
                        const int d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 > r * r) continue;
                        const Game::BlockID id = world.GetBlock(c.x + dx, y, c.z + dz);
                        if (!IsOre(id)) continue;
                        hits.push_back({ glm::ivec3(c.x + dx, y, c.z + dz), d2,
                                         id == Game::BlockID::EchoOre });
                    }
                }
            }
            // Nearest first when a vein-rich pocket would flood the packet.
            if (hits.size() > kMaxOreOutlines) {
                std::partial_sort(hits.begin(), hits.begin() + kMaxOreOutlines, hits.end(),
                                  [](const Hit& a, const Hit& b) { return a.d2 < b.d2; });
                hits.resize(kMaxOreOutlines);
            }
            for (const Hit& h : hits) {
                out.push_back({ glm::vec3(h.p), glm::vec3(h.p) + glm::vec3(1.0f),
                                h.echo ? Signal::TargetClass::EchoOre : Signal::TargetClass::Ore });
            }
        }

        void CollectMobs(ServerLevelBridge& level, const glm::dvec3& c, std::vector<Signal::Target>& out) {
            const double r = static_cast<double>(Game::HushItems::kPingRadius);
            std::vector<Game::Entity*> nearby;
            level.GetEntitiesInBox(Game::AABB::FromMinMax(glm::vec3(c - glm::dvec3(r)),
                                                          glm::vec3(c + glm::dvec3(r))),
                                   nullptr, nearby);
            for (Game::Entity* e : nearby) {
                if (!e || e->IsRemoved() || e->IsPlayer() || !e->IsAlive()) continue;
                if (!dynamic_cast<Game::LivingEntity*>(e)) continue;
                const glm::dvec3 d = e->position - c;
                if (glm::dot(d, d) > r * r) continue;
                const float hw = e->GetBbWidth() * 0.5f;
                const glm::vec3 feet(e->position);
                out.push_back({ feet - glm::vec3(hw, 0.0f, hw),
                                feet + glm::vec3(hw, e->GetBbHeight(), hw),
                                Signal::TargetClass::Mob });
            }
        }

        // The nearest Echo Vault or Warden's Tomb, marked where /locate would
        // send you: the structure's locate point on the surface. Both are
        // buried, and the point is where the dig (the vault's sinkhole, the
        // tomb's ladder shaft) starts — the "entrance" a player needs.
        void CollectStructure(ServerLevel& level, const glm::ivec3& c, std::vector<Signal::Target>& out) {
            if (level.Dimension() != Game::DimensionId::Hush) return;
            const auto found = FindNearestStructure(
                level, { "minecraft:echo_vaults", "minecraft:wardens_tomb" }, c, 1);
            if (!found) return;
            const double dx = found->pos.x + 0.5 - (c.x + 0.5);
            const double dz = found->pos.z + 0.5 - (c.z + 0.5);
            const double reach = static_cast<double>(Game::HushItems::kPingStructureRadius);
            if (dx * dx + dz * dz > reach * reach) return;
            const int y = LocateSurfaceY(level, found->pos.x, found->pos.z, c.y);
            out.push_back({ glm::vec3(found->pos.x - 2, y - 6, found->pos.z - 2),
                            glm::vec3(found->pos.x + 3, y + 3, found->pos.z + 3),
                            Signal::TargetClass::Structure });
        }

        // ── Recall chime ─────────────────────────────────────────────────

        bool IsPortalOrSolid(Game::World& world, const glm::ivec3& p) {
            const Game::BlockID id = world.GetBlock(p.x, p.y, p.z);
            return Game::FamilyOfPortalBlock(id) != nullptr ||
                   Game::BlockRegistry::HasCollision(id);
        }

        // A gate landing is INSIDE the portal's opening (MC's exit rule). "In
        // front of the gate" is the first spot along the facing the player
        // arrived with, up to two blocks out, whose feet and head cells hold
        // neither portal nor anything solid; failing that, the landing itself
        // (the arrival's portal cooldown keeps the gate from firing at once).
        glm::dvec3 InFrontOfGate(Game::World& world, const glm::dvec3& landing, float yaw) {
            const glm::vec3 look = Game::Mth::ViewVector(0.0f, yaw);
            for (double step = 1.0; step <= 2.01; step += 0.5) {
                const glm::dvec3 p = landing + glm::dvec3(look.x, 0.0, look.z) * step;
                const glm::ivec3 feet(static_cast<int>(std::floor(p.x)),
                                      static_cast<int>(std::floor(p.y)),
                                      static_cast<int>(std::floor(p.z)));
                if (!IsPortalOrSolid(world, feet) &&
                    !IsPortalOrSolid(world, feet + glm::ivec3(0, 1, 0))) {
                    return glm::dvec3(feet.x + 0.5, p.y, feet.z + 0.5);
                }
            }
            return landing;
        }

        // The recall itself, once the hold has run its course. Run from
        // TickPlayer rather than from inside the finish: the finish fires in
        // the middle of ServerPlayer::tick (completeUsingItem), which goes on
        // to integrate the player against the level it was handed at the
        // start, and a dimension change under it would move the player
        // through the wrong world for the rest of that tick. TickPlayer runs
        // right after it in the same PlayerSession::Tick, so the recall
        // still lands on the tick the note ends.
        void RingRecall(PlayerSession& session, ServerPlayer& player) {
            IntegratedServer* server = g_integratedServer.get();
            const auto gate = player.getLastHushGate();
            if (!server || !gate) return;
            {
                std::lock_guard<std::mutex> lock(StateMutex());
                if (g_now.load() < States()[player.getPlayerId()].recallReady) return;
            }

            const Game::DimensionId toDim   = Game::DimensionFromRaw(gate->dimensionId);
            const Game::DimensionId fromDim = Game::DimensionFromRaw(player.getDimensionId());
            ServerLevel* to   = server->GetOrCreateLevel(toDim);
            ServerLevel* from = server->GetLevel(fromDim);
            PlayerEntityView* view = (from && from->MobLevel())
                ? from->MobLevel()->GetPlayerView(session.GetConnectionId()) : nullptr;
            if (!to || !to->World() || !from || !view) {
                player.DisplayClientMessage("The chime's note finds no way back to the gate", true);
                Log::Warning("[Hush] %s rang the recall chime but '%s' could not be reached",
                             player.getName().c_str(), std::string(Game::DimensionName(toDim)).c_str());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(StateMutex());
                States()[player.getPlayerId()].recallReady =
                    g_now.load() + Game::HushItems::kRecallCooldownTicks;
            }

            const glm::ivec3 around(static_cast<int>(std::floor(gate->pos.x)),
                                    static_cast<int>(std::floor(gate->pos.y)),
                                    static_cast<int>(std::floor(gate->pos.z)));
            PortalTravel::EnsureExitAreaLoaded(*to, around);
            const glm::dvec3 arrival = InFrontOfGate(*to->World(), gate->pos, gate->yaw);

            // The departure toll, where the player stood.
            if (from->World()) {
                from->World()->PlaySound(nullptr, player.getPosition(), Game::SoundEvents::BELL_BLOCK,
                                         Game::SoundSource::Players, 1.0f, 0.7f);
            }
            player.setRotation(gate->yaw, player.getPitch());
            // The arrival stands at a gate: its portal must not fire
            // underfoot. The view's own state is the one the portal tick
            // reads (a cross-dimension move carries it over, Execute).
            view->SetPortalCooldown();
            if (toDim == fromDim) {
                // Same level: the server's copy moves now (so the chunk
                // tracking re-centres this tick), the client by packet.
                player.teleport(arrival);
                session.ResyncChunkPosition();
                if (ServerConnection* conn = session.GetConnection()) {
                    conn->Teleport(arrival.x, arrival.y, arrival.z, gate->yaw, player.getPitch());
                }
            } else {
                // MC TeleportTransition through the portal machinery's own
                // arrival: tickets, the dimension flip and the position packet.
                PortalTravel::ArriveAt(*server, *from, *to, *view, arrival);
            }
            // The arrival's answer, at the gate.
            to->World()->PlaySound(nullptr, arrival, Game::SoundEvents::AMETHYST_BLOCK_RESONATE,
                                   Game::SoundSource::Players, 1.5f, 0.8f);
            Log::Info("[Hush] %s rang the recall chime -> '%s' (%.1f, %.1f, %.1f)",
                      player.getName().c_str(), std::string(Game::DimensionName(toDim)).c_str(),
                      arrival.x, arrival.y, arrival.z);
        }

        // ── Bow ──────────────────────────────────────────────────────────

        bool IsArrowItem(Game::ItemID id) {
            return id == Game::Items::Arrow || id == Game::Items::SpectralArrow ||
                   id == Game::Items::TippedArrow;
        }

        // MC Player.getProjectile for a bow: the offhand, then the main hand,
        // then the inventory in order (hotbar first — MC's slot order puts the
        // hotbar at 0..8). -1 when there is none.
        int FindArrowSlot(const ServerPlayer& player) {
            const Game::Inventory& inv = player.getInventory();
            if (IsArrowItem(inv.GetSlot(Game::Inventory::OFFHAND_BEGIN).itemId)) {
                return Game::Inventory::OFFHAND_BEGIN;
            }
            for (int h = 0; h < 9; ++h) {
                const int i = Game::Inventory::HotbarToIndex(h);
                if (IsArrowItem(inv.GetSlot(i).itemId)) return i;
            }
            for (int i = 9; i < 36; ++i) {
                if (IsArrowItem(inv.GetSlot(i).itemId)) return i;
            }
            return -1;
        }

        // MC BowItem.getPowerForTime.
        float PowerForTime(int timeHeld) {
            float pow = static_cast<float>(timeHeld) / 20.0f;
            pow = (pow * pow + pow * 2.0f) / 3.0f;
            return std::min(pow, 1.0f);
        }

        // ── Echo compass ─────────────────────────────────────────────────

        bool CarriesEchoCompass(const ServerPlayer& player) {
            const Game::Inventory& inv = player.getInventory();
            for (int i = 0; i < Game::Inventory::TOTAL_SIZE; ++i) {
                if (inv.GetSlot(i).itemId == Game::Items::EchoCompass) return true;
            }
            return false;
        }

    } // namespace

    // ── Server hooks ─────────────────────────────────────────────────────

    void TickPlayer(PlayerSession& session, int64_t serverTick) {
        g_now.store(serverTick);
        ServerPlayer* player = session.GetPlayer();
        if (!player) return;

        // A recall chime whose hold ended in this tick's ServerPlayer::tick.
        bool recall = false;
        {
            std::lock_guard<std::mutex> lock(StateMutex());
            auto it = States().find(player->getPlayerId());
            if (it != States().end() && it->second.recallPending) {
                it->second.recallPending = false;
                recall = true;
            }
        }
        if (recall) RingRecall(session, *player);

        // Staggered by player so a full server does not search together.
        if ((serverTick + player->getPlayerId() * 7) % kCompassCheckTicks != 0) return;

        const int dimRaw = player->getDimensionId();
        const Game::DimensionId dim = Game::DimensionFromRaw(dimRaw);
        const bool carries = CarriesEchoCompass(*player);

        bool hasTarget = false;
        glm::dvec2 target(0.0);
        {
            std::lock_guard<std::mutex> lock(StateMutex());
            PlayerState& st = States()[player->getPlayerId()];
            if (carries && dim == Game::DimensionId::Hush) {
                const glm::dvec3 pos = player->getPosition();
                const glm::dvec2 moved(pos.x - st.searchOrigin.x, pos.z - st.searchOrigin.z);
                const double research = static_cast<double>(kCompassResearch);
                if (!st.searched || st.searchDimension != dimRaw ||
                    glm::dot(moved, moved) > research * research) {
                    st.searched = true;
                    st.searchDimension = dimRaw;
                    st.searchOrigin = pos;
                    st.found = false;
                    IntegratedServer* server = g_integratedServer.get();
                    if (ServerLevel* level = server ? server->GetLevel(dim) : nullptr) {
                        const glm::ivec3 from(static_cast<int>(std::floor(pos.x)),
                                              static_cast<int>(std::floor(pos.y)),
                                              static_cast<int>(std::floor(pos.z)));
                        if (const auto vault = FindNearestStructure(
                                *level, { "minecraft:echo_vaults" }, from, kCompassSearchRings)) {
                            st.found = true;
                            st.target = glm::dvec2(vault->pos.x + 0.5, vault->pos.z + 0.5);
                        }
                    }
                }
                hasTarget = st.found;
                target = st.target;
            }
            // Tell the client only what changed. A player who carries no
            // compass is still told once when a target goes away, so a
            // compass picked up later does not show a stale bearing.
            const bool changed = !st.sent || st.sentHasTarget != hasTarget ||
                                 st.sentDimension != dimRaw ||
                                 (hasTarget && st.sentTarget != target);
            if (!changed || (!carries && !st.sent)) return;
            st.sent = true;
            st.sentHasTarget = hasTarget;
            st.sentDimension = dimRaw;
            st.sentTarget = target;
        }
        Signal packet;
        packet.kind = Signal::Kind::CompassTarget;
        packet.dimension = static_cast<int8_t>(dimRaw);
        packet.hasTarget = hasTarget;
        packet.origin = glm::dvec3(target.x, 0.0, target.y);
        Send(session, packet);
    }

    void RecordGateCrossing(ServerPlayer& player, Game::DimensionId dimension,
                            const glm::dvec3& landing, float yaw) {
        ServerPlayer::HushGateMark mark;
        mark.dimensionId = Game::DimensionToRaw(dimension);
        mark.pos = landing;
        mark.yaw = yaw;
        player.setLastHushGate(mark);
    }

    void ForgetPlayer(uint32_t playerId) {
        std::lock_guard<std::mutex> lock(StateMutex());
        States().erase(playerId);
    }

} // namespace Server::HushItems

// ── Common bridges (common/world/level/HushItems.hpp) ────────────────────────

namespace Game::HushItems {

    using Server::HushItems::g_now;

    UseResult TuningForkStrike(IUsePlayer& user, const glm::ivec3& crystal) {
        Server::ServerPlayer* player = Server::HushItems::AsServerPlayer(user);
        Server::IntegratedServer* server = Server::g_integratedServer.get();
        if (!player || !server) return UseResult::Pass;
        auto session = Server::HushItems::SessionOf(*player);
        Server::ServerLevel* level = server->GetLevel(DimensionFromRaw(player->getDimensionId()));
        if (!session || !level || !level->World() || !level->MobLevel()) return UseResult::Pass;

        {
            std::lock_guard<std::mutex> lock(Server::HushItems::StateMutex());
            auto& st = Server::HushItems::States()[player->getPlayerId()];
            if (g_now.load() < st.pingReady) {
                player->DisplayClientMessage(
                    "The fork is still ringing (" +
                    std::to_string(Server::HushItems::SecondsLeft(st.pingReady)) + "s)", true);
                return UseResult::Fail;
            }
            st.pingReady = g_now.load() + kPingCooldownTicks;
        }

        Network::HushSignalS2CPacket packet;
        packet.kind = Network::HushSignalS2CPacket::Kind::ResonancePing;
        packet.dimension = static_cast<int8_t>(player->getDimensionId());
        packet.origin = glm::dvec3(crystal.x + 0.5, crystal.y, crystal.z + 0.5);
        packet.durationTicks = static_cast<uint32_t>(kPingTicks);
        Server::HushItems::CollectOres(*level->World(), crystal, packet.targets);
        Server::HushItems::CollectMobs(*level->MobLevel(), packet.origin + glm::dvec3(0.0, 0.5, 0.0),
                                       packet.targets);
        Server::HushItems::CollectStructure(*level, crystal, packet.targets);
        Server::HushItems::Send(*session, packet);

        // The fork's strike: the crystal rings low and long. Server-only (the
        // client's predictor has no ServerPlayer), so everyone hears it from
        // here, the striker included.
        level->World()->PlaySound(nullptr, packet.origin, SoundEvents::AMETHYST_BLOCK_RESONATE,
                                  SoundSource::Players, 2.0f, 0.5f);
        Log::Info("[Hush] %s struck the tuning fork at (%d, %d, %d): %zu echoes",
                  player->getName().c_str(), crystal.x, crystal.y, crystal.z,
                  packet.targets.size());
        return UseResult::Success;
    }

    UseResult RecallChimeBegin(IUsePlayer& user, uint32_t hand) {
        Server::ServerPlayer* player = Server::HushItems::AsServerPlayer(user);
        if (!player) return UseResult::Pass;
        if (!player->getLastHushGate()) {
            player->DisplayClientMessage("The chime has no gate to remember: step through a hush portal first", true);
            return UseResult::Fail;
        }
        {
            std::lock_guard<std::mutex> lock(Server::HushItems::StateMutex());
            const auto& st = Server::HushItems::States()[player->getPlayerId()];
            if (g_now.load() < st.recallReady) {
                player->DisplayClientMessage(
                    "The chime is still ringing (" +
                    std::to_string(Server::HushItems::SecondsLeft(st.recallReady)) + "s)", true);
                return UseResult::Fail;
            }
        }
        player->startUsingItem(hand);
        // The chime starts to sing while it is held.
        if (Server::IntegratedServer* server = Server::g_integratedServer.get()) {
            if (Server::ServerLevel* level = server->GetLevel(DimensionFromRaw(player->getDimensionId()))) {
                if (level->World()) {
                    level->World()->PlaySound(nullptr, player->getPosition(), SoundEvents::BELL_RESONATE,
                                              SoundSource::Players, 0.6f, 1.6f);
                }
            }
        }
        return UseResult::Consume;
    }

    void RecallChimeFinish(IUsePlayer& user, ItemStack& stack) {
        (void)stack;   // the chime is not used up
        Server::ServerPlayer* player = Server::HushItems::AsServerPlayer(user);
        if (!player) return;
        // The note has run out: the recall goes on this same tick, from
        // TickPlayer (RingRecall says why not from here).
        std::lock_guard<std::mutex> lock(Server::HushItems::StateMutex());
        Server::HushItems::States()[player->getPlayerId()].recallPending = true;
    }

    // Let go before the note rang out (RELEASE_USE_ITEM): nothing happens,
    // no cooldown is spent, and the player is told why rather than left to
    // guess.
    void RecallChimeRelease(IUsePlayer& user, ItemStack& stack, int remaining) {
        (void)stack;
        Server::ServerPlayer* player = Server::HushItems::AsServerPlayer(user);
        if (!player || remaining <= 0) return;
        player->DisplayClientMessage("The note broke off: hold the chime until it rings out", true);
    }

    UseResult BowBegin(IUsePlayer& user, uint32_t hand) {
        Server::ServerPlayer* player = Server::HushItems::AsServerPlayer(user);
        if (!player) return UseResult::Pass;
        // BowItem.use: no arrow and not creative → FAIL (no draw).
        if (!player->isCreative() && Server::HushItems::FindArrowSlot(*player) < 0) {
            return UseResult::Fail;
        }
        player->startUsingItem(hand);
        return UseResult::Consume;
    }

    void BowRelease(IUsePlayer& user, ItemStack& bow, int remaining) {
        Server::ServerPlayer* player = Server::HushItems::AsServerPlayer(user);
        Server::IntegratedServer* server = Server::g_integratedServer.get();
        if (!player || !server) return;

        // BowItem.releaseUsing: timeHeld = getUseDuration - remaining; below
        // power 0.1 nothing is fired (and nothing spent).
        const int timeHeld = GetUseDuration(bow) - remaining;
        const float power = Server::HushItems::PowerForTime(timeHeld);
        if (power < 0.1f) return;

        const int arrowSlot = Server::HushItems::FindArrowSlot(*player);
        if (arrowSlot < 0 && !player->isCreative()) return;

        auto session = Server::HushItems::SessionOf(*player);
        Server::ServerLevel* level = server->GetLevel(DimensionFromRaw(player->getDimensionId()));
        if (!session || !level || !level->MobLevel()) return;
        Server::PlayerEntityView* view = level->MobLevel()->GetPlayerView(session->GetConnectionId());
        if (!view) return;

        const bool resonance = bow.itemId == Items::ResonanceBow;
        std::unique_ptr<Arrow> arrow = resonance
            ? std::make_unique<ResonanceArrow>(level->MobLevel())
            : std::make_unique<Arrow>(level->MobLevel());
        arrow->SetOwner(view);
        // ArrowItem.createArrow → new Arrow(level, owner, ammo.copyWithCount(1),
        // weapon): a tipped arrow's POTION_CONTENTS (and its 0.125 duration
        // scale) become the shot's payload.
        if (arrowSlot >= 0) {
            const ItemStack& ammoStack = player->getInventory().GetSlot(arrowSlot);
            if (ammoStack.itemId == Items::TippedArrow) arrow->SetPotionFromPickupStack(ammoStack);
        }
        const glm::dvec3 pos = player->getPosition();
        arrow->position = glm::dvec3(pos.x, pos.y + player->getEyeHeight() - 0.1, pos.z);
        // shootFromRotation(player, xRot, yRot, 0, power * 3, 1).
        arrow->ShootFromRotation(*view, player->getPitch(), player->getYaw(), 0.0f,
                                 power * 3.0f, 1.0f);
        level->MobLevel()->AddFreshEntity(std::move(arrow));
        // MC BowItem.releaseUsing:46 — playSound(null, player, ARROW_SHOOT,
        // PLAYERS, 1.0, 1 / (nextFloat * 0.4 + 1.2) + power * 0.5).
        {
            JavaRandom& r = level->MobLevel()->Random();
            level->MobLevel()->PlaySound(nullptr, pos, SoundEvents::ARROW_SHOOT, SoundSource::Players, 1.0f,
                                         1.0f / (r.NextFloat() * 0.4f + 1.2f) + power * 0.5f);
        }

        // useAmmo: one arrow, unless creative (hasInfiniteMaterials).
        if (!player->isCreative() && arrowSlot >= 0) {
            ItemStack& ammo = player->getInventory().MutableSlot(arrowSlot);
            ammo.count -= 1;
            if (ammo.count <= 0) ammo.Clear();
            player->markSlotDirty(arrowSlot);
        }
    }

    void BroadcastSonicBurst(DimensionId dimension, const glm::dvec3& at, float radius) {
        Server::IntegratedServer* server = Server::g_integratedServer.get();
        Server::PlayerSessionManager* sessions = server ? server->GetSessionManager() : nullptr;
        if (!sessions) return;
        Network::HushSignalS2CPacket packet;
        packet.kind = Network::HushSignalS2CPacket::Kind::SonicBurst;
        packet.dimension = static_cast<int8_t>(DimensionToRaw(dimension));
        packet.origin = at;
        packet.radius = radius;
        // The client's own particle limiter drops anything past 32 blocks;
        // there is no point sending further.
        constexpr double kReach = 48.0;
        for (const auto& session : sessions->GetAllSessions()) {
            if (!session || session->GetDimensionId() != DimensionToRaw(dimension)) continue;
            const Server::ServerPlayer* p = session->GetPlayer();
            if (!p) continue;
            const glm::dvec3 d = p->getPosition() - at;
            if (glm::dot(d, d) > kReach * kReach) continue;
            Server::HushItems::Send(*session, packet);
        }
    }

} // namespace Game::HushItems
