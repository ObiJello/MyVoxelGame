#pragma once

#include <string>
#include <unordered_map>

// Twilight Forest block-name resolution for the terrain library.
//
// The engine registers every ported TF block as "minecraft:<slug>" (the mod's
// own name with the namespace swapped), except where the vanilla name is
// taken (TF's mangrove set is tf_mangrove_*). TF blocks still missing from the
// registry resolve to the nearest block the engine has — the STAND-IN table
// in TwilightBlocks.cpp — and each stand-in is logged once, so a block that
// lands later (another pass adds it to Blocks.cpp) is picked up by name with
// no change here: the real slug is always tried first.
//
// Used by the TF features (data/worldgen/features/Twilight*), the TF
// structure pieces (levelgen/structure/Twilight*) and the template loader
// (TemplateEngine: "twilightforest:" palette names in the mod's NBT files).

namespace minecraft {
namespace world { namespace level { namespace block {
class Block;
namespace state { class BlockState; }
}}}
using BlockState = world::level::block::state::BlockState;

namespace levelgen {
namespace twilight_blocks {

/**
 * The registry id a TF block resolves to: "twilightforest:x" / "x" ->
 * "minecraft:<renamed x>" when registered, else the first registered entry of
 * its stand-in chain, else "" (nothing fits — the caller decides). Plain
 * "minecraft:" names pass through a small rename table (26.x names the mod's
 * NBT still uses, e.g. minecraft:chain -> minecraft:iron_chain) and then the
 * same stand-in chain. Thread-safe; results are cached.
 */
std::string resolveName(const std::string& name);

/** The default state of resolveName(name), or null (logged once). */
BlockState* defaultState(const std::string& name);

/**
 * The state of resolveName(name) that best matches `properties`: every
 * property the resolved block also has is applied (values it does not accept
 * are skipped); properties it lacks are dropped. Stand-ins therefore keep
 * what they can (a banister stand-in fence keeps waterlogged, a stairs
 * stand-in keeps facing/half/shape). Null when nothing resolves.
 */
BlockState* state(const std::string& name,
                  const std::unordered_map<std::string, std::string>& properties);

/** True when `name` resolved to something other than its own slug. */
bool isStandIn(const std::string& name);

} // namespace twilight_blocks
} // namespace levelgen
} // namespace minecraft
