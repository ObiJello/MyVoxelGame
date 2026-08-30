// File: src/common/entity/ai/RandomPos.cpp
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/JavaRandom.hpp"

#include <cmath>
#include <limits>

namespace Game::RandomPos {

    namespace {

        constexpr int kAttempts = 10;

        // MC RandomPos.generateRandomDirection — a uniform offset in a box.
        glm::ivec3 GenerateRandomDirection(JavaRandom& rng, int horizontalDist, int verticalDist) {
            const int x = rng.NextInt(2 * horizontalDist + 1) - horizontalDist;
            const int y = rng.NextInt(2 * verticalDist + 1) - verticalDist;
            const int z = rng.NextInt(2 * horizontalDist + 1) - horizontalDist;
            return glm::ivec3(x, y, z);
        }

        // MC RandomPos.generateRandomDirectionWithinRadians, with the Default
        // callers' minHorizontalDist = 0 and flyingHeight = 0 folded in. The
        // load-bearing parts, each of which this used to get wrong:
        //   * dist = lerp(sqrt(u), min, max) * SQRT_2 — sqrt biases the rolls
        //     OUTWARD (area-uniform, not radius-uniform) and SQRT_2 lets a
        //     diagonal reach the full box corner;
        //   * the draw order/type is nextFloat (angle) then nextDouble (dist);
        //   * a candidate outside the |x|,|z| <= max box is REJECTED and
        //     consumes the attempt (MC returns null);
        //   * floor, not round (BlockPos.containing);
        //   * (0,0) direction proceeds — Java's atan2(0,0) is 0, so a mob
        //     standing exactly on its threat still picks an escape arc.
        // (MC's Mth.atan2 is a fast approximation; std::atan2 differs only in
        // the last ulps of the angle, below the 1-block grid this feeds.)
        bool GenerateRandomDirectionWithinRadians(JavaRandom& rng, int horizontalDist,
                                                  int verticalDist, double dirX, double dirZ,
                                                  double maxRadians, glm::ivec3& out) {
            constexpr double kSqrt2 = 1.4142135623730951;
            const double yRadiansCenter =
                std::atan2(dirZ, dirX) - 3.14159265358979323846 / 2.0;
            const double yRadians = yRadiansCenter +
                static_cast<double>(2.0f * rng.NextFloat() - 1.0f) * maxRadians;
            const double maxDist = static_cast<double>(horizontalDist);
            const double dist = std::sqrt(rng.NextDouble()) * maxDist * kSqrt2;
            const double xt = -dist * std::sin(yRadians);
            const double zt =  dist * std::cos(yRadians);
            if (std::abs(xt) > maxDist || std::abs(zt) > maxDist) return false;

            const int yt = rng.NextInt(2 * verticalDist + 1) - verticalDist;
            out.x = static_cast<int>(std::floor(xt));
            out.y = yt;
            out.z = static_cast<int>(std::floor(zt));
            return true;
        }

        // MC GoalUtils.isNotStable asks the mob's OWN navigation: solid-below
        // for a walker, non-air-below for an amphibian, any non-solid cell for
        // a swimmer, standable for a flyer. Hardcoding solid-below here (as
        // this used to) made every water and air wander target invalid.
        bool IsStable(PathfinderMob& mob, const glm::ivec3& pos) {
            return mob.GetNavigation().IsStableDestination(pos);
        }

        // MC GoalUtils.hasMalus — reject anything that is not free to walk on.
        // Note this is `!= 0`, not `< 0`: a merely EXPENSIVE tile (water at 8)
        // is rejected as a wander destination even though it is pathable.
        bool HasMalus(PathfinderMob& mob, const glm::ivec3& pos) {
            PathfindingContext ctx;
            ctx.blocks = mob.Level() ? mob.Level()->Blocks() : nullptr;
            ctx.mob = &mob;
            const PathType type = WalkNodeEvaluator::GetPathTypeStatic(ctx, pos.x, pos.y, pos.z);
            return mob.GetPathfindingMalus(type) != 0.0f;
        }

        bool IsOutsideLimits(const glm::ivec3& pos) {
            return pos.y < -64 || pos.y >= 320;
        }

        // MC GoalUtils.mobRestricted — the mob has a home AND stands close
        // enough to it (homeRadius + horizontalDist + 1 of the home block's
        // CENTRE) that a roll could plausibly land inside. Only then are
        // out-of-home candidates rejected; a mob dragged far from home wanders
        // freely rather than freezing.
        bool MobRestricted(PathfinderMob& mob, double horizontalDist) {
            if (!mob.HasHome()) return false;
            const glm::ivec3& home = mob.GetHomePosition();
            const glm::dvec3 centre(home.x + 0.5, home.y + 0.5, home.z + 0.5);
            const glm::dvec3 d = mob.position - centre;
            const double limit = static_cast<double>(mob.GetHomeRadius()) +
                                 horizontalDist + 1.0;
            return glm::dot(d, d) < limit * limit;
        }

        // MC RandomPos.generateRandomPosTowardDirection — offset + mob
        // position, floored AFTER the add (BlockPos.containing). A homed mob's
        // roll is first shortened back toward the home centre by up to half
        // the horizontal range on each axis — the bias that keeps a restricted
        // mob orbiting its home instead of pinning at the boundary. Note the
        // bias applies whenever the mob HAS a home; the `restrict` flag only
        // controls the rejection below.
        glm::ivec3 GenerateRandomPosTowardDirection(PathfinderMob& mob, double xzDist,
                                                    JavaRandom& rng,
                                                    const glm::ivec3& direction) {
            double xt = static_cast<double>(direction.x);
            double zt = static_cast<double>(direction.z);
            if (mob.HasHome() && xzDist > 1.0) {
                const glm::ivec3& home = mob.GetHomePosition();
                if (mob.position.x > static_cast<double>(home.x)) {
                    xt -= rng.NextDouble() * xzDist / 2.0;
                } else {
                    xt += rng.NextDouble() * xzDist / 2.0;
                }
                if (mob.position.z > static_cast<double>(home.z)) {
                    zt -= rng.NextDouble() * xzDist / 2.0;
                } else {
                    zt += rng.NextDouble() * xzDist / 2.0;
                }
            }
            return glm::ivec3(
                static_cast<int>(std::floor(xt + mob.position.x)),
                static_cast<int>(std::floor(static_cast<double>(direction.y) + mob.position.y)),
                static_cast<int>(std::floor(zt + mob.position.z)));
        }

        // Turn a candidate offset into an accepted absolute position, or
        // reject — MC DefaultRandomPos.generateRandomPosTowardDirection's
        // check chain: outside limits, GoalUtils.isRestricted (home), not
        // stable, has malus.
        bool AcceptCandidate(PathfinderMob& mob, double horizontalDist, bool restrict,
                             JavaRandom& rng, const glm::ivec3& direction,
                             glm::ivec3& out) {
            out = GenerateRandomPosTowardDirection(mob, horizontalDist, rng, direction);

            if (IsOutsideLimits(out))               return false;
            if (restrict && !mob.IsWithinHome(out)) return false;
            if (!IsStable(mob, out))                return false;
            if (HasMalus(mob, out))                 return false;
            return true;
        }

        // MC RandomPos.generateRandomPos — 10 rolls, keep the highest-scoring.
        // The result is the BOTTOM CENTRE of the chosen block, which matters:
        // aiming at the corner makes a mob drift diagonally on every wander.
        template <typename Generator>
        std::optional<glm::dvec3> BestOf(PathfinderMob& mob, Generator&& generate) {
            double bestWeight = -std::numeric_limits<double>::infinity();
            glm::ivec3 bestPos(0);
            bool found = false;

            for (int i = 0; i < kAttempts; ++i) {
                glm::ivec3 candidate;
                if (!generate(candidate)) continue;

                const double weight = static_cast<double>(mob.GetWalkTargetValue(candidate));
                if (weight > bestWeight) {
                    bestWeight = weight;
                    bestPos = candidate;
                    found = true;
                }
            }

            if (!found) return std::nullopt;
            return glm::dvec3(bestPos.x + 0.5, bestPos.y, bestPos.z + 0.5);
        }

    } // namespace

    std::optional<glm::dvec3> GetPos(PathfinderMob& mob, int horizontalDist, int verticalDist) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();
        const bool restrict = MobRestricted(mob, horizontalDist);

        return BestOf(mob, [&](glm::ivec3& out) {
            const glm::ivec3 dir = GenerateRandomDirection(rng, horizontalDist, verticalDist);
            return AcceptCandidate(mob, horizontalDist, restrict, rng, dir, out);
        });
    }

    std::optional<glm::dvec3> GetPosAway(PathfinderMob& mob, int horizontalDist,
                                         int verticalDist, const glm::dvec3& avoidPos) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();

        const glm::dvec3 away = mob.position - avoidPos;
        const bool restrict = MobRestricted(mob, horizontalDist);

        return BestOf(mob, [&](glm::ivec3& out) {
            glm::ivec3 dir;
            // Half pi: the whole hemisphere pointing away from the threat.
            if (!GenerateRandomDirectionWithinRadians(rng, horizontalDist, verticalDist,
                                                      away.x, away.z, 3.14159265358979323846 / 2.0,
                                                      dir)) {
                return false;
            }
            return AcceptCandidate(mob, horizontalDist, restrict, rng, dir, out);
        });
    }

    std::optional<glm::dvec3> GetPosTowards(PathfinderMob& mob, int horizontalDist,
                                            int verticalDist, const glm::dvec3& towardsPos,
                                            double maxRadians) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();

        const glm::dvec3 toward = towardsPos - mob.position;
        const bool restrict = MobRestricted(mob, horizontalDist);

        return BestOf(mob, [&](glm::ivec3& out) {
            glm::ivec3 dir;
            if (!GenerateRandomDirectionWithinRadians(rng, horizontalDist, verticalDist,
                                                      toward.x, toward.z, maxRadians, dir)) {
                return false;
            }
            return AcceptCandidate(mob, horizontalDist, restrict, rng, dir, out);
        });
    }

    std::optional<glm::dvec3> GetSwimmablePos(PathfinderMob& mob, int horizontalDist,
                                              int verticalDist) {
        // MC BehaviorUtils.getRandomSwimmablePos: roll DefaultRandomPos until
        // the target cell is water-pathable, up to ten extra times.
        std::optional<glm::dvec3> target = GetPos(mob, horizontalDist, verticalDist);
        const IBlockAccess* blocks = mob.Level() ? mob.Level()->Blocks() : nullptr;
        if (!blocks) return target;

        const auto isSwimmable = [&](const glm::dvec3& p) {
            const glm::ivec3 bp(static_cast<int>(std::floor(p.x)),
                                static_cast<int>(std::floor(p.y)),
                                static_cast<int>(std::floor(p.z)));
            // MC BlockBehaviour.isPathfindable(WATER): the cell's fluid state
            // IS water — NOT merely "no collision", or the reroll would accept
            // air above the lake and fish would beach themselves aiming at it.
            return blocks->ContainsWater(bp.x, bp.y, bp.z);
        };

        for (int i = 0; target && !isSwimmable(*target) && i < 10; ++i) {
            target = GetPos(mob, horizontalDist, verticalDist);
        }
        return target;
    }

    std::optional<glm::dvec3> GetHoverPos(PathfinderMob& mob, int horizontalDist,
                                          int verticalDist, double dirX, double dirZ,
                                          double maxRadians, int hoverMaxHeight,
                                          int hoverMinHeight) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();
        const IBlockAccess* blocks = mob.Level()->Blocks();

        const auto isSolid = [&](const glm::ivec3& p) {
            return blocks && BlockRegistry::HasCollision(blocks->GetBlock(p.x, p.y, p.z));
        };
        const auto isWater = [&](const glm::ivec3& p) {
            return blocks && blocks->ContainsWater(p.x, p.y, p.z);
        };

        const bool restrict = MobRestricted(mob, horizontalDist);

        return BestOf(mob, [&](glm::ivec3& out) {
            glm::ivec3 dir;
            if (!GenerateRandomDirectionWithinRadians(rng, horizontalDist, verticalDist,
                                                      dirX, dirZ, maxRadians, dir)) {
                return false;
            }

            // MC HoverRandomPos routes through LandRandomPos.
            // generateRandomPosTowardDirection: home bias, then outside-limits
            // / restriction / stability rejection — all BEFORE the perch walk.
            glm::ivec3 pos = GenerateRandomPosTowardDirection(mob, horizontalDist, rng, dir);
            if (IsOutsideLimits(pos))               return false;
            if (restrict && !mob.IsWithinHome(pos)) return false;
            if (!IsStable(mob, pos))                return false;

            // MC RandomPos.moveUpToAboveSolid: if the cell is solid, climb out
            // of it, then up to rand(hoverMax - hoverMin + 1) + hoverMin more
            // blocks while the air stays clear — the perch height.
            const int hover = rng.NextInt(hoverMaxHeight - hoverMinHeight + 1) + hoverMinHeight;
            if (isSolid(pos)) {
                glm::ivec3 p = pos;
                ++p.y;
                while (p.y < 320 && isSolid(p)) ++p.y;
                const int firstFree = p.y;
                while (p.y < 320 && p.y - firstFree < hover) {
                    glm::ivec3 above(p.x, p.y + 1, p.z);
                    if (isSolid(above)) break;
                    ++p.y;
                }
                pos = p;
            }

            if (isWater(pos)) return false;
            if (HasMalus(mob, pos)) return false;
            out = pos;
            return true;
        });
    }

    std::optional<glm::dvec3> GetAirAndWaterPos(PathfinderMob& mob, int horizontalDist,
                                                int verticalDist, int flyingHeight,
                                                double dirX, double dirZ, double maxRadians) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();
        const IBlockAccess* blocks = mob.Level()->Blocks();

        const auto isSolid = [&](const glm::ivec3& p) {
            return blocks && BlockRegistry::HasCollision(blocks->GetBlock(p.x, p.y, p.z));
        };

        const bool restrict = MobRestricted(mob, horizontalDist);

        return BestOf(mob, [&](glm::ivec3& out) {
            glm::ivec3 dir;
            if (!GenerateRandomDirectionWithinRadians(rng, horizontalDist, verticalDist,
                                                      dirX, dirZ, maxRadians, dir)) {
                return false;
            }
            // MC: the flyingHeight bias lifts (or, negative, sinks) the
            // vertical roll.
            dir.y += flyingHeight;

            // MC AirAndWaterRandomPos.generateRandomPos: home-biased position,
            // outside-limits and restriction rejection (no stability test for
            // a flyer), THEN the climb out of solid, then the malus test.
            glm::ivec3 pos = GenerateRandomPosTowardDirection(mob, horizontalDist, rng, dir);
            if (IsOutsideLimits(pos))               return false;
            if (restrict && !mob.IsWithinHome(pos)) return false;

            // moveUpOutOfSolid.
            if (isSolid(pos)) {
                while (pos.y < 320 && isSolid(pos)) ++pos.y;
            }

            if (HasMalus(mob, pos)) return false;
            out = pos;
            return true;
        });
    }

    std::optional<glm::dvec3> GetLandPos(PathfinderMob& mob, int horizontalDist, int verticalDist) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();
        const IBlockAccess* blocks = mob.Level()->Blocks();
        const bool restrict = MobRestricted(mob, horizontalDist);

        return BestOf(mob, [&](glm::ivec3& out) {
            const glm::ivec3 dir = GenerateRandomDirection(rng, horizontalDist, verticalDist);

            // MC LandRandomPos.generateRandomPosTowardDirection: the limits /
            // restriction / STABILITY tests run on the raw candidate, BEFORE
            // any climb — a cell at the bottom of a lake fails stability here
            // and is thrown away rather than promoted to the surface.
            glm::ivec3 candidate = GenerateRandomPosTowardDirection(mob, horizontalDist,
                                                                    rng, dir);
            if (IsOutsideLimits(candidate))               return false;
            if (restrict && !mob.IsWithinHome(candidate)) return false;
            if (!IsStable(mob, candidate))                return false;

            // MC LandRandomPos.movePosUpOutOfSolid: climb out of SOLID blocks
            // only (fluids are not solid), then reject a water end-cell
            // outright — a land wander target is never in the drink.
            if (blocks) {
                while (candidate.y < 320 &&
                       BlockRegistry::HasCollision(
                           blocks->GetBlock(candidate.x, candidate.y, candidate.z))) {
                    ++candidate.y;
                }
                if (blocks->ContainsWater(candidate.x, candidate.y, candidate.z)) {
                    return false;
                }
            }
            if (HasMalus(mob, candidate)) return false;

            out = candidate;
            return true;
        });
    }

} // namespace Game::RandomPos
