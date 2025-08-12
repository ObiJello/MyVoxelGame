// File: src/server/world/ticketing/ChunkTicketManager.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <string>

namespace Server {

    // Ticket types matching Minecraft's system
    enum class TicketType {
        PLAYER,      // Player simulation distance
        SPAWN,       // World spawn chunks
        PORTAL,      // Nether portals
        DRAGON,      // Ender dragon fight
        FORCED,      // Forced chunks (command/API)
        TEMPORARY,   // Temporary for chunk operations
        START,       // Initial spawn area loading
        UNKNOWN      // Fallback type
    };

    // Individual ticket with level and source information
    template<typename T>
    struct Ticket {
        TicketType type;
        int level;           // Lower level = stronger ticket (keeps chunk loaded at higher level)
        T identifier;        // Type-specific identifier (e.g., playerId for PLAYER tickets)
        int64_t createdTick; // Server tick when created
        int lifespan;        // Ticks until expiry (-1 for permanent)

        Ticket(TicketType t, int l, T id, int64_t tick, int life = -1)
            : type(t), level(l), identifier(id), createdTick(tick), lifespan(life) {}

        bool IsExpired(int64_t currentTick) const {
            return lifespan > 0 && (currentTick - createdTick) >= lifespan;
        }

        bool operator==(const Ticket& other) const {
            return type == other.type && identifier == other.identifier;
        }
    };

    // Hash function for tickets
    template<typename T>
    struct TicketHash {
        size_t operator()(const Ticket<T>& ticket) const {
            size_t h1 = std::hash<int>()(static_cast<int>(ticket.type));
            size_t h2 = std::hash<T>()(ticket.identifier);
            return h1 ^ (h2 << 1);
        }
    };

    // Manages chunk loading tickets for the server
    class ChunkTicketManager {
    public:
        // Minecraft's chunk load levels
        // 33 = Entity ticking (full simulation)
        // 34 = Ticking (block ticks, no entities) 
        // 35 = Border (generation only)
        // 36+ = Inaccessible (can be unloaded)
        static constexpr int ENTITY_TICKING_LEVEL = 33;
        static constexpr int BLOCK_TICKING_LEVEL = 34;
        static constexpr int BORDER_LEVEL = 35;
        static constexpr int INACCESSIBLE_LEVEL = 36;
        static constexpr int MAX_LEVEL = 44;

        ChunkTicketManager();
        ~ChunkTicketManager();

        // === TICKET MANAGEMENT ===

        // Add a player ticket for simulation distance
        void AddPlayerTicket(uint32_t playerId, Game::Math::ChunkPos chunk, int level);
        
        // Remove a player ticket
        void RemovePlayerTicket(uint32_t playerId, Game::Math::ChunkPos chunk);
        
        // Remove all tickets for a player
        void RemoveAllPlayerTickets(uint32_t playerId);

        // Add spawn chunk tickets
        void AddSpawnTickets(Game::Math::ChunkPos spawnChunk, int radius);
        
        // Add a forced ticket (admin command or API)
        void AddForcedTicket(const std::string& identifier, Game::Math::ChunkPos chunk, int level);
        
        // Remove a forced ticket
        void RemoveForcedTicket(const std::string& identifier, Game::Math::ChunkPos chunk);

        // Add temporary ticket for operations
        void AddTemporaryTicket(Game::Math::ChunkPos chunk, int level, int lifespanTicks);

        // === LEVEL QUERIES ===

        // Get the strongest (lowest) ticket level for a chunk
        int GetChunkLevel(Game::Math::ChunkPos chunk) const;
        
        // Check if chunk should be loaded (level < INACCESSIBLE_LEVEL)
        bool ShouldChunkBeLoaded(Game::Math::ChunkPos chunk) const;
        
        // Check if chunk should tick blocks
        bool ShouldChunkTickBlocks(Game::Math::ChunkPos chunk) const;
        
        // Check if chunk should tick entities
        bool ShouldChunkTickEntities(Game::Math::ChunkPos chunk) const;

        // Get all chunks that should be loaded
        std::vector<Game::Math::ChunkPos> GetLoadedChunks() const;

        // === DISTANCE CALCULATIONS ===

        // Convert simulation distance to ticket level
        static int SimulationDistanceToLevel(int distance);
        
        // Convert ticket level to distance
        static int LevelToDistance(int level);
        
        // Calculate ticket level from chunk distance
        static int CalculateTicketLevel(int chunkDistance);

        // === MAINTENANCE ===

        // Process expired tickets (call each tick)
        void ProcessExpiredTickets(int64_t currentTick);
        
        // Update chunk levels after ticket changes
        void UpdateChunkLevels();

        // Get statistics
        struct Stats {
            size_t totalTickets;
            size_t playerTickets;
            size_t spawnTickets;
            size_t forcedTickets;
            size_t temporaryTickets;
            size_t loadedChunks;
            size_t tickingChunks;
            size_t entityTickingChunks;
        };
        Stats GetStats() const;

        // Clear all tickets
        void Clear();

    private:
        // Player tickets indexed by playerId
        using PlayerTicket = Ticket<uint32_t>;
        std::unordered_map<uint32_t, std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>> m_playerChunks;
        std::unordered_map<Game::Math::ChunkPos, std::unordered_set<PlayerTicket, TicketHash<uint32_t>>, 
                          Game::Math::ChunkPosHash> m_playerTickets;

        // Spawn tickets
        using SpawnTicket = Ticket<std::string>;
        std::unordered_map<Game::Math::ChunkPos, std::unordered_set<SpawnTicket, TicketHash<std::string>>, 
                          Game::Math::ChunkPosHash> m_spawnTickets;

        // Forced tickets indexed by identifier
        using ForcedTicket = Ticket<std::string>;
        std::unordered_map<std::string, std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash>> m_forcedChunks;
        std::unordered_map<Game::Math::ChunkPos, std::unordered_set<ForcedTicket, TicketHash<std::string>>, 
                          Game::Math::ChunkPosHash> m_forcedTickets;

        // Temporary tickets
        using TempTicket = Ticket<int64_t>;
        std::unordered_map<Game::Math::ChunkPos, std::unordered_set<TempTicket, TicketHash<int64_t>>, 
                          Game::Math::ChunkPosHash> m_temporaryTickets;

        // Computed chunk levels (cached)
        mutable std::unordered_map<Game::Math::ChunkPos, int, Game::Math::ChunkPosHash> m_chunkLevels;
        mutable bool m_levelsCacheDirty = true;

        // Synchronization
        mutable std::mutex m_mutex;

        // Current server tick
        int64_t m_currentTick = 0;

        // === INTERNAL HELPERS ===

        // Compute the minimum level from all tickets for a chunk
        int ComputeChunkLevel(Game::Math::ChunkPos chunk) const;
        
        // Remove expired temporary tickets for a chunk
        void RemoveExpiredTemporaryTickets(Game::Math::ChunkPos chunk);
        
        // Invalidate level cache
        void InvalidateLevelCache();
        
        // Rebuild level cache if needed
        void RebuildLevelCacheIfNeeded() const;
    };

} // namespace Server