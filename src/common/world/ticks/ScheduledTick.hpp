// File: src/common/world/ticks/ScheduledTick.hpp
//
// MC net.minecraft.world.ticks.{ScheduledTick, SavedTick, TickPriority}.
//
// A scheduled tick is "run this block's tick() N ticks from now". It is the
// mechanism behind every delayed block behaviour in vanilla — sand deciding to
// fall two ticks after its support went, a repeater flipping, water spreading —
// and it is NOT the same thing as a random tick. Random ticks sample positions
// at random and are a probability; these are an appointment.
//
// ORDERING IS OBSERVABLE. Two sand blocks in the same column must fall in the
// order they were scheduled, or a falling stack rearranges itself. MC gets that
// from a three-level sort:
//
//   1. triggerTick   — earlier appointments first
//   2. priority      — MC's TickPriority, used by redstone to force a
//                      deterministic order among ticks in the same game tick
//   3. subTickOrder  — a monotonically increasing counter, i.e. insertion
//                      order, which is the final tiebreak that makes the whole
//                      thing reproducible rather than hash-order-dependent
//
// UNIQUENESS is (pos, type) and NOT the whole record: scheduling a tick for a
// block that already has one pending is a no-op. That is load-bearing, not an
// optimisation — a single block change fires neighbour updates on six cells,
// each of which can re-schedule its own neighbours, and without the dedupe a
// wall of sand would queue thousands of redundant appointments per tick.
#pragma once

#include "common/world/block/Blocks.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    // MC TickPriority, values verbatim — they are serialized to NBT as ints.
    // Nothing in this engine schedules off NORMAL yet; the enum exists because
    // the save format carries it and because redstone will need it.
    enum class TickPriority : int8_t {
        ExtremelyHigh = -3,
        VeryHigh      = -2,
        High          = -1,
        Normal        =  0,
        Low           =  1,
        VeryLow       =  2,
        ExtremelyLow  =  3,
    };

    // MC TickPriority.byValue — clamps rather than throwing, so a save written
    // by a future version with a wider range still loads.
    inline TickPriority TickPriorityByValue(int value) {
        if (value < -3) return TickPriority::ExtremelyHigh;
        if (value >  3) return TickPriority::ExtremelyLow;
        return static_cast<TickPriority>(value);
    }

    // MC ScheduledTick — an appointment that has been unpacked against the
    // current game time.
    //
    // `type` is a BlockID where MC holds a Block*. It is re-checked against the
    // world before the tick runs (MC ServerLevel.tickBlock: `if (state.is(type))`),
    // which is what stops a tick scheduled for sand from firing on the stone
    // somebody put there in the meantime.
    struct ScheduledTick {
        BlockID      type         = BlockID::Air;
        glm::ivec3   pos{0, 0, 0};
        int64_t      triggerTick  = 0;
        TickPriority priority     = TickPriority::Normal;
        int64_t      subTickOrder = 0;

        // MC ScheduledTick.DRAIN_ORDER. Returns true when `a` should run first.
        static bool DrainOrderLess(const ScheduledTick& a, const ScheduledTick& b) {
            if (a.triggerTick != b.triggerTick) return a.triggerTick < b.triggerTick;
            if (a.priority   != b.priority)     return a.priority   < b.priority;
            return a.subTickOrder < b.subTickOrder;
        }

        // MC ScheduledTick.INTRA_TICK_DRAIN_ORDER — the same comparison with the
        // trigger time dropped, for ordering ticks that all fire this tick.
        static bool IntraTickOrderLess(const ScheduledTick& a, const ScheduledTick& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.subTickOrder < b.subTickOrder;
        }
    };

    // MC SavedTick — the on-disk form. `delay` is RELATIVE to the current game
    // time, which is why a save can be loaded into a world whose clock has
    // moved on without every pending tick firing at once.
    struct SavedTick {
        BlockID      type     = BlockID::Air;
        glm::ivec3   pos{0, 0, 0};
        int32_t      delay    = 0;
        TickPriority priority = TickPriority::Normal;

        ScheduledTick Unpack(int64_t currentTick, int64_t subTickOrder) const {
            return ScheduledTick{type, pos, currentTick + delay, priority, subTickOrder};
        }
    };

    // MC ScheduledTick.UNIQUE_TICK_HASH — identity is (pos, type) only.
    struct TickIdentity {
        glm::ivec3 pos;
        BlockID    type;

        bool operator==(const TickIdentity& o) const {
            return type == o.type && pos == o.pos;
        }
    };

    struct TickIdentityHash {
        std::size_t operator()(const TickIdentity& t) const {
            // MC: 31 * pos.hashCode() + type.hashCode(). The mix below is the
            // same shape with constants that behave better for a 64-bit
            // std::unordered_set than Java's int hash does.
            std::size_t h = static_cast<std::size_t>(static_cast<uint32_t>(t.pos.x));
            h = h * 31u + static_cast<std::size_t>(static_cast<uint32_t>(t.pos.y));
            h = h * 31u + static_cast<std::size_t>(static_cast<uint32_t>(t.pos.z));
            h = h * 31u + static_cast<std::size_t>(t.type);
            return h;
        }
    };

} // namespace Game
