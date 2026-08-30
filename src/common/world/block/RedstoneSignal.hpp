// File: src/common/world/block/RedstoneSignal.hpp
//
// MC Level.hasNeighborSignal — "is anything next to this block powering it?"
//
// THIS IS A SEAM, NOT AN IMPLEMENTATION. There is no redstone power simulation
// in this engine: RedstoneWire.hpp:8-12 records that POWER is held at 0 and
// the wire block models only its visual connections. So this answers false,
// always, and TNT cannot be lit by a lever.
//
// It exists as a named function anyway, and the TNT hooks already call it, for
// one reason: when redstone lands, making a lever light TNT is a change to
// THIS function's body and nothing else. Without the seam it would be a hunt
// through TntBlock, and then through every other block that wants the same
// question, for the places that need re-wiring.
//
// When implementing: MC's rule is `getSignal(pos.relative(dir), dir) > 0` for
// all six directions, where getSignal covers direct power (levers, buttons,
// pressure plates, redstone blocks, torches) AND weak power conducted through
// a solid block. TNT only needs the "> 0" answer, not the strength.
#pragma once

#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;

    // Always false today — see the file note.
    bool HasNeighborSignal(const IBlockAccess& level, const glm::ivec3& pos);

} // namespace Game
