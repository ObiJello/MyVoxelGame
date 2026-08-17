// File: src/common/entity/ai/navigation/AmphibiousPathNavigation.hpp
//
// MC AmphibiousPathNavigation + AmphibiousNodeEvaluator, and the frog's two
// small overrides of both.
//
// WHY A GROUND NAVIGATION IS NOT ENOUGH. WalkNodeEvaluator treats water as a
// hazard to route around: it wants a solid block underfoot at every node, and a
// swimming mob has none. A frog on GroundPathNavigation could therefore not
// path THROUGH water at all — the SWIM activity would pick swim targets its
// navigation refused to reach, and the frog would sit at the water's edge
// looking like the behaviour was broken when the pathfinder was.
//
// The amphibious evaluator instead makes water free (malus 0), makes land
// EXPENSIVE (6) and the shoreline more so (4), and adds vertical neighbours so
// a path can rise and dive. The costs are the interesting part: they are what
// make an amphibious mob prefer to swim the whole way rather than climb out and
// walk, which is exactly how frogs behave in MC.
#pragma once

#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"

namespace Game {

    class AmphibiousNodeEvaluator : public WalkNodeEvaluator {
    public:
        explicit AmphibiousNodeEvaluator(bool prefersShallowSwimming)
            : m_prefersShallowSwimming(prefersShallowSwimming) {}

        void Prepare(const IBlockAccess* blocks, Mob* mob) override;
        void Done() override;

        Node*  GetStart() override;
        Target GetTarget(double x, double y, double z) override;
        int    GetNeighbors(Node** out, int maxOut, Node& from) override;
        PathType GetPathType(const PathfindingContext& ctx, int x, int y, int z) override;

    protected:
        bool IsAmphibious() const override { return true; }

    private:
        bool IsVerticalNeighborValid(const Node* vertical, const Node& current) const;

        bool  m_prefersShallowSwimming;
        float m_oldWalkableCost = 0.0f;
        float m_oldWaterBorderCost = 0.0f;
    };

    // MC Frog.FrogNodeEvaluator — a lily pad or big dripleaf is OPEN rather
    // than whatever the block underneath would make it, so a frog will path
    // across a pond by hopping the pads.
    class FrogNodeEvaluator : public AmphibiousNodeEvaluator {
    public:
        FrogNodeEvaluator() : AmphibiousNodeEvaluator(true) {}
        PathType GetPathType(const PathfindingContext& ctx, int x, int y, int z) override;
    };

    class AmphibiousPathNavigation : public PathNavigation {
    public:
        AmphibiousPathNavigation(Mob* mob, EntityLevel* level)
            : PathNavigation(mob, level) {}

    protected:
        std::unique_ptr<PathFinder> CreatePathFinder(int maxVisitedNodes) override;
        bool CanUpdatePath() const override { return true; }
        glm::dvec3 GetTempMobPos() const override;
        double GetGroundY(const glm::dvec3& target) const override { return target.y; }
        bool CanMoveDirectly(const glm::dvec3& from, const glm::dvec3& to) const override;
        bool IsStableDestination(const glm::ivec3& pos) const override;
    };

    // MC Frog.FrogPathNavigation.
    class FrogPathNavigation : public AmphibiousPathNavigation {
    public:
        FrogPathNavigation(Mob* mob, EntityLevel* level)
            : AmphibiousPathNavigation(mob, level) {}

        // MC refuses to cut a WATER_BORDER corner: the shoreline has to be
        // stepped on, not clipped past, or the frog diagonals straight back
        // into the water it was climbing out of.
        bool CanCutCorner(PathType type) const override;

    protected:
        std::unique_ptr<PathFinder> CreatePathFinder(int maxVisitedNodes) override;
    };

} // namespace Game
