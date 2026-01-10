#pragma once

#include "server/level/Ticket.h"
#include "server/level/TicketType.h"
#include "server/level/ChunkLevel.h"
#include "world/ChunkPos.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <cstdint>

// Reference: net/minecraft/world/level/TicketStorage.java

// Forward declarations
namespace minecraft {
namespace server {
namespace level {
class ChunkMap;
class ChunkHolder;
}
}
}

namespace minecraft {
namespace world {
namespace level {

/**
 * TicketStorage - Manages chunk loading tickets
 * Reference: TicketStorage.java
 *
 * This class stores and manages tickets that control chunk loading.
 * Tickets have types, levels, and timeouts.
 */
class TicketStorage {
public:
    // Callback type for chunk level updates
    // Reference: TicketStorage.java lines 429-431
    using ChunkUpdated = std::function<void(int64_t node, int newLevel, bool onlyDecreased)>;

    // Predicate for filtering tickets
    // Reference: TicketStorage.java lines 433-435
    using TicketPredicate = std::function<bool(const server::level::Ticket&, int64_t chunkPos)>;

    // Reference: TicketStorage.java lines 51-53
    TicketStorage();

    // Reference: TicketStorage.java lines 97-109
    void activateAllDeactivatedTickets();

    // Reference: TicketStorage.java lines 111-113
    void setLoadingChunkUpdatedListener(ChunkUpdated listener);

    // Reference: TicketStorage.java lines 115-117
    void setSimulationChunkUpdatedListener(ChunkUpdated listener);

    // Reference: TicketStorage.java lines 119-121
    bool hasTickets() const;

    // Reference: TicketStorage.java lines 123-137
    bool shouldKeepDimensionActive() const;

    // Reference: TicketStorage.java lines 139-141
    const std::vector<server::level::Ticket>& getTickets(int64_t key) const;

    // Reference: TicketStorage.java lines 147-149
    void addTicketWithRadius(const server::level::TicketType& type,
                             const ChunkPos& chunkPos, int radius);

    // Reference: TicketStorage.java lines 152-154
    void addTicket(const server::level::Ticket& ticket, const ChunkPos& chunkPos);

    // Reference: TicketStorage.java lines 156-188
    bool addTicket(int64_t key, const server::level::Ticket& ticket);

    // Reference: TicketStorage.java lines 194-196
    int getTicketLevelAt(int64_t key, bool simulation) const;

    // Reference: TicketStorage.java lines 223-226
    void removeTicketWithRadius(const server::level::TicketType& type,
                                const ChunkPos& chunkPos, int radius);

    // Reference: TicketStorage.java lines 228-230
    void removeTicket(const server::level::Ticket& ticket, const ChunkPos& chunkPos);

    // Reference: TicketStorage.java lines 232-276
    bool removeTicket(int64_t key, const server::level::Ticket& ticket);

    // Reference: TicketStorage.java lines 282-286
    std::string getTicketDebugString(int64_t key, bool simulation) const;

    // Reference: TicketStorage.java lines 288-298
    void purgeStaleTickets(server::level::ChunkMap& chunkMap);

    // Reference: TicketStorage.java lines 311-313
    void deactivateTicketsOnClosing();

    // Reference: TicketStorage.java lines 315-369
    void removeTicketIf(TicketPredicate predicate,
                        std::unordered_map<int64_t, std::vector<server::level::Ticket>>* removedTickets = nullptr);

    // Reference: TicketStorage.java lines 371-393
    void replaceTicketLevelOfType(int newLevel, const server::level::TicketType& ticketType);

    // Reference: TicketStorage.java lines 395-398
    bool updateChunkForced(const ChunkPos& chunkPos, bool forced);

    // Reference: TicketStorage.java lines 400-402
    const std::unordered_set<int64_t>& getForceLoadedChunks() const;

    // Mark storage as dirty (needs saving)
    void setDirty() { m_dirty = true; }
    bool isDirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }

private:
    static constexpr int INITIAL_TICKET_LIST_CAPACITY = 4;

    // Reference: TicketStorage.java lines 143-145
    std::vector<server::level::Ticket>& getOrCreateTickets(int64_t key);

    // Reference: TicketStorage.java lines 198-221
    static int getTicketLevelAt(const std::vector<server::level::Ticket>& tickets, bool simulation);

    // Reference: TicketStorage.java lines 203-220
    static const server::level::Ticket* getLowestTicket(
        const std::vector<server::level::Ticket>* tickets, bool simulation);

    // Reference: TicketStorage.java lines 278-280
    void updateForcedChunks();

    // Reference: TicketStorage.java lines 404-420
    std::unordered_set<int64_t> getAllChunksWithTicketThat(
        std::function<bool(const server::level::Ticket&)> ticketCheck) const;

    // Reference: TicketStorage.java lines 300-309
    bool canTicketExpire(server::level::ChunkMap& chunkMap,
                         const server::level::Ticket& ticket, int64_t chunkPos) const;

    std::unordered_map<int64_t, std::vector<server::level::Ticket>> m_tickets;
    std::unordered_map<int64_t, std::vector<server::level::Ticket>> m_deactivatedTickets;
    std::unordered_set<int64_t> m_chunksWithForcedTickets;
    ChunkUpdated m_loadingChunkUpdatedListener;
    ChunkUpdated m_simulationChunkUpdatedListener;
    bool m_dirty = false;

    // Empty vector for returning when no tickets exist
    static const std::vector<server::level::Ticket> s_emptyTicketList;
};

} // namespace level
} // namespace world
} // namespace minecraft
