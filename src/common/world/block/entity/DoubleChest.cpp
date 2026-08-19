// File: src/common/world/block/entity/DoubleChest.cpp
#include "DoubleChest.hpp"
#include "../BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include <string_view>

namespace Game {

    namespace {
        // The engine's horizontal `facing` values, in the order
        // BlockRegistry declares them (kHorizontalFacingValues).
        enum class Horizontal { North = 0, East = 1, South = 2, West = 3, Invalid = 4 };

        Horizontal ParseFacing(std::string_view v) {
            if (v == "north") return Horizontal::North;
            if (v == "east")  return Horizontal::East;
            if (v == "south") return Horizontal::South;
            if (v == "west")  return Horizontal::West;
            return Horizontal::Invalid;
        }

        // MC Direction.getClockWise / getCounterClockWise for the horizontal
        // ring, which runs north → east → south → west viewed from above.
        Horizontal ClockWise(Horizontal d) {
            return static_cast<Horizontal>((static_cast<int>(d) + 1) & 3);
        }
        Horizontal CounterClockWise(Horizontal d) {
            return static_cast<Horizontal>((static_cast<int>(d) + 3) & 3);
        }

        glm::ivec3 Offset(Horizontal d) {
            switch (d) {
                case Horizontal::North: return {0, 0, -1};
                case Horizontal::East:  return {1, 0, 0};
                case Horizontal::South: return {0, 0, 1};
                case Horizontal::West:  return {-1, 0, 0};
                default:                return {0, 0, 0};
            }
        }

        // Raw `facing` string at a cell, for comparing two chests' facings.
        std::string_view FacingOf(const IBlockAccess& world, const glm::ivec3& pos, BlockID id) {
            const auto& def = BlockRegistry::GetStateDefinition(id);
            return world.GetBlockState(pos.x, pos.y, pos.z).GetValueByName("facing");
        }

        // The chest's facing, or Invalid when the block carries no facing.
        Horizontal FacingAt(const IBlockAccess& world, const glm::ivec3& pos, BlockID id) {
            const auto& def = BlockRegistry::GetStateDefinition(id);
            const BlockState state = world.GetBlockState(pos.x, pos.y, pos.z);
            return ParseFacing(state.GetValueByName("facing"));
        }
    }

    std::optional<ChestPairing> FindChestPartner(const IBlockAccess& world, const glm::ivec3& pos) {
        const BlockID self = world.GetBlock(pos.x, pos.y, pos.z);

        // Ender chests never pair in MC — each is a view onto the player's own
        // ender inventory, not block storage (ChestBlock vs EnderChestBlock).
        if (self != BlockID::Chest && self != BlockID::TrappedChest) return std::nullopt;

        const auto& def = BlockRegistry::GetStateDefinition(self);
        const BlockState state = world.GetBlockState(pos.x, pos.y, pos.z);
        const std::string_view type = state.GetValueByName("type");
        if (type != "left" && type != "right") return std::nullopt;   // SINGLE

        const Horizontal facing = ParseFacing(state.GetValueByName("facing"));
        if (facing == Horizontal::Invalid) return std::nullopt;

        // MC ChestBlock.getConnectedDirection (ChestBlock.java:135-138):
        //     LEFT  -> facing.clockWise()
        //     RIGHT -> facing.counterClockWise()
        // The stored type is what remembers the player's click; deriving it
        // from geometry instead cannot, which is why placing between two lone
        // chests always joined the same side.
        const bool isLeft = (type == "left");
        const glm::ivec3 toPartner =
            Offset(isLeft ? ClockWise(facing) : CounterClockWise(facing));
        const glm::ivec3 partner = pos + toPartner;

        // Trust but verify. A pair is only a pair when BOTH halves point at
        // each other, so the partner must be the same block, same facing, the
        // complementary type, and its own connected direction must come back
        // here. MC never has to check this because updateShape keeps every
        // type consistent on any neighbour change; we maintain types only at
        // the two sites we control, so a chest whose partner was re-typed
        // out from under it must read as SINGLE rather than claim a half that
        // is already spoken for.
        //
        // This is what stops a chest placed BETWEEN two others from chaining:
        // the middle one pairs with exactly one side, and the other side —
        // still typed single, or pointing elsewhere — refuses to join.
        if (world.GetBlock(partner.x, partner.y, partner.z) != self) return std::nullopt;
        if (ParseFacing(FacingOf(world, partner, self)) != facing) return std::nullopt;

        const BlockState partnerState = world.GetBlockState(partner.x, partner.y, partner.z);
        const std::string_view partnerType = partnerState.GetValueByName("type");
        if (partnerType == type) return std::nullopt;                 // both left / both right
        if (partnerType != "left" && partnerType != "right") return std::nullopt;  // single

        const bool partnerIsLeft = (partnerType == "left");
        const glm::ivec3 partnerToUs =
            Offset(partnerIsLeft ? ClockWise(facing) : CounterClockWise(facing));
        if (partner + partnerToUs != pos) return std::nullopt;        // it points elsewhere

        // MC's getBlockType: RIGHT is FIRST, so it supplies the top 27 slots
        // and draws the half whose seam faces +X.
        return ChestPairing{partner, !isLeft};
    }

} // namespace Game
