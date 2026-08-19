// File: src/common/world/pathfinder/NodeEvaluator.cpp
#include "common/world/pathfinder/NodeEvaluator.hpp"
#include "common/world/pathfinder/PathTypeTable.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        // MC iterates Direction.Plane.HORIZONTAL in the order N, S, W, E and
        // indexes reusableNeighbors by get2DDataValue(): S=0, W=1, N=2, E=3.
        // The order only has to be SELF-CONSISTENT with the clockwise lookup
        // used by the diagonal pass, so both tables are defined together here.
        struct Horizontal { int dx, dz; };
        constexpr Horizontal kHorizontal[4] = {
            /* 0 South */ {  0,  1 },
            /* 1 West  */ { -1,  0 },
            /* 2 North */ {  0, -1 },
            /* 3 East  */ {  1,  0 },
        };
        // Clockwise from each index: S->W->N->E->S.
        constexpr int kClockwise[4] = { 1, 2, 3, 0 };

        int64_t PosKey(int x, int y, int z) { return Node::CreateHash(x, y, z); }
    }

    // ── PathfindingContext ─────────────────────────────────────────────────

    PathType PathfindingContext::GetPathTypeFromState(int x, int y, int z) const {
        if (!blocks) return PathType::Blocked;
        return GetPathTypeFromBlock(blocks->GetBlock(x, y, z));
    }

    // ── NodeEvaluator ──────────────────────────────────────────────────────

    void NodeEvaluator::Prepare(const IBlockAccess* blocks, Mob* mob) {
        m_ctx.blocks = blocks;
        m_ctx.mob = mob;
        m_mob = mob;
        m_nodes.clear();

        // MC floors width+1 / height+1, so a 0.6-wide mob occupies a 1x2x1
        // cell and a 1.4-wide spider occupies 2x1x2. This is what makes the
        // spider unable to path through a 1-wide gap.
        m_entityWidth  = static_cast<int>(std::floor(mob->GetBbWidth() + 1.0f));
        m_entityHeight = static_cast<int>(std::floor(mob->GetBbHeight() + 1.0f));
        m_entityDepth  = m_entityWidth;
    }

    void NodeEvaluator::Done() {
        m_ctx.blocks = nullptr;
        m_ctx.mob = nullptr;
        m_mob = nullptr;
        m_nodes.clear();
    }

    Node* NodeEvaluator::GetNode(int x, int y, int z) {
        const int64_t key = PosKey(x, y, z);
        auto it = m_nodes.find(key);
        if (it != m_nodes.end()) return it->second.get();

        auto node = std::make_unique<Node>();
        node->x = x; node->y = y; node->z = z;
        Node* raw = node.get();
        m_nodes.emplace(key, std::move(node));
        return raw;
    }

    Target NodeEvaluator::GetTarget(double x, double y, double z) {
        Target t;
        t.node.x = static_cast<int>(std::floor(x));
        t.node.y = static_cast<int>(std::floor(y));
        t.node.z = static_cast<int>(std::floor(z));
        return t;
    }

    // ── WalkNodeEvaluator ──────────────────────────────────────────────────

    void WalkNodeEvaluator::Prepare(const IBlockAccess* blocks, Mob* mob) {
        NodeEvaluator::Prepare(blocks, mob);
        m_pathTypeCache.clear();
    }

    void WalkNodeEvaluator::Done() {
        m_pathTypeCache.clear();
        NodeEvaluator::Done();
    }

    double WalkNodeEvaluator::GetFloorLevel(const IBlockAccess& blocks, const glm::ivec3& pos) {
        // MC: the top face of the block BELOW. An empty shape means the block
        // below is not solid, so the floor is that block's own base.
        const glm::ivec3 below(pos.x, pos.y - 1, pos.z);
        const BlockID id = blocks.GetBlock(below.x, below.y, below.z);
        if (!BlockRegistry::HasCollision(id)) {
            return static_cast<double>(below.y);
        }
        const BlockRegistry::BlockShape& shape =
            BlockRegistry::GetBlockShape(blocks.GetBlockState(below.x, below.y, below.z));
        return static_cast<double>(below.y) + static_cast<double>(shape.max.y);
    }

    double WalkNodeEvaluator::GetFloorLevel(const glm::ivec3& pos) const {
        if (!m_ctx.blocks) return static_cast<double>(pos.y);

        // A floating or amphibious mob treats water as a surface at mid-block,
        // which is what lets it path across the top of a pond.
        if ((CanFloat() || IsAmphibious()) &&
            GetPathTypeFromBlock(m_ctx.blocks->GetBlock(pos.x, pos.y, pos.z)) == PathType::Water) {
            return static_cast<double>(pos.y) + 0.5;
        }
        return GetFloorLevel(*m_ctx.blocks, pos);
    }

    double WalkNodeEvaluator::GetMobJumpHeight() const {
        return std::max(kDefaultMobJumpHeight, static_cast<double>(m_mob->MaxUpStep()));
    }

    bool WalkNodeEvaluator::HasCollisions(const AABB& box) const {
        PhysicsContext ctx;
        ctx.blockAccess = m_ctx.blocks;
        return CollidesAt(box, ctx);
    }

    bool WalkNodeEvaluator::CanReachWithoutCollision(const Node& target) const {
        // MC steps the mob's own box along the straight line to the candidate
        // node. Used only when the CURRENT node has partial collision (a fence
        // or closed door), where "the destination is clear" is not enough —
        // the mob also has to be able to get out of where it is.
        AABB box = m_mob->GetAABB();
        const glm::vec3 size = box.max - box.min;

        glm::dvec3 delta(
            static_cast<double>(target.x) - m_mob->position.x + size.x / 2.0,
            static_cast<double>(target.y) - m_mob->position.y + size.y / 2.0,
            static_cast<double>(target.z) - m_mob->position.z + size.z / 2.0);

        const double maxSize = std::max({size.x, size.y, size.z});
        const double len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / std::max(1.0e-4, maxSize))));
        delta /= static_cast<double>(steps);

        for (int i = 1; i <= steps; ++i) {
            box.min += glm::vec3(delta);
            box.max += glm::vec3(delta);
            if (HasCollisions(box)) return false;
        }
        return true;
    }

    PathType WalkNodeEvaluator::GetCachedPathType(int x, int y, int z) {
        const int64_t key = PosKey(x, y, z);
        auto it = m_pathTypeCache.find(key);
        if (it != m_pathTypeCache.end()) return it->second;

        const PathType type = GetPathTypeOfMob(m_ctx, x, y, z);
        m_pathTypeCache.emplace(key, type);
        return type;
    }

    PathType WalkNodeEvaluator::GetPathType(const PathfindingContext& ctx, int x, int y, int z) {
        return GetPathTypeStatic(ctx, x, y, z);
    }

    PathType WalkNodeEvaluator::GetPathTypeStatic(const PathfindingContext& ctx,
                                                  int x, int y, int z) {
        const PathType own = ctx.GetPathTypeFromState(x, y, z);
        if (own != PathType::Open || y < ctx.GetMinY() + 1) return own;

        // An OPEN position is classified by what is UNDER it: air over ground
        // is WALKABLE, air over air is still OPEN (a mob would fall), air over
        // fire is DAMAGE_FIRE, and so on.
        switch (ctx.GetPathTypeFromState(x, y - 1, z)) {
            case PathType::Open:
            case PathType::Water:
            case PathType::Lava:
            case PathType::Walkable:     return PathType::Open;
            case PathType::DamageFire:   return PathType::DamageFire;
            case PathType::DamageOther:  return PathType::DamageOther;
            case PathType::StickyHoney:  return PathType::StickyHoney;
            case PathType::PowderSnow:   return PathType::DangerPowderSnow;
            case PathType::DamageCautious: return PathType::DamageCautious;
            case PathType::Trapdoor:     return PathType::DangerTrapdoor;
            default:
                return CheckNeighbourBlocks(ctx, x, y, z, PathType::Walkable);
        }
    }

    PathType WalkNodeEvaluator::CheckNeighbourBlocks(const PathfindingContext& ctx,
                                                     int x, int y, int z, PathType fallback) {
        // The 3x3x3 around the position, MINUS its own column. This is what
        // gives water and lava a one-block "keep away" halo — a mob will walk
        // beside a lava pool but the tiles adjacent to it cost extra.
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dz == 0) continue;
                    const PathType t = ctx.GetPathTypeFromState(x + dx, y + dy, z + dz);
                    if (t == PathType::DamageOther) return PathType::DangerOther;
                    if (t == PathType::DamageFire || t == PathType::Lava) return PathType::DangerFire;
                    if (t == PathType::Water) return PathType::WaterBorder;
                    if (t == PathType::DamageCautious) return PathType::DamageCautious;
                }
            }
        }
        return fallback;
    }

    PathType WalkNodeEvaluator::GetPathTypeOfMob(const PathfindingContext& ctx,
                                                 int x, int y, int z) {
        // Collect every type inside the mob's footprint. MC uses an EnumSet;
        // a small fixed bitset over PathType::Count is the same thing without
        // an allocation, and this runs thousands of times per search.
        bool present[static_cast<size_t>(PathType::Count)] = {};

        for (int dx = 0; dx < m_entityWidth; ++dx) {
            for (int dy = 0; dy < m_entityHeight; ++dy) {
                for (int dz = 0; dz < m_entityDepth; ++dz) {
                    PathType t = GetPathType(ctx, x + dx, y + dy, z + dz);

                    // Door handling: a mob that can open doors treats a closed
                    // wooden one as walkable; one that cannot pass doors at all
                    // treats even an open door as solid.
                    if (t == PathType::DoorWoodClosed && CanOpenDoors() && CanPassDoors()) {
                        t = PathType::WalkableDoor;
                    }
                    if (t == PathType::DoorOpen && !CanPassDoors()) {
                        t = PathType::Blocked;
                    }
                    present[static_cast<size_t>(t)] = true;
                }
            }
        }

        if (present[static_cast<size_t>(PathType::Fence)])          return PathType::Fence;
        if (present[static_cast<size_t>(PathType::UnpassableRail)])  return PathType::UnpassableRail;

        // Otherwise: any impassable type present wins outright, else keep the
        // MOST EXPENSIVE passable one. Taking the cheapest would let a mob
        // ignore a hazard that only touches part of its body.
        PathType result = PathType::Blocked;
        for (size_t i = 0; i < static_cast<size_t>(PathType::Count); ++i) {
            if (!present[i]) continue;
            const PathType t = static_cast<PathType>(i);
            if (m_mob->GetPathfindingMalus(t) < 0.0f) return t;
            if (m_mob->GetPathfindingMalus(t) >= m_mob->GetPathfindingMalus(result)) result = t;
        }

        // MC's narrow-mob relaxation: a 1-wide mob standing in something free
        // whose own block is OPEN is treated as OPEN, so it can fall through.
        if (m_entityWidth <= 1 && result != PathType::Open &&
            m_mob->GetPathfindingMalus(result) == 0.0f &&
            GetPathType(ctx, x, y, z) == PathType::Open) {
            return PathType::Open;
        }
        return result;
    }

    // ── Node construction helpers ──────────────────────────────────────────

    Node* WalkNodeEvaluator::GetNodeAndUpdateCostToMax(int x, int y, int z,
                                                       PathType type, float cost) {
        Node* node = GetNode(x, y, z);
        node->type = type;
        node->costMalus = std::max(node->costMalus, cost);
        return node;
    }

    Node* WalkNodeEvaluator::GetBlockedNode(int x, int y, int z) {
        Node* node = GetNode(x, y, z);
        node->type = PathType::Blocked;
        node->costMalus = -1.0f;
        return node;
    }

    Node* WalkNodeEvaluator::GetClosedNode(int x, int y, int z, PathType type) {
        Node* node = GetNode(x, y, z);
        node->closed = true;
        node->type = type;
        node->costMalus = GetDefaultPathMalus(type);
        return node;
    }

    // ── Start node ─────────────────────────────────────────────────────────

    bool WalkNodeEvaluator::CanStartAt(const glm::ivec3& pos) {
        const PathType t = GetCachedPathType(pos.x, pos.y, pos.z);
        return t != PathType::Open && m_mob->GetPathfindingMalus(t) >= 0.0f;
    }

    Node* WalkNodeEvaluator::GetStartNode(const glm::ivec3& pos) {
        Node* node = GetNode(pos.x, pos.y, pos.z);
        node->type = GetCachedPathType(node->x, node->y, node->z);
        node->costMalus = m_mob->GetPathfindingMalus(node->type);
        return node;
    }

    Node* WalkNodeEvaluator::GetStart() {
        const glm::ivec3 mobBlock = m_mob->BlockPosition();
        int startY = mobBlock.y;

        if (!m_ctx.blocks) return GetStartNode(mobBlock);

        if (CanFloat() && m_mob->IsInWater()) {
            // Float up to the surface: the start node for a swimming mob is the
            // first non-water block, not where its feet are. Bounded so a mob at
            // the bottom of an ocean trench does not walk the whole column —
            // MC relies on hitting air, which is not guaranteed here because an
            // unloaded chunk reports air-or-water indistinguishably.
            const int surfaceScanLimit = startY + 64;
            while (startY < surfaceScanLimit &&
                   GetPathTypeFromBlock(m_ctx.blocks->GetBlock(mobBlock.x, startY, mobBlock.z)) ==
                       PathType::Water) {
                ++startY;
            }
            --startY;
        } else if (m_mob->onGround) {
            startY = static_cast<int>(std::floor(m_mob->position.y + 0.5));
        } else {
            // Airborne: walk DOWN to the first thing that would stop a fall, so
            // a mob knocked into the air still paths from where it will land.
            glm::ivec3 probe(mobBlock.x, static_cast<int>(std::floor(m_mob->position.y + 1.0)),
                             mobBlock.z);
            while (probe.y > m_ctx.GetMinY()) {
                startY = probe.y;
                --probe.y;
                const BlockID below = m_ctx.blocks->GetBlock(probe.x, probe.y, probe.z);
                if (below != BlockID::Air && BlockRegistry::HasCollision(below)) break;
            }
        }

        const glm::ivec3 candidate(mobBlock.x, startY, mobBlock.z);
        if (!CanStartAt(candidate)) {
            // Standing on a corner: try each of the four box corners before
            // giving up, which is what stops a mob half-off a ledge from
            // failing to path at all.
            const AABB box = m_mob->GetAABB();
            const glm::ivec3 corners[4] = {
                { static_cast<int>(std::floor(box.min.x)), startY, static_cast<int>(std::floor(box.min.z)) },
                { static_cast<int>(std::floor(box.min.x)), startY, static_cast<int>(std::floor(box.max.z)) },
                { static_cast<int>(std::floor(box.max.x)), startY, static_cast<int>(std::floor(box.min.z)) },
                { static_cast<int>(std::floor(box.max.x)), startY, static_cast<int>(std::floor(box.max.z)) },
            };
            for (const glm::ivec3& c : corners) {
                if (CanStartAt(c)) return GetStartNode(c);
            }
        }

        return GetStartNode(candidate);
    }

    // ── Neighbour expansion ────────────────────────────────────────────────

    bool WalkNodeEvaluator::IsNeighborValid(const Node* neighbor, const Node& current) const {
        // The `current.costMalus < 0` clause lets a mob that is ALREADY stuck
        // somewhere impassable path its way out.
        return neighbor && !neighbor->closed &&
               (neighbor->costMalus >= 0.0f || current.costMalus < 0.0f);
    }

    bool WalkNodeEvaluator::IsDiagonalValid(const Node& from, const Node* ew, const Node* ns) const {
        if (!ns || !ew) return false;
        if (ns->y > from.y || ew->y > from.y) return false;
        if (ew->type == PathType::WalkableDoor || ns->type == PathType::WalkableDoor) return false;

        // A narrow mob may cut the diagonal between two fence posts; a wide one
        // may not. This is why chickens slip through fence corners and cows
        // cannot.
        const bool canPassBetweenPosts = ns->type == PathType::Fence &&
                                         ew->type == PathType::Fence &&
                                         static_cast<double>(m_mob->GetBbWidth()) < kSpaceBetweenWallPosts;

        return (ns->y < from.y || ns->costMalus >= 0.0f || canPassBetweenPosts) &&
               (ew->y < from.y || ew->costMalus >= 0.0f || canPassBetweenPosts);
    }

    bool WalkNodeEvaluator::IsDiagonalValid(const Node* diagonal) const {
        if (!diagonal || diagonal->closed) return false;
        if (diagonal->type == PathType::WalkableDoor) return false;
        return diagonal->costMalus >= 0.0f;
    }

    int WalkNodeEvaluator::GetNeighbors(Node** out, int maxOut, Node& from) {
        int count = 0;
        int jumpSize = 0;

        const PathType typeAbove   = GetCachedPathType(from.x, from.y + 1, from.z);
        const PathType typeCurrent = GetCachedPathType(from.x, from.y, from.z);

        // A mob can only gain height if the space above it is passable and it
        // is not stuck in honey.
        if (m_mob->GetPathfindingMalus(typeAbove) >= 0.0f && typeCurrent != PathType::StickyHoney) {
            jumpSize = static_cast<int>(std::floor(std::max(1.0f, m_mob->MaxUpStep())));
        }

        const double posHeight = GetFloorLevel(glm::ivec3(from.x, from.y, from.z));

        for (int i = 0; i < 4; ++i) {
            const Horizontal& h = kHorizontal[i];
            Node* node = FindAcceptedNode(from.x + h.dx, from.y, from.z + h.dz,
                                          jumpSize, posHeight, i, typeCurrent);
            m_reusableNeighbors[i] = node;
            if (count < maxOut && IsNeighborValid(node, from)) out[count++] = node;
        }

        for (int i = 0; i < 4; ++i) {
            const int j = kClockwise[i];
            if (!IsDiagonalValid(from, m_reusableNeighbors[i], m_reusableNeighbors[j])) continue;

            const int dx = kHorizontal[i].dx + kHorizontal[j].dx;
            const int dz = kHorizontal[i].dz + kHorizontal[j].dz;
            Node* diagonal = FindAcceptedNode(from.x + dx, from.y, from.z + dz,
                                              jumpSize, posHeight, i, typeCurrent);
            if (count < maxOut && IsDiagonalValid(diagonal)) out[count++] = diagonal;
        }

        return count;
    }

    Node* WalkNodeEvaluator::FindAcceptedNode(int x, int y, int z, int jumpSize,
                                              double nodeHeight, int travelDir,
                                              PathType currentType) {
        Node* best = nullptr;

        const double targetFloor = GetFloorLevel(glm::ivec3(x, y, z));
        // Too tall to reach even by jumping — reject before doing any more work.
        if (targetFloor - nodeHeight > GetMobJumpHeight()) return nullptr;

        const PathType type = GetCachedPathType(x, y, z);
        const float cost = m_mob->GetPathfindingMalus(type);

        if (cost >= 0.0f) best = GetNodeAndUpdateCostToMax(x, y, z, type, cost);

        // Leaving a fence or closed door needs a clear line, not just a clear
        // destination.
        const auto hasPartialCollision = [](PathType t) {
            return t == PathType::Fence || t == PathType::DoorWoodClosed ||
                   t == PathType::DoorIronClosed;
        };
        if (hasPartialCollision(currentType) && best && best->costMalus >= 0.0f &&
            !CanReachWithoutCollision(*best)) {
            best = nullptr;
        }

        if (type == PathType::Walkable || (IsAmphibious() && type == PathType::Water)) {
            return best;
        }

        if ((!best || best->costMalus < 0.0f) && jumpSize > 0 &&
            (type != PathType::Fence || CanWalkOverFences()) &&
            type != PathType::UnpassableRail && type != PathType::Trapdoor &&
            type != PathType::PowderSnow) {
            best = TryJumpOn(x, y, z, jumpSize, nodeHeight, travelDir, currentType);
        } else if (!IsAmphibious() && type == PathType::Water && !CanFloat()) {
            best = TryFindFirstNonWaterBelow(x, y, z, best);
        } else if (type == PathType::Open) {
            best = TryFindFirstGroundNodeBelow(x, y, z);
        } else if (hasPartialCollision(type) && !best) {
            best = GetClosedNode(x, y, z, type);
        }

        return best;
    }

    Node* WalkNodeEvaluator::TryJumpOn(int x, int y, int z, int jumpSize, double nodeHeight,
                                       int travelDir, PathType currentType) {
        // Recurse one block higher with one less jump allowance.
        Node* above = FindAcceptedNode(x, y + 1, z, jumpSize - 1, nodeHeight,
                                       travelDir, currentType);
        if (!above) return nullptr;

        // Mobs at least a block wide always fit; narrower ones need the headroom
        // check below, which is what stops a chicken jumping into a 1-block slot
        // it cannot actually occupy.
        if (m_mob->GetBbWidth() >= 1.0f) return above;
        if (above->type != PathType::Open && above->type != PathType::Walkable) return above;

        // A vertical step has no horizontal component, which is what MC's
        // Direction.UP.getStepX() == 0 says.
        const int stepX = (travelDir == kVerticalTravel) ? 0 : kHorizontal[travelDir & 3].dx;
        const int stepZ = (travelDir == kVerticalTravel) ? 0 : kHorizontal[travelDir & 3].dz;

        const double centerX = static_cast<double>(x - stepX) + 0.5;
        const double centerZ = static_cast<double>(z - stepZ) + 0.5;
        const double halfWidth = static_cast<double>(m_mob->GetBbWidth()) / 2.0;

        AABB grow;
        grow.min = glm::vec3(
            centerX - halfWidth,
            GetFloorLevel(glm::ivec3(static_cast<int>(std::floor(centerX)), y + 1,
                                     static_cast<int>(std::floor(centerZ)))) + 0.001,
            centerZ - halfWidth);
        grow.max = glm::vec3(
            centerX + halfWidth,
            static_cast<double>(m_mob->GetBbHeight()) +
                GetFloorLevel(glm::ivec3(above->x, above->y, above->z)) - 0.002,
            centerZ + halfWidth);

        return HasCollisions(grow) ? nullptr : above;
    }

    Node* WalkNodeEvaluator::TryFindFirstNonWaterBelow(int x, int y, int z, Node* best) {
        --y;
        while (y > m_ctx.GetMinY()) {
            const PathType t = GetCachedPathType(x, y, z);
            if (t != PathType::Water) return best;
            best = GetNodeAndUpdateCostToMax(x, y, z, t, m_mob->GetPathfindingMalus(t));
            --y;
        }
        return best;
    }

    Node* WalkNodeEvaluator::TryFindFirstGroundNodeBelow(int x, int y, int z) {
        // MC caps the drop at getMaxFallDistance (3 by default), which is what
        // stops mobs from cheerfully pathing off cliffs.
        const int maxFall = 3;

        for (int currentY = y - 1; currentY >= m_ctx.GetMinY(); --currentY) {
            if (y - currentY > maxFall) return GetBlockedNode(x, currentY, z);

            const PathType t = GetCachedPathType(x, currentY, z);
            const float cost = m_mob->GetPathfindingMalus(t);
            if (t != PathType::Open) {
                if (cost >= 0.0f) return GetNodeAndUpdateCostToMax(x, currentY, z, t, cost);
                return GetBlockedNode(x, currentY, z);
            }
        }
        return GetBlockedNode(x, y, z);
    }

} // namespace Game
