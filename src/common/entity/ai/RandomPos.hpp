// File: src/common/entity/ai/RandomPos.hpp
//
// MC net.minecraft.world.entity.ai.util.{RandomPos, DefaultRandomPos,
// GoalUtils} — where a wandering mob decides to go.
//
// The algorithm is "roll 10 candidates, keep the best": each candidate is a
// random offset within a box, rejected outright if it fails the stability and
// malus filters, and otherwise scored by the mob's own GetWalkTargetValue. That
// scoring is what makes cows prefer grass and monsters prefer the dark, so it
// must go through the mob rather than being inlined as a constant.
//
// `hasHome` / restriction handling is omitted: nothing in this port sets a home
// position (no villagers, no golems), so the branches would be dead code. The
// direction-biased variants used by PanicGoal and AvoidEntityGoal are present
// because those two mobs' behaviour depends on them.
#pragma once

#include <glm/glm.hpp>
#include <optional>

namespace Game {

    class PathfinderMob;

    namespace RandomPos {

        // MC DefaultRandomPos.getPos — an unbiased wander target.
        std::optional<glm::dvec3> GetPos(PathfinderMob& mob, int horizontalDist, int verticalDist);

        // MC DefaultRandomPos.getPosAway — biased into the hemisphere pointing
        // AWAY from `avoidPos`. Used by PanicGoal and AvoidEntityGoal; without
        // the bias a fleeing mob picks directions that run past its attacker
        // half the time.
        std::optional<glm::dvec3> GetPosAway(PathfinderMob& mob, int horizontalDist,
                                             int verticalDist, const glm::dvec3& avoidPos);

        // MC DefaultRandomPos.getPosTowards — biased toward `towardsPos` within
        // `maxRadians` of that direction.
        std::optional<glm::dvec3> GetPosTowards(PathfinderMob& mob, int horizontalDist,
                                                int verticalDist, const glm::dvec3& towardsPos,
                                                double maxRadians);

        // MC BehaviorUtils.getRandomSwimmablePos — a wander target for a
        // swimmer: DefaultRandomPos re-rolled until the cell is water-pathable.
        std::optional<glm::dvec3> GetSwimmablePos(PathfinderMob& mob, int horizontalDist,
                                                  int verticalDist);

        // MC HoverRandomPos.getPos — a flyer's perch-seeking wander target:
        // ground-based, lifted hoverMin..hoverMax blocks above the surface.
        std::optional<glm::dvec3> GetHoverPos(PathfinderMob& mob, int horizontalDist,
                                              int verticalDist, double dirX, double dirZ,
                                              double maxRadians, int hoverMaxHeight,
                                              int hoverMinHeight);

        // MC AirAndWaterRandomPos.getPos — the fallback flight target when no
        // perch is found; `flyingHeight` biases the vertical roll.
        std::optional<glm::dvec3> GetAirAndWaterPos(PathfinderMob& mob, int horizontalDist,
                                                    int verticalDist, int flyingHeight,
                                                    double dirX, double dirZ,
                                                    double maxRadians);

        // MC LandRandomPos.getPos — like GetPos but the result is pulled up out
        // of any solid it landed inside, so a swimming mob aims at dry land.
        std::optional<glm::dvec3> GetLandPos(PathfinderMob& mob, int horizontalDist,
                                             int verticalDist);

    } // namespace RandomPos

} // namespace Game
