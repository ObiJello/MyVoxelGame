#pragma once

#include <cstdint>
#include <string>

// Reference: net/minecraft/server/level/TicketType.java

namespace minecraft {
namespace server {
namespace level {

/**
 * TicketType - Defines types of chunk loading tickets with various flags
 * Reference: TicketType.java
 *
 * Tickets control chunk loading behavior:
 * - timeout: How long before the ticket expires (0 = no timeout)
 * - flags: Bitmask of behaviors (see FLAG_* constants)
 */
struct TicketType {
    int64_t timeout;
    int flags;
    std::string name;

    // Reference: TicketType.java lines 10-16
    static constexpr int64_t NO_TIMEOUT = 0L;
    static constexpr int FLAG_PERSIST = 1;              // Survives server restarts
    static constexpr int FLAG_LOADING = 2;              // Triggers chunk loading
    static constexpr int FLAG_SIMULATION = 4;           // Enables entity/block ticking
    static constexpr int FLAG_KEEP_DIMENSION_ACTIVE = 8; // Keeps dimension active
    static constexpr int FLAG_CAN_EXPIRE_IF_UNLOADED = 16; // Can expire before chunk loads

    TicketType() : timeout(0), flags(0), name("") {}
    TicketType(int64_t t, int f, const std::string& n) : timeout(t), flags(f), name(n) {}

    // Reference: TicketType.java lines 31-33
    bool persist() const {
        return (flags & FLAG_PERSIST) != 0;
    }

    // Reference: TicketType.java lines 35-37
    bool doesLoad() const {
        return (flags & FLAG_LOADING) != 0;
    }

    // Reference: TicketType.java lines 39-41
    bool doesSimulate() const {
        return (flags & FLAG_SIMULATION) != 0;
    }

    // Reference: TicketType.java lines 43-45
    bool shouldKeepDimensionActive() const {
        return (flags & FLAG_KEEP_DIMENSION_ACTIVE) != 0;
    }

    // Reference: TicketType.java lines 47-49
    bool canExpireIfUnloaded() const {
        return (flags & FLAG_CAN_EXPIRE_IF_UNLOADED) != 0;
    }

    // Reference: TicketType.java lines 51-53
    bool hasTimeout() const {
        return timeout != NO_TIMEOUT;
    }

    bool operator==(const TicketType& other) const {
        return name == other.name;
    }

    bool operator!=(const TicketType& other) const {
        return !(*this == other);
    }

    // Reference: TicketType.java lines 17-25
    // Standard ticket types
    static const TicketType PLAYER_SPAWN;       // timeout=20, flags=2 (LOADING)
    static const TicketType SPAWN_SEARCH;       // timeout=1, flags=2 (LOADING)
    static const TicketType DRAGON;             // timeout=0, flags=6 (LOADING | SIMULATION)
    static const TicketType PLAYER_LOADING;     // timeout=0, flags=2 (LOADING)
    static const TicketType PLAYER_SIMULATION;  // timeout=0, flags=12 (SIMULATION | KEEP_DIMENSION_ACTIVE)
    static const TicketType FORCED;             // timeout=0, flags=15 (PERSIST | LOADING | SIMULATION | KEEP_DIMENSION_ACTIVE)
    static const TicketType PORTAL;             // timeout=300, flags=15 (PERSIST | LOADING | SIMULATION | KEEP_DIMENSION_ACTIVE)
    static const TicketType ENDER_PEARL;        // timeout=40, flags=14 (LOADING | SIMULATION | KEEP_DIMENSION_ACTIVE)
    static const TicketType UNKNOWN;            // timeout=1, flags=18 (LOADING | CAN_EXPIRE_IF_UNLOADED)
};

// Static definitions (defined in cpp file)
// Reference: TicketType.java lines 17-25

} // namespace level
} // namespace server
} // namespace minecraft
