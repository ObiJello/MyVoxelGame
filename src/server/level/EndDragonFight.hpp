// File: src/server/level/EndDragonFight.hpp
//
// MC net.minecraft.world.level.dimension.end.EndDragonFight — the controller
// that owns everything about the End's boss encounter that is not the dragon
// itself: finding/spawning the dragon, the pillar crystals, the exit portal
// (and with it the way home), the end gateways, the dragon egg, the respawn
// ritual, and the boss bar.
//
// One instance per SERVER End level, created by ServerLevel when the End is
// built and handed to the level's bridge so common code reaches it through
// EntityLevel::DragonFight() (see common/entity/DragonFight.hpp).
//
// ADAPTATIONS from MC, each marked at its site:
//   * persistence — MC stores DragonFight in level.dat; LevelDat.hpp keeps
//     that file byte-vanilla on purpose, so the fight writes
//     <world>/data/dragon_fight.json beside the obeycraft sidecar. The
//     CONTENT matches MC's Data record field-for-field (dragonUUID included,
//     as vanilla's four-int codec).
//   * pillar crystals — MC's SpikeFeature spawns them during worldgen; the
//     vendored terrain library places the obsidian/cage/fire but cannot spawn
//     game entities, so the fight seeds them on the first arena scan (once,
//     flagged in the JSON) unless the arena already has crystals (an imported
//     MC world's entity chunks carry their own).
//   * exit-portal scanning — MC pattern-matches the bedrock podium via block
//     entities; podiums here are only ever placed by this class at the fixed
//     origin, so the scan looks for END_PORTAL blocks in the origin columns.
#pragma once

#include "common/entity/DragonFight.hpp"
#include "common/core/Uuid.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Game {
    class World;
    class EnderDragon;
    class EndCrystal;
    class Entity;
}

namespace Server {

    class ServerLevel;
    class PlayerSessionManager;

    class EndDragonFight final : public Game::IDragonFight {
    public:
        // MC EndDragonFight constants.
        static constexpr int kMaxTicksBeforeDragonRespawn = 1200;
        static constexpr int kTimeBetweenCrystalScans = 100;
        static constexpr int kTimeBetweenPlayerScans = 20;
        static constexpr int kArenaSizeChunks = 8;
        static constexpr int kGatewayCount = 20;
        static constexpr int kGatewayDistance = 96;
        static constexpr int kDragonSpawnY = 128;

        EndDragonFight(ServerLevel& level, PlayerSessionManager* sessions);
        ~EndDragonFight() override;

        // One server tick, called by IntegratedServer for the End level only.
        void Tick();

        // Persist the fight state (called from the server's save points and
        // on shutdown). No-op for a read-only or unsaved world.
        void Save();

        // MC EndDragonFight.tryRespawn — called by the End-crystal item after
        // a placement beside the exit portal.
        void TryRespawn();

        // MC EndGatewayFeature.place's block half — the gateway block in its
        // bedrock hourglass. Static and public because the gateway TELEPORT
        // (PortalTravel) places the return gateway on the far islands with
        // exactly the same frame.
        static void PlaceGatewayFrame(Game::World& world, const glm::ivec3& pos);

        // ── Game::IDragonFight ────────────────────────────────────────────
        int  CrystalsAlive() const override { return m_crystalsAlive; }
        bool HasPreviouslyKilledDragon() const override { return m_previouslyKilled; }
        void UpdateDragon(Game::EnderDragon& dragon) override;
        void SetDragonKilled(Game::EnderDragon& dragon) override;
        void OnCrystalDestroyed(Game::EndCrystal& crystal,
                                Game::Entity* attacker) override;

    private:
        // MC SpikeFeature.EndSpike, reduced to the fields the fight reads.
        struct EndSpike {
            int centerX = 0, centerZ = 0, radius = 0, height = 0;
            bool guarded = false;
        };

        // MC DragonRespawnAnimation stages.
        enum class RespawnStage : uint8_t {
            Start,
            PreparingToSummonPillars,
            SummoningPillars,
            SummoningDragon,
            End,
        };

        // ── Tick pieces ───────────────────────────────────────────────────
        void UpdatePlayers();
        void EnsureArenaTickets(bool wanted);
        bool IsArenaLoaded() const;
        // Chunk LOADING in this engine is watch-set driven (see CLAUDE.md's
        // chunk-loading notes); the forced tickets above only keep loaded
        // chunks ticking. While the arena is short of chunks, this asks the
        // server's async loader for the missing ones — MC's DRAGON ticket
        // does both jobs at once.
        void RequestArenaChunks();
        void ScanState();
        void FindOrCreateDragon();
        Game::EnderDragon* CreateNewDragon();
        void UpdateCrystalCount();
        void TickRespawn();
        void SetRespawnStage(RespawnStage stage);
        void RespawnDragon(std::vector<int32_t> crystalIds);
        void ResetSpikeCrystals();

        // ── World writes ──────────────────────────────────────────────────
        void SpawnExitPortal(bool active);
        void PlacePodium(const glm::ivec3& origin, bool active);
        void SpawnNewGateway();
        void PlaceGatewayBlocks(const glm::ivec3& pos);
        void PlaceSpike(const EndSpike& spike, bool crystalInvulnerable,
                        bool beamToOrigin);
        void SpawnPillarCrystalsIfNeeded();

        // ── Helpers ───────────────────────────────────────────────────────
        const std::vector<EndSpike>& Spikes() const;
        Game::EnderDragon* ResolveDragon() const;
        Game::EndCrystal*  ResolveCrystal(int32_t id) const;
        // First free cell above the highest motion-blocking block in the
        // column (MC getHeightmapPos(MOTION_BLOCKING*, ...) stand-in).
        int SurfaceY(int x, int z) const;

        // ── Boss bar (MC ServerBossEvent, inlined) ────────────────────────
        void SendBossAdd(uint32_t connectionId);
        void SendBossRemove(uint32_t connectionId);
        void BroadcastBossProgress();
        // MC ServerBossEvent.setVisible — flipping it sends the add/remove
        // packet to every subscribed player. Tick's first line re-asserts
        // `visible = !dragonKilled` every tick (MC EndDragonFight.tick:135),
        // which is what makes the bar SELF-HEALING: whatever state the fight
        // loaded with, the bar converges on the truth within a tick of the
        // scan correcting it.
        void SetBarVisible(bool visible);

        // ── Persistence ───────────────────────────────────────────────────
        void LoadData();

        ServerLevel&          m_level;
        PlayerSessionManager* m_sessions = nullptr;

        // Persisted state (MC EndDragonFight.Data + the crystal-seed flag).
        bool m_needsStateScanning = true;
        bool m_dragonKilled = false;
        bool m_previouslyKilled = false;
        bool m_pillarCrystalsSpawned = false;
        bool m_hasPortalLocation = false;
        glm::ivec3 m_portalLocation{0};
        std::vector<int> m_gateways;
        bool m_loadedIsRespawning = false;

        // MC dragonUUID — the fight's dragon, persisted, nil = none adopted.
        // (The runtime entity id is per-session; the UUID is the identity
        // that survives a save, exactly as in MC.)
        Game::Uuid m_dragonUuid{};
        int m_ticksSinceDragonSeen = 0;
        int m_crystalsAlive = 0;
        int m_ticksSinceCrystalsScanned = 0;
        int m_ticksSinceLastPlayerScan = 21;   // MC: first tick scans
        bool m_arenaTicketed = false;

        // Respawn ritual.
        bool m_respawning = false;
        RespawnStage m_respawnStage = RespawnStage::Start;
        int m_respawnTime = 0;
        std::vector<int32_t> m_respawnCrystalIds;

        // Boss bar.
        std::unordered_set<uint32_t> m_barPlayers;   // connection ids
        bool  m_barVisible = false;                  // MC ServerBossEvent.visible
        float m_barProgress = 1.0f;
        float m_lastSentProgress = -1.0f;

        // Lazily computed spike geometry (seed-derived, never changes).
        mutable std::vector<EndSpike> m_spikes;
    };

} // namespace Server
