#pragma once

#include <string>
#include <unordered_map>

// The Aether block-name resolution for the terrain library.
//
// The engine registers every ported Aether block as "minecraft:<slug>" (the
// mod's own name with the namespace swapped). Aether blocks still missing
// from the registry resolve to the nearest block the engine has — the
// STAND-IN chains in AetherBlocks.cpp — each logged once, so a block that
// lands later (Blocks.cpp) is picked up by name with no change here: the real
// slug is always tried first.
//
// Used by the Aether structure pieces (levelgen/structure/AetherStructures),
// the template loader ("aether:" palette names in the mod's NBT files) and
// the processor-list loader ("aether:" rule blocks and output states).

namespace minecraft {
namespace world { namespace level { namespace block {
class Block;
namespace state { class BlockState; }
}}}
using BlockState = world::level::block::state::BlockState;

namespace levelgen {
namespace aether_blocks {

/** True for an "aether:" id. */
bool isAetherId(const std::string& name);

/**
 * The registry id an Aether block resolves to: "aether:x" -> "minecraft:x"
 * when registered, else the first registered entry of its stand-in chain,
 * else "" (nothing fits — the caller decides). Thread-safe; cached.
 */
std::string resolveName(const std::string& name);

/** The default state of resolveName(name), or null. */
BlockState* defaultState(const std::string& name);

/**
 * The state of resolveName(name) matching `properties`: every property the
 * resolved block has AND accepts the value of is applied; the rest stay at
 * their defaults (the mod's double_drops, a stand-in's missing facing, …).
 * Null when nothing resolves.
 */
BlockState* state(const std::string& name,
                  const std::unordered_map<std::string, std::string>& properties);

/** True when `name` resolved to something other than its own slug. */
bool isStandIn(const std::string& name);

} // namespace aether_blocks
} // namespace levelgen
} // namespace minecraft
