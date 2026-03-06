#include "server/level/TicketType.h"

// Reference: net/minecraft/server/level/TicketType.java

namespace minecraft {
namespace server {
namespace level {

// Reference: TicketType.java lines 17-25
// Static ticket type definitions

const TicketType TicketType::PLAYER_SPAWN{20L, FLAG_LOADING, "player_spawn"};
const TicketType TicketType::SPAWN_SEARCH{1L, FLAG_LOADING, "spawn_search"};
const TicketType TicketType::DRAGON{0L, FLAG_LOADING | FLAG_SIMULATION, "dragon"};
const TicketType TicketType::PLAYER_LOADING{0L, FLAG_LOADING, "player_loading"};
const TicketType TicketType::PLAYER_SIMULATION{0L, FLAG_SIMULATION | FLAG_KEEP_DIMENSION_ACTIVE, "player_simulation"};
const TicketType TicketType::FORCED{0L, FLAG_PERSIST | FLAG_LOADING | FLAG_SIMULATION | FLAG_KEEP_DIMENSION_ACTIVE, "forced"};
const TicketType TicketType::PORTAL{300L, FLAG_PERSIST | FLAG_LOADING | FLAG_SIMULATION | FLAG_KEEP_DIMENSION_ACTIVE, "portal"};
const TicketType TicketType::ENDER_PEARL{40L, FLAG_LOADING | FLAG_SIMULATION | FLAG_KEEP_DIMENSION_ACTIVE, "ender_pearl"};
const TicketType TicketType::UNKNOWN{1L, FLAG_LOADING | FLAG_CAN_EXPIRE_IF_UNLOADED, "unknown"};

} // namespace level
} // namespace server
} // namespace minecraft
