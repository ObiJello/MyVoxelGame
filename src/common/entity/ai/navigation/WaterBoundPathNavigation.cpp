// File: src/common/entity/ai/navigation/WaterBoundPathNavigation.cpp
#include "common/entity/ai/navigation/WaterBoundPathNavigation.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/pathfinder/PathFinder.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"
#include "common/physics/Physics.hpp"

#include <cmath>

namespace Game {

    namespace {
        int64_t PosKey(int x, int y, int z) {
            return (static_cast<int64_t>(x) & 0x3FFFFFF) |
                   ((static_cast<int64_t>(z) & 0x3FFFFFF) << 26) |
                   ((static_cast<int64_t>(y) & 0xFFF) << 52);
        }
    }

    // ── SwimNodeEvaluator ──────────────────────────────────────────────────

    void SwimNodeEvaluator::Prepare(const IBlockAccess* blocks, Mob* mob) {
        NodeEvaluator::Prepare(blocks, mob);
        m_pathTypeCache.clear();
    }

    void SwimNodeEvaluator::Done() {
        NodeEvaluator::Done();
        m_pathTypeCache.clear();
    }

    Node* SwimNodeEvaluator::GetStart() {
        // MC: the box corner, half a block up — the search starts INSIDE the
        // body of water the mob occupies.
        const AABB box = m_mob->GetAABB();
        return GetNode(static_cast<int>(std::floor(box.min.x)),
                       static_cast<int>(std::floor(box.min.y + 0.5)),
                       static_cast<int>(std::floor(box.min.z)));
    }

    PathType SwimNodeEvaluator::GetCachedBlockType(int x, int y, int z) {
        const int64_t key = PosKey(x, y, z);
        const auto it = m_pathTypeCache.find(key);
        if (it != m_pathTypeCache.end()) return it->second;
        const PathType type = GetPathType(m_ctx, x, y, z);
        m_pathTypeCache.emplace(key, type);
        return type;
    }

    Node* SwimNodeEvaluator::SwimFindAcceptedNode(int x, int y, int z) {
        Node* best = nullptr;
        const PathType pathType = GetCachedBlockType(x, y, z);
        if ((m_allowBreaching && pathType == PathType::Breach) ||
            pathType == PathType::Water) {
            const float cost = m_mob->GetPathfindingMalus(pathType);
            if (cost >= 0.0f) {
                best = GetNode(x, y, z);
                best->type = pathType;
                best->costMalus = std::max(best->costMalus, cost);
                // MC: a node whose own cell holds no fluid costs +8 — the
                // out-of-water surcharge that keeps fish submerged.
                if (m_ctx.blocks && !m_ctx.blocks->IsBlockFluid(x, y, z)) {
                    best->costMalus += 8.0f;
                }
            }
        }
        return best;
    }

    int SwimNodeEvaluator::GetNeighbors(Node** out, int maxOut, Node& from) {
        int count = 0;

        // MC order: the six axis directions (DOWN, UP, NORTH, SOUTH, WEST,
        // EAST — Direction.values() order), then the four horizontal
        // diagonals, each gated on its two orthogonals having a malus.
        static constexpr int kSteps[6][3] = {
            { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
            { -1, 0, 0 }, { 1, 0, 0 },
        };
        Node* axis[6] = {};
        for (int i = 0; i < 6; ++i) {
            axis[i] = SwimFindAcceptedNode(from.x + kSteps[i][0], from.y + kSteps[i][1],
                                           from.z + kSteps[i][2]);
            if (count < maxOut && IsNodeValid(axis[i])) out[count++] = axis[i];
        }

        // Direction.Plane.HORIZONTAL iteration: SOUTH, WEST, NORTH, EAST with
        // getClockWise pairs (SOUTH->WEST, WEST->NORTH, NORTH->EAST,
        // EAST->SOUTH). Axis indices: NORTH 2, SOUTH 3, WEST 4, EAST 5.
        static constexpr int kDiagonals[4][2] = {
            { 3, 4 },   // south + west
            { 4, 2 },   // west + north
            { 2, 5 },   // north + east
            { 5, 3 },   // east + south
        };
        for (const auto& d : kDiagonals) {
            const Node* a = axis[d[0]];
            const Node* b = axis[d[1]];
            if (HasMalus(a) && HasMalus(b)) {
                Node* diag = SwimFindAcceptedNode(
                    from.x + kSteps[d[0]][0] + kSteps[d[1]][0], from.y,
                    from.z + kSteps[d[0]][2] + kSteps[d[1]][2]);
                if (count < maxOut && IsNodeValid(diag)) out[count++] = diag;
            }
        }

        return count;
    }

    PathType SwimNodeEvaluator::GetPathType(const PathfindingContext& ctx,
                                            int x, int y, int z) {
        if (!ctx.blocks) return PathType::Blocked;

        // MC getPathTypeOfMob: every cell of the mob's box must be water; a
        // cell of passable air makes the whole position BREACH; anything else
        // makes it BLOCKED.
        for (int dx = 0; dx < m_entityWidth; ++dx) {
            for (int dy = 0; dy < m_entityHeight; ++dy) {
                for (int dz = 0; dz < m_entityDepth; ++dz) {
                    const int cx = x + dx, cy = y + dy, cz = z + dz;
                    const bool fluid = ctx.blocks->IsBlockFluid(cx, cy, cz);
                    const BlockID block = ctx.blocks->GetBlock(cx, cy, cz);
                    if (!fluid && block == BlockID::Air) {
                        return PathType::Breach;
                    }
                    if (!ctx.blocks->ContainsWater(cx, cy, cz)) {
                        return PathType::Blocked;
                    }
                }
            }
        }

        // The origin cell itself must be passable water (not, say, inside a
        // waterlogged full block).
        const BlockID block = ctx.blocks->GetBlock(x, y, z);
        return BlockRegistry::HasCollision(block) ? PathType::Blocked : PathType::Water;
    }

    // ── WaterBoundPathNavigation ───────────────────────────────────────────

    std::unique_ptr<PathFinder> WaterBoundPathNavigation::CreatePathFinder(int maxVisitedNodes) {
        auto evaluator = std::make_unique<SwimNodeEvaluator>(m_allowBreaching);
        evaluator->SetCanPassDoors(false);
        return std::make_unique<PathFinder>(std::move(evaluator), maxVisitedNodes);
    }

    bool WaterBoundPathNavigation::CanUpdatePath() const {
        return m_allowBreaching || m_mob->IsInLiquid();
    }

    glm::dvec3 WaterBoundPathNavigation::GetTempMobPos() const {
        // MC getY(0.5) — halfway up the body.
        return glm::dvec3(m_mob->position.x,
                          m_mob->position.y + m_mob->GetBbHeight() * 0.5,
                          m_mob->position.z);
    }

    bool WaterBoundPathNavigation::CanMoveDirectly(const glm::dvec3& from,
                                                   const glm::dvec3& to) const {
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

    bool WaterBoundPathNavigation::IsStableDestination(const glm::ivec3& pos) const {
        // MC: !isSolidRender — any block that is not a full solid cube is a
        // fine place for a swimmer to stop.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return false;
        return !IsCollisionShapeFullBlock(*blocks, pos.x, pos.y, pos.z);
    }

} // namespace Game
