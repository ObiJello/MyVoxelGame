// File: src/common/world/level/NeighborUpdater.cpp
#include "common/world/level/NeighborUpdater.hpp"

#include "common/core/Log.hpp"
#include "common/world/level/World.hpp"

namespace Game {

    CollectingNeighborUpdater::CollectingNeighborUpdater(World& level, int maxChainedNeighborUpdates)
        : m_level(level), m_maxChainedNeighborUpdates(maxChainedNeighborUpdates) {}

    void CollectingNeighborUpdater::ShapeUpdate(Direction direction, BlockState neighborState,
                                                const glm::ivec3& pos, const glm::ivec3& neighborPos,
                                                uint32_t updateFlags, int updateLimit) {
        Update u{};
        u.kind        = Kind::Shape;
        u.direction   = direction;
        u.state       = neighborState;
        u.pos         = pos;
        u.neighborPos = neighborPos;
        u.updateFlags = updateFlags;
        u.updateLimit = updateLimit;
        AddAndRun(pos, u);
    }

    void CollectingNeighborUpdater::NeighborChanged(const glm::ivec3& pos, BlockID block) {
        Update u{};
        u.kind  = Kind::Simple;
        u.pos   = pos;
        u.block = block;
        AddAndRun(pos, u);
    }

    void CollectingNeighborUpdater::NeighborChanged(BlockState state, const glm::ivec3& pos,
                                                    BlockID block, bool movedByPiston) {
        Update u{};
        u.kind          = Kind::Full;
        u.state         = state;
        u.pos           = pos;
        u.block         = block;
        u.movedByPiston = movedByPiston;
        AddAndRun(pos, u);
    }

    void CollectingNeighborUpdater::UpdateNeighborsAtExceptFromFacing(
            const glm::ivec3& pos, BlockID block, std::optional<Direction> skipDirection) {
        Update u{};
        u.kind          = Kind::Multi;
        u.pos           = pos;
        u.block         = block;
        u.skipDirection = skipDirection ? static_cast<int8_t>(*skipDirection) : int8_t(-1);
        u.idx           = 0;
        // MultiNeighborUpdate's constructor: skip the first slot if it IS the
        // skipped direction.
        if (u.skipDirection >= 0 && kUpdateOrder[0] == static_cast<Direction>(u.skipDirection)) {
            ++u.idx;
        }
        AddAndRun(pos, u);
    }

    // CollectingNeighborUpdater.addAndRun, verbatim in shape.
    void CollectingNeighborUpdater::AddAndRun(const glm::ivec3& pos, Update update) {
        const bool runningAlready = m_count > 0;
        const bool tooManyUpdates =
            m_maxChainedNeighborUpdates >= 0 && m_count >= m_maxChainedNeighborUpdates;
        ++m_count;
        if (!tooManyUpdates) {
            if (runningAlready) m_addedThisLayer.push_back(update);
            else                m_stack.push_back(update);
        } else if (m_count - 1 == m_maxChainedNeighborUpdates) {
            Log::Error("Too many chained neighbor updates. Skipping the rest. "
                       "First skipped position: %d, %d, %d", pos.x, pos.y, pos.z);
        }

        if (!runningAlready) RunUpdates();
    }

    // CollectingNeighborUpdater.runUpdates, verbatim in shape. Java's
    // ArrayDeque.push/peek/pop act on the HEAD; a vector's back() plays the
    // same role.
    void CollectingNeighborUpdater::RunUpdates() {
        while (!m_stack.empty() || !m_addedThisLayer.empty()) {
            for (size_t i = m_addedThisLayer.size(); i-- > 0;) {
                m_stack.push_back(m_addedThisLayer[i]);
            }
            m_addedThisLayer.clear();

            // `nextUpdates` is the top of the stack and stays valid across
            // RunNext: anything booked meanwhile lands in m_addedThisLayer,
            // never in m_stack.
            Update& nextUpdates = m_stack.back();
            while (m_addedThisLayer.empty()) {
                if (!RunNext(nextUpdates)) {
                    m_stack.pop_back();
                    break;
                }
            }
        }
        // The `finally` — cleared even after an exception in vanilla; here
        // nothing throws, but the reset is what re-arms `runningAlready`.
        m_stack.clear();
        m_addedThisLayer.clear();
        m_count = 0;
    }

    bool CollectingNeighborUpdater::RunNext(Update& u) {
        switch (u.kind) {
            case Kind::Shape:
                m_level.ExecuteShapeUpdate(u.direction, u.pos, u.neighborPos, u.state,
                                           u.updateFlags, u.updateLimit);
                return false;

            case Kind::Simple: {
                const BlockState state = m_level.GetBlockState(u.pos.x, u.pos.y, u.pos.z);
                m_level.ExecuteNeighborUpdate(state, u.pos, u.block, false);
                return false;
            }

            case Kind::Full:
                m_level.ExecuteNeighborUpdate(u.state, u.pos, u.block, u.movedByPiston);
                return false;

            case Kind::Multi: {
                // MultiNeighborUpdate.runNext. Orientation is null without the
                // redstone_experiments flag, so nothing is computed for it.
                const Direction direction = kUpdateOrder[u.idx++];
                const glm::ivec3 neighborPos(u.pos.x + StepX(direction),
                                             u.pos.y + StepY(direction),
                                             u.pos.z + StepZ(direction));
                const BlockState state =
                    m_level.GetBlockState(neighborPos.x, neighborPos.y, neighborPos.z);
                m_level.ExecuteNeighborUpdate(state, neighborPos, u.block, false);
                if (u.idx < 6 && u.skipDirection >= 0 &&
                    kUpdateOrder[u.idx] == static_cast<Direction>(u.skipDirection)) {
                    ++u.idx;
                }
                return u.idx < 6;
            }
        }
        return false;
    }

} // namespace Game
