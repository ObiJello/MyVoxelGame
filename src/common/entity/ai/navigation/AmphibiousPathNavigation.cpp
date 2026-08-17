// File: src/common/entity/ai/navigation/AmphibiousPathNavigation.cpp
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/pathfinder/PathFinder.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ── AmphibiousNodeEvaluator ────────────────────────────────────────────

    void AmphibiousNodeEvaluator::Prepare(const IBlockAccess* blocks, Mob* mob) {
        WalkNodeEvaluator::Prepare(blocks, mob);

        // The three costs that ARE the behaviour. Water becomes free, land
        // becomes expensive, and the shoreline sits between them — so A* routes
        // an amphibious mob through the water rather than around it, and it
        // climbs out only when the destination is genuinely on land.
        //
        // Saved and restored in Done() because the malus map lives on the MOB,
        // not the evaluator: leaving WALKABLE at 6 would make every later path
        // this mob computes avoid dry ground forever.
        mob->SetPathfindingMalus(PathType::Water, 0.0f);
        m_oldWalkableCost = mob->GetPathfindingMalus(PathType::Walkable);
        mob->SetPathfindingMalus(PathType::Walkable, 6.0f);
        m_oldWaterBorderCost = mob->GetPathfindingMalus(PathType::WaterBorder);
        mob->SetPathfindingMalus(PathType::WaterBorder, 4.0f);
    }

    void AmphibiousNodeEvaluator::Done() {
        if (m_mob) {
            m_mob->SetPathfindingMalus(PathType::Walkable, m_oldWalkableCost);
            m_mob->SetPathfindingMalus(PathType::WaterBorder, m_oldWaterBorderCost);
        }
        WalkNodeEvaluator::Done();
    }

    Node* AmphibiousNodeEvaluator::GetStart() {
        if (!m_mob || !m_mob->IsInWater()) return WalkNodeEvaluator::GetStart();
        // Swimming: start from the box's own corner raised half a block, not
        // from the ground below — there is no ground below.
        const AABB box = m_mob->GetAABB();
        return GetStartNode(glm::ivec3(static_cast<int>(std::floor(box.min.x)),
                                       static_cast<int>(std::floor(box.min.y + 0.5)),
                                       static_cast<int>(std::floor(box.min.z))));
    }

    Target AmphibiousNodeEvaluator::GetTarget(double x, double y, double z) {
        // MC aims half a block higher so a swim target sits in the water column
        // rather than on the floor of it.
        return WalkNodeEvaluator::GetTarget(x, y + 0.5, z);
    }

    bool AmphibiousNodeEvaluator::IsVerticalNeighborValid(const Node* vertical,
                                                          const Node& current) const {
        // MC requires the vertical step to be WATER: an amphibious mob may swim
        // up and down, but it may not levitate through air.
        return IsNeighborValid(vertical, current) && vertical->type == PathType::Water;
    }

    int AmphibiousNodeEvaluator::GetNeighbors(Node** out, int maxOut, Node& from) {
        int count = WalkNodeEvaluator::GetNeighbors(out, maxOut, from);

        const PathType typeAbove   = GetCachedPathType(from.x, from.y + 1, from.z);
        const PathType typeCurrent = GetCachedPathType(from.x, from.y, from.z);

        int jumpSize = 0;
        if (m_mob && m_mob->GetPathfindingMalus(typeAbove) >= 0.0f
            && typeCurrent != PathType::StickyHoney) {
            jumpSize = static_cast<int>(std::floor(std::max(1.0f, m_mob->MaxUpStep())));
        }

        const double nodeHeight = GetFloorLevel(glm::ivec3(from.x, from.y, from.z));

        // MC passes Direction.UP / Direction.DOWN, whose horizontal steps are
        // zero; kVerticalTravel is this port's spelling of that.
        Node* up = FindAcceptedNode(from.x, from.y + 1, from.z,
                                    std::max(0, jumpSize - 1), nodeHeight,
                                    kVerticalTravel, typeCurrent);
        Node* down = FindAcceptedNode(from.x, from.y - 1, from.z,
                                      jumpSize, nodeHeight,
                                      kVerticalTravel, typeCurrent);

        if (count < maxOut && IsVerticalNeighborValid(up, from)) {
            out[count++] = up;
        }
        if (count < maxOut && IsVerticalNeighborValid(down, from)
            && typeCurrent != PathType::Trapdoor) {
            out[count++] = down;
        }

        // MC's shallow-swimming preference: deep water costs more, so a frog
        // that prefers shallows hugs the surface instead of diving.
        if (m_prefersShallowSwimming) {
            constexpr int kSeaLevel = 63;
            for (int i = 0; i < count; ++i) {
                if (out[i] && out[i]->type == PathType::Water
                    && out[i]->y < kSeaLevel - 10) {
                    ++out[i]->costMalus;
                }
            }
        }
        return count;
    }

    PathType AmphibiousNodeEvaluator::GetPathType(const PathfindingContext& ctx,
                                                  int x, int y, int z) {
        const PathType type = ctx.GetPathTypeFromState(x, y, z);
        if (type != PathType::Water) {
            return WalkNodeEvaluator::GetPathType(ctx, x, y, z);
        }
        // Water touching a blocked neighbour is a BORDER, which carries its own
        // malus. That is how MC distinguishes open water from the shoreline
        // without a separate scan.
        static const glm::ivec3 kDirs[6] = {
            {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}
        };
        for (const glm::ivec3& d : kDirs) {
            if (ctx.GetPathTypeFromState(x + d.x, y + d.y, z + d.z) == PathType::Blocked) {
                return PathType::WaterBorder;
            }
        }
        return PathType::Water;
    }

    // ── FrogNodeEvaluator ──────────────────────────────────────────────────

    PathType FrogNodeEvaluator::GetPathType(const PathfindingContext& ctx,
                                            int x, int y, int z) {
        // MC BlockTags.FROG_PREFER_JUMP_TO below the node makes it OPEN, which
        // is what lets a frog treat a lily pad as a stepping stone rather than
        // as the water it is floating on.
        if (ctx.blocks) {
            const BlockID below = ctx.blocks->GetBlock(x, y - 1, z);
            if (below == BlockID::LilyPad || below == BlockID::BigDripleaf) {
                return PathType::Open;
            }
        }
        return AmphibiousNodeEvaluator::GetPathType(ctx, x, y, z);
    }

    // ── AmphibiousPathNavigation ───────────────────────────────────────────

    std::unique_ptr<PathFinder> AmphibiousPathNavigation::CreatePathFinder(int maxVisitedNodes) {
        return std::make_unique<PathFinder>(
            std::make_unique<AmphibiousNodeEvaluator>(false), maxVisitedNodes);
    }

    glm::dvec3 AmphibiousPathNavigation::GetTempMobPos() const {
        // Mid-body, not the feet: a swimming mob's feet are a meaningless
        // reference point for how far along its path it is.
        return glm::dvec3(m_mob->position.x,
                          m_mob->position.y + m_mob->GetBbHeight() * 0.5,
                          m_mob->position.z);
    }

    bool AmphibiousPathNavigation::CanMoveDirectly(const glm::dvec3& from,
                                                   const glm::dvec3& to) const {
        // MC only allows the straight-line shortcut while actually IN liquid.
        // On land it must follow the path, because the shortcut test does not
        // know about the step height.
        (void)from; (void)to;
        return m_mob && m_mob->IsInLiquid();
    }

    bool AmphibiousPathNavigation::IsStableDestination(const glm::ivec3& pos) const {
        // MC: anything but air below. A lily pad, a water column or solid
        // ground all count — the point is that the mob will not fall forever.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        return blocks && blocks->GetBlock(pos.x, pos.y - 1, pos.z) != BlockID::Air;
    }

    // ── FrogPathNavigation ─────────────────────────────────────────────────

    bool FrogPathNavigation::CanCutCorner(PathType type) const {
        return type != PathType::WaterBorder && AmphibiousPathNavigation::CanCutCorner(type);
    }

    std::unique_ptr<PathFinder> FrogPathNavigation::CreatePathFinder(int maxVisitedNodes) {
        return std::make_unique<PathFinder>(
            std::make_unique<FrogNodeEvaluator>(), maxVisitedNodes);
    }

} // namespace Game
