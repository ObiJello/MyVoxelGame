// File: src/common/world/block/piston/PistonStructureResolver.hpp
//
// MC PistonStructureResolver — works out which blocks a piston move drags
// along (toPush, in the order they must be written) and which it breaks
// (toDestroy), honouring the 12-block limit and slime/honey stickiness.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <vector>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    class  ILevelWrite;

    // MC PistonBaseBlock.isPushable(state, level, pos, direction,
    // allowDestroyable, connectionDirection). Public because the sticky
    // piston's retraction asks it about the block two ahead.
    bool PistonIsPushable(BlockState state, const ILevelWrite& level, const glm::ivec3& pos,
                          Direction direction, bool allowDestroyable, Direction connectionDirection);

    class PistonStructureResolver {
    public:
        static constexpr int kMaxPushDepth = 12;

        PistonStructureResolver(ILevelWrite& level, const glm::ivec3& pistonPos,
                                Direction direction, bool extending);

        bool Resolve();

        Direction GetPushDirection() const { return m_pushDirection; }
        const std::vector<glm::ivec3>& GetToPush()    const { return m_toPush; }
        const std::vector<glm::ivec3>& GetToDestroy() const { return m_toDestroy; }

    private:
        bool AddBlockLine(const glm::ivec3& start, Direction direction);
        void ReorderListAtCollision(int blocksAdded, int collisionPos);
        bool AddBranchingBlocks(const glm::ivec3& fromPos);

        ILevelWrite& m_level;
        glm::ivec3   m_pistonPos;
        bool         m_extending;
        glm::ivec3   m_startPos;
        Direction    m_pushDirection;
        Direction    m_pistonDirection;
        std::vector<glm::ivec3> m_toPush;
        std::vector<glm::ivec3> m_toDestroy;
    };

} // namespace Game
