// File: src/common/world/block/BedBlock.cpp
#include "BedBlock.hpp"
#include "BlockRegistry.hpp"
#include "GeneratedBlockStates.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/physics/Physics.hpp"
#include "common/core/Mth.hpp"
#include "common/sound/SoundEvents.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace Game {

    namespace {
        constexpr bool SlugIsBed(std::string_view slug) {
            constexpr std::string_view kSuffix = "_bed";
            return slug.size() > kSuffix.size() &&
                   slug.substr(slug.size() - kSuffix.size()) == kSuffix;
        }

        // Built from the block table at compile time so the answer is a
        // table read: the mesher's per-chunk index and the shape cache both
        // ask this per voxel, and neither can afford a string compare.
        constexpr std::array<bool, static_cast<size_t>(BlockID::Count)> kIsBed = [] {
            std::array<bool, static_cast<size_t>(BlockID::Count)> table{};
            #define BLOCK_DEF(e, m, d, o) table[static_cast<size_t>(BlockID::e)] = SlugIsBed(m);
            #include "BlockDefs.inc"
            #undef BLOCK_DEF
            return table;
        }();

        // MC EntityType.isBlockDangerous for the player: the blocks a
        // dismount / stand-up must not put you in or on when a safe cell is
        // wanted.
        bool IsDangerousForPlayer(BlockID id) {
            switch (id) {
                case BlockID::Fire:
                case BlockID::SoulFire:
                case BlockID::Lava:
                case BlockID::Cactus:
                case BlockID::SweetBerryBush:
                case BlockID::WitherRose:
                case BlockID::Campfire:
                case BlockID::SoulCampfire:
                case BlockID::MagmaBlock:
                case BlockID::PowderSnow:
                    return true;
                default:
                    return false;
            }
        }

        // The top of a cell's collision shape, or -inf for a cell with none —
        // MC DismountHelper.nonClimbableShape + CollisionGetter
        // .getBlockFloorHeight, reduced to the boxes this engine keeps.
        double CollisionTop(const IBlockAccess& level, const glm::ivec3& cell) {
            const BlockState state = level.GetBlockState(cell.x, cell.y, cell.z);
            if (!BlockRegistry::HasCollision(state.Block())) {
                return -std::numeric_limits<double>::infinity();
            }
            const BlockRegistry::BlockShapeSet set = BlockRegistry::GetBlockCollisionShapeSet(state);
            double top = -std::numeric_limits<double>::infinity();
            for (const BlockRegistry::BlockShape& box : set) {
                top = std::max(top, static_cast<double>(box.max.y));
            }
            return top;
        }

        // MC DismountHelper.findSafeDismountLocation(EntityType.PLAYER, …).
        // The floor is the cell's own collision top, or the cell below's top
        // brought up by one; a floor at or above the cell top is not one
        // (MC isBlockFloorValid). The player's box on that floor must be
        // clear of every collision box, and, on the safe pass, neither the
        // cell nor the floor block may be dangerous.
        std::optional<glm::dvec3> FindSafeDismountLocation(const IBlockAccess& level,
                                                           const glm::ivec3& cell,
                                                           bool checkDangerous) {
            if (checkDangerous &&
                IsDangerousForPlayer(level.GetBlockState(cell.x, cell.y, cell.z).Block())) {
                return std::nullopt;
            }
            double floor = CollisionTop(level, cell);
            if (std::isinf(floor)) {
                floor = CollisionTop(level, cell - glm::ivec3(0, 1, 0)) - 1.0;
            }
            if (std::isinf(floor) || floor >= 1.0) return std::nullopt;
            if (checkDangerous && floor <= 0.0 &&
                IsDangerousForPlayer(level.GetBlockState(cell.x, cell.y - 1, cell.z).Block())) {
                return std::nullopt;
            }

            const glm::dvec3 result(cell.x + 0.5, cell.y + floor, cell.z + 0.5);
            const float halfWidth = PlayerPhysics::WIDTH * 0.5f;
            const AABB box = AABB::FromMinMax(
                glm::vec3(static_cast<float>(result.x) - halfWidth,
                          static_cast<float>(result.y),
                          static_cast<float>(result.z) - halfWidth),
                glm::vec3(static_cast<float>(result.x) + halfWidth,
                          static_cast<float>(result.y) + PlayerPhysics::HEIGHT_STANDING,
                          static_cast<float>(result.z) + halfWidth));
            PhysicsContext context;
            context.blockAccess = &level;
            if (CollidesAt(box, context)) return std::nullopt;
            return result;
        }

        using Offset = std::array<int, 2>;

        // MC AbstractBedBlock.bedSurroundStandUpOffsets: the ten cells
        // around the pair, starting beside the head on the sleeper's side.
        std::array<Offset, 10> SurroundOffsets(Direction forward, Direction side) {
            const int fx = StepX(forward), fz = StepZ(forward);
            const int sx = StepX(side),    sz = StepZ(side);
            return {{
                { sx,            sz            },
                { sx - fx,       sz - fz       },
                { sx - fx * 2,   sz - fz * 2   },
                { -fx * 2,       -fz * 2       },
                { -sx - fx * 2,  -sz - fz * 2  },
                { -sx - fx,      -sz - fz      },
                { -sx,           -sz           },
                { -sx + fx,      -sz + fz      },
                { fx,            fz            },
                { sx + fx,       sz + fz       },
            }};
        }

        // MC bedAboveStandUpOffsets: on the bed itself, head then foot.
        std::array<Offset, 2> AboveOffsets(Direction forward) {
            return {{ { 0, 0 }, { -StepX(forward), -StepZ(forward) } }};
        }

        template <size_t N>
        std::optional<glm::dvec3> FindAtOffsets(const IBlockAccess& level, const glm::ivec3& pos,
                                                const std::array<Offset, N>& offsets,
                                                bool checkDangerous) {
            for (const Offset& o : offsets) {
                if (auto p = FindSafeDismountLocation(
                        level, glm::ivec3(pos.x + o[0], pos.y, pos.z + o[1]), checkDangerous)) {
                    return p;
                }
            }
            return std::nullopt;
        }

        bool IsBunkBed(const IBlockAccess& level, const glm::ivec3& pos) {
            return IsBedBlock(level.GetBlockState(pos.x, pos.y - 1, pos.z).Block());
        }

        // MC Direction.isFacingAngle: the direction within 45° of the yaw.
        bool IsFacingAngle(Direction d, float yaw) {
            const float angle = Mth::WrapDegrees(yaw - ToYRot(d));
            return angle > -45.0f && angle < 45.0f;
        }
    } // namespace

    bool IsBedBlock(BlockID id) {
        const size_t idx = static_cast<size_t>(id);
        return idx < kIsBed.size() && kIsBed[idx];
    }

    Direction BedFacing(BlockState state) {
        return HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING));
    }

    BlockRegistry::BlockShapeSet BedShapeBoxes(BlockState state) {
        using BlockShape = BlockRegistry::BlockShape;
        BlockRegistry::BlockShapeSet set;
        // column(16, 3, 9): full footprint from y = 3 to y = 9.
        set.boxes[0] = BlockShape{ glm::vec3(0.0f, 3.0f / 16.0f, 0.0f),
                                   glm::vec3(1.0f, 9.0f / 16.0f, 1.0f) };
        // The two legs, box(0,0,0, 3,3,3) and its Y-90 rotation, sit on the
        // side SHAPES is keyed by: getConnectedDirection(state).getOpposite(),
        // i.e. the facing for the head half and its opposite for the foot.
        const Direction facing   = BedFacing(state);
        const Direction legsSide = IsBedHead(state) ? facing : Opposite(facing);
        constexpr float k3 = 3.0f / 16.0f, k13 = 13.0f / 16.0f;
        switch (legsSide) {
            case Direction::North:
                set.boxes[1] = BlockShape{ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(k3,   k3, k3) };
                set.boxes[2] = BlockShape{ glm::vec3(k13,  0.0f, 0.0f), glm::vec3(1.0f, k3, k3) };
                break;
            case Direction::South:
                set.boxes[1] = BlockShape{ glm::vec3(0.0f, 0.0f, k13), glm::vec3(k3,   k3, 1.0f) };
                set.boxes[2] = BlockShape{ glm::vec3(k13,  0.0f, k13), glm::vec3(1.0f, k3, 1.0f) };
                break;
            case Direction::West:
                set.boxes[1] = BlockShape{ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(k3, k3, k3) };
                set.boxes[2] = BlockShape{ glm::vec3(0.0f, 0.0f, k13),  glm::vec3(k3, k3, 1.0f) };
                break;
            default:   // East
                set.boxes[1] = BlockShape{ glm::vec3(k13, 0.0f, 0.0f), glm::vec3(1.0f, k3, k3) };
                set.boxes[2] = BlockShape{ glm::vec3(k13, 0.0f, k13),  glm::vec3(1.0f, k3, 1.0f) };
                break;
        }
        set.count = 3;
        return set;
    }

    bool IsBedHead(BlockState state) {
        return state.GetName(PropertyId::PART) == "head";
    }

    bool IsBedOccupied(BlockState state) {
        return state.GetName(PropertyId::OCCUPIED) == "true";
    }

    glm::ivec3 BedOtherHalfPos(const glm::ivec3& pos, BlockState state) {
        const Direction facing = BedFacing(state);
        const Direction toOther = IsBedHead(state) ? Opposite(facing) : facing;
        return glm::ivec3(pos.x + StepX(toOther), pos.y, pos.z + StepZ(toOther));
    }

    glm::ivec3 BedHeadPos(const glm::ivec3& pos, BlockState state) {
        return IsBedHead(state) ? pos : BedOtherHalfPos(pos, state);
    }

    bool BedPairIntact(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        if (!IsBedBlock(state.Block())) return false;
        const glm::ivec3 other = BedOtherHalfPos(pos, state);
        const BlockState otherState = level.GetBlockState(other.x, other.y, other.z);
        return otherState.Block() == state.Block() && IsBedHead(otherState) != IsBedHead(state);
    }

    BlockState BedFootPlacementState(BlockState state) {
        return state.SetName(PropertyId::PART, "foot").SetName(PropertyId::OCCUPIED, "false");
    }

    BlockState BedHeadState(BlockState foot) {
        return foot.SetName(PropertyId::PART, "head");
    }

    BlockState BedWithOccupied(BlockState state, bool occupied) {
        return state.SetName(PropertyId::OCCUPIED, occupied ? "true" : "false");
    }

    std::optional<glm::dvec3> FindBedStandUpPosition(const IBlockAccess& level,
                                                     const glm::ivec3& headPos,
                                                     Direction facing, float yaw) {
        // MC: `side` is clockwise of the bed's facing, flipped to the other
        // side when the sleeper already looks that way.
        const Direction right = ClockWise(facing);
        const Direction side  = IsFacingAngle(right, yaw) ? Opposite(right) : right;

        if (IsBunkBed(level, headPos)) {
            // MC findBunkBedStandUpPosition: around this bed, then around the
            // one below, then on top — safe cells first for all three, then
            // the same three again without the danger check.
            const glm::ivec3 below = headPos - glm::ivec3(0, 1, 0);
            const auto surround = SurroundOffsets(facing, side);
            const auto above    = AboveOffsets(facing);
            for (bool safe : {true, false}) {
                if (auto p = FindAtOffsets(level, headPos, surround, safe)) return p;
                if (auto p = FindAtOffsets(level, below,   surround, safe)) return p;
                if (auto p = FindAtOffsets(level, headPos, above,    safe)) return p;
            }
            return std::nullopt;
        }

        // MC bedStandUpOffsets = surround + above, safe pass then unsafe.
        std::array<Offset, 12> offsets{};
        {
            const auto surround = SurroundOffsets(facing, side);
            const auto above    = AboveOffsets(facing);
            size_t i = 0;
            for (const Offset& o : surround) offsets[i++] = o;
            for (const Offset& o : above)    offsets[i++] = o;
        }
        if (auto p = FindAtOffsets(level, headPos, offsets, true)) return p;
        return FindAtOffsets(level, headPos, offsets, false);
    }

    void DestroyStrawBed(ILevelWrite* world, const glm::ivec3& headPos) {
        if (!world) return;
        const BlockState state = world->GetBlockState(headPos.x, headPos.y, headPos.z);
        if (state.Block() != BlockID::StrawBed) return;
        // MC StrawBedBlock.destroyBed:43 — playSound(null, pos,
        // STRAW_BED_BREAK_LEAVE, BLOCKS, 1.0, 1.0).
        world->PlaySound(nullptr, headPos, SoundEvents::STRAW_BED_BREAK_LEAVE, SoundSource::Blocks, 1.0f, 1.0f);
        const glm::ivec3 other = BedOtherHalfPos(headPos, state);
        world->SetBlock(headPos.x, headPos.y, headPos.z, BlockID::Air, World::UpdateFlags::All);
        if (world->GetBlockState(other.x, other.y, other.z).Block() == BlockID::StrawBed) {
            world->SetBlock(other.x, other.y, other.z, BlockID::Air, World::UpdateFlags::All);
        }
    }

    BedRule DimensionBedRule(DimensionId dimension, BlockID bed) {
        using C = BedRule::Check;
        using M = BedRule::Message;
        // MC BedRule.CAN_SLEEP_WHEN_DARK / DESTROY_ON_USE / DESTROY_ON_LEAVE.
        constexpr BedRule kCanSleepWhenDark{C::WhenDark, C::Always, false, false, M::NoSleep};
        constexpr BedRule kDestroyOnUse    {C::Never,    C::Never,  true,  false, M::None};
        constexpr BedRule kDestroyOnLeave  {C::WhenDark, C::Never,  false, true,  M::NoSleep};
        // The Hush: a soundless hollow under a night that never ends. Nothing
        // sleeps there and nothing makes a home of it, but the bed is left
        // whole — no blast, no respawn point.
        constexpr BedRule kHushNoRest      {C::Never,    C::Never,  false, false, M::Hush};
        // Twilight Forest (TFDimensionGenerator: new BedRule(NEVER, ALWAYS,
        // false, Optional.empty())): a bed is a home but never a night's
        // sleep, and says nothing about it.
        constexpr BedRule kTwilightForest  {C::Never,    C::Always, false, false, M::None};
        const bool straw = bed == BlockID::StrawBed;
        switch (dimension) {
            case DimensionId::Overworld:
            case DimensionId::Aether:     // bed_works true: the attribute defaults
                return straw ? kDestroyOnLeave : kCanSleepWhenDark;
            case DimensionId::TwilightForest:
                return straw ? kDestroyOnLeave : kTwilightForest;   // STRAW_BED_RULE left at its default
            case DimensionId::Hush:
                return kHushNoRest;
            case DimensionId::Nether:
            case DimensionId::End:
                return kDestroyOnUse;
        }
        return kCanSleepWhenDark;
    }

    std::string_view HushBedRefusal(uint32_t pick) {
        static constexpr std::string_view kLines[] = {
            "The silence is listening. You cannot rest here.",
            "Something in the dark holds its breath. You dare not close your eyes.",
            "No dawn is coming to wake you. The Hush will not let you sleep.",
            "You lie down, and the quiet leans closer. You cannot rest here.",
        };
        return kLines[pick % (sizeof(kLines) / sizeof(kLines[0]))];
    }

    UseResult BedUse(ILevelWrite* world, const glm::ivec3& clickedPos,
                     IUsePlayer* player, const BlockHitResult& /*hit*/) {
        if (!world || !player) return UseResult::Pass;

        glm::ivec3 pos = clickedPos;
        BlockState state = world->GetBlockState(pos.x, pos.y, pos.z);
        if (!IsBedBlock(state.Block())) return UseResult::Pass;

        // Half a bed is not a bed: MC returns CONSUME (click eaten, nothing
        // happens) when the partner cell does not hold the other part.
        if (!BedPairIntact(*world, pos, state)) return UseResult::Consume;

        // Everything below works on the HEAD.
        if (!IsBedHead(state)) {
            pos   = BedOtherHalfPos(pos, state);
            state = world->GetBlockState(pos.x, pos.y, pos.z);
        }

        // BedRule per dimension (DimensionBedRule): DESTROY_ON_USE in the
        // Nether and the End, for both BED_RULE and STRAW_BED_RULE. A dyed bed
        // (BedBlock.destroyOnUse) removes the head, then the foot, then
        // explodes — the removal runs on both sides (the client predicts the
        // blocks going), the blast is the server's (PlayerSession
        // ::FlushPendingBedUse). A straw bed (StrawBedBlock.destroyOnUse) just
        // breaks, with its sound, and nothing explodes. The Hush's rule does
        // not destroy: it falls through to startSleepInBed, which refuses.
        if (DimensionBedRule(world->GetDimension(), state.Block()).destroyOnUse) {
            if (state.Block() == BlockID::StrawBed) {
                DestroyStrawBed(world, pos);
                return UseResult::SuccessServer;
            }
            world->SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
            const glm::ivec3 foot = BedOtherHalfPos(pos, state);
            if (world->GetBlockState(foot.x, foot.y, foot.z).Block() == state.Block()) {
                world->SetBlock(foot.x, foot.y, foot.z, BlockID::Air, World::UpdateFlags::All);
            }
            player->UseBed(pos, /*destroyOnUse=*/true);
            return UseResult::SuccessServer;
        }

        if (IsBedOccupied(state)) {
            // MC kicks a sleeping villager out first; there are none in
            // beds here, so it is straight to the message.
            player->DisplayClientMessage("This bed is occupied", /*actionBar=*/true);
            return UseResult::SuccessServer;
        }

        // MC player.startSleepInBed(...). The server runs the checks and
        // reports a problem through the action bar; the client only learns
        // that the click was swallowed.
        player->UseBed(pos, /*destroyOnUse=*/false);
        return UseResult::SuccessServer;
    }

} // namespace Game
