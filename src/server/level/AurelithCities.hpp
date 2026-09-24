// File: src/server/level/AurelithCities.hpp
//
// The server half of reawakening the Heart of Aurelith (docs/the-hush.md,
// "Reawakening the Heart"; the shared facts in common/world/level/
// AurelithQuest.hpp). One per Hush ServerLevel, owned by it like the
// stillness, ticked from IntegratedServer's per-level tick after the mobs.
//
// ── The records ──────────────────────────────────────────────────────────
// A city is known once the chunk holding its Heart (the resonance engine)
// has been loaded: OnChunkLoaded registers it from the engine's block entity
// (its position and the city's rotation). Each record is keyed by the Heart
// — the start piece's centre, so one per structure start — and carries the
// quest's state and when it began. They are saved as SavedData in
// <dimension>/data/obeycraft_aurelith.dat ({data: {Cities: [...]}}), written
// on every state change and with the world's saves, and read at start.
//
// ── The quest ────────────────────────────────────────────────────────────
//   Dormant → Awakening   the fourth voice key seated at the Podium, in the
//                         Chord's order (from the floor to the crown: Bass,
//                         Tenor, Alto, Soprano). A wrong order is the discord:
//                         the keys are thrown out, the plaza's lights stutter.
//   Awakening → Contested the timeline (AurelithQuest.hpp): the arpeggio and
//                         the Chord, the rising sound, the light wave outward
//                         from the Heart (every dim_* block in the loaded city
//                         swapped for its lit twin, batched per wave step), the
//                         motes, the Undersong — then the Unsung rises out of
//                         the dais (Game::TheUnsung, its arena the Heart).
//   Contested → Awakened  the Unsung dies. The Chord resolves; the Held Note
//                         is given once, at the Heart's foot; the Coda is set
//                         open on the Conductor's seat; the Podium's plaque
//                         and the dais's say what happened.
// A boss lost without dying (peaceful, a chunk unload that lost it, a
// command) is raised again from the dais the next time a player stands in
// the plaza, so a city can never be stranded half-awake.
//
// Awakened (and Contested, whose wave has passed) cities keep their light:
// a chunk that loads later is swapped on arrival. Awakened cities also stop
// hostile natural spawns inside their walls (Game::Aurelith::
// SetAwakenedCities → the natural spawner's gate).
//
// ── The clients ──────────────────────────────────────────────────────────
// Every kSyncEvery ticks each player in the Hush is told (AurelithS2C
// CityState) about the cities within kSyncRange they have not heard of yet,
// and told to forget those they left; a state change goes to everyone in
// range at once. Bursts (a cabinet sung open, the Heart's motes) go to the
// players near them.
//
// Threads: server thread only.
#pragma once

#include "common/world/level/AurelithQuest.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {
    class Chunk;
    class IUsePlayer;
    class ILevelWrite;
}

namespace Server {

    class ServerLevel;
    class PlayerSessionManager;

    class AurelithCities {
    public:
        static constexpr double kSyncRange = 640.0;   // blocks from a Heart
        static constexpr int    kSyncEvery = 20;      // ticks
        static constexpr double kBurstRange = 96.0;   // who sees a burst
        static constexpr int    kWaveStepTicks = 5;   // the wave front advances in steps
        static constexpr int    kScanChunksPerTick = 4;

        AurelithCities(ServerLevel& level, PlayerSessionManager* sessions, std::filesystem::path dataDir);
        ~AurelithCities();

        AurelithCities(const AurelithCities&) = delete;
        AurelithCities& operator=(const AurelithCities&) = delete;

        // Once per server tick, after the level's mobs (a death this tick is
        // seen this tick).
        void Tick();

        // A chunk of this level finished loading or generating (server
        // thread, before it is sent): register the city whose Heart it holds;
        // light an awakened city's dim blocks; feed an awakening's wave.
        void OnChunkLoaded(const Game::Math::ChunkPos& pos, Game::Chunk& chunk);

        // A voice key was seated at `socket` (the chord socket's use).
        void OnSocketSeated(const glm::ivec3& socket, Game::IUsePlayer* player);

        // A burst of particles for the players near `origin`.
        void Burst(const glm::dvec3& origin, uint8_t style, uint32_t colour);

        // SavedData. Save() writes only when something changed.
        void Save();
        // Level teardown: tell every client to forget the cities.
        void RemoveAll();

        // ── /aurelith (debug) ─────────────────────────────────────────────
        struct Summary {
            glm::ivec3 heart{0};
            int rotation = -1;
            Game::Aurelith::CityState state = Game::Aurelith::CityState::Dormant;
            int64_t stageTicks = 0;
            bool heldNoteGiven = false;
            double distance = 0.0;
        };
        // The nearest known city to `pos` within `maxDistance`, if any.
        bool Nearest(const glm::dvec3& pos, double maxDistance, Summary& out) const;
        // World position of a design offset in the city at `heart`.
        bool DesignPoint(const glm::ivec3& heart, glm::ivec2 design, int aboveStreet, glm::ivec3& out) const;
        // Seat the four keys in the Chord's order (the real path: the city
        // judges them). Returns a message for the command.
        std::string DebugSingTheChord(const glm::ivec3& heart);
        // Jump the city to the next stage (Awakening → the Unsung rises;
        // Contested → the Unsung falls).
        std::string DebugAdvance(const glm::ivec3& heart);
        // Back to Dormant: sockets emptied (the keys handed back), the
        // loaded lights dimmed, the boss removed. For testing only.
        std::string DebugReset(const glm::ivec3& heart, Game::IUsePlayer* keysTo);

    private:
        struct City;
        struct Flicker;

        City* Find(const glm::ivec3& heart);
        const City* Find(const glm::ivec3& heart) const;
        City* CityForPoint(const glm::ivec3& p, int reach);
        City* Register(const glm::ivec3& heart, int rotation);

        // The quest.
        void JudgeChord(City& city, Game::IUsePlayer* player);
        void Discord(City& city, Game::IUsePlayer* player);
        void BeginAwakening(City& city);
        void TickAwakening(City& city, int64_t now);
        void TickContested(City& city, int64_t now);
        void RaiseUnsung(City& city);
        void Resolve(City& city);
        void SetState(City& city, Game::Aurelith::CityState state);

        // The light.
        void QueueWaveScan(City& city);
        void ScanChunkForWave(City& city, const Game::Math::ChunkPos& pos);
        void AdvanceWave(City& city, int64_t now);
        int  LightChunk(const City& city, Game::Chunk& chunk, const Game::Math::ChunkPos& pos, bool toLit);
        void StartFlicker(City& city, const glm::ivec3& centre, int radius, int64_t now);
        void TickFlickers(int64_t now);

        // The podium, the plaques.
        bool Sockets(const City& city, std::vector<glm::ivec3>& out) const;
        void RevealCoda(City& city);

        // Clients.
        void SyncPlayers(bool force);
        void SendState(uint32_t connectionId, const City& city);
        void SendForget(uint32_t connectionId, const glm::ivec3& heart);
        void BroadcastState(const City& city);
        void PublishAwakened();

        // SavedData.
        void Load();
        void WriteFile();

        int64_t Now() const;

        ServerLevel&          m_level;
        PlayerSessionManager* m_sessions = nullptr;
        std::filesystem::path m_dataDir;

        std::vector<std::unique_ptr<City>>    m_cities;
        std::vector<std::unique_ptr<Flicker>> m_flickers;
        bool    m_dirty = false;
        int64_t m_tickCounter = 0;
        // connection id → the Hearts that client was told about.
        std::unordered_map<uint32_t, std::vector<glm::ivec3>> m_known;
    };

} // namespace Server
