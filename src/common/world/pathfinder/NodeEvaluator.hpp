// File: src/common/world/pathfinder/NodeEvaluator.hpp
//
// MC net.minecraft.world.level.pathfinder.{NodeEvaluator, WalkNodeEvaluator}.
//
// This is where a block position becomes a graph node: what type it is, what it
// costs, and which neighbours a mob can reach from it. It is also where every
// piece of MC's characteristic navigation behaviour lives — stepping up,
// jumping, cutting diagonals, refusing to squeeze between fence posts, and
// dropping off ledges only within fall-damage range.
//
// Node ownership: nodes are pooled per search in `m_nodes` and handed out as
// raw pointers. The pool is cleared by Prepare() and freed by Done(), so no
// node outlives one call to PathFinder::FindPath. Path copies what it needs.
#pragma once

#include "common/world/pathfinder/Node.hpp"
#include "common/world/pathfinder/PathType.hpp"
#include "common/world/block/Direction.hpp"
#include "common/physics/Physics.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Game {

    class Mob;
    struct IBlockAccess;

    // MC PathfindingContext — the world view a search reads, plus the mob it is
    // searching for. Held by value; it is three pointers.
    struct PathfindingContext {
        const IBlockAccess* blocks = nullptr;
        Mob*                mob = nullptr;

        PathType GetPathTypeFromState(int x, int y, int z) const;
        int      GetMinY() const { return -64; }
    };

    class NodeEvaluator {
    public:
        virtual ~NodeEvaluator() = default;

        virtual void Prepare(const IBlockAccess* blocks, Mob* mob);
        virtual void Done();

        virtual Node*  GetStart() = 0;
        virtual Target GetTarget(double x, double y, double z);
        virtual int    GetNeighbors(Node** out, int maxOut, Node& from) = 0;
        virtual PathType GetPathType(const PathfindingContext& ctx, int x, int y, int z) = 0;

        void SetCanPassDoors(bool v)      { m_canPassDoors = v; }
        void SetCanOpenDoors(bool v)      { m_canOpenDoors = v; }
        void SetCanFloat(bool v)          { m_canFloat = v; }
        void SetCanWalkOverFences(bool v) { m_canWalkOverFences = v; }

        bool CanPassDoors() const      { return m_canPassDoors; }
        bool CanOpenDoors() const      { return m_canOpenDoors; }
        bool CanFloat() const          { return m_canFloat; }
        bool CanWalkOverFences() const { return m_canWalkOverFences; }

    protected:
        Node* GetNode(int x, int y, int z);

        PathfindingContext m_ctx;
        Mob*  m_mob = nullptr;
        int   m_entityWidth = 1, m_entityHeight = 1, m_entityDepth = 1;

        bool m_canPassDoors = true;
        bool m_canOpenDoors = false;
        bool m_canFloat = false;
        bool m_canWalkOverFences = false;

    private:
        // Pool + index. `deque`-like stability matters: GetNode hands out
        // pointers that must survive later insertions, so the nodes live in
        // individually-allocated cells rather than a vector that reallocates.
        std::unordered_map<int64_t, std::unique_ptr<Node>> m_nodes;
    };

    class WalkNodeEvaluator : public NodeEvaluator {
    public:
        static constexpr double kSpaceBetweenWallPosts = 0.5;
        static constexpr double kDefaultMobJumpHeight  = 1.125;

        void Prepare(const IBlockAccess* blocks, Mob* mob) override;
        void Done() override;

        Node* GetStart() override;
        int   GetNeighbors(Node** out, int maxOut, Node& from) override;
        PathType GetPathType(const PathfindingContext& ctx, int x, int y, int z) override;

        // MC getPathTypeOfMob — the type of a position as the WHOLE mob box
        // sees it, which is why a 1.4-wide spider is blocked by geometry a
        // 0.6-wide zombie walks past.
        PathType GetPathTypeOfMob(const PathfindingContext& ctx, int x, int y, int z);

        // MC getPathTypeStatic — the block's own type, promoted by what is
        // underneath and beside it.
        static PathType GetPathTypeStatic(const PathfindingContext& ctx, int x, int y, int z);
        static PathType CheckNeighbourBlocks(const PathfindingContext& ctx,
                                             int x, int y, int z, PathType fallback);

        // Floor height at a position — the top face of the block BELOW it.
        // Everything about step-up and jump decisions is expressed against this.
        static double GetFloorLevel(const IBlockAccess& blocks, const glm::ivec3& pos);

    protected:
        virtual bool IsAmphibious() const { return false; }

        double GetFloorLevel(const glm::ivec3& pos) const;
        double GetMobJumpHeight() const;

        PathType GetCachedPathType(int x, int y, int z);

        // `travelDir` is an index into the evaluator's own horizontal table,
        // NOT a Game::Direction. MC passes a Direction here, but this engine's
        // Direction enum orders its values differently (Down/Up first), so
        // casting an index to it would silently mean a different face. Keeping
        // it an index makes the table the single source of truth.
        //
        // kVerticalTravel is MC passing Direction.UP or Direction.DOWN, whose
        // getStepX/getStepZ are both zero. The amphibious evaluator's vertical
        // neighbours use it; without an explicit value it was spelled -1, and
        // `-1 & 3` is 3, so a vertical step would have silently borrowed the
        // horizontal offsets of the fourth compass direction.
        static constexpr int kVerticalTravel = -1;
        Node* FindAcceptedNode(int x, int y, int z, int jumpSize, double nodeHeight,
                               int travelDir, PathType currentType);

        Node* TryJumpOn(int x, int y, int z, int jumpSize, double nodeHeight,
                        int travelDir, PathType currentType);
        Node* TryFindFirstNonWaterBelow(int x, int y, int z, Node* best);
        Node* TryFindFirstGroundNodeBelow(int x, int y, int z);

        Node* GetNodeAndUpdateCostToMax(int x, int y, int z, PathType type, float cost);
        Node* GetBlockedNode(int x, int y, int z);
        Node* GetClosedNode(int x, int y, int z, PathType type);

        bool IsNeighborValid(const Node* neighbor, const Node& current) const;
        bool IsDiagonalValid(const Node& from, const Node* ew, const Node* ns) const;
        bool IsDiagonalValid(const Node* diagonal) const;

        bool CanStartAt(const glm::ivec3& pos);
        Node* GetStartNode(const glm::ivec3& pos);

        bool HasCollisions(const AABB& box) const;
        bool CanReachWithoutCollision(const Node& target) const;

        // Per-search memo of GetPathTypeOfMob. Hit rate is very high — the
        // 3x3x3 neighbour scan re-reads the same positions constantly.
        std::unordered_map<int64_t, PathType> m_pathTypeCache;

        // The four orthogonal neighbours of the node being expanded, kept so
        // the diagonal pass can test whether both of its components are open.
        Node* m_reusableNeighbors[4] = {nullptr, nullptr, nullptr, nullptr};
    };

} // namespace Game
