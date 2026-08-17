// File: src/common/world/pathfinder/Node.hpp
//
// MC net.minecraft.world.level.pathfinder.{Node, Target, BinaryHeap}.
//
// Nodes are owned by the NodeEvaluator's arena and referred to by raw pointer
// everywhere else — they live exactly as long as one pathfinding call, and the
// A* needs to mutate a node it reaches by a cheaper route after it has already
// been queued. Anything with value semantics here would either copy on every
// neighbour expansion or lose those updates.
#pragma once

#include "common/world/pathfinder/PathType.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace Game {

    struct Node {
        int x = 0, y = 0, z = 0;

        int   heapIdx = -1;      // position in the open set; -1 = not queued
        float g = 0.0f;          // cost from start
        float h = 0.0f;          // heuristic to goal (already FUDGED by 1.5)
        float f = 0.0f;          // g + h, the heap key
        Node* cameFrom = nullptr;
        bool  closed = false;
        float walkedDistance = 0.0f;
        float costMalus = 0.0f;
        PathType type = PathType::Blocked;

        bool InOpenSet() const { return heapIdx >= 0; }

        float DistanceTo(const Node& other) const {
            const float dx = static_cast<float>(other.x - x);
            const float dy = static_cast<float>(other.y - y);
            const float dz = static_cast<float>(other.z - z);
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        float DistanceToSqr(const Node& other) const {
            const float dx = static_cast<float>(other.x - x);
            const float dy = static_cast<float>(other.y - y);
            const float dz = static_cast<float>(other.z - z);
            return dx * dx + dy * dy + dz * dz;
        }

        float DistanceManhattan(const Node& other) const {
            return static_cast<float>(std::abs(other.x - x) +
                                      std::abs(other.y - y) +
                                      std::abs(other.z - z));
        }

        // MC Node.createHash — packs the coordinate into one int so the
        // evaluator can dedupe nodes without a 3-int hash.
        static int64_t CreateHash(int x, int y, int z) {
            return (static_cast<int64_t>(y) & 0xFF)
                 | ((static_cast<int64_t>(x) & 0x7FFF) << 8)
                 | ((static_cast<int64_t>(z) & 0x7FFF) << 24)
                 | (x < 0 ? (int64_t{1} << 63) : 0)
                 | (z < 0 ? (int64_t{1} << 39) : 0);
        }
    };

    // MC Target — a goal node that also remembers the best (closest) node the
    // search reached, so a failed search can still return a partial path
    // toward the target rather than nothing.
    struct Target {
        Node  node;
        float bestHeuristic = 3.4028235e38f;
        Node* bestNode = nullptr;
        bool  reached = false;

        void UpdateBest(float h, Node* candidate) {
            if (h < bestHeuristic) { bestHeuristic = h; bestNode = candidate; }
        }
        void SetReached() { reached = true; }
    };

    // MC BinaryHeap. A hand-rolled min-heap rather than std::priority_queue
    // because the A* must DECREASE a queued node's key in place
    // (changeCost) — something the standard adaptor cannot do.
    class BinaryHeap {
    public:
        void Insert(Node* node);
        Node* Pop();
        void  ChangeCost(Node* node, float newF);
        bool  IsEmpty() const { return m_heap.empty(); }
        void  Clear();
        size_t Size() const { return m_heap.size(); }

    private:
        void UpHeap(int index);
        void DownHeap(int index);

        std::vector<Node*> m_heap;
    };

} // namespace Game
