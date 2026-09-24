// File: src/common/world/block/RedstonePlus.hpp
//
// The redstone_plus engine rule: redstone freed from its physical limits so
// large logic builds get small and fast. Off = vanilla, exactly. On:
//
//   * A dust network carries the strongest signal fed into it to EVERY cell,
//     with no decay per block. There is no 15-block range, so a wire never
//     needs a repeater to go further, and a whole lane switches in one
//     evaluation instead of one repeater delay per 15 blocks. Comparator
//     arithmetic (signal strength as a value) survives any distance too.
//   * Redstone torches never burn out.
//
// Everything else (component delays, connection shapes, what powers what)
// is unchanged, so a vanilla contraption behaves the same except that its
// signals reach further. Server-wide, kept per world in level.dat's
// `obeycraft` compound (a key Minecraft does not know inside `game_rules`
// would fail its decode); set with /gamerule redstone_plus.
//
// The flag is read on the server thread by the wire evaluator and the
// torch tick; set from the server thread by the rule. A change takes effect
// as circuits update: a network is re-evaluated the next time anything in
// it changes.
#pragma once

#include <atomic>

namespace Game::RedstonePlus {

    inline std::atomic<bool> g_enabled{false};

    inline bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
    inline void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

} // namespace Game::RedstonePlus
