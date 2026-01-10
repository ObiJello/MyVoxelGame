#include "world/level/TicketStorage.h"
#include "server/level/ChunkLevel.h"
#include "server/level/FullChunkStatus.h"
#include "server/level/ChunkMap.h"
#include "server/level/ChunkHolder.h"
#include <algorithm>
#include <limits>

// Reference: net/minecraft/world/level/TicketStorage.java

namespace minecraft {
namespace world {
namespace level {

using namespace server::level;

// Static empty ticket list
const std::vector<Ticket> TicketStorage::s_emptyTicketList;

// Reference: TicketStorage.java lines 51-53
TicketStorage::TicketStorage() {
    updateForcedChunks();
}

// Reference: TicketStorage.java lines 97-109
void TicketStorage::activateAllDeactivatedTickets() {
    for (const auto& entry : m_deactivatedTickets) {
        for (const Ticket& ticket : entry.second) {
            addTicket(entry.first, ticket);
        }
    }
    m_deactivatedTickets.clear();
}

// Reference: TicketStorage.java lines 111-113
void TicketStorage::setLoadingChunkUpdatedListener(ChunkUpdated listener) {
    m_loadingChunkUpdatedListener = std::move(listener);
}

// Reference: TicketStorage.java lines 115-117
void TicketStorage::setSimulationChunkUpdatedListener(ChunkUpdated listener) {
    m_simulationChunkUpdatedListener = std::move(listener);
}

// Reference: TicketStorage.java lines 119-121
bool TicketStorage::hasTickets() const {
    return !m_tickets.empty();
}

// Reference: TicketStorage.java lines 123-137
bool TicketStorage::shouldKeepDimensionActive() const {
    for (const auto& entry : m_tickets) {
        for (const Ticket& ticket : entry.second) {
            if (ticket.getType().shouldKeepDimensionActive()) {
                return true;
            }
        }
    }
    return false;
}

// Reference: TicketStorage.java lines 139-141
const std::vector<Ticket>& TicketStorage::getTickets(int64_t key) const {
    auto it = m_tickets.find(key);
    if (it != m_tickets.end()) {
        return it->second;
    }
    return s_emptyTicketList;
}

// Reference: TicketStorage.java lines 143-145
std::vector<Ticket>& TicketStorage::getOrCreateTickets(int64_t key) {
    auto& tickets = m_tickets[key];
    if (tickets.empty()) {
        tickets.reserve(INITIAL_TICKET_LIST_CAPACITY);
    }
    return tickets;
}

// Reference: TicketStorage.java lines 147-149
void TicketStorage::addTicketWithRadius(const TicketType& type,
                                         const ChunkPos& chunkPos, int radius) {
    Ticket ticket(type, ChunkLevel::byStatus(FullChunkStatus::FULL) - radius);
    addTicket(chunkPos.toLong(), ticket);
}

// Reference: TicketStorage.java lines 152-154
void TicketStorage::addTicket(const Ticket& ticket, const ChunkPos& chunkPos) {
    addTicket(chunkPos.toLong(), ticket);
}

// Reference: TicketStorage.java lines 156-188
bool TicketStorage::addTicket(int64_t key, const Ticket& ticket) {
    auto& tickets = getOrCreateTickets(key);

    // Check if ticket already exists (same type and level)
    for (Ticket& t : tickets) {
        if (isTicketSameTypeAndLevel(ticket, t)) {
            t.resetTicksLeft();
            setDirty();
            return false;
        }
    }

    int oldSimulationTicketLevel = getTicketLevelAt(tickets, true);
    int oldLoadingTicketLevel = getTicketLevelAt(tickets, false);
    tickets.push_back(ticket);

    // Notify listeners if level decreased (lower is higher priority)
    if (ticket.getType().doesSimulate() &&
        ticket.getTicketLevel() < oldSimulationTicketLevel &&
        m_simulationChunkUpdatedListener) {
        m_simulationChunkUpdatedListener(key, ticket.getTicketLevel(), true);
    }

    if (ticket.getType().doesLoad() &&
        ticket.getTicketLevel() < oldLoadingTicketLevel &&
        m_loadingChunkUpdatedListener) {
        m_loadingChunkUpdatedListener(key, ticket.getTicketLevel(), true);
    }

    if (ticket.getType() == TicketType::FORCED) {
        m_chunksWithForcedTickets.insert(key);
    }

    setDirty();
    return true;
}

// Reference: TicketStorage.java lines 198-201
int TicketStorage::getTicketLevelAt(int64_t key, bool simulation) const {
    return getTicketLevelAt(getTickets(key), simulation);
}

// Reference: TicketStorage.java lines 198-221
int TicketStorage::getTicketLevelAt(const std::vector<Ticket>& tickets, bool simulation) {
    const Ticket* lowestTicket = getLowestTicket(&tickets, simulation);
    return lowestTicket == nullptr ? ChunkLevel::getMaxLevel() + 1 : lowestTicket->getTicketLevel();
}

// Reference: TicketStorage.java lines 203-220
const Ticket* TicketStorage::getLowestTicket(
    const std::vector<Ticket>* tickets, bool simulation) {
    if (tickets == nullptr || tickets->empty()) {
        return nullptr;
    }

    const Ticket* lowest = nullptr;
    for (const Ticket& ticket : *tickets) {
        if (lowest == nullptr || ticket.getTicketLevel() < lowest->getTicketLevel()) {
            if (simulation && ticket.getType().doesSimulate()) {
                lowest = &ticket;
            } else if (!simulation && ticket.getType().doesLoad()) {
                lowest = &ticket;
            }
        }
    }
    return lowest;
}

// Reference: TicketStorage.java lines 223-226
void TicketStorage::removeTicketWithRadius(const TicketType& type,
                                            const ChunkPos& chunkPos, int radius) {
    Ticket ticket(type, ChunkLevel::byStatus(FullChunkStatus::FULL) - radius);
    removeTicket(chunkPos.toLong(), ticket);
}

// Reference: TicketStorage.java lines 228-230
void TicketStorage::removeTicket(const Ticket& ticket, const ChunkPos& chunkPos) {
    removeTicket(chunkPos.toLong(), ticket);
}

// Reference: TicketStorage.java lines 232-276
bool TicketStorage::removeTicket(int64_t key, const Ticket& ticket) {
    auto it = m_tickets.find(key);
    if (it == m_tickets.end()) {
        return false;
    }

    auto& tickets = it->second;
    bool found = false;

    for (auto ticketIt = tickets.begin(); ticketIt != tickets.end(); ++ticketIt) {
        if (isTicketSameTypeAndLevel(ticket, *ticketIt)) {
            tickets.erase(ticketIt);
            found = true;
            break;
        }
    }

    if (!found) {
        return false;
    }

    if (tickets.empty()) {
        m_tickets.erase(it);
    }

    if (ticket.getType().doesSimulate() && m_simulationChunkUpdatedListener) {
        m_simulationChunkUpdatedListener(key, getTicketLevelAt(tickets, true), false);
    }

    if (ticket.getType().doesLoad() && m_loadingChunkUpdatedListener) {
        m_loadingChunkUpdatedListener(key, getTicketLevelAt(tickets, false), false);
    }

    if (ticket.getType() == TicketType::FORCED) {
        updateForcedChunks();
    }

    setDirty();
    return true;
}

// Reference: TicketStorage.java lines 278-280
void TicketStorage::updateForcedChunks() {
    m_chunksWithForcedTickets = getAllChunksWithTicketThat(
        [](const Ticket& t) { return t.getType() == TicketType::FORCED; });
}

// Reference: TicketStorage.java lines 282-286
std::string TicketStorage::getTicketDebugString(int64_t key, bool simulation) const {
    const auto& tickets = getTickets(key);
    const Ticket* lowestTicket = getLowestTicket(&tickets, simulation);
    return lowestTicket == nullptr ? "no_ticket" : lowestTicket->toString();
}

// Reference: TicketStorage.java lines 288-298
void TicketStorage::purgeStaleTickets(ChunkMap& chunkMap) {
    // First pass: decrease ticks on expirable tickets
    for (auto& entry : m_tickets) {
        int64_t chunkPos = entry.first;
        for (Ticket& ticket : entry.second) {
            if (canTicketExpire(chunkMap, ticket, chunkPos)) {
                ticket.decreaseTicksLeft();
            }
        }
    }

    // Second pass: remove timed out tickets
    removeTicketIf([this, &chunkMap](const Ticket& ticket, int64_t chunkPos) {
        if (canTicketExpire(chunkMap, ticket, chunkPos)) {
            return ticket.isTimedOut();
        }
        return false;
    }, nullptr);
    setDirty();
}

// Reference: TicketStorage.java lines 300-309
bool TicketStorage::canTicketExpire(ChunkMap& chunkMap,
                                     const Ticket& ticket, int64_t chunkPos) const {
    if (!ticket.getType().hasTimeout()) {
        return false;
    }
    if (ticket.getType().canExpireIfUnloaded()) {
        return true;
    }
    // Check if chunk is ready for saving
    server::level::ChunkHolder* updatingChunk = chunkMap.getUpdatingChunkIfPresent(chunkPos);
    return updatingChunk == nullptr || updatingChunk->isReadyForSaving();
}

// Reference: TicketStorage.java lines 311-313
void TicketStorage::deactivateTicketsOnClosing() {
    removeTicketIf([](const Ticket& ticket, int64_t) {
        return ticket.getType() != TicketType::UNKNOWN;
    }, &m_deactivatedTickets);
}

// Reference: TicketStorage.java lines 315-369
void TicketStorage::removeTicketIf(TicketPredicate predicate,
                                    std::unordered_map<int64_t, std::vector<Ticket>>* removedTickets) {
    bool removedForced = false;
    std::vector<int64_t> emptyChunks;

    for (auto& entry : m_tickets) {
        int64_t chunkPos = entry.first;
        auto& tickets = entry.second;
        bool removedSimulation = false;
        bool removedLoading = false;

        for (auto it = tickets.begin(); it != tickets.end(); ) {
            if (predicate(*it, chunkPos)) {
                if (removedTickets != nullptr) {
                    (*removedTickets)[chunkPos].push_back(*it);
                }

                if (it->getType().doesLoad()) {
                    removedLoading = true;
                }
                if (it->getType().doesSimulate()) {
                    removedSimulation = true;
                }
                if (it->getType() == TicketType::FORCED) {
                    removedForced = true;
                }

                it = tickets.erase(it);
            } else {
                ++it;
            }
        }

        if (removedLoading || removedSimulation) {
            if (removedLoading && m_loadingChunkUpdatedListener) {
                m_loadingChunkUpdatedListener(chunkPos, getTicketLevelAt(tickets, false), false);
            }
            if (removedSimulation && m_simulationChunkUpdatedListener) {
                m_simulationChunkUpdatedListener(chunkPos, getTicketLevelAt(tickets, true), false);
            }
            setDirty();
        }

        if (tickets.empty()) {
            emptyChunks.push_back(chunkPos);
        }
    }

    // Remove empty ticket lists
    for (int64_t pos : emptyChunks) {
        m_tickets.erase(pos);
    }

    if (removedForced) {
        updateForcedChunks();
    }
}

// Reference: TicketStorage.java lines 371-393
void TicketStorage::replaceTicketLevelOfType(int newLevel, const TicketType& ticketType) {
    std::vector<std::pair<Ticket, int64_t>> affectedTickets;

    for (const auto& entry : m_tickets) {
        for (const Ticket& ticket : entry.second) {
            if (ticket.getType() == ticketType) {
                affectedTickets.emplace_back(ticket, entry.first);
            }
        }
    }

    for (const auto& pair : affectedTickets) {
        int64_t key = pair.second;
        const Ticket& ticket = pair.first;
        removeTicket(key, ticket);
        addTicket(key, Ticket(ticket.getType(), newLevel));
    }
}

// Reference: TicketStorage.java lines 395-398
bool TicketStorage::updateChunkForced(const ChunkPos& chunkPos, bool forced) {
    // Note: FORCED_TICKET_LEVEL should come from ChunkMap, using ENTITY_TICKING level for now
    Ticket ticket(TicketType::FORCED, ChunkLevel::byStatus(FullChunkStatus::ENTITY_TICKING));
    return forced ? addTicket(chunkPos.toLong(), ticket) : removeTicket(chunkPos.toLong(), ticket);
}

// Reference: TicketStorage.java lines 400-402
const std::unordered_set<int64_t>& TicketStorage::getForceLoadedChunks() const {
    return m_chunksWithForcedTickets;
}

// Reference: TicketStorage.java lines 404-420
std::unordered_set<int64_t> TicketStorage::getAllChunksWithTicketThat(
    std::function<bool(const Ticket&)> ticketCheck) const {
    std::unordered_set<int64_t> chunks;

    for (const auto& entry : m_tickets) {
        for (const Ticket& ticket : entry.second) {
            if (ticketCheck(ticket)) {
                chunks.insert(entry.first);
                break;
            }
        }
    }

    return chunks;
}

} // namespace level
} // namespace world
} // namespace minecraft
