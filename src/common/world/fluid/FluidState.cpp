// File: src/common/world/fluid/FluidState.cpp
#include "common/world/fluid/FluidState.hpp"

#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/tags/DataTags.hpp"

#include <glm/geometric.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>

namespace Game {

    namespace {

        // MC FluidRenderer / FlowingFluid.getFlow: the height a neighbour one
        // step down is compared against, 8/9 — the top of a full cell.
        constexpr float kMaxFluidHeight = 0.8888889f;

        // ── Tag cache ──────────────────────────────────────────────────────
        //
        // 0 = not yet resolved, 1 = not in the tag, 2 = in the tag. Atomic so
        // the client's mesher workers (FluidFlow → BlockBlocksFluidFlow) and
        // the server thread can all fill it without a lock; a racing pair
        // resolves the same answer twice, which is harmless.
        enum class Tag : uint8_t { BlocksMotion, AllSigns, WashedAway, SoulFireBase, Count };

        struct TagCache {
            std::array<std::atomic<uint8_t>, BlockRegistry::Size> cells{};
        };
        TagCache& CacheFor(Tag tag) {
            static TagCache caches[static_cast<size_t>(Tag::Count)];
            return caches[static_cast<size_t>(tag)];
        }

        bool HasTag(const std::vector<std::string>& tags, const char* tag) {
            // TagsFor's list is sorted, so this is a binary search.
            return std::binary_search(tags.begin(), tags.end(), std::string(tag));
        }

        bool ResolveTag(Tag tag, BlockID id) {
            // Air has no BlockDefs.inc row and so no registry slug, but it
            // is the one block every tag question here must get right: it
            // is in #washed_away_by_fluids (via #air) — the cell water flows
            // INTO — and in nothing else.
            if (id == BlockID::Air) return tag == Tag::WashedAway;
            const Block& def = BlockRegistry::Get(id);
            if (def.registrySlug.empty()) return false;
            const auto& tags = DataTags::TagsFor(DataTags::Registry::Block, def.registrySlug);
            switch (tag) {
                case Tag::BlocksMotion:
                    // VanillaBlockTagsProvider: BLOCKS_MOTION =
                    // #blocks_motion_no_leaves + #leaves.
                    return HasTag(tags, "#minecraft:blocks_motion_no_leaves") ||
                           HasTag(tags, "#minecraft:leaves");
                case Tag::AllSigns:     return HasTag(tags, "#minecraft:all_signs");
                case Tag::WashedAway:   return HasTag(tags, "#minecraft:washed_away_by_fluids");
                case Tag::SoulFireBase: return HasTag(tags, "#minecraft:soul_fire_base_blocks");
                default:                return false;
            }
        }

        bool InTag(Tag tag, BlockID id) {
            const size_t i = static_cast<size_t>(id);
            if (i >= BlockRegistry::Size) return false;
            std::atomic<uint8_t>& cell = CacheFor(tag).cells[i];
            uint8_t v = cell.load(std::memory_order_relaxed);
            if (v == 0) {
                v = ResolveTag(tag, id) ? 2 : 1;
                cell.store(v, std::memory_order_relaxed);
            }
            return v == 2;
        }

        constexpr Direction kHorizontal[4] = {
            Direction::North, Direction::South, Direction::West, Direction::East
        };

        // MC Vec3.normalize: the zero vector (length < 1e-4) stays zero.
        glm::dvec3 Normalize(const glm::dvec3& v) {
            const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return len < 1.0e-4 ? glm::dvec3(0.0) : v / len;
        }

        // MC FlowingFluid.affectsFlow: empty, or the same fluid.
        bool AffectsFlow(const FluidState& neighbour, FluidType self) {
            return neighbour.IsEmpty() || neighbour.IsSame(self);
        }

        // MC FlowingFluid.isSolidFace(level, pos, direction). Note vanilla's
        // quirk, kept: the face asked about is `direction` OF THE NEIGHBOUR,
        // i.e. its far face, not the one it presents to the fluid.
        bool IsSolidFace(const IBlockAccess& level, const glm::ivec3& p, Direction direction,
                         FluidType self) {
            const BlockState state = level.GetBlockState(p.x, p.y, p.z);
            if (FluidStateOf(state).IsSame(self)) return false;
            if (direction == Direction::Up) return true;
            if (IsIceBlock(state.Block())) return false;
            return IsStateFaceSturdy(state, direction);
        }

    } // namespace

    FluidState FluidStateOf(BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Water || id == BlockID::Lava) {
            const FluidType type = id == BlockID::Water ? FluidType::Water : FluidType::Lava;
            // LiquidBlock.getFluidState: stateCache.get(min(level, 8)), where
            // the cache is [source, flowing(7..1), flowing(8, falling)].
            int level = state.GetIndex(PropertyId::LEVEL);
            if (level < 0) level = 0;
            if (level == 0) return FluidState::Source(type);
            if (level >= 8) return FluidState::Flowing(type, FluidState::kAmountFull, true);
            return FluidState::Flowing(type, FluidState::kAmountFull - level, false);
        }
        if (id == BlockID::Air) return FluidState::Empty();
        // SimpleWaterloggedBlock.getFluidState: `WATERLOGGED ? Fluids.WATER
        // .getSource(false) : super`; kelp, seagrass and the bubble column
        // answer Fluids.WATER unconditionally.
        if (BlockRegistry::ContainsWater(state)) return FluidState::Source(FluidType::Water);
        return FluidState::Empty();
    }

    FluidState GetFluidState(const IBlockAccess& level, int x, int y, int z) {
        const BlockID id = level.GetBlock(x, y, z);
        // Air is most of the world; skip the state read for it.
        if (id == BlockID::Air) return FluidState::Empty();
        return FluidStateOf(level.GetBlockState(x, y, z));
    }

    float FluidHeight(const IBlockAccess& level, const glm::ivec3& pos, const FluidState& state) {
        if (state.IsEmpty()) return 0.0f;
        // FlowingFluid.hasSameAbove.
        if (GetFluidState(level, pos.x, pos.y + 1, pos.z).IsSame(state.type)) return 1.0f;
        return state.OwnHeight();
    }

    float FluidHeightForCamera(const IBlockAccess& level, const glm::ivec3& pos, const FluidState& state) {
        if (state.IsSource()) {
            const glm::ivec3 above(pos.x, pos.y + 1, pos.z);
            if (IsFaceSturdyAt(level, above, Direction::Down)) return 1.0f;
        }
        return FluidHeight(level, pos, state);
    }

    glm::dvec3 FluidFlow(const IBlockAccess& level, const glm::ivec3& pos, const FluidState& state) {
        if (state.IsEmpty()) return glm::dvec3(0.0);

        double flowX = 0.0;
        double flowZ = 0.0;
        const float ownHeight = state.OwnHeight();

        for (Direction direction : kHorizontal) {
            const glm::ivec3 np(pos.x + StepX(direction), pos.y, pos.z + StepZ(direction));
            const FluidState neighbourFluid = GetFluidState(level, np);
            if (!AffectsFlow(neighbourFluid, state.type)) continue;

            float neighbourHeight = neighbourFluid.OwnHeight();
            float distance = 0.0f;
            if (neighbourHeight == 0.0f) {
                // An open neighbour: the fluid is running over an edge, so
                // the gradient is measured to the cell below it.
                if (!BlockBlocksFluidFlow(level.GetBlock(np.x, np.y, np.z))) {
                    const FluidState belowNeighbour = GetFluidState(level, np.x, np.y - 1, np.z);
                    if (AffectsFlow(belowNeighbour, state.type)) {
                        neighbourHeight = belowNeighbour.OwnHeight();
                        if (neighbourHeight > 0.0f) {
                            distance = ownHeight - (neighbourHeight - kMaxFluidHeight);
                        }
                    }
                }
            } else if (neighbourHeight > 0.0f) {
                distance = ownHeight - neighbourHeight;
            }

            if (distance != 0.0f) {
                flowX += static_cast<double>(static_cast<float>(StepX(direction)) * distance);
                flowZ += static_cast<double>(static_cast<float>(StepZ(direction)) * distance);
            }
        }

        glm::dvec3 flow(flowX, 0.0, flowZ);
        if (state.falling) {
            // A falling column pressed against a wall runs DOWN it: any
            // horizontal neighbour (or the one above it) with a solid face
            // bends the flow steeply downward.
            for (Direction direction : kHorizontal) {
                const glm::ivec3 np(pos.x + StepX(direction), pos.y, pos.z + StepZ(direction));
                if (IsSolidFace(level, np, direction, state.type) ||
                    IsSolidFace(level, glm::ivec3(np.x, np.y + 1, np.z), direction, state.type)) {
                    flow = Normalize(flow) + glm::dvec3(0.0, -6.0, 0.0);
                    break;
                }
            }
        }
        return Normalize(flow);
    }

    int FluidLegacyLevel(const FluidState& state) {
        if (state.IsSource()) return 0;
        return 8 - std::min<int>(state.amount, 8) + (state.falling ? 8 : 0);
    }

    BlockState FluidLegacyBlock(const FluidState& state) {
        if (state.IsEmpty()) return BlockState{};
        return BlockStates::Default(FluidBlockId(state.type))
            .SetIndex(PropertyId::LEVEL, FluidLegacyLevel(state));
    }

    BlockID FluidBlockId(FluidType type) {
        switch (type) {
            case FluidType::Water: return BlockID::Water;
            case FluidType::Lava:  return BlockID::Lava;
            default:               return BlockID::Air;
        }
    }

    bool BlockBlocksMotion(BlockID id)        { return InTag(Tag::BlocksMotion, id); }
    bool BlockBlocksFluidFlow(BlockID id)     { return InTag(Tag::BlocksMotion, id) || InTag(Tag::AllSigns, id); }
    bool BlockWashedAwayByFluids(BlockID id)  { return InTag(Tag::WashedAway, id); }
    bool BlockBlocksLavaFireSpread(BlockID id){ return InTag(Tag::BlocksMotion, id); }
    bool IsSoulFireBaseBlock(BlockID id)      { return InTag(Tag::SoulFireBase, id); }

    bool IsIceBlock(BlockID id) {
        return id == BlockID::Ice || id == BlockID::FrostedIce;
    }

} // namespace Game
