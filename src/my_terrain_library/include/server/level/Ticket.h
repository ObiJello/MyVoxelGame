#pragma once

#include "server/level/TicketType.h"
#include <cstdint>
#include <string>

// Reference: net/minecraft/server/level/Ticket.java

namespace minecraft {
namespace server {
namespace level {

/**
 * Ticket - A chunk loading ticket with type, level, and timeout
 * Reference: Ticket.java
 *
 * Tickets are used to control chunk loading priority and lifetime.
 * Lower ticket levels = higher priority for loading.
 */
class Ticket {
public:
    // Reference: Ticket.java lines 16-18
    Ticket(const TicketType& type, int ticketLevel)
        : m_type(&type)
        , m_ticketLevel(ticketLevel)
        , m_ticksLeft(type.timeout)
    {}

    // Reference: Ticket.java lines 20-24
    Ticket(const TicketType& type, int ticketLevel, int64_t ticksLeft)
        : m_type(&type)
        , m_ticketLevel(ticketLevel)
        , m_ticksLeft(ticksLeft)
    {}

    // Reference: Ticket.java lines 36-38
    const TicketType& getType() const {
        return *m_type;
    }

    // Reference: Ticket.java lines 40-42
    int getTicketLevel() const {
        return m_ticketLevel;
    }

    // Reference: Ticket.java lines 44-46
    void resetTicksLeft() {
        m_ticksLeft = m_type->timeout;
    }

    // Reference: Ticket.java lines 48-53
    void decreaseTicksLeft() {
        if (m_type->hasTimeout()) {
            --m_ticksLeft;
        }
    }

    // Reference: Ticket.java lines 55-57
    bool isTimedOut() const {
        return m_type->hasTimeout() && m_ticksLeft < 0;
    }

    int64_t getTicksLeft() const {
        return m_ticksLeft;
    }

    // Reference: Ticket.java lines 26-34
    std::string toString() const {
        if (m_type->hasTimeout()) {
            return "Ticket[" + m_type->name + " " + std::to_string(m_ticketLevel) +
                   "] with " + std::to_string(m_ticksLeft) + " ticks left (out of " +
                   std::to_string(m_type->timeout) + ")";
        } else {
            return "Ticket[" + m_type->name + " " + std::to_string(m_ticketLevel) +
                   "] with no timeout";
        }
    }

private:
    const TicketType* m_type;
    int m_ticketLevel;
    int64_t m_ticksLeft;
};

/**
 * Helper function to check if two tickets have same type and level
 * Reference: TicketStorage.java lines 190-192
 */
inline bool isTicketSameTypeAndLevel(const Ticket& ticket, const Ticket& other) {
    return ticket.getType() == other.getType() &&
           ticket.getTicketLevel() == other.getTicketLevel();
}

} // namespace level
} // namespace server
} // namespace minecraft
