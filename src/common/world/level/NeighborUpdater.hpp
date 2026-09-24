// File: src/common/world/level/NeighborUpdater.hpp
//
// MC net.minecraft.world.level.redstone.CollectingNeighborUpdater — the queue
// that turns what would be recursive neighbour notification into a flat,
// deterministic walk.
//
// WHY A QUEUE AND NOT RECURSION. A lever flips; its six neighbours are told;
// one of them is a wire, which re-evaluates its power and tells ITS
// neighbours; and so on across the whole circuit. Vanilla does not recurse
// through that: `addAndRun` notices that an update is already running and
// parks the new work in `addedThisLayer`, and `runUpdates` folds that layer
// onto a stack and drains it. The ORDER this produces — a six-way fan-out is
// interrupted the moment one neighbour books work of its own, that work runs
// to completion, then the fan-out resumes with the next direction — is what
// every "locational" redstone quirk players build around depends on. It is
// reproduced here instruction for instruction.
//
// UPDATE_ORDER (west, east, down, up, north, south) is vanilla's and is NOT
// the same order the shape walk uses (UPDATE_SHAPE_ORDER swaps the vertical
// and the north/south pairs). Both are observable.
//
// `maxChainedNeighborUpdates` is MC's server property of the same name,
// default 1,000,000: past it, further updates are dropped with one logged
// error rather than hanging the tick.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace Game {

    class World;

    class CollectingNeighborUpdater {
    public:
        // MC NeighborUpdater.UPDATE_ORDER.
        static constexpr Direction kUpdateOrder[6] = {
            Direction::West, Direction::East, Direction::Down,
            Direction::Up,   Direction::North, Direction::South,
        };
        // MC BlockBehaviour.UPDATE_SHAPE_ORDER.
        static constexpr Direction kUpdateShapeOrder[6] = {
            Direction::West,  Direction::East, Direction::North,
            Direction::South, Direction::Down, Direction::Up,
        };

        // MC DedicatedServerProperties.maxChainedNeighborUpdates default.
        static constexpr int kDefaultMaxChainedNeighborUpdates = 1000000;

        CollectingNeighborUpdater(World& level, int maxChainedNeighborUpdates);

        // NeighborUpdater.shapeUpdate — a ShapeUpdate record.
        void ShapeUpdate(Direction direction, BlockState neighborState, const glm::ivec3& pos,
                         const glm::ivec3& neighborPos, uint32_t updateFlags, int updateLimit);

        // NeighborUpdater.neighborChanged(pos, block, orientation) — a
        // SimpleNeighborUpdate: the state is read when the update RUNS.
        void NeighborChanged(const glm::ivec3& pos, BlockID block);

        // NeighborUpdater.neighborChanged(state, pos, block, orientation,
        // movedByPiston) — a FullNeighborUpdate with the state captured now.
        void NeighborChanged(BlockState state, const glm::ivec3& pos, BlockID block,
                             bool movedByPiston);

        // NeighborUpdater.updateNeighborsAtExceptFromFacing — a
        // MultiNeighborUpdate that walks kUpdateOrder lazily, one direction
        // per runNext, so work booked by an earlier neighbour runs before the
        // later neighbours are even told.
        void UpdateNeighborsAtExceptFromFacing(const glm::ivec3& pos, BlockID block,
                                               std::optional<Direction> skipDirection);

        // True while a drain is in progress — the condition under which a new
        // update is parked rather than run.
        bool IsRunning() const { return m_count > 0; }

    private:
        enum class Kind : uint8_t { Shape, Simple, Full, Multi };

        // One of the four NeighborUpdates records, flattened. Which fields
        // are meaningful depends on `kind`.
        struct Update {
            Kind       kind;
            Direction  direction;      // Shape: the direction the neighbour is in
            BlockState state;          // Shape: neighbour state; Full: this cell's state
            glm::ivec3 pos;            // Shape/Simple/Full: the cell; Multi: the source
            glm::ivec3 neighborPos;    // Shape only
            uint32_t   updateFlags;    // Shape only
            int        updateLimit;    // Shape only
            BlockID    block;          // Simple/Full/Multi: the source block
            bool       movedByPiston;  // Full only
            int8_t     skipDirection;  // Multi: -1 or a Direction ordinal
            int8_t     idx;            // Multi: next kUpdateOrder slot
        };

        void AddAndRun(const glm::ivec3& pos, Update update);
        void RunUpdates();
        // NeighborUpdates.runNext — returns true while there is more to run.
        bool RunNext(Update& update);

        World& m_level;
        int    m_maxChainedNeighborUpdates;
        // MC's ArrayDeque<NeighborUpdates> stack (top = back()) and the
        // List<NeighborUpdates> addedThisLayer.
        std::vector<Update> m_stack;
        std::vector<Update> m_addedThisLayer;
        int m_count = 0;
    };

} // namespace Game
