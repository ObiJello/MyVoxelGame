// File: src/common/entity/ai/navigation/FlyingPathNavigation.cpp
#include "common/entity/ai/navigation/FlyingPathNavigation.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/world/pathfinder/PathFinder.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/physics/Physics.hpp"
#include "common/core/JavaRandom.hpp"

#include <cmath>

namespace Game {

    namespace {
        int64_t PosKey(int x, int y, int z) {
            return (static_cast<int64_t>(x) & 0x3FFFFFF) |
                   ((static_cast<int64_t>(z) & 0x3FFFFFF) << 26) |
                   ((static_cast<int64_t>(y) & 0xFFF) << 52);
        }
    }

    // ── FlyNodeEvaluator ───────────────────────────────────────────────────

    void FlyNodeEvaluator::Prepare(const IBlockAccess* blocks, Mob* mob) {
        WalkNodeEvaluator::Prepare(blocks, mob);
        m_flyPathTypeCache.clear();
    }

    void FlyNodeEvaluator::Done() {
        m_flyPathTypeCache.clear();
        WalkNodeEvaluator::Done();
    }

    PathType FlyNodeEvaluator::GetCachedFlyPathType(int x, int y, int z) {
        const int64_t key = PosKey(x, y, z);
        const auto it = m_flyPathTypeCache.find(key);
        if (it != m_flyPathTypeCache.end()) return it->second;
        const PathType type = GetPathTypeOfMob(m_ctx, x, y, z);
        m_flyPathTypeCache.emplace(key, type);
        return type;
    }

    Node* FlyNodeEvaluator::GetStart() {
        int startY;
        const glm::ivec3 mobBlock = m_mob->BlockPosition();

        if (CanFloat() && m_mob->IsInWater() && m_ctx.blocks) {
            // Surface first, exactly like the walker's swim-up, but unbounded
            // upward until the water column ends.
            startY = mobBlock.y;
            const int limit = startY + 64;
            while (startY < limit &&
                   m_ctx.blocks->GetBlock(mobBlock.x, startY, mobBlock.z) == BlockID::Water) {
                ++startY;
            }
        } else {
            startY = static_cast<int>(std::floor(m_mob->position.y + 0.5));
        }

        glm::ivec3 startPos(mobBlock.x, startY, mobBlock.z);
        if (m_mob->GetPathfindingMalus(GetCachedFlyPathType(startPos.x, startPos.y,
                                                            startPos.z)) < 0.0f) {
            // MC probes candidate positions around the bounding box (four
            // corners for a big mob, up to ten random cells inside an inflated
            // box for a small one). The corner set covers both faithfully
            // enough for the sizes this port ships.
            const AABB box = m_mob->GetAABB();
            const glm::ivec3 candidates[] = {
                { static_cast<int>(std::floor(box.min.x)), mobBlock.y,
                  static_cast<int>(std::floor(box.min.z)) },
                { static_cast<int>(std::floor(box.min.x)), mobBlock.y,
                  static_cast<int>(std::floor(box.max.z)) },
                { static_cast<int>(std::floor(box.max.x)), mobBlock.y,
                  static_cast<int>(std::floor(box.min.z)) },
                { static_cast<int>(std::floor(box.max.x)), mobBlock.y,
                  static_cast<int>(std::floor(box.max.z)) },
            };
            for (const glm::ivec3& c : candidates) {
                if (m_mob->GetPathfindingMalus(GetCachedFlyPathType(c.x, c.y, c.z)) >= 0.0f) {
                    return GetStartNode(c);
                }
            }
        }
        return GetStartNode(startPos);
    }

    Node* FlyNodeEvaluator::FlyFindAcceptedNode(int x, int y, int z) {
        Node* best = nullptr;
        const PathType pathType = GetCachedFlyPathType(x, y, z);
        const float cost = m_mob->GetPathfindingMalus(pathType);
        if (cost >= 0.0f) {
            best = GetNode(x, y, z);
            best->type = pathType;
            best->costMalus = std::max(best->costMalus, cost);
            // MC: +1 on WALKABLE, which is what makes free flight fractionally
            // cheaper than hugging the ground.
            if (pathType == PathType::Walkable) ++best->costMalus;
        }
        return best;
    }

    int FlyNodeEvaluator::GetNeighbors(Node** out, int maxOut, Node& from) {
        int count = 0;
        const auto push = [&](Node* n) {
            if (count < maxOut && IsOpenNode(n)) out[count++] = n;
        };

        // MC FlyNodeEvaluator.getNeighbors, in its exact order: the six axis
        // steps, then the twelve edge diagonals (each gated on its two
        // orthogonals), then the eight corner diagonals (each gated on all six
        // nodes it clips past).
        Node* south = FlyFindAcceptedNode(from.x, from.y, from.z + 1);
        push(south);
        Node* west = FlyFindAcceptedNode(from.x - 1, from.y, from.z);
        push(west);
        Node* east = FlyFindAcceptedNode(from.x + 1, from.y, from.z);
        push(east);
        Node* north = FlyFindAcceptedNode(from.x, from.y, from.z - 1);
        push(north);
        Node* up = FlyFindAcceptedNode(from.x, from.y + 1, from.z);
        push(up);
        Node* down = FlyFindAcceptedNode(from.x, from.y - 1, from.z);
        push(down);

        const auto edge = [&](int x, int y, int z, const Node* a, const Node* b) -> Node* {
            Node* n = FlyFindAcceptedNode(x, y, z);
            if (IsOpenNode(n) && HasMalus(a) && HasMalus(b)) {
                if (count < maxOut) out[count++] = n;
            }
            return n;
        };

        Node* southUp   = edge(from.x, from.y + 1, from.z + 1, south, up);
        Node* westUp    = edge(from.x - 1, from.y + 1, from.z, west, up);
        Node* eastUp    = edge(from.x + 1, from.y + 1, from.z, east, up);
        Node* northUp   = edge(from.x, from.y + 1, from.z - 1, north, up);
        Node* southDown = edge(from.x, from.y - 1, from.z + 1, south, down);
        Node* westDown  = edge(from.x - 1, from.y - 1, from.z, west, down);
        Node* eastDown  = edge(from.x + 1, from.y - 1, from.z, east, down);
        Node* northDown = edge(from.x, from.y - 1, from.z - 1, north, down);
        Node* northEast = edge(from.x + 1, from.y, from.z - 1, north, east);
        Node* southEast = edge(from.x + 1, from.y, from.z + 1, south, east);
        Node* northWest = edge(from.x - 1, from.y, from.z - 1, north, west);
        Node* southWest = edge(from.x - 1, from.y, from.z + 1, south, west);

        const auto corner = [&](int x, int y, int z, const Node* a, const Node* b,
                                const Node* c, const Node* d, const Node* e,
                                const Node* f) {
            Node* n = FlyFindAcceptedNode(x, y, z);
            if (count < maxOut && IsOpenNode(n) && HasMalus(a) && HasMalus(b) &&
                HasMalus(c) && HasMalus(d) && HasMalus(e) && HasMalus(f)) {
                out[count++] = n;
            }
        };

        corner(from.x + 1, from.y + 1, from.z - 1, northEast, north, east, up, northUp, eastUp);
        corner(from.x + 1, from.y + 1, from.z + 1, southEast, south, east, up, southUp, eastUp);
        corner(from.x - 1, from.y + 1, from.z - 1, northWest, north, west, up, northUp, westUp);
        corner(from.x - 1, from.y + 1, from.z + 1, southWest, south, west, up, southUp, westUp);
        corner(from.x + 1, from.y - 1, from.z - 1, northEast, north, east, down, northDown, eastDown);
        corner(from.x + 1, from.y - 1, from.z + 1, southEast, south, east, down, southDown, eastDown);
        corner(from.x - 1, from.y - 1, from.z - 1, northWest, north, west, down, northDown, westDown);
        corner(from.x - 1, from.y - 1, from.z + 1, southWest, south, west, down, southDown, westDown);

        return count;
    }

    PathType FlyNodeEvaluator::GetPathType(const PathfindingContext& ctx,
                                           int x, int y, int z) {
        // MC FlyNodeEvaluator.getPathType: OPEN air is folded down onto the
        // surface below it, so flight nodes inherit the danger of what they
        // hover over — DANGER_FIRE above lava, FENCE above a fence post.
        PathType type = ctx.GetPathTypeFromState(x, y, z);
        if (type == PathType::Open && y >= ctx.GetMinY() + 1) {
            const PathType below = ctx.GetPathTypeFromState(x, y - 1, z);
            if (below == PathType::DamageFire || below == PathType::Lava) {
                type = PathType::DamageFire;
            } else if (below == PathType::DamageOther) {
                type = PathType::DamageOther;
            } else if (below == PathType::Cocoa) {
                type = PathType::Cocoa;
            } else if (below == PathType::Fence) {
                const glm::ivec3 mobBlock = m_mob ? m_mob->BlockPosition() : glm::ivec3(0);
                if (!(mobBlock.x == x && mobBlock.y == y - 1 && mobBlock.z == z)) {
                    type = PathType::Fence;
                }
            } else {
                type = (below != PathType::Walkable && below != PathType::Open &&
                        below != PathType::Water)
                    ? PathType::Walkable : PathType::Open;
            }
        }

        if (type == PathType::Walkable || type == PathType::Open) {
            type = CheckNeighbourBlocks(ctx, x, y, z, type);
        }
        return type;
    }

    // ── FlyingPathNavigation ───────────────────────────────────────────────

    std::unique_ptr<PathFinder> FlyingPathNavigation::CreatePathFinder(int maxVisitedNodes) {
        return std::make_unique<PathFinder>(std::make_unique<FlyNodeEvaluator>(),
                                            maxVisitedNodes);
    }

    bool FlyingPathNavigation::CanUpdatePath() const {
        // MC FlyingPathNavigation.canUpdatePath: a flyer can always follow its
        // path UNLESS it is riding something — and even then a floater in
        // liquid still updates (a drowning passenger bee keeps swimming up).
        return (CanFloat() && m_mob->IsInLiquid()) || !m_mob->IsPassenger();
    }

    glm::dvec3 FlyingPathNavigation::GetTempMobPos() const {
        return m_mob->position;
    }

    bool FlyingPathNavigation::IsStableDestination(const glm::ivec3& pos) const {
        // MC: getBlockState(pos).entityCanStandOn(level, pos, mob) — a sturdy
        // UP face at the position itself. The engine's solidity test is the
        // same answer for every full-cube perch a flyer is actually offered.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        return blocks && blocks->IsBlockSolid(pos.x, pos.y, pos.z);
    }

    bool FlyingPathNavigation::CanMoveDirectly(const glm::dvec3& from,
                                               const glm::dvec3& to) const {
        // MC isClearForMovementBetween with allowSwimming — the same sampled
        // sweep the ground navigation uses, in three dimensions.
        if (!m_level->Blocks()) return false;

        PhysicsContext ctx = m_level->Physics();
        const glm::vec3 half = m_mob->HalfExtents();
        const glm::dvec3 delta = to - from;
        const double dist = glm::length(delta);
        if (dist < 1.0e-8) return true;
        const int steps = std::max(1, static_cast<int>(std::ceil(dist * 2.0)));

        for (int i = 1; i <= steps; ++i) {
            const glm::dvec3 p = from + delta * (static_cast<double>(i) / steps);
            const AABB box(glm::vec3(p.x, p.y + half.y, p.z), half * 2.0f);
            if (CollidesAt(box, ctx)) return false;
        }
        return true;
    }

    void FlyingPathNavigation::Tick() {
        ++m_tick;
        if (m_hasDelayedRecomputation) RecomputePath();
        if (IsDone()) return;

        if (CanUpdatePath()) {
            FollowThePath();
        } else if (m_path && !m_path->IsDone()) {
            // MC: advance on exact block equality in all three axes.
            const glm::dvec3 nodePos = m_path->GetNextEntityPos(*m_mob);
            const glm::ivec3 mobBlock = m_mob->BlockPosition();
            if (mobBlock.x == static_cast<int>(std::floor(nodePos.x)) &&
                mobBlock.y == static_cast<int>(std::floor(nodePos.y)) &&
                mobBlock.z == static_cast<int>(std::floor(nodePos.z))) {
                m_path->Advance();
            }
        }

        if (IsDone()) return;

        const glm::dvec3 target = m_path->GetNextEntityPos(*m_mob);
        m_mob->GetMoveControl().SetWantedPosition(target.x, target.y, target.z,
                                                  m_speedModifier);
    }

} // namespace Game
