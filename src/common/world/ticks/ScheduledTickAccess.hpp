// File: src/common/world/ticks/ScheduledTickAccess.hpp
//
// MC net.minecraft.world.level.ScheduledTickAccess — the narrow "I can book an
// appointment" interface that block callbacks are handed.
//
// It is separate from ILevelWrite on purpose, mirroring vanilla: MC's
// `updateShape` receives a read-only `LevelReader` plus a `ScheduledTickAccess`,
// because the shape update is allowed to schedule work but not to write blocks.
// Handing those callbacks a full writable level would let them do the one thing
// the neighbour-update pass must not do — recurse into setBlock mid-walk.
//
// A NULL ScheduledTickAccess* means "you cannot schedule here", which is the
// honest answer on the client: MC hands its ClientLevel a BlackholeTickAccess
// that silently swallows every scheduleTick, because block ticks are server
// authority and a client that ran them would desync the moment its prediction
// disagreed. Callbacks must null-check rather than assume.
#pragma once

#include "common/world/block/Blocks.hpp"
#include "common/world/ticks/ScheduledTick.hpp"

#include <glm/glm.hpp>

namespace Game {

    struct ScheduledTickAccess {
        virtual ~ScheduledTickAccess() = default;

        // MC LevelAccessor.scheduleTick(pos, block, delay, priority).
        //
        // `delay` is in ticks from now and is clamped at 0 — MC allows a
        // same-tick schedule, which lands in the current drain pass rather than
        // being lost.
        //
        // Scheduling over an existing (pos, block) appointment is a NO-OP, not
        // a reschedule. See the uniqueness note in ScheduledTick.hpp.
        virtual void ScheduleTick(const glm::ivec3& pos, BlockID block, int delay,
                                  TickPriority priority = TickPriority::Normal) = 0;

        // MC LevelTickAccess.hasScheduledTick. Blocks that want "only act if
        // nothing is already pending" ask this instead of scheduling blindly.
        virtual bool HasScheduledTick(const glm::ivec3& pos, BlockID block) const = 0;
    };

} // namespace Game
