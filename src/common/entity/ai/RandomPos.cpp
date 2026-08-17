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

        // MC RandomPos.generateRandomDirectionWithinRadians — same, but only
        // accepted when it falls within `maxRadians` of (dirX, dirZ). MC
        // computes the angle directly; rejection-sampling the box version would
        // change the distribution, so the angle test is done explicitly.
        bool GenerateRandomDirectionWithinRadians(JavaRandom& rng, int horizontalDist,
                                                  int verticalDist, double dirX, double dirZ,
                                                  double maxRadians, glm::ivec3& out) {
            if (dirX == 0.0 && dirZ == 0.0) return false;

            const double baseAngle = std::atan2(dirZ, dirX);
            // Uniform angle inside the allowed arc, uniform radius inside the
            // allowed distance — MC's formulation.
            const double angle = baseAngle + (2.0 * rng.NextDouble() - 1.0) * maxRadians;
            const double radius = 1.0 + rng.NextDouble() * (static_cast<double>(horizontalDist) - 1.0);

            out.x = static_cast<int>(std::round(std::cos(angle) * radius));
            out.z = static_cast<int>(std::round(std::sin(angle) * radius));
            out.y = rng.NextInt(2 * verticalDist + 1) - verticalDist;
            return true;
        }

        // MC GoalUtils.isNotStable — the block below must be solid, or the mob
        // would be aiming at thin air.
        bool IsStable(const PathfinderMob& mob, const glm::ivec3& pos) {
            const IBlockAccess* blocks = mob.Level() ? mob.Level()->Blocks() : nullptr;
            if (!blocks) return false;
            return BlockRegistry::HasCollision(blocks->GetBlock(pos.x, pos.y - 1, pos.z));
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

        // Turn a candidate offset into an accepted absolute position, or reject.
        bool AcceptCandidate(PathfinderMob& mob, const glm::ivec3& direction, glm::ivec3& out) {
            out = glm::ivec3(
                static_cast<int>(std::floor(mob.position.x + direction.x)),
                static_cast<int>(std::floor(mob.position.y + direction.y)),
                static_cast<int>(std::floor(mob.position.z + direction.z)));

            if (IsOutsideLimits(out)) return false;
            if (!IsStable(mob, out))  return false;
            if (HasMalus(mob, out))   return false;
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

        return BestOf(mob, [&](glm::ivec3& out) {
            const glm::ivec3 dir = GenerateRandomDirection(rng, horizontalDist, verticalDist);
            return AcceptCandidate(mob, dir, out);
        });
    }

    std::optional<glm::dvec3> GetPosAway(PathfinderMob& mob, int horizontalDist,
                                         int verticalDist, const glm::dvec3& avoidPos) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();

        const glm::dvec3 away = mob.position - avoidPos;

        return BestOf(mob, [&](glm::ivec3& out) {
            glm::ivec3 dir;
            // Half pi: the whole hemisphere pointing away from the threat.
            if (!GenerateRandomDirectionWithinRadians(rng, horizontalDist, verticalDist,
                                                      away.x, away.z, 3.14159265358979323846 / 2.0,
                                                      dir)) {
                return false;
            }
            return AcceptCandidate(mob, dir, out);
        });
    }

    std::optional<glm::dvec3> GetPosTowards(PathfinderMob& mob, int horizontalDist,
                                            int verticalDist, const glm::dvec3& towardsPos,
                                            double maxRadians) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();

        const glm::dvec3 toward = towardsPos - mob.position;

        return BestOf(mob, [&](glm::ivec3& out) {
            glm::ivec3 dir;
            if (!GenerateRandomDirectionWithinRadians(rng, horizontalDist, verticalDist,
                                                      toward.x, toward.z, maxRadians, dir)) {
                return false;
            }
            return AcceptCandidate(mob, dir, out);
        });
    }

    std::optional<glm::dvec3> GetLandPos(PathfinderMob& mob, int horizontalDist, int verticalDist) {
        if (!mob.Level()) return std::nullopt;
        JavaRandom& rng = mob.Level()->Random();
        const IBlockAccess* blocks = mob.Level()->Blocks();

        return BestOf(mob, [&](glm::ivec3& out) {
            const glm::ivec3 dir = GenerateRandomDirection(rng, horizontalDist, verticalDist);

            glm::ivec3 candidate(
                static_cast<int>(std::floor(mob.position.x + dir.x)),
                static_cast<int>(std::floor(mob.position.y + dir.y)),
                static_cast<int>(std::floor(mob.position.z + dir.z)));

            // MC LandRandomPos: climb up out of anything solid or liquid, so a
            // mob swimming in a lake aims at the shore rather than the bottom.
            if (blocks) {
                const int maxY = candidate.y + 8;
                while (candidate.y <= maxY &&
                       (BlockRegistry::HasCollision(blocks->GetBlock(candidate.x, candidate.y, candidate.z)) ||
                        blocks->IsBlockFluid(candidate.x, candidate.y, candidate.z))) {
                    ++candidate.y;
                }
            }

            if (IsOutsideLimits(candidate)) return false;
            if (!IsStable(mob, candidate))  return false;
            if (HasMalus(mob, candidate))   return false;

            out = candidate;
            return true;
        });
    }

} // namespace Game::RandomPos
