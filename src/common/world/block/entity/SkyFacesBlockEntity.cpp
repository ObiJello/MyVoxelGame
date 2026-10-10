// File: src/common/world/block/entity/SkyFacesBlockEntity.cpp
// See the header for the door frame and why the entity is lazy.

#include "SkyFacesBlockEntity.hpp"

#include "common/network/PacketRegistry.hpp"
#include "common/world/block/BlockPlacement.hpp"

#include <vector>

namespace Game {

    namespace SkyFaces {

        bool AppliesTo(BlockID id) {
            // IsDoorBlock is a slug compare; the mesher asks per block.
            static const std::vector<bool> s_doors = [] {
                std::vector<bool> doors(static_cast<size_t>(BlockID::Count), false);
                for (size_t i = 0; i < doors.size(); ++i) doors[i] = IsDoorBlock(static_cast<BlockID>(i));
                return doors;
            }();
            const auto idx = static_cast<size_t>(id);
            return idx < s_doors.size() && s_doors[idx];
        }

        int DoorQuarterTurns(BlockState state) {
            // The closed variant's y rotation (blockstates/*_door.json):
            // east 0, south 90, west 180, north 270.
            int turns = 0;
            switch (HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING))) {
                case Direction::East:  turns = 0; break;
                case Direction::South: turns = 1; break;
                case Direction::West:  turns = 2; break;
                default:               turns = 3; break;   // north
            }
            // Open: the slab swings a quarter turn about its hinge —
            // counter-clockwise for a left hinge, clockwise for a right one.
            if (state.GetName(PropertyId::OPEN) == "true") {
                turns += (state.GetName(PropertyId::HINGE) == "left") ? 3 : 1;
            }
            return turns & 3;
        }

        namespace {
            Direction Turn(Direction d, int clockwiseTurns) {
                if (!IsHorizontal(d)) return d;
                for (int i = 0; i < (clockwiseTurns & 3); ++i) d = ClockWise(d);
                return d;
            }
        } // namespace

        Direction ToDoorFrame(BlockState state, Direction worldFace) {
            return Turn(worldFace, 4 - DoorQuarterTurns(state));
        }

        Direction ToWorld(BlockState state, Direction frameFace) {
            return Turn(frameFace, DoorQuarterTurns(state));
        }

    } // namespace SkyFaces

    void SkyFacesBlockEntity::Save(Network::PacketBuffer& out) const {
        out.WriteByte(m_mask);
    }

    void SkyFacesBlockEntity::Load(Network::PacketReader& in) {
        if (!in.HasMore()) return;
        SetMask(in.ReadByte());
    }

} // namespace Game
