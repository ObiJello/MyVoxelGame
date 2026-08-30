// File: src/server/world/ticketing/ChunkTicketManager.cpp
#include "server/world/ticketing/ChunkTicketManager.hpp"

#include "common/core/Log.hpp"

#include <algorithm>
#include <deque>

namespace Server {

    ChunkTicketManager::ChunkTicketManager() = default;
    ChunkTicketManager::~ChunkTicketManager() = default;

    // ── Ticket mutation ────────────────────────────────────────────────────

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
        list.push_back(Ticket{type, level, m_currentTick, lifespan, identifier, 1});
        m_dirty = true;
    }

    void ChunkTicketManager::RemoveTicketLocked(Game::Math::ChunkPos chunk, TicketType type,
                                                const std::string& identifier) {
        auto it = m_tickets.find(chunk);
        if (it == m_tickets.end()) return;

        auto& list = it->second;
        for (auto t = list.begin(); t != list.end(); ++t) {
            if (t->type != type || t->identifier != identifier) continue;
            if (--t->refCount > 0) return;   // someone else still holds it
            list.erase(t);
            m_dirty = true;
            break;
        }
        if (list.empty()) m_tickets.erase(it);
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
            for (Ticket& t : list) {
                if (t.type == TicketType::PlayerSimulation) t.level = level;
            }
        }
        m_dirty = true;
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

    // ── The solve ──────────────────────────────────────────────────────────

    void ChunkTicketManager::SolveIfDirty() const {
        if (!m_dirty) return;
        m_dirty = false;
        m_levels.clear();

        // Multi-source BFS by level. Every edge costs exactly +1 over the 3x3
        // neighbourhood, so processing sources in increasing level order and
        // expanding level-by-level reaches every chunk with its minimum first —
        // the same fixed point MC's DynamicGraphMinFixedPoint converges to.
        //
        // Bounded at MAX_LEVEL: past that a chunk is INACCESSIBLE and there is
        // nothing to record.
        std::vector<std::pair<Game::Math::ChunkPos, int>> sources;
        sources.reserve(m_tickets.size());
        for (const auto& [chunk, list] : m_tickets) {
            int best = ChunkLevel::UNLOADED;
            for (const Ticket& t : list) best = std::min(best, t.level);
            if (best <= ChunkLevel::MAX_LEVEL) sources.emplace_back(chunk, best);
        }
        if (sources.empty()) return;

        std::sort(sources.begin(), sources.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });

        std::deque<Game::Math::ChunkPos> frontier;
        size_t nextSource = 0;
        int currentLevel = sources.front().second;

        const auto seed = [&](int level) {
            while (nextSource < sources.size() && sources[nextSource].second == level) {
                const auto& [chunk, lvl] = sources[nextSource++];
                auto it = m_levels.find(chunk);
                if (it == m_levels.end() || lvl < it->second) {
                    m_levels[chunk] = lvl;
                    frontier.push_back(chunk);
                }
            }
        };

        seed(currentLevel);

        while (!frontier.empty()) {
            const size_t widthOfThisLevel = frontier.size();
            for (size_t i = 0; i < widthOfThisLevel; ++i) {
                const Game::Math::ChunkPos chunk = frontier.front();
                frontier.pop_front();
                if (m_levels[chunk] != currentLevel) continue;   // superseded
                if (currentLevel >= ChunkLevel::MAX_LEVEL) continue;

                const int nextLevel = currentLevel + 1;
                for (int dx = -1; dx <= 1; ++dx) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        if (dx == 0 && dz == 0) continue;
                        const Game::Math::ChunkPos n(chunk.x + dx, chunk.z + dz);
                        auto it = m_levels.find(n);
                        if (it != m_levels.end() && it->second <= nextLevel) continue;
                        m_levels[n] = nextLevel;
                        frontier.push_back(n);
                    }
                }
            }
            ++currentLevel;
            seed(currentLevel);   // sources that start at this level join now
        }
    }

    // ── Queries ────────────────────────────────────────────────────────────

    int ChunkTicketManager::GetChunkLevelLocked(Game::Math::ChunkPos chunk) const {
        SolveIfDirty();
        const auto it = m_levels.find(chunk);
        return it == m_levels.end() ? ChunkLevel::UNLOADED : it->second;
    }

    int ChunkTicketManager::GetChunkLevel(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return GetChunkLevelLocked(chunk);
    }

    void ChunkTicketManager::RunAllUpdates() {
        std::lock_guard<std::mutex> lock(m_mutex);
        SolveIfDirty();
    }

    bool ChunkTicketManager::IsEntityTickingAfterUpdates(Game::Math::ChunkPos chunk) const {
        // Deliberately NO lock and NO SolveIfDirty. See the header.
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
        SolveIfDirty();
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
        return CollectAtMostLocked(ChunkLevel::BLOCK_TICKING);
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
            const size_t before = list.size();
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const Ticket& t) { return t.IsExpired(currentTick); }),
                       list.end());
            if (list.size() != before) m_dirty = true;
            it = list.empty() ? m_tickets.erase(it) : std::next(it);
        }
    }

    void ChunkTicketManager::Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tickets.clear();
        m_playersPerChunk.clear();
        m_playerTicketChunk.clear();
        m_levels.clear();
        m_dirty = true;
    }

    ChunkTicketManager::Stats ChunkTicketManager::GetStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        SolveIfDirty();

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
