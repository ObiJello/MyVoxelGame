// File: src/common/world/portal/EndPortalFrame.cpp
//
// Line references are to minecraft_code/decompiled_net/minecraft/world/level/
// block/EndPortalFrameBlock.java.

#include "EndPortalFrame.hpp"

namespace Game {
    namespace EndPortalFrame {

        bool HasEye(BlockState state) {
            // MC BooleanProperty lists `true` before `false`, so index 0 is
            // true. Comparing the serialised name instead of the index keeps
            // that fact in one place (the generated table) rather than two.
            return state.GetName(PropertyId::EYE) == "true";
        }

        Direction Facing(BlockState state) {
            const int idx = state.GetIndex(PropertyId::HORIZONTAL_FACING);
            if (idx < 0) return Direction::North;
            return HorizontalFacingFromIndex(idx);
        }

        BlockState WithEye(BlockState state, bool hasEye) {
            return state.SetName(PropertyId::EYE, hasEye ? "true" : "false");
        }

        namespace {
            // The four ring predicates. `facing` is the direction the frame
            // block points, which in a completed ring is toward the centre —
            // so a frame whose facing is NORTH sits on the SOUTH side.
            BlockPattern::StatePredicate EyedFrameFacing(Direction facing) {
                return [facing](BlockState state) {
                    return state.Is(BlockID::EndPortalFrame)
                        && HasEye(state)
                        && Facing(state) == facing;
                };
            }
        } // namespace

        // EndPortalFrameBlock.java:75
        const BlockPattern& PortalShapePattern() {
            // Function-local static: built on first use and never rebuilt,
            // matching MC's lazily-initialised `portalShape` field. Thread-safe
            // initialisation is guaranteed by the language here, which MC's
            // null check is not — a small improvement on vanilla.
            // The middle rows are MC's ">???<". Written with the third '?'
            // escaped because "??<" is a trigraph — gone from the language in
            // C++17 but still warned about, and the escape costs nothing.
            static const BlockPattern pattern = BlockPatternBuilder()
                .Aisle({ "?vvv?",
                         ">??\?<",
                         ">??\?<",
                         ">??\?<",
                         "?^^^?" })
                .Where('?', [](BlockState) { return true; })
                .Where('^', EyedFrameFacing(Direction::South))
                .Where('>', EyedFrameFacing(Direction::West))
                .Where('v', EyedFrameFacing(Direction::North))
                .Where('<', EyedFrameFacing(Direction::East))
                .Build();
            return pattern;
        }

    } // namespace EndPortalFrame
} // namespace Game
