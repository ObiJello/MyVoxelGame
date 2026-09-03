// File: src/server/level/EndDragonFight.cpp
#include "server/level/EndDragonFight.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/world/level/World.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/ticketing/ChunkTicketManager.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Server {

    namespace {

        // The fight's origin — MC BlockPos.ZERO for the End dimension.
        constexpr glm::ivec3 kOrigin{0, 0, 0};

        // The chunk-ticket footprint that keeps the arena simulating while
        // players fight — MC's one DRAGON ticket with radius 9 at (0,0)
        // (EndDragonFight.tick's addTicketWithRadius), expressed as this
        // engine's per-chunk forced tickets.
        constexpr int kArenaTicketRadius = 9;
        constexpr const char* kArenaTicketId = "dragon_fight";

        // std::numbers is overkill for two call sites; M_PI is not portable
        // to MSVC without _USE_MATH_DEFINES (see CLAUDE.md's MSVC notes).
        constexpr double kPi = 3.14159265358979323846;

        std::filesystem::path FightDataPath(const std::string& worldRoot) {
            return std::filesystem::path(worldRoot) / "data" / "dragon_fight.json";
        }

    } // namespace

    EndDragonFight::EndDragonFight(ServerLevel& level, PlayerSessionManager* sessions)
        : m_level(level), m_sessions(sessions) {
        LoadData();
        if (m_gateways.empty() && !m_previouslyKilled && m_needsStateScanning) {
            // MC: a fresh fight shuffles the 20 gateway slots by the seed, so
            // each world spends them in its own fixed order.
            for (int i = 0; i < kGatewayCount; ++i) m_gateways.push_back(i);
            Game::JavaRandom rng(m_level.Config().seed);
            for (int i = static_cast<int>(m_gateways.size()); i > 1; --i) {
                std::swap(m_gateways[i - 1], m_gateways[rng.NextInt(i)]);
            }
        }
        // MC: `if (dragonFightData.isRespawning) respawnStage = START` — and
        // the first tick with a loaded arena sees the empty crystal list and
        // calls tryRespawn(), which re-finds the four ritual crystals and
        // restarts the animation. m_loadedIsRespawning carries that pending
        // resume; Tick consumes it once the arena is loaded.
    }

    EndDragonFight::~EndDragonFight() = default;

    // ── Persistence ────────────────────────────────────────────────────────

    void EndDragonFight::LoadData() {
        const std::string& root = m_level.Config().worldPath;
        if (root.empty()) return;
        const auto path = FightDataPath(root);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;
            m_needsStateScanning    = j.value("needsStateScanning", true);
            m_dragonKilled          = j.value("dragonKilled", false);
            m_previouslyKilled      = j.value("previouslyKilled", false);
            m_pillarCrystalsSpawned = j.value("pillarCrystalsSpawned", false);
            m_loadedIsRespawning    = j.value("isRespawning", false);
            if (j.contains("exitPortalLocation") && j["exitPortalLocation"].is_array() &&
                j["exitPortalLocation"].size() == 3) {
                m_portalLocation = glm::ivec3(j["exitPortalLocation"][0].get<int>(),
                                              j["exitPortalLocation"][1].get<int>(),
                                              j["exitPortalLocation"][2].get<int>());
                m_hasPortalLocation = true;
            }
            if (j.contains("gateways") && j["gateways"].is_array()) {
                m_gateways.clear();
                for (const auto& g : j["gateways"]) m_gateways.push_back(g.get<int>());
            }
            // MC Data.dragonUUID, stored as vanilla's four-int codec
            // (UUIDUtil.CODEC) so the identity survives the session the same
            // way every saved entity reference does.
            if (j.contains("dragonUUID") && j["dragonUUID"].is_array() &&
                j["dragonUUID"].size() == 4) {
                int32_t ints[4];
                for (int i = 0; i < 4; ++i) ints[i] = j["dragonUUID"][i].get<int32_t>();
                m_dragonUuid = Game::UuidFromIntArray(ints);
            }
        } catch (const std::exception& e) {
            Log::Warning("[DragonFight] could not read %s: %s",
                         path.string().c_str(), e.what());
        }
    }

    void EndDragonFight::Save() {
        const std::string& root = m_level.Config().worldPath;
        if (root.empty() || m_level.Config().readOnly) return;
        const auto path = FightDataPath(root);
        try {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            nlohmann::json j;
            j["needsStateScanning"]    = m_needsStateScanning;
            j["dragonKilled"]          = m_dragonKilled;
            j["previouslyKilled"]      = m_previouslyKilled;
            j["pillarCrystalsSpawned"] = m_pillarCrystalsSpawned;
            // A loaded-but-not-yet-resumed ritual (m_loadedIsRespawning) is
            // still a ritual in progress — an autosave before the arena
            // streams in must not erase it.
            j["isRespawning"]          = m_respawning || m_loadedIsRespawning;
            if (m_hasPortalLocation) {
                j["exitPortalLocation"] = { m_portalLocation.x, m_portalLocation.y,
                                            m_portalLocation.z };
            }
            j["gateways"] = m_gateways;
            if (!Game::UuidIsNil(m_dragonUuid)) {
                int32_t ints[4];
                Game::UuidToIntArray(m_dragonUuid, ints);
                j["dragonUUID"] = { ints[0], ints[1], ints[2], ints[3] };
            }
            std::ofstream f(path);
            f << j.dump(2) << "\n";
        } catch (const std::exception& e) {
            Log::Warning("[DragonFight] could not write %s: %s",
                         path.string().c_str(), e.what());
        }
    }

    // ── Spike geometry (must reproduce the terrain library's) ──────────────

    const std::vector<EndDragonFight::EndSpike>& EndDragonFight::Spikes() const {
        if (!m_spikes.empty()) return m_spikes;
        // MC SpikeFeature.getSpikesForLevel + SpikeCacheLoader.load, and
        // EXACTLY the vendored library's EndSpikeFeature::getSpikesForLevel —
        // the obsidian towers in the ground came from that stream, so any
        // divergence here would put crystals beside their pillars.
        Game::JavaRandom seedRandom(m_level.Config().seed);
        const int64_t key = seedRandom.NextLong() & 65535LL;

        std::vector<int> sizes;
        for (int i = 0; i < 10; ++i) sizes.push_back(i);
        Game::JavaRandom shuffleRandom(key);
        for (int i = static_cast<int>(sizes.size()); i > 1; --i) {
            std::swap(sizes[i - 1], sizes[shuffleRandom.NextInt(i)]);
        }

        for (int i = 0; i < 10; ++i) {
            const int x = static_cast<int>(std::floor(
                42.0 * std::cos(2.0 * (-kPi + (kPi / 10.0) * i))));
            const int z = static_cast<int>(std::floor(
                42.0 * std::sin(2.0 * (-kPi + (kPi / 10.0) * i))));
            const int size = sizes[static_cast<size_t>(i)];
            EndSpike spike;
            spike.centerX = x;
            spike.centerZ = z;
            spike.radius  = 2 + size / 3;
            spike.height  = 76 + size * 3;
            spike.guarded = (size == 1 || size == 2);
            m_spikes.push_back(spike);
        }
        return m_spikes;
    }

    // ── Small helpers ──────────────────────────────────────────────────────

    Game::EnderDragon* EndDragonFight::ResolveDragon() const {
        // MC level.getEntity(dragonUUID) — identity, not the session-local id.
        if (Game::UuidIsNil(m_dragonUuid)) return nullptr;
        MobManager* mobs = m_level.Mobs();
        if (!mobs) return nullptr;
        return dynamic_cast<Game::EnderDragon*>(mobs->FindByUuid(m_dragonUuid));
    }

    Game::EndCrystal* EndDragonFight::ResolveCrystal(int32_t id) const {
        MobManager* mobs = m_level.Mobs();
        if (!mobs) return nullptr;
        auto it = mobs->All().find(id);
        if (it == mobs->All().end()) return nullptr;
        return dynamic_cast<Game::EndCrystal*>(it->second.get());
    }

    int EndDragonFight::SurfaceY(int x, int z) const {
        // MC getHeightmapPos(MOTION_BLOCKING, ...) stand-in — the End has no
        // leaves or fluids, so "has collision" is the whole predicate.
        Game::World* world = m_level.World();
        if (!world) return 64;
        const int maxY = Game::DimensionMinY(Game::DimensionId::End) +
                         Game::DimensionLogicalHeight(Game::DimensionId::End) - 1;
        for (int y = maxY; y >= 0; --y) {
            if (Game::BlockRegistry::HasCollision(world->GetBlock(x, y, z))) {
                return y + 1;
            }
        }
        return 0;
    }

    // ── The tick (MC EndDragonFight.tick) ──────────────────────────────────

    void EndDragonFight::Tick() {
        // MC EndDragonFight.tick's first line: the bar's visibility is
        // re-asserted from dragonKilled EVERY tick. This is what makes the
        // bar self-correcting — a fight that loaded with stale or missing
        // state (and briefly showed, or withheld, the bar) converges the
        // moment ScanState fixes dragonKilled.
        SetBarVisible(!m_dragonKilled);

        if (++m_ticksSinceLastPlayerScan >= kTimeBetweenPlayerScans) {
            UpdatePlayers();
            m_ticksSinceLastPlayerScan = 0;
        }

        if (m_barPlayers.empty()) {
            EnsureArenaTickets(false);
            return;
        }
        EnsureArenaTickets(true);

        const bool arenaLoaded = IsArenaLoaded();
        if (!arenaLoaded && m_ticksSinceLastPlayerScan == 0) {
            // Re-request missing arena chunks at the player-scan cadence.
            // RequestChunkLoad dedupes in-flight requests per level, so this
            // is cheap even while generation is still catching up.
            RequestArenaChunks();
        }
        if (m_needsStateScanning && arenaLoaded) {
            ScanState();
            m_needsStateScanning = false;
        }

        // MC: a fight loaded mid-ritual holds respawnStage START with a null
        // crystal list; the first loaded tick nulls the stage and calls
        // tryRespawn(), which re-finds the four ritual crystals and restarts
        // the animation from the beginning.
        if (m_loadedIsRespawning && arenaLoaded) {
            m_loadedIsRespawning = false;
            TryRespawn();
        }

        if (m_respawning) {
            TickRespawn();
        }

        if (!m_dragonKilled) {
            if ((Game::UuidIsNil(m_dragonUuid) ||
                 ++m_ticksSinceDragonSeen >= kMaxTicksBeforeDragonRespawn) &&
                arenaLoaded) {
                FindOrCreateDragon();
                m_ticksSinceDragonSeen = 0;
            }
            if (++m_ticksSinceCrystalsScanned >= kTimeBetweenCrystalScans &&
                arenaLoaded) {
                UpdateCrystalCount();
                m_ticksSinceCrystalsScanned = 0;
            }
            BroadcastBossProgress();
        }
    }

    void EndDragonFight::EnsureArenaTickets(bool wanted) {
        if (wanted == m_arenaTicketed) return;
        ChunkTicketManager* tickets = m_level.Tickets();
        if (!tickets) return;
        for (int x = -kArenaTicketRadius; x <= kArenaTicketRadius; ++x) {
            for (int z = -kArenaTicketRadius; z <= kArenaTicketRadius; ++z) {
                const Game::Math::ChunkPos chunk{x, z};
                if (wanted) {
                    tickets->AddForcedTicket(kArenaTicketId, chunk,
                                             ChunkTicketManager::ENTITY_TICKING_LEVEL);
                } else {
                    tickets->RemoveForcedTicket(kArenaTicketId, chunk);
                }
            }
        }
        m_arenaTicketed = wanted;
    }

    void EndDragonFight::RequestArenaChunks() {
        if (!Server::g_integratedServer) return;
        Game::World* world = m_level.World();
        if (!world) return;
        for (int x = -kArenaSizeChunks; x <= kArenaSizeChunks; ++x) {
            for (int z = -kArenaSizeChunks; z <= kArenaSizeChunks; ++z) {
                if (world->IsChunkLoaded(x, z)) continue;
                Server::g_integratedServer->RequestChunkLoad(
                    Game::DimensionId::End, Game::Math::ChunkPos{x, z},
                    /*priority=*/0);
            }
        }
    }

    bool EndDragonFight::IsArenaLoaded() const {
        Game::World* world = m_level.World();
        if (!world) return false;
        for (int x = -kArenaSizeChunks; x <= kArenaSizeChunks; ++x) {
            for (int z = -kArenaSizeChunks; z <= kArenaSizeChunks; ++z) {
                if (!world->IsChunkLoaded(x, z)) return false;
            }
        }
        return true;
    }

    // ── Players / boss bar ─────────────────────────────────────────────────

    void EndDragonFight::UpdatePlayers() {
        if (!m_sessions) return;

        std::unordered_set<uint32_t> present;
        for (const auto& session : m_sessions->GetAllSessions()) {
            if (!session) continue;
            if (session->GetDimensionId() !=
                static_cast<int>(Game::DimensionId::End)) {
                continue;
            }
            ServerPlayer* player = session->GetPlayer();
            if (!player) continue;
            // MC validPlayer: alive and within 192 of (0, 128, 0).
            const glm::dvec3 d = player->getPosition() -
                                 glm::dvec3(kOrigin.x, 128 + kOrigin.y, kOrigin.z);
            if (glm::dot(d, d) > 192.0 * 192.0) continue;
            present.insert(session->GetConnectionId());
        }

        // Joins. MC's updatePlayers adds every valid player to the event
        // UNCONDITIONALLY — whether they see a bar is the event's visibility
        // (ServerBossEvent.addPlayer only sends the packet while visible).
        for (uint32_t id : present) {
            if (m_barPlayers.insert(id).second && m_barVisible) {
                SendBossAdd(id);
            }
        }
        // Leaves. Removing a player who never saw the bar sends a harmless
        // remove; MC's removePlayer likewise sends whenever the event had
        // been visible to them.
        for (auto it = m_barPlayers.begin(); it != m_barPlayers.end();) {
            if (present.count(*it) == 0) {
                SendBossRemove(*it);
                it = m_barPlayers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void EndDragonFight::SetBarVisible(bool visible) {
        if (visible == m_barVisible) return;
        m_barVisible = visible;
        for (uint32_t id : m_barPlayers) {
            if (visible) SendBossAdd(id);
            else         SendBossRemove(id);
        }
    }

    void EndDragonFight::SendBossAdd(uint32_t connectionId) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op = Network::BossEventS2CPacket::Op::Add;
        p.progress = m_barProgress;
        p.color = Network::BossEventS2CPacket::Color::Pink;
        p.notches = 0;
        p.name = "Ender Dragon";
        session->GetConnection()->SendPacket(
            static_cast<uint8_t>(Network::PacketId::BossEventS2C),
            Network::Serialization::Serialize(p));
    }

    void EndDragonFight::SendBossRemove(uint32_t connectionId) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op = Network::BossEventS2CPacket::Op::Remove;
        session->GetConnection()->SendPacket(
            static_cast<uint8_t>(Network::PacketId::BossEventS2C),
            Network::Serialization::Serialize(p));
    }

    void EndDragonFight::BroadcastBossProgress() {
        // One dragon HP is 1/200 of the bar; resend only when the bar can
        // visibly move (MC's ServerBossEvent broadcasts on setProgress, but
        // updateDragon calls it every tick with mostly-unchanged health).
        if (std::abs(m_barProgress - m_lastSentProgress) < 0.004f) return;
        m_lastSentProgress = m_barProgress;
        if (!m_sessions) return;
        Network::BossEventS2CPacket p;
        p.op = Network::BossEventS2CPacket::Op::UpdateProgress;
        p.progress = m_barProgress;
        const auto data = Network::Serialization::Serialize(p);
        for (uint32_t id : m_barPlayers) {
            auto session = m_sessions->GetSessionByConnection(id);
            if (!session || !session->GetConnection()) continue;
            session->GetConnection()->SendPacket(
                static_cast<uint8_t>(Network::PacketId::BossEventS2C), data);
        }
    }

    // ── State scanning (MC scanState) ──────────────────────────────────────

    void EndDragonFight::ScanState() {
        Log::Info("[DragonFight] Scanning dragon-fight state...");
        Game::World* world = m_level.World();
        if (!world) return;

        // ADAPTATION: podiums only ever sit at the fixed origin, so instead
        // of MC's chunk-wide block-entity sweep the scan reads the origin
        // columns for an END_PORTAL block.
        bool activePortal = false;
        const int maxY = Game::DimensionLogicalHeight(Game::DimensionId::End) - 1;
        for (int y = maxY; y >= 0 && !activePortal; --y) {
            for (int dx = -2; dx <= 2 && !activePortal; ++dx) {
                for (int dz = -2; dz <= 2; ++dz) {
                    if (world->GetBlock(dx, y, dz) == Game::BlockID::EndPortal) {
                        activePortal = true;
                        if (!m_hasPortalLocation) {
                            m_portalLocation = glm::ivec3(0, y, 0);
                            m_hasPortalLocation = true;
                        }
                        break;
                    }
                }
            }
        }

        // MC scanState, line for line from here.
        if (activePortal) {
            Log::Info("[DragonFight] Found that the dragon has been killed in "
                      "this world already.");
            m_previouslyKilled = true;
        } else {
            Log::Info("[DragonFight] Found that the dragon has not yet been "
                      "killed in this world.");
            m_previouslyKilled = false;
            SpawnExitPortal(false);
        }

        // MC: an empty dragon list means killed; a live dragon is adopted by
        // UUID — unless there is no active portal, in which case the dragon
        // is a legacy-world leftover and is REMOVED (MC "But we didn't have a
        // portal, let's remove it").
        Game::EnderDragon* found = nullptr;
        if (MobManager* mobs = m_level.Mobs()) {
            for (Game::Mob* mob : mobs->List()) {
                if (auto* dragon = dynamic_cast<Game::EnderDragon*>(mob)) {
                    found = dragon;
                    break;
                }
            }
        }
        if (!found) {
            m_dragonKilled = true;
        } else {
            m_dragonUuid = found->GetUuid();
            Log::Info("[DragonFight] Found that there's a dragon still alive "
                      "(id %d).", found->GetId());
            m_dragonKilled = false;
            if (!activePortal) {
                Log::Info("[DragonFight] But we didn't have a portal, let's "
                          "remove it.");
                found->Discard();
                m_dragonUuid = Game::Uuid{};
            }
        }

        // MC: a world that never beat the dragon cannot be in the "killed,
        // portal open" state — the missing dragon just hasn't spawned yet.
        if (!m_previouslyKilled && m_dragonKilled) {
            m_dragonKilled = false;
        }

        UpdateCrystalCount();
        SpawnPillarCrystalsIfNeeded();
        Save();
    }

    void EndDragonFight::SpawnPillarCrystalsIfNeeded() {
        // ADAPTATION (header note): the terrain library's spikes carry no
        // entities, so a fresh world's crystals are seeded here, once. An
        // imported MC world's crystals arrive through the entity chunks, in
        // which case the scan found some and the seed is skipped for good.
        if (m_pillarCrystalsSpawned) return;
        m_pillarCrystalsSpawned = true;
        if (m_previouslyKilled) return;   // a beaten world keeps whatever it has
        if (m_crystalsAlive > 0) {
            Log::Info("[DragonFight] Arena already has %d crystal(s) — not "
                      "seeding.", m_crystalsAlive);
            return;
        }

        ServerLevelBridge* bridge = m_level.MobLevel();
        MobManager* mobs = m_level.Mobs();
        if (!bridge || !mobs) return;

        int spawned = 0;
        for (const EndSpike& spike : Spikes()) {
            auto crystal = std::make_unique<Game::EndCrystal>(bridge);
            crystal->position = glm::dvec3(spike.centerX + 0.5,
                                           spike.height + 1,
                                           spike.centerZ + 0.5);
            crystal->yRot = bridge->Random().NextFloat() * 360.0f;
            if (mobs->Add(std::move(crystal)) != 0) ++spawned;
        }
        Log::Info("[DragonFight] Seeded %d pillar crystal(s).", spawned);
        UpdateCrystalCount();
    }

    // ── The dragon (MC findOrCreateDragon / createNewDragon) ──────────────

    void EndDragonFight::FindOrCreateDragon() {
        // MC findOrCreateDragon: any live dragon in the level is adopted
        // ("found another one to use"); only an empty level spawns one.
        MobManager* mobs = m_level.Mobs();
        if (!mobs) return;
        for (Game::Mob* mob : mobs->List()) {
            if (auto* dragon = dynamic_cast<Game::EnderDragon*>(mob)) {
                m_dragonUuid = dragon->GetUuid();
                return;
            }
        }
        CreateNewDragon();
    }

    Game::EnderDragon* EndDragonFight::CreateNewDragon() {
        ServerLevelBridge* bridge = m_level.MobLevel();
        MobManager* mobs = m_level.Mobs();
        Game::World* world = m_level.World();
        if (!bridge || !mobs || !world) return nullptr;

        // MC getChunkAt(origin.x, 128, origin.z) — make sure the spawn chunk
        // exists before the dragon does.
        world->GetChunk(0, 0);

        auto dragon = std::make_unique<Game::EnderDragon>(bridge);
        dragon->SetFightOrigin(kOrigin);
        dragon->position = glm::dvec3(kOrigin.x, kDragonSpawnY + kOrigin.y,
                                      kOrigin.z);
        dragon->yRot = dragon->yBodyRot = dragon->yHeadRot =
            bridge->Random().NextFloat() * 360.0f;
        dragon->xRot = 0.0f;
        dragon->SetPhase(Game::DragonPhase::HoldingPattern);

        Game::EnderDragon* raw = dragon.get();
        const int32_t id = mobs->Add(std::move(dragon));
        if (id == 0) return nullptr;
        // MC: `this.dragonUUID = dragon.getUUID()` — Add minted it.
        m_dragonUuid = raw->GetUuid();
        m_barProgress = 1.0f;
        Log::Info("[DragonFight] Spawned the ender dragon (id %d).", id);
        return raw;
    }

    void EndDragonFight::UpdateDragon(Game::EnderDragon& dragon) {
        if (dragon.GetUuid() != m_dragonUuid) return;
        const float maxHealth = dragon.GetMaxHealth();
        m_barProgress = maxHealth > 0.0f ? dragon.GetHealth() / maxHealth : 0.0f;
        m_ticksSinceDragonSeen = 0;
    }

    // ── Crystals ───────────────────────────────────────────────────────────

    void EndDragonFight::UpdateCrystalCount() {
        m_ticksSinceCrystalsScanned = 0;
        m_crystalsAlive = 0;
        MobManager* mobs = m_level.Mobs();
        if (!mobs) return;
        const auto& spikes = Spikes();
        for (Game::Mob* mob : mobs->List()) {
            auto* crystal = dynamic_cast<Game::EndCrystal*>(mob);
            if (!crystal || crystal->IsRemoved()) continue;
            for (const EndSpike& spike : spikes) {
                // MC EndSpike.getTopBoundingBox: full-height column over the
                // spike's radius square.
                if (std::abs(crystal->position.x - spike.centerX) <= spike.radius &&
                    std::abs(crystal->position.z - spike.centerZ) <= spike.radius) {
                    ++m_crystalsAlive;
                    break;
                }
            }
        }
    }

    void EndDragonFight::OnCrystalDestroyed(Game::EndCrystal& crystal,
                                            Game::Entity* attacker) {
        if (m_respawning &&
            std::find(m_respawnCrystalIds.begin(), m_respawnCrystalIds.end(),
                      crystal.GetId()) != m_respawnCrystalIds.end()) {
            // MC: popping a ritual crystal aborts the summoning.
            Log::Info("[DragonFight] Respawn ritual aborted.");
            m_respawning = false;
            m_respawnTime = 0;
            m_respawnCrystalIds.clear();
            ResetSpikeCrystals();
            SpawnExitPortal(true);
            Save();
            return;
        }

        UpdateCrystalCount();
        if (Game::EnderDragon* dragon = ResolveDragon()) {
            dragon->OnCrystalDestroyed(crystal, attacker);
        }
    }

    void EndDragonFight::ResetSpikeCrystals() {
        MobManager* mobs = m_level.Mobs();
        if (!mobs) return;
        const auto& spikes = Spikes();
        for (Game::Mob* mob : mobs->List()) {
            auto* crystal = dynamic_cast<Game::EndCrystal*>(mob);
            if (!crystal || crystal->IsRemoved()) continue;
            for (const EndSpike& spike : spikes) {
                if (std::abs(crystal->position.x - spike.centerX) <= spike.radius &&
                    std::abs(crystal->position.z - spike.centerZ) <= spike.radius) {
                    crystal->SetInvulnerable(false);
                    crystal->ClearBeamTarget();
                    break;
                }
            }
        }
    }

    // ── Kill / rewards (MC setDragonKilled) ────────────────────────────────

    void EndDragonFight::SetDragonKilled(Game::EnderDragon& dragon) {
        if (dragon.GetUuid() != m_dragonUuid) return;

        // MC: setProgress(0) then setVisible(false); the tick-top visibility
        // line keeps it down for as long as dragonKilled holds.
        m_barProgress = 0.0f;
        BroadcastBossProgress();
        SetBarVisible(false);

        SpawnExitPortal(true);
        SpawnNewGateway();

        if (!m_previouslyKilled && m_level.World()) {
            // MC: the egg on the podium pillar, first kill only.
            const int y = SurfaceY(m_portalLocation.x, m_portalLocation.z);
            m_level.World()->SetBlock(m_portalLocation.x, y, m_portalLocation.z,
                                      Game::BlockID::DragonEgg,
                                      Game::World::UpdateFlags::All);
        }

        m_previouslyKilled = true;
        m_dragonKilled = true;
        // MC keeps dragonUUID as-is — the corpse is discarded by its own
        // death cinematic, and ResolveDragon answers null from then on.
        Save();
        Log::Info("[DragonFight] The dragon is dead. The exit portal is open.");
    }

    // ── The exit portal (MC spawnExitPortal + EndPodiumFeature) ────────────

    void EndDragonFight::SpawnExitPortal(bool active) {
        Game::World* world = m_level.World();
        if (!world) return;

        if (!m_hasPortalLocation) {
            // MC: heightmap below, then walk down through any bedrock, floored
            // at minY + 1.
            int y = SurfaceY(kOrigin.x, kOrigin.z) - 1;
            while (world->GetBlock(kOrigin.x, y, kOrigin.z) ==
                       Game::BlockID::Bedrock &&
                   y > 63) {
                --y;
            }
            m_portalLocation = glm::ivec3(kOrigin.x, std::max(1, y), kOrigin.z);
            m_hasPortalLocation = true;
        }

        PlacePodium(m_portalLocation, active);
        Save();
    }

    void EndDragonFight::PlacePodium(const glm::ivec3& origin, bool active) {
        // MC EndPodiumFeature.place, transcribed. Distances are 3D — the
        // "cleared dome" above the portal really is only a few blocks tall.
        Game::World* world = m_level.World();
        if (!world) return;
        const auto flags = Game::World::UpdateFlags::All;

        for (int y = origin.y - 1; y <= origin.y + 32; ++y) {
            for (int x = origin.x - 4; x <= origin.x + 4; ++x) {
                for (int z = origin.z - 4; z <= origin.z + 4; ++z) {
                    const double dx = x - origin.x, dy = y - origin.y,
                                 dz = z - origin.z;
                    const double distSqr = dx * dx + dy * dy + dz * dz;
                    const bool insideRim = distSqr < 2.5 * 2.5;
                    if (!insideRim && !(distSqr < 3.5 * 3.5)) continue;

                    if (y < origin.y) {
                        world->SetBlock(x, y, z,
                                        insideRim ? Game::BlockID::Bedrock
                                                  : Game::BlockID::EndStone,
                                        flags);
                    } else if (y > origin.y) {
                        world->SetBlock(x, y, z, Game::BlockID::Air, flags);
                    } else if (!insideRim) {
                        world->SetBlock(x, y, z, Game::BlockID::Bedrock, flags);
                    } else if (active) {
                        world->SetBlock(x, y, z, Game::BlockID::EndPortal, flags);
                    } else {
                        world->SetBlock(x, y, z, Game::BlockID::Air, flags);
                    }
                }
            }
        }

        // The bedrock pillar the egg sits on.
        for (int y = 0; y < 4; ++y) {
            world->SetBlock(origin.x, origin.y + y, origin.z,
                            Game::BlockID::Bedrock, flags);
        }

        // Four wall torches at pillar height +2.
        const auto& torchDef =
            Game::BlockRegistry::GetStateDefinition(Game::BlockID::WallTorch);
        const struct { int dx, dz; const char* facing; } torches[] = {
            { 1, 0, "east" }, { -1, 0, "west" }, { 0, 1, "south" }, { 0, -1, "north" },
        };
        for (const auto& t : torches) {
            Game::BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["facing"] = t.facing;
            world->SetBlock(origin.x + t.dx, origin.y + 2, origin.z + t.dz,
                            Game::BlockID::WallTorch, flags,
                            torchDef.IndexOf(props));
        }
    }

    // ── Gateways (MC spawnNewGateway + EndGatewayFeature) ─────────────────

    void EndDragonFight::SpawnNewGateway() {
        if (m_gateways.empty()) return;
        const int gateway = m_gateways.back();
        m_gateways.pop_back();
        const int x = static_cast<int>(std::floor(
            96.0 * std::cos(2.0 * (-kPi + 0.15707963267948966 * gateway))));
        const int z = static_cast<int>(std::floor(
            96.0 * std::sin(2.0 * (-kPi + 0.15707963267948966 * gateway))));
        const glm::ivec3 pos(x, 75, z);

        // MC levelEvent 3000 — the explosion-emitter flash and gateway-spawn
        // sound. The explosion packet is the closest visual this engine has.
        if (ServerLevelBridge* bridge = m_level.MobLevel()) {
            bridge->BroadcastExplosion(glm::dvec3(pos) + glm::dvec3(0.5),
                                       1.0f, 0, /*small=*/false);
        }
        PlaceGatewayBlocks(pos);
        Save();
    }

    void EndDragonFight::PlaceGatewayBlocks(const glm::ivec3& pos) {
        if (Game::World* world = m_level.World()) PlaceGatewayFrame(*world, pos);
    }

    void EndDragonFight::PlaceGatewayFrame(Game::World& world, const glm::ivec3& pos) {
        // MC EndGatewayFeature.place, transcribed: the gateway block wrapped
        // in its bedrock hourglass frame. World::SetBlock creates the
        // gateway's block entity as a side effect.
        const auto flags = Game::World::UpdateFlags::All;

        for (int y = pos.y - 2; y <= pos.y + 2; ++y) {
            for (int x = pos.x - 1; x <= pos.x + 1; ++x) {
                for (int z = pos.z - 1; z <= pos.z + 1; ++z) {
                    const bool sameX = (x == pos.x);
                    const bool sameY = (y == pos.y);
                    const bool sameZ = (z == pos.z);
                    const bool end = std::abs(y - pos.y) == 2;
                    Game::BlockID put;
                    if (sameX && sameY && sameZ) {
                        put = Game::BlockID::EndGateway;
                    } else if (sameY) {
                        put = Game::BlockID::Air;
                    } else if (end && sameX && sameZ) {
                        put = Game::BlockID::Bedrock;
                    } else if ((sameX || sameZ) && !end) {
                        put = Game::BlockID::Bedrock;
                    } else {
                        put = Game::BlockID::Air;
                    }
                    world.SetBlock(x, y, z, put, flags);
                }
            }
        }
    }

    // ── Spikes (MC SpikeFeature.placeSpike — the respawn ritual's reset) ──

    void EndDragonFight::PlaceSpike(const EndSpike& spike, bool crystalInvulnerable,
                                    bool beamToOrigin) {
        Game::World* world = m_level.World();
        ServerLevelBridge* bridge = m_level.MobLevel();
        MobManager* mobs = m_level.Mobs();
        if (!world || !bridge || !mobs) return;
        const auto flags = Game::World::UpdateFlags::All;
        const int radius = spike.radius;

        // Obsidian column; everything else above y=65 in the box is cleared.
        for (int x = spike.centerX - radius; x <= spike.centerX + radius; ++x) {
            for (int z = spike.centerZ - radius; z <= spike.centerZ + radius; ++z) {
                const double dx = x - spike.centerX;
                const double dz = z - spike.centerZ;
                for (int y = 0; y <= spike.height + 10; ++y) {
                    if (dx * dx + dz * dz <= radius * radius + 1 &&
                        y < spike.height) {
                        world->SetBlock(x, y, z, Game::BlockID::Obsidian, flags);
                    } else if (y > 65) {
                        world->SetBlock(x, y, z, Game::BlockID::Air, flags);
                    }
                }
            }
        }

        // The iron-bars cage on guarded spikes.
        if (spike.guarded) {
            const auto& barsDef =
                Game::BlockRegistry::GetStateDefinition(Game::BlockID::IronBars);
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dz = -2; dz <= 2; ++dz) {
                    for (int dy = 0; dy <= 3; ++dy) {
                        const bool isXSide = std::abs(dx) == 2;
                        const bool isZSide = std::abs(dz) == 2;
                        const bool top = dy == 3;
                        if (!isXSide && !isZSide && !top) continue;
                        const bool xEdge = dx == -2 || dx == 2 || top;
                        const bool zEdge = dz == -2 || dz == 2 || top;
                        Game::BlockRegistry::BlockStateDefinition::PropertyMap props;
                        props["north"] = (xEdge && dz != -2) ? "true" : "false";
                        props["south"] = (xEdge && dz != 2) ? "true" : "false";
                        props["west"]  = (zEdge && dx != -2) ? "true" : "false";
                        props["east"]  = (zEdge && dx != 2) ? "true" : "false";
                        world->SetBlock(spike.centerX + dx, spike.height + dy,
                                        spike.centerZ + dz, Game::BlockID::IronBars,
                                        flags, barsDef.IndexOf(props));
                    }
                }
            }
        }

        // The fresh crystal, its bedrock base and its fire.
        auto crystal = std::make_unique<Game::EndCrystal>(bridge);
        crystal->position = glm::dvec3(spike.centerX + 0.5, spike.height + 1,
                                       spike.centerZ + 0.5);
        crystal->yRot = bridge->Random().NextFloat() * 360.0f;
        crystal->SetInvulnerable(crystalInvulnerable);
        if (beamToOrigin) {
            crystal->SetBeamTarget(glm::ivec3(0, 128, 0));
        }
        world->SetBlock(spike.centerX, spike.height, spike.centerZ,
                        Game::BlockID::Bedrock, flags);
        world->SetBlock(spike.centerX, spike.height + 1, spike.centerZ,
                        Game::BlockID::Fire, flags);
        mobs->Add(std::move(crystal));
    }

    // ── The respawn ritual ─────────────────────────────────────────────────

    void EndDragonFight::TryRespawn() {
        if (!m_dragonKilled || m_respawning) return;
        Game::World* world = m_level.World();
        MobManager* mobs = m_level.Mobs();
        if (!world || !mobs) return;

        if (!m_hasPortalLocation) {
            // MC: find (or make) the portal before measuring around it.
            SpawnExitPortal(true);
        }

        // MC: one crystal on each of the four sides, two blocks out from the
        // block above the portal centre.
        const glm::ivec3 center = m_portalLocation + glm::ivec3(0, 1, 0);
        const glm::ivec3 sides[4] = {
            center + glm::ivec3(2, 0, 0), center + glm::ivec3(-2, 0, 0),
            center + glm::ivec3(0, 0, 2), center + glm::ivec3(0, 0, -2),
        };

        std::vector<int32_t> crystalIds;
        for (const glm::ivec3& cell : sides) {
            bool found = false;
            for (Game::Mob* mob : mobs->List()) {
                auto* crystal = dynamic_cast<Game::EndCrystal*>(mob);
                if (!crystal || crystal->IsRemoved()) continue;
                // MC getEntitiesOfClass(EndCrystal, AABB(cell)) — the
                // crystal's box against the unit cell.
                const Game::AABB box = crystal->GetAABB();
                if (box.max.x > cell.x && box.min.x < cell.x + 1 &&
                    box.max.y > cell.y && box.min.y < cell.y + 1 &&
                    box.max.z > cell.z && box.min.z < cell.z + 1) {
                    crystalIds.push_back(crystal->GetId());
                    found = true;
                }
            }
            if (!found) return;
        }

        Log::Info("[DragonFight] Four crystals placed — respawning the dragon.");
        RespawnDragon(std::move(crystalIds));
    }

    void EndDragonFight::RespawnDragon(std::vector<int32_t> crystalIds) {
        if (!m_dragonKilled || m_respawning) return;
        m_respawning = true;
        m_respawnStage = RespawnStage::Start;
        m_respawnTime = 0;
        m_respawnCrystalIds = std::move(crystalIds);
        // MC: the portal is torn down for the ritual (the inactive podium
        // write clears the portal blocks).
        SpawnExitPortal(false);
        Save();
    }

    void EndDragonFight::SetRespawnStage(RespawnStage stage) {
        m_respawnTime = 0;
        if (stage == RespawnStage::End) {
            m_respawning = false;
            m_dragonKilled = false;
            // CreateNewDragon resets the bar to full; the bar itself returns
            // through Tick's `SetBarVisible(!m_dragonKilled)` next tick,
            // exactly as MC's tick-top setVisible brings it back.
            m_lastSentProgress = -1.0f;
            CreateNewDragon();
            Save();
        } else {
            m_respawnStage = stage;
        }
    }

    void EndDragonFight::TickRespawn() {
        // MC DragonRespawnAnimation, transcribed stage by stage. Level events
        // 3001 (the world growl) are sound-system work and skipped.
        ServerLevelBridge* bridge = m_level.MobLevel();
        const glm::ivec3 beamPos(0, 128, 0);
        const int time = m_respawnTime++;

        switch (m_respawnStage) {
            case RespawnStage::Start: {
                for (int32_t id : m_respawnCrystalIds) {
                    if (Game::EndCrystal* crystal = ResolveCrystal(id)) {
                        crystal->SetBeamTarget(beamPos);
                    }
                }
                SetRespawnStage(RespawnStage::PreparingToSummonPillars);
                break;
            }
            case RespawnStage::PreparingToSummonPillars: {
                if (time >= 100) {
                    SetRespawnStage(RespawnStage::SummoningPillars);
                }
                break;
            }
            case RespawnStage::SummoningPillars: {
                const bool startOfBeam = time % 40 == 0;
                const bool endOfBeam = time % 40 == 39;
                if (!startOfBeam && !endOfBeam) break;
                const auto& spikes = Spikes();
                const int index = time / 40;
                if (index < static_cast<int>(spikes.size())) {
                    const EndSpike& spike = spikes[static_cast<size_t>(index)];
                    if (startOfBeam) {
                        for (int32_t id : m_respawnCrystalIds) {
                            if (Game::EndCrystal* crystal = ResolveCrystal(id)) {
                                crystal->SetBeamTarget(glm::ivec3(
                                    spike.centerX, spike.height + 1,
                                    spike.centerZ));
                            }
                        }
                    } else {
                        // Clear ±10 around the spike top, blow it up, rebuild
                        // it with a fresh (invulnerable) crystal.
                        if (Game::World* world = m_level.World()) {
                            for (int x = spike.centerX - 10; x <= spike.centerX + 10; ++x) {
                                for (int y = spike.height - 10; y <= spike.height + 10; ++y) {
                                    for (int z = spike.centerZ - 10; z <= spike.centerZ + 10; ++z) {
                                        world->SetBlock(x, y, z, Game::BlockID::Air,
                                                        Game::World::UpdateFlags::All);
                                    }
                                }
                            }
                        }
                        if (bridge) {
                            Game::ExplosionParams params;
                            params.center = glm::dvec3(spike.centerX + 0.5,
                                                       spike.height,
                                                       spike.centerZ + 0.5);
                            params.radius = 5.0f;
                            params.interaction = Game::ExplosionInteraction::Block;
                            bridge->QueueExplosion(params);
                        }
                        PlaceSpike(spike, /*crystalInvulnerable=*/true,
                                   /*beamToOrigin=*/true);
                    }
                } else if (startOfBeam) {
                    SetRespawnStage(RespawnStage::SummoningDragon);
                }
                break;
            }
            case RespawnStage::SummoningDragon: {
                if (time >= 100) {
                    ResetSpikeCrystals();
                    for (int32_t id : m_respawnCrystalIds) {
                        Game::EndCrystal* crystal = ResolveCrystal(id);
                        if (!crystal) continue;
                        crystal->ClearBeamTarget();
                        if (bridge) {
                            Game::ExplosionParams params;
                            params.center = crystal->position;
                            params.radius = 6.0f;
                            params.source = crystal;
                            params.interaction = Game::ExplosionInteraction::None;
                            bridge->QueueExplosion(params);
                        }
                        crystal->Discard();
                    }
                    m_respawnCrystalIds.clear();
                    SetRespawnStage(RespawnStage::End);
                } else if (time == 0) {
                    for (int32_t id : m_respawnCrystalIds) {
                        if (Game::EndCrystal* crystal = ResolveCrystal(id)) {
                            crystal->SetBeamTarget(beamPos);
                        }
                    }
                }
                break;
            }
            case RespawnStage::End:
                break;
        }
    }

} // namespace Server
