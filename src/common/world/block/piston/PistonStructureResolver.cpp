// File: src/common/world/block/piston/PistonStructureResolver.cpp
#include "common/world/block/piston/PistonStructureResolver.hpp"

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include <algorithm>

namespace Game {

    namespace {
        BlockState StateAt(const IBlockAccess& level, const glm::ivec3& p) {
            return level.GetBlockState(p.x, p.y, p.z);
        }

        // MC PistonStructureResolver.isSticky / canStickToEachOther.
        bool IsSticky(BlockState state) {
            return state.Is(BlockID::SlimeBlock) || state.Is(BlockID::HoneyBlock);
        }
        bool CanStickToEachOther(BlockState state1, BlockState state2) {
            if (state1.Is(BlockID::HoneyBlock) && state2.Is(BlockID::SlimeBlock)) return false;
            if (state1.Is(BlockID::SlimeBlock) && state2.Is(BlockID::HoneyBlock)) return false;
            return IsSticky(state1) || IsSticky(state2);
        }

        bool Contains(const std::vector<glm::ivec3>& v, const glm::ivec3& p) {
            return std::find(v.begin(), v.end(), p) != v.end();
        }
        int IndexOf(const std::vector<glm::ivec3>& v, const glm::ivec3& p) {
            auto it = std::find(v.begin(), v.end(), p);
            return it == v.end() ? -1 : static_cast<int>(it - v.begin());
        }
    }

    bool PistonIsPushable(BlockState state, const ILevelWrite& level, const glm::ivec3& pos,
                          Direction direction, bool allowDestroyable, Direction connectionDirection) {
        const int minY = DimensionMinY(level.GetDimension());
        const int maxY = minY + DimensionLogicalHeight(level.GetDimension()) - 1;
        if (pos.y < minY || pos.y > maxY) return false;
        const BlockID id = state.Block();
        if (id == BlockID::Air) return true;
        if (direction == Direction::Down && pos.y == minY) return false;
        if (direction == Direction::Up && pos.y == maxY) return false;

        if (id != BlockID::Piston && id != BlockID::StickyPiston) {
            const Block& def = BlockRegistry::Get(id);
            // getDestroySpeed == -1: bedrock, barrier, the portal frames.
            if (def.destroyTime < 0.0f) return false;
            switch (def.pushReaction) {
                case PushReaction::Immoveable: return false;
                case PushReaction::Popped:     return allowDestroyable;
                case PushReaction::Push:       return direction == connectionDirection;
                default: break;
            }
        } else if (BoolOf(state, PropertyId::EXTENDED)) {
            return false;
        }
        return !BlockEntityTypes::HasBlockEntity(id);
    }

    PistonStructureResolver::PistonStructureResolver(ILevelWrite& level, const glm::ivec3& pistonPos,
                                                     Direction direction, bool extending)
        : m_level(level), m_pistonPos(pistonPos), m_extending(extending),
          m_pistonDirection(direction) {
        if (extending) {
            m_pushDirection = direction;
            m_startPos      = Relative(pistonPos, direction);
        } else {
            m_pushDirection = Opposite(direction);
            m_startPos      = Relative(pistonPos, direction, 2);
        }
    }

    bool PistonStructureResolver::Resolve() {
        m_toPush.clear();
        m_toDestroy.clear();
        const BlockState nextState = StateAt(m_level, m_startPos);
        if (!PistonIsPushable(nextState, m_level, m_startPos, m_pushDirection, false, m_pistonDirection)) {
            if (m_extending && BlockRegistry::Get(nextState.Block()).pushReaction == PushReaction::Popped) {
                m_toDestroy.push_back(m_startPos);
                return true;
            }
            return false;
        }
        if (!AddBlockLine(m_startPos, m_pushDirection)) return false;
        for (size_t i = 0; i < m_toPush.size(); ++i) {
            const glm::ivec3 pos = m_toPush[i];
            if (IsSticky(StateAt(m_level, pos)) && !AddBranchingBlocks(pos)) return false;
        }
        return true;
    }

    bool PistonStructureResolver::AddBlockLine(const glm::ivec3& start, Direction direction) {
        BlockState nextState = StateAt(m_level, start);
        if (nextState.Block() == BlockID::Air) return true;
        if (!PistonIsPushable(nextState, m_level, start, m_pushDirection, false, direction)) return true;
        if (start == m_pistonPos) return true;
        if (Contains(m_toPush, start)) return true;

        int blockCount = 1;
        if (blockCount + static_cast<int>(m_toPush.size()) > kMaxPushDepth) return false;

        while (IsSticky(nextState)) {
            const glm::ivec3 pos = Relative(start, Opposite(m_pushDirection), blockCount);
            const BlockState previousState = nextState;
            nextState = StateAt(m_level, pos);
            if (nextState.Block() == BlockID::Air || !CanStickToEachOther(previousState, nextState) ||
                !PistonIsPushable(nextState, m_level, pos, m_pushDirection, false, Opposite(m_pushDirection)) ||
                pos == m_pistonPos) {
                break;
            }
            ++blockCount;
            if (blockCount + static_cast<int>(m_toPush.size()) > kMaxPushDepth) return false;
        }

        int blocksAdded = 0;
        for (int i = blockCount - 1; i >= 0; --i) {
            m_toPush.push_back(Relative(start, Opposite(m_pushDirection), i));
            ++blocksAdded;
        }

        int i = 1;
        while (true) {
            const glm::ivec3 pos = Relative(start, m_pushDirection, i);
            const int collisionPos = IndexOf(m_toPush, pos);
            if (collisionPos > -1) {
                ReorderListAtCollision(blocksAdded, collisionPos);
                for (int j = 0; j <= collisionPos + blocksAdded; ++j) {
                    const glm::ivec3 blockPos = m_toPush[static_cast<size_t>(j)];
                    if (IsSticky(StateAt(m_level, blockPos)) && !AddBranchingBlocks(blockPos)) return false;
                }
                return true;
            }
            nextState = StateAt(m_level, pos);
            if (nextState.Block() == BlockID::Air) return true;
            if (!PistonIsPushable(nextState, m_level, pos, m_pushDirection, true, m_pushDirection) ||
                pos == m_pistonPos) {
                return false;
            }
            if (BlockRegistry::Get(nextState.Block()).pushReaction == PushReaction::Popped) {
                m_toDestroy.push_back(pos);
                return true;
            }
            if (static_cast<int>(m_toPush.size()) >= kMaxPushDepth) return false;
            m_toPush.push_back(pos);
            ++blocksAdded;
            ++i;
        }
    }

    void PistonStructureResolver::ReorderListAtCollision(int blocksAdded, int collisionPos) {
        std::vector<glm::ivec3> head(m_toPush.begin(), m_toPush.begin() + collisionPos);
        std::vector<glm::ivec3> lastLineAdded(m_toPush.end() - blocksAdded, m_toPush.end());
        std::vector<glm::ivec3> collisionToLine(m_toPush.begin() + collisionPos, m_toPush.end() - blocksAdded);
        m_toPush.clear();
        m_toPush.insert(m_toPush.end(), head.begin(), head.end());
        m_toPush.insert(m_toPush.end(), lastLineAdded.begin(), lastLineAdded.end());
        m_toPush.insert(m_toPush.end(), collisionToLine.begin(), collisionToLine.end());
    }

    bool PistonStructureResolver::AddBranchingBlocks(const glm::ivec3& fromPos) {
        const BlockState fromState = StateAt(m_level, fromPos);
        for (Direction direction : kAllDirections) {
            if (AxisOf(direction) == AxisOf(m_pushDirection)) continue;
            const glm::ivec3 neighbourPos = Relative(fromPos, direction);
            const BlockState neighbourState = StateAt(m_level, neighbourPos);
            if (CanStickToEachOther(neighbourState, fromState) && !AddBlockLine(neighbourPos, direction)) {
                return false;
            }
        }
        return true;
    }

} // namespace Game
