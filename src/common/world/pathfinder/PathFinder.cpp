// File: src/common/world/pathfinder/PathFinder.cpp
#include "common/world/pathfinder/PathFinder.hpp"
#include "common/entity/Mob.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <limits>

namespace Game {

    std::optional<Path> PathFinder::FindPath(const IBlockAccess* blocks, Mob* mob,
                                             const std::vector<glm::ivec3>& targets,
                                             float maxPathLength, int reachRange,
                                             float maxVisitedNodesMultiplier) {
        PROFILE_ZONE_N("Path.FindPath");

        if (targets.empty()) return std::nullopt;

        m_openSet.Clear();
        m_evaluator->Prepare(blocks, mob);

        Node* from = m_evaluator->GetStart();
        if (!from) {
            m_evaluator->Done();
            return std::nullopt;
        }

        std::vector<Target> targetNodes;
        targetNodes.reserve(targets.size());
        for (const glm::ivec3& t : targets) {
            targetNodes.push_back(m_evaluator->GetTarget(t.x, t.y, t.z));
        }

        std::optional<Path> path = Search(from, targetNodes, targets, maxPathLength,
                                          reachRange, maxVisitedNodesMultiplier);
        m_evaluator->Done();
        return path;
    }

    float PathFinder::GetBestH(Node& from, std::vector<Target>& targets) const {
        float best = std::numeric_limits<float>::max();
        for (Target& t : targets) {
            const float h = from.DistanceTo(t.node);
            // Every evaluated node updates each target's best-so-far, which is
            // what makes a failed search still able to return a partial path.
            t.UpdateBest(h, &from);
            best = std::min(h, best);
        }
        return best;
    }

    Path PathFinder::ReconstructPath(Node* closest, const glm::ivec3& target, bool reached) {
        std::vector<Node> nodes;
        for (Node* n = closest; n != nullptr; n = n->cameFrom) {
            nodes.push_back(*n);
        }
        std::reverse(nodes.begin(), nodes.end());
        return Path(std::move(nodes), target, reached);
    }

    std::optional<Path> PathFinder::Search(Node* from, std::vector<Target>& targets,
                                           const std::vector<glm::ivec3>& targetPositions,
                                           float maxPathLength, int reachRange,
                                           float maxVisitedNodesMultiplier) {
        from->g = 0.0f;
        from->h = GetBestH(*from, targets);
        from->f = from->h;

        m_openSet.Clear();
        m_openSet.Insert(from);

        int visited = 0;
        const int maxVisited = static_cast<int>(m_maxVisitedNodes * maxVisitedNodesMultiplier);
        bool anyReached = false;

        while (!m_openSet.IsEmpty()) {
            if (++visited >= maxVisited) break;

            Node* current = m_openSet.Pop();
            current->closed = true;

            for (Target& t : targets) {
                if (current->DistanceManhattan(t.node) <= static_cast<float>(reachRange)) {
                    t.SetReached();
                    t.bestNode = current;
                    anyReached = true;
                }
            }
            if (anyReached) break;

            // Radius cap. MC uses `continue` semantics via an if-block: the
            // node stays closed but is not expanded, so the search keeps
            // draining the queue rather than terminating.
            if (current->DistanceTo(*from) >= maxPathLength) continue;

            const int neighborCount =
                m_evaluator->GetNeighbors(m_neighbors, 32, *current);

            for (int i = 0; i < neighborCount; ++i) {
                Node* neighbor = m_neighbors[i];
                const float distance = current->DistanceTo(*neighbor);
                neighbor->walkedDistance = current->walkedDistance + distance;

                // The malus is added to g, so an expensive tile costs its malus
                // in extra blocks of detour — see PathType.hpp.
                const float tentativeG = current->g + distance + neighbor->costMalus;

                if (neighbor->walkedDistance >= maxPathLength) continue;
                if (neighbor->InOpenSet() && tentativeG >= neighbor->g) continue;

                neighbor->cameFrom = current;
                neighbor->g = tentativeG;
                neighbor->h = GetBestH(*neighbor, targets) * kFudging;

                if (neighbor->InOpenSet()) {
                    m_openSet.ChangeCost(neighbor, neighbor->g + neighbor->h);
                } else {
                    neighbor->f = neighbor->g + neighbor->h;
                    m_openSet.Insert(neighbor);
                }
            }
        }

        // ── Pick a result ──────────────────────────────────────────────────
        //
        // Reached: shortest by node count among the targets that were reached.
        // Not reached: the partial path that ends closest to its target, with
        // node count as the tiebreak. Both orders are MC's.
        std::optional<Path> best;
        if (anyReached) {
            for (size_t i = 0; i < targets.size(); ++i) {
                if (!targets[i].reached || !targets[i].bestNode) continue;
                Path candidate = ReconstructPath(targets[i].bestNode, targetPositions[i], true);
                if (!best || candidate.GetNodeCount() < best->GetNodeCount()) {
                    best = std::move(candidate);
                }
            }
        } else {
            for (size_t i = 0; i < targets.size(); ++i) {
                if (!targets[i].bestNode) continue;
                Path candidate = ReconstructPath(targets[i].bestNode, targetPositions[i], false);
                if (!best) {
                    best = std::move(candidate);
                } else {
                    const float bd = best->GetDistToTarget();
                    const float cd = candidate.GetDistToTarget();
                    if (cd < bd || (cd == bd && candidate.GetNodeCount() < best->GetNodeCount())) {
                        best = std::move(candidate);
                    }
                }
            }
        }

        return best;
    }

} // namespace Game
