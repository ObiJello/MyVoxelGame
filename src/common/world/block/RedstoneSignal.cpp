// File: src/common/world/block/RedstoneSignal.cpp
#include "common/world/block/RedstoneSignal.hpp"

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>

namespace Game {

    namespace {
        inline BlockState StateAt(const IBlockAccess& level, const glm::ivec3& pos) {
            return level.GetBlockState(pos.x, pos.y, pos.z);
        }
        inline glm::ivec3 Relative(const glm::ivec3& pos, Direction d) {
            return glm::ivec3(pos.x + StepX(d), pos.y + StepY(d), pos.z + StepZ(d));
        }
    }

    bool IsRedstoneConductor(const IBlockAccess& level, const glm::ivec3& pos) {
        return IsRedstoneConductor(level, pos, StateAt(level, pos));
    }

    bool IsRedstoneConductor(const IBlockAccess& /*level*/, const glm::ivec3& /*pos*/,
                             BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;
        const Block& def = BlockRegistry::Get(id);
        switch (def.redstoneConductor) {
            case RedstoneConductor::Never:  return false;
            case RedstoneConductor::Always: return true;
            case RedstoneConductor::Default: break;
        }
        // BlockStateBase::isCollisionShapeFullBlock — a noCollision block has
        // an empty collision shape, which is not a full block.
        if (!def.hasCollision) return false;
        return BlockRegistry::GetBlockCollisionShapeSet(state).IsFullCube();
    }

    bool IsSignalSource(BlockState state) {
        return BlockRegistry::Get(state.Block()).isSignalSource;
    }

    int GetBlockSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                       Direction direction) {
        const Block& def = BlockRegistry::Get(state.Block());
        return def.getSignal ? def.getSignal(level, pos, state, direction) : 0;
    }

    int GetBlockDirectSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                             Direction direction) {
        const Block& def = BlockRegistry::Get(state.Block());
        return def.getDirectSignal ? def.getDirectSignal(level, pos, state, direction) : 0;
    }

    int GetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos, Direction direction) {
        return GetBlockDirectSignal(level, pos, StateAt(level, pos), direction);
    }

    // SignalGetter.getDirectSignalTo, unrolled in vanilla with an early-out
    // at 15 after each step.
    int GetDirectSignalTo(const IBlockAccess& level, const glm::ivec3& pos) {
        int result = 0;
        static constexpr Direction kOrder[6] = {
            Direction::Down, Direction::Up, Direction::North,
            Direction::South, Direction::West, Direction::East,
        };
        for (Direction d : kOrder) {
            result = std::max(result, GetDirectSignal(level, Relative(pos, d), d));
            if (result >= 15) return result;
        }
        return result;
    }

    int GetSignal(const IBlockAccess& level, const glm::ivec3& pos, Direction direction) {
        const BlockState state = StateAt(level, pos);
        const int signal = GetBlockSignal(level, pos, state, direction);
        return IsRedstoneConductor(level, pos, state)
            ? std::max(signal, GetDirectSignalTo(level, pos))
            : signal;
    }

    bool HasSignal(const IBlockAccess& level, const glm::ivec3& pos, Direction direction) {
        return GetSignal(level, pos, direction) > 0;
    }

    bool HasNeighborSignal(const IBlockAccess& level, const glm::ivec3& pos) {
        if (GetSignal(level, Relative(pos, Direction::Down),  Direction::Down)  > 0) return true;
        if (GetSignal(level, Relative(pos, Direction::Up),    Direction::Up)    > 0) return true;
        if (GetSignal(level, Relative(pos, Direction::North), Direction::North) > 0) return true;
        if (GetSignal(level, Relative(pos, Direction::South), Direction::South) > 0) return true;
        if (GetSignal(level, Relative(pos, Direction::West),  Direction::West)  > 0) return true;
        return GetSignal(level, Relative(pos, Direction::East), Direction::East) > 0;
    }

    int GetBestNeighborSignal(const IBlockAccess& level, const glm::ivec3& pos) {
        int best = 0;
        // Direction.values() order: DOWN, UP, NORTH, SOUTH, WEST, EAST.
        for (int i = 0; i < 6; ++i) {
            const Direction d = static_cast<Direction>(i);
            const int signal = GetSignal(level, Relative(pos, d), d);
            if (signal >= 15) return 15;
            if (signal > best) best = signal;
        }
        return best;
    }

    int GetControlInputSignal(const IBlockAccess& level, const glm::ivec3& pos,
                              Direction direction, bool onlyDiodes) {
        const BlockState state = StateAt(level, pos);
        const BlockID id = state.Block();
        if (onlyDiodes) {
            // DiodeBlock.isDiode: a repeater or a comparator.
            return (id == BlockID::Repeater || id == BlockID::Comparator)
                ? GetDirectSignal(level, pos, direction) : 0;
        }
        if (id == BlockID::RedstoneBlock) return 15;
        if (id == BlockID::RedstoneWire)  return state.GetIndex(PropertyId::POWER);
        return IsSignalSource(state) ? GetDirectSignal(level, pos, direction) : 0;
    }

    int GetBestOwnOrNeighbourSignal(const IBlockAccess& level, const glm::ivec3& pos) {
        // MC: getBlockState(pos).getOwnSignal → the block's ownSignal hook,
        // which only dust overrides (its POWER). Everything else is 0 and the
        // neighbour scan decides.
        const BlockState state = StateAt(level, pos);
        const int own = state.Is(BlockID::RedstoneWire) ? state.GetIndex(PropertyId::POWER) : 0;
        return std::max(own, GetBestNeighborSignal(level, pos));
    }

} // namespace Game
