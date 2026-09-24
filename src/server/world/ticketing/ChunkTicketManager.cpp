// File: src/server/world/ticketing/ChunkTicketManager.cpp
#include "server/world/ticketing/ChunkTicketManager.hpp"

#include "common/core/Log.hpp"

// MC's level propagator, from the terrain library's port of
// net.minecraft.server.level.ChunkTracker.
#include "server/level/ChunkTracker.h"
#include "world/ChunkPos.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace Server {

    // ── The level tracker ──────────────────────────────────────────────────
    //
    // MC LoadingChunkTracker: a ChunkTracker whose sources are the tickets and
    // whose levels are the chunk levels. levelCount is UNLOADED + 1, so the
    // tracker's "no level" (levelCount - 1) is UNLOADED — MC's MAX_LEVEL + 2.

    class ChunkTicketManager::LevelTracker final : public minecraft::server::level::ChunkTracker {
    public:
        explicit LevelTracker(ChunkTicketManager& owner)
            : ChunkTracker(ChunkLevel::UNLOADED + 1, 16, 256)
            , m_owner(owner) {}

        static int64_t Key(Game::Math::ChunkPos chunk) {
            return minecraft::world::ChunkPos::asLong(chunk.x, chunk.z);
        }

        // MC DistanceManager.runAllUpdates -> loadingChunkTracker.runDistanceUpdates.
        void RunAll() { runUpdates(std::numeric_limits<int>::max()); }

    protected:
        int getLevelFromSource(int64_t node) override {
            return m_owner.TicketLevelAtLocked(Chunk(node));
        }

        int getLevel(int64_t node) override {
            const auto it = m_owner.m_levels.find(Chunk(node));
            return it == m_owner.m_levels.end() ? ChunkLevel::UNLOADED : it->second;
        }

        void setLevel(int64_t node, int level) override {
            const Game::Math::ChunkPos chunk = Chunk(node);
            if (level >= ChunkLevel::UNLOADED) {
                if (m_owner.m_levels.erase(chunk) != 0) m_owner.m_levelsChanged = true;
                return;
            }
            auto [it, inserted] = m_owner.m_levels.try_emplace(chunk, level);
            if (inserted || it->second != level) {
                it->second = level;
                m_owner.m_levelsChanged = true;
            }
        }

    private:
        static Game::Math::ChunkPos Chunk(int64_t node) {
            const minecraft::world::ChunkPos pos(node);
            return Game::Math::ChunkPos(pos.x(), pos.z());
        }

        ChunkTicketManager& m_owner;
    };

    ChunkTicketManager::ChunkTicketManager()
        : m_tracker(std::make_unique<LevelTracker>(*this)) {}
    ChunkTicketManager::~ChunkTicketManager() = default;

    // ── Ticket mutation ────────────────────────────────────────────────────

    int ChunkTicketManager::TicketLevelAtLocked(Game::Math::ChunkPos chunk) const {
        const auto it = m_tickets.find(chunk);
        if (it == m_tickets.end()) return ChunkLevel::UNLOADED;
        int best = ChunkLevel::UNLOADED;
        for (const Ticket& t : it->second) best = std::min(best, t.level);
        return best;
    }

    void ChunkTicketManager::OnTicketsChangedLocked(Game::Math::ChunkPos chunk, int oldSourceLevel) {
        // MC TicketStorage.addTicket / removeTicket: a lower level only ever
        // decreases levels (onlyDecreased = true, the cheap path); anything
        // else re-derives the chunk from its neighbours.
        const int level = TicketLevelAtLocked(chunk);
        if (level == oldSourceLevel) return;
        m_tracker->update(LevelTracker::Key(chunk), level, level < oldSourceLevel);
    }

    void ChunkTicketManager::AddTicketLocked(Game::Math::ChunkPos chunk, TicketType type,
                                             int level, int lifespan,
                                             const std::string& identifier) {
        auto& list = m_tickets[chunk];
        for (Ticket& t : list) {
            // MC TicketStorage.addTicket dedupes on (type, level). We refcount
            // the duplicate instead, because two players in the same chunk must
            // not have one leaving remove the other's ticket.
            if (t.type == type && t.level == level && t.identifier == identifier) {
                ++t.refCount;
                t.createdTick = m_currentTick;   // refresh an expiring ticket
                return;
            }
        }
        int oldSourceLevel = ChunkLevel::UNLOADED;
        for (const Ticket& t : list) oldSourceLevel = std::min(oldSourceLevel, t.level);
        list.push_back(Ticket{type, level, m_currentTick, lifespan, identifier, 1});
        OnTicketsChangedLocked(chunk, oldSourceLevel);
    }

    void ChunkTicketManager::RemoveTicketLocked(Game::Math::ChunkPos chunk, TicketType type,
                                                const std::string& identifier) {
        auto it = m_tickets.find(chunk);
        if (it == m_tickets.end()) return;

        const int oldSourceLevel = TicketLevelAtLocked(chunk);
        auto& list = it->second;
        bool removed = false;
        for (auto t = list.begin(); t != list.end(); ++t) {
            if (t->type != type || t->identifier != identifier) continue;
            if (--t->refCount > 0) return;   // someone else still holds it
            list.erase(t);
            removed = true;
            break;
        }
        if (list.empty()) m_tickets.erase(it);
        if (removed) OnTicketsChangedLocked(chunk, oldSourceLevel);
    }

    // ── Players ────────────────────────────────────────────────────────────

    void ChunkTicketManager::AddPlayer(Game::Math::ChunkPos chunk, uint32_t playerId) {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Idempotent, and self-correcting: registering a player who is already
        // registered somewhere else moves them rather than leaking a ticket.
        // MC gets this for free because ChunkMap.move is the only mover and it
        // always removes from lastSectionPos first; doing it here as well means
        // a caller cannot leak by getting the order wrong.
        auto existing = m_playerTicketChunk.find(playerId);
        if (existing != m_playerTicketChunk.end()) {
            if (existing->second == chunk) return;
            const Game::Math::ChunkPos old = existing->second;
            auto& occupants = m_playersPerChunk[old];
            occupants.erase(playerId);
            if (occupants.empty()) {
                m_playersPerChunk.erase(old);
                RemoveTicketLocked(old, TicketType::PlayerSimulation, {});
            }
        }

        m_playersPerChunk[chunk].insert(playerId);
        m_playerTicketChunk[playerId] = chunk;
        AddTicketLocked(chunk, TicketType::PlayerSimulation,
                        ChunkLevel::PlayerTicketLevel(m_simulationDistance), -1, {});
    }

    void ChunkTicketManager::RemovePlayer(Game::Math::ChunkPos chunk, uint32_t playerId) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto occupantsIt = m_playersPerChunk.find(chunk);
        if (occupantsIt == m_playersPerChunk.end()) return;

        occupantsIt->second.erase(playerId);
        if (occupantsIt->second.empty()) {
            m_playersPerChunk.erase(occupantsIt);
            RemoveTicketLocked(chunk, TicketType::PlayerSimulation, {});
        }
        auto recorded = m_playerTicketChunk.find(playerId);
        if (recorded != m_playerTicketChunk.end() && recorded->second == chunk) {
            m_playerTicketChunk.erase(recorded);
        }
    }

    void ChunkTicketManager::RemoveAllPlayerTickets(uint32_t playerId) {
        Game::Math::ChunkPos chunk{0, 0};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_playerTicketChunk.find(playerId);
            if (it == m_playerTicketChunk.end()) return;
            chunk = it->second;
        }
        RemovePlayer(chunk, playerId);
    }

    void ChunkTicketManager::SetSimulationDistance(int distance) {
        std::lock_guard<std::mutex> lock(m_mutex);
        distance = std::max(2, distance);
        if (distance == m_simulationDistance) return;
        m_simulationDistance = distance;

        // MC TicketStorage.replaceTicketLevelOfType — re-level in place rather
        // than tearing down and rebuilding, so no chunk momentarily drops out.
        const int level = ChunkLevel::PlayerTicketLevel(distance);
        for (auto& [chunk, list] : m_tickets) {
            int oldSourceLevel = ChunkLevel::UNLOADED;
            bool relevelled = false;
            for (Ticket& t : list) {
                oldSourceLevel = std::min(oldSourceLevel, t.level);
                if (t.type == TicketType::PlayerSimulation) {
                    t.level = level;
                    relevelled = true;
                }
            }
            if (relevelled) OnTicketsChangedLocked(chunk, oldSourceLevel);
        }
    }

    // ── Other sources ──────────────────────────────────────────────────────

    void ChunkTicketManager::AddSpawnTickets(Game::Math::ChunkPos spawnChunk, int radius) {
        std::lock_guard<std::mutex> lock(m_mutex);
        // A single source at the centre would do, but MC keeps the spawn area
        // explicitly FULL rather than merely reachable, so each chunk in the
        // radius gets its own ticket at the full-chunk level. They are sources
        // like any other; propagation still fills in around them.
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                AddTicketLocked(Game::Math::ChunkPos(spawnChunk.x + dx, spawnChunk.z + dz),
                                TicketType::Spawn, ChunkLevel::FULL, -1, {});
            }
        }
    }

    void ChunkTicketManager::AddPlayerSpawnTicket(Game::Math::ChunkPos chunk, int radius,
                                                  int lifespanTicks) {
        std::lock_guard<std::mutex> lock(m_mutex);
        // MC PLAYER_SPAWN: register("player_spawn", 20L, 2) — a LOADING ticket
        // only. Level FULL, so the terrain arrives but nothing simulates, and
        // it expires on its own if the join never completes.
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                AddTicketLocked(Game::Math::ChunkPos(chunk.x + dx, chunk.z + dz),
                                TicketType::PlayerSpawn, ChunkLevel::FULL,
                                lifespanTicks, {});
            }
        }
    }

    void ChunkTicketManager::AddForcedTicket(const std::string& identifier,
                                             Game::Math::ChunkPos chunk, int level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        AddTicketLocked(chunk, TicketType::Forced, level, -1, identifier);
    }

    void ChunkTicketManager::RemoveForcedTicket(const std::string& identifier,
                                                Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_mutex);
        RemoveTicketLocked(chunk, TicketType::Forced, identifier);
    }

    void ChunkTicketManager::AddTemporaryTicket(Game::Math::ChunkPos chunk, int level,
                                                int lifespanTicks) {
        std::lock_guard<std::mutex> lock(m_mutex);
        AddTicketLocked(chunk, TicketType::Temporary, level, lifespanTicks, {});
    }

    // ── Propagation ────────────────────────────────────────────────────────

    void ChunkTicketManager::RunAllUpdates() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_tracker->hasWork()) return;
        m_tracker->RunAll();
        if (!m_levelsChanged) return;
        m_levelsChanged = false;
        ++m_levelsVersion;

        // The one invariant worth asserting at runtime: a player's OWN chunk
        // is always entity-ticking — their ticket sits on it at
        // PlayerTicketLevel(simulation distance) <= ENTITY_TICKING, so this can
        // only fail if propagation is broken. It never fires in a healthy
        // world, which is what makes it worth keeping: the bug it guards
        // against (the player outside their own simulation range) froze every
        // entity around them for a whole session and was invisible from every
        // other vantage point — healthy TPS, chunks streaming, commands
        // answered. Checked here, after propagation, and not where the ticket
        // moves: levels only change in this call.
        for (const auto& [playerId, chunk] : m_playerTicketChunk) {
            const int level = GetChunkLevelLocked(chunk);
            if (!ChunkLevel::IsEntityTicking(level)) {
                Log::Warning("[Tickets] player %u at chunk (%d,%d) is NOT in its own "
                             "entity-ticking range (level=%d, needs <= %d) — chunk "
                             "level propagation is broken",
                             playerId, chunk.x, chunk.z, level, ChunkLevel::ENTITY_TICKING);
            }
        }
    }

    // ── Queries ────────────────────────────────────────────────────────────

    int ChunkTicketManager::GetChunkLevelLocked(Game::Math::ChunkPos chunk) const {
        const auto it = m_levels.find(chunk);
        return it == m_levels.end() ? ChunkLevel::UNLOADED : it->second;
    }

    int ChunkTicketManager::GetChunkLevel(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return GetChunkLevelLocked(chunk);
    }

    bool ChunkTicketManager::IsEntityTickingAfterUpdates(Game::Math::ChunkPos chunk) const {
        // Deliberately NO lock. See the header.
        const auto it = m_levels.find(chunk);
        return ChunkLevel::IsEntityTicking(it == m_levels.end() ? ChunkLevel::UNLOADED
                                                                : it->second);
    }

    bool ChunkTicketManager::IsEntityTicking(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return ChunkLevel::IsEntityTicking(GetChunkLevelLocked(chunk));
    }

    bool ChunkTicketManager::IsBlockTicking(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return ChunkLevel::IsBlockTicking(GetChunkLevelLocked(chunk));
    }

    bool ChunkTicketManager::ShouldChunkBeLoaded(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return ChunkLevel::IsLoaded(GetChunkLevelLocked(chunk));
    }

    std::vector<Game::Math::ChunkPos>
    ChunkTicketManager::CollectAtMostLocked(int maxLevel) const {
        std::vector<Game::Math::ChunkPos> out;
        out.reserve(m_levels.size());
        for (const auto& [chunk, level] : m_levels) {
            if (level <= maxLevel) out.push_back(chunk);
        }
        // SORTED, so the order does not depend on the hash function.
        //
        // This list is what the random-tick pass walks, and that pass draws
        // from the shared level RNG per chunk — so an unspecified iteration
        // order made the world's evolution a function of ChunkPosHash's
        // internals. Changing the hash (which was collapsing a whole region
        // onto a handful of buckets) would silently have reshuffled every
        // random tick in the world. Sorting removes the hash from the RNG path
        // entirely, which is worth more than the microseconds it costs on a
        // list of ~1,000 chunks.
        std::sort(out.begin(), out.end(),
                  [](const Game::Math::ChunkPos& a, const Game::Math::ChunkPos& b) {
                      return a.x != b.x ? a.x < b.x : a.z < b.z;
                  });
        return out;
    }

    std::vector<Game::Math::ChunkPos> ChunkTicketManager::GetLoadedChunks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return CollectAtMostLocked(ChunkLevel::MAX_LEVEL);
    }

    std::vector<Game::Math::ChunkPos> ChunkTicketManager::GetBlockTickingChunks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        RefreshTickingCachesLocked();
        return m_cachedBlockTicking;
    }

    std::vector<Game::Math::ChunkPos> ChunkTicketManager::GetRandomTickingChunks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        RefreshTickingCachesLocked();
        return m_cachedRandomTicking;
    }

    uint64_t ChunkTicketManager::LevelsVersion() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_levelsVersion;
    }

    void ChunkTicketManager::RefreshTickingCachesLocked() const {
        if (m_cacheVersion == m_levelsVersion) return;
        m_cacheVersion = m_levelsVersion;
        m_cachedBlockTicking = CollectAtMostLocked(ChunkLevel::BLOCK_TICKING);

        // Vanilla's reach: 32 chunks of entity ticking plus the block-ticking
        // margin. A player ticket at PlayerTicketLevel(d) puts the chunk at
        // Chebyshev distance k on level PlayerTicketLevel(d) + k, so within a
        // simulation distance of 32 every block-ticking chunk is within 33 of
        // a player anyway and the filter below changes nothing.
        constexpr int kVanillaMaxSimulationDistance = 32;
        const int reach = std::min(m_simulationDistance, kVanillaMaxSimulationDistance) + 1;
        std::vector<Game::Math::ChunkPos> players;
        players.reserve(m_playersPerChunk.size());
        for (const auto& [chunk, occupants] : m_playersPerChunk) players.push_back(chunk);
        // Non-player tickets strong enough to make chunks tick on their own
        // (portal far sides, forced chunks): their reach is what it is in MC.
        std::vector<std::pair<Game::Math::ChunkPos, int>> others;
        for (const auto& [chunk, list] : m_tickets) {
            for (const Ticket& t : list) {
                if (t.type != TicketType::PlayerSimulation && t.level <= ChunkLevel::BLOCK_TICKING)
                    others.emplace_back(chunk, t.level);
            }
        }
        const auto cheb = [](Game::Math::ChunkPos a, Game::Math::ChunkPos b) {
            return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z));
        };
        m_cachedRandomTicking.clear();
        m_cachedRandomTicking.reserve(m_cachedBlockTicking.size());
        for (const Game::Math::ChunkPos& c : m_cachedBlockTicking) {
            bool ticks = false;
            for (const Game::Math::ChunkPos& p : players) {
                if (cheb(c, p) <= reach) { ticks = true; break; }
            }
            if (!ticks) {
                for (const auto& [chunk, level] : others) {
                    if (level + cheb(c, chunk) <= ChunkLevel::BLOCK_TICKING) { ticks = true; break; }
                }
            }
            if (ticks) m_cachedRandomTicking.push_back(c);
        }
    }

    std::vector<Game::Math::ChunkPos> ChunkTicketManager::GetEntityTickingChunks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return CollectAtMostLocked(ChunkLevel::ENTITY_TICKING);
    }

    // ── Maintenance ────────────────────────────────────────────────────────

    void ChunkTicketManager::ProcessExpiredTickets(int64_t currentTick) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_currentTick = currentTick;

        for (auto it = m_tickets.begin(); it != m_tickets.end();) {
            auto& list = it->second;
            const Game::Math::ChunkPos chunk = it->first;
            int oldSourceLevel = ChunkLevel::UNLOADED;
            for (const Ticket& t : list) oldSourceLevel = std::min(oldSourceLevel, t.level);
            const size_t before = list.size();
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const Ticket& t) { return t.IsExpired(currentTick); }),
                       list.end());
            const bool expired = list.size() != before;
            it = list.empty() ? m_tickets.erase(it) : std::next(it);
            // After the erase: the tracker reads the chunk's tickets back
            // through TicketLevelAtLocked, and this iterator is done with it.
            if (expired) OnTicketsChangedLocked(chunk, oldSourceLevel);
        }
    }

    void ChunkTicketManager::Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tickets.clear();
        m_playersPerChunk.clear();
        m_playerTicketChunk.clear();
        m_levels.clear();
        // A fresh tracker: the old one's queue describes tickets that are gone.
        m_tracker = std::make_unique<LevelTracker>(*this);
        m_levelsChanged = false;
        ++m_levelsVersion;
    }

    ChunkTicketManager::Stats ChunkTicketManager::GetStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);

        Stats s{};
        for (const auto& [chunk, list] : m_tickets) {
            s.totalTickets += list.size();
            for (const Ticket& t : list) {
                if (t.type == TicketType::PlayerSimulation) ++s.playerTickets;
            }
        }
        for (const auto& [chunk, level] : m_levels) {
            if (ChunkLevel::IsLoaded(level))        ++s.loadedChunks;
            if (ChunkLevel::IsBlockTicking(level))  ++s.blockTickingChunks;
            if (ChunkLevel::IsEntityTicking(level)) ++s.entityTickingChunks;
        }
        return s;
    }

} // namespace Server
