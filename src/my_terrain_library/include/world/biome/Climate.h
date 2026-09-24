#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <memory>
#include <functional>
#include <unordered_map>

#include "levelgen/density/DensitySampler.h"

// Forward declarations
namespace minecraft {
    namespace core {
        class BlockPos;
    }
}

// Reference: net/minecraft/world/level/biome/Climate.java

namespace minecraft {
namespace world {
namespace biome {

/**
 * Climate - Climate parameter system for biome selection
 *
 * This class handles:
 * - Climate parameter sampling (temperature, humidity, continentalness, erosion, depth, weirdness)
 * - Climate parameter quantization (float to long)
 * - Target point creation for biome lookup
 * - Parameter ranges for biome definitions
 * - RTree-based biome search
 */
class Climate {
public:
    // Forward declarations of nested types
    struct Parameter;
    struct ParameterPoint;
    struct TargetPoint;
    template<typename T> class RTree;
    template<typename T> class ParameterList;
    class Sampler;

    // Reference: Climate.java line 28
    static constexpr float QUANTIZATION_FACTOR = 10000.0F;

    // Reference: Climate.java line 30
    static constexpr int PARAMETER_COUNT = 7;

    /**
     * Parameter - A range of climate parameter values
     * Reference: Climate.java lines 341-383
     */
    struct Parameter {
        int64_t m_min;
        int64_t m_max;

        // Constructor
        Parameter(int64_t min, int64_t max) : m_min(min), m_max(max) {}
        Parameter() : m_min(0), m_max(0) {}

        // Accessors (matching Java record)
        int64_t min() const { return m_min; }
        int64_t max() const { return m_max; }

        /**
         * Create a point parameter (min == max)
         * Reference: Climate.java lines 344-346
         */
        static Parameter point(float value) {
            return span(value, value);
        }

        /**
         * Create a span parameter from two float values
         * Reference: Climate.java lines 348-354
         */
        static Parameter span(float min, float max) {
            if (min > max) {
                throw std::invalid_argument("min > max: " + std::to_string(min) + " " + std::to_string(max));
            }
            return Parameter(quantizeCoord(min), quantizeCoord(max));
        }

        /**
         * Create a span parameter from two Parameter values
         * Reference: Climate.java lines 356-363
         */
        static Parameter span(const Parameter& min, const Parameter& max) {
            if (min.min() > max.max()) {
                throw std::invalid_argument("min > max: Parameter span error");
            }
            return Parameter(min.min(), max.max());
        }

        /**
         * Calculate distance from a target value to this range
         * Reference: Climate.java lines 369-373
         */
        int64_t distance(int64_t target) const {
            int64_t above = target - m_max;
            int64_t below = m_min - target;
            return above > 0 ? above : std::max(below, static_cast<int64_t>(0));
        }

        /**
         * Calculate distance from a target parameter to this range
         * Reference: Climate.java lines 375-379
         */
        int64_t distance(const Parameter& target) const {
            int64_t above = target.min() - m_max;
            int64_t below = m_min - target.max();
            return above > 0 ? above : std::max(below, static_cast<int64_t>(0));
        }

        /**
         * Merge this parameter with another, expanding to cover both ranges
         * Reference: Climate.java lines 381-383
         */
        Parameter span(const Parameter* other) const {
            if (other == nullptr) {
                return *this;
            }
            return Parameter(std::min(m_min, other->min()), std::max(m_max, other->max()));
        }

        // Quantization helper (uses Climate's static method)
        static int64_t quantizeCoord(float coord) {
            return static_cast<int64_t>(coord * QUANTIZATION_FACTOR);
        }
    };

    /**
     * TargetPoint - A sampled climate point with quantized values
     * Reference: Climate.java lines 322-327
     */
    struct TargetPoint {
        int64_t temperature;
        int64_t humidity;
        int64_t continentalness;
        int64_t erosion;
        int64_t depth;
        int64_t weirdness;

        TargetPoint(int64_t temp, int64_t humid, int64_t cont, int64_t eros, int64_t dep, int64_t weird)
            : temperature(temp)
            , humidity(humid)
            , continentalness(cont)
            , erosion(eros)
            , depth(dep)
            , weirdness(weird)
        {
        }

        /**
         * Convert to a 7-element array for RTree distance calculations
         * Reference: Climate.java lines 324-326
         * Note: The 7th element is 0L (for offset distance)
         */
        std::vector<int64_t> toParameterArray() const {
            return {temperature, humidity, continentalness, erosion, depth, weirdness, 0};
        }
    };

    /**
     * ParameterPoint - Climate parameter ranges for biome definitions
     * Reference: Climate.java lines 329-339
     */
    struct ParameterPoint {
        Parameter temperature;
        Parameter humidity;
        Parameter continentalness;
        Parameter erosion;
        Parameter depth;
        Parameter weirdness;
        int64_t offset;

        ParameterPoint(const Parameter& temp, const Parameter& humid, const Parameter& cont,
                      const Parameter& eros, const Parameter& dep, const Parameter& weird,
                      int64_t off)
            : temperature(temp)
            , humidity(humid)
            , continentalness(cont)
            , erosion(eros)
            , depth(dep)
            , weirdness(weird)
            , offset(off)
        {
        }

        ParameterPoint()
            : temperature(), humidity(), continentalness()
            , erosion(), depth(), weirdness(), offset(0)
        {
        }

        /**
         * Calculate fitness (squared distance) to a target point
         * Reference: Climate.java lines 332-334
         *
         * CRITICAL: This is the main function used for biome matching.
         * Lower fitness = better match.
         */
        int64_t fitness(const TargetPoint& target) const {
            return square(temperature.distance(target.temperature)) +
                   square(humidity.distance(target.humidity)) +
                   square(continentalness.distance(target.continentalness)) +
                   square(erosion.distance(target.erosion)) +
                   square(depth.distance(target.depth)) +
                   square(weirdness.distance(target.weirdness)) +
                   square(offset);
        }

        /**
         * Get the parameter space as a list of 7 parameters
         * Reference: Climate.java lines 336-338
         */
        std::vector<Parameter> parameterSpace() const {
            return {
                temperature,
                humidity,
                continentalness,
                erosion,
                depth,
                weirdness,
                Parameter(offset, offset)  // offset as a point parameter
            };
        }

    private:
        static int64_t square(int64_t x) {
            return x * x;
        }
    };

    // =========================================================================
    // RTree - R-Tree data structure for efficient biome lookup
    // Reference: Climate.java lines 61-271
    // =========================================================================

    template<typename T>
    class RTree {
    public:
        // Reference: 26.3 Climate.RTree.CHILDREN_PER_NODE (ParameterList
        // builds with 19; 1.18-1.21 used 6).
        static constexpr int CHILDREN_PER_NODE = 19;

        class Node;
        class Leaf;
        class SubTree;

    private:
        std::unique_ptr<Node> m_root;

        // Java keeps a ThreadLocal<Leaf> lastResult per RTree instance: the
        // previous answer seeds the next search, which decides ties between
        // equally distant leaves. A few owner-tagged slots per thread give
        // every live tree its own history (a thread alternating between two
        // dimensions' trees keeps both), without a hash lookup per search.
        struct LastResultSlot {
            const RTree* owner = nullptr;
            Leaf* leaf = nullptr;
        };
        static constexpr int LAST_RESULT_SLOTS = 8;

        static std::array<LastResultSlot, LAST_RESULT_SLOTS>& lastResultSlots() {
            static thread_local std::array<LastResultSlot, LAST_RESULT_SLOTS> slots{};
            return slots;
        }

        Leaf* getLastResult() const {
            for (const LastResultSlot& slot : lastResultSlots()) {
                if (slot.owner == this) return slot.leaf;
            }
            return nullptr;
        }

        void setLastResult(Leaf* leaf) const {
            auto& slots = lastResultSlots();
            for (LastResultSlot& slot : slots) {
                if (slot.owner == this) { slot.leaf = leaf; return; }
            }
            // A new tree on this thread: take the first free slot, else
            // shift out the oldest (a tree is only ever evicted once more than
            // LAST_RESULT_SLOTS trees alternate on one thread).
            for (LastResultSlot& slot : slots) {
                if (slot.owner == nullptr) { slot.owner = this; slot.leaf = leaf; return; }
            }
            for (int i = 1; i < LAST_RESULT_SLOTS; ++i) slots[static_cast<size_t>(i - 1)] = slots[static_cast<size_t>(i)];
            slots[LAST_RESULT_SLOTS - 1] = LastResultSlot{this, leaf};
        }

    public:
        /**
         * Node - Base class for RTree nodes
         * Reference: Climate.RTree.Node
         */
        class Node {
        public:
            std::vector<Parameter> parameterSpace;

            Node(const std::vector<Parameter>& params) : parameterSpace(params) {}
            virtual ~Node() = default;

            virtual Leaf* search(const int64_t* target, Leaf* candidate) = 0;

            // Node.distance: the squared distance of each of the 7 targets to
            // this node's span, summed.
            int64_t distance(const int64_t* target) const {
                int64_t dist = 0;
                for (size_t i = 0; i < 7; ++i) {
                    int64_t d = parameterSpace[i].distance(target[i]);
                    dist += d * d;  // Mth.square
                }
                return dist;
            }
        };

        class Leaf : public Node {
        public:
            T value;

            Leaf(const ParameterPoint& point, const T& val)
                : Node(point.parameterSpace()), value(val) {}

            Leaf* search(const int64_t*, Leaf*) override {
                return this;
            }
        };

        class SubTree : public Node {
        public:
            std::vector<std::unique_ptr<Node>> children;

            SubTree(std::vector<std::unique_ptr<Node>>&& kids)
                : Node(buildParameterSpace(kids)), children(std::move(kids)) {}

            // Reference: Climate.RTree.SubTree.search.
            Leaf* search(const int64_t* target, Leaf* candidate) override {
                int64_t minDistance = candidate == nullptr ? INT64_MAX : candidate->distance(target);
                Leaf* closestLeaf = candidate;

                for (auto& child : children) {
                    int64_t childDistance = child->distance(target);
                    if (minDistance > childDistance) {
                        Leaf* leaf = child->search(target, closestLeaf);
                        int64_t leafDistance = (child.get() == leaf) ? childDistance : leaf->distance(target);
                        if (minDistance > leafDistance) {
                            minDistance = leafDistance;
                            closestLeaf = leaf;
                        }
                    }
                }

                return closestLeaf;
            }

            // RTree.buildParameterSpace: the per-dimension span of the children.
            static std::vector<Parameter> buildParameterSpace(const std::vector<std::unique_ptr<Node>>& kids) {
                if (kids.empty()) {
                    throw std::invalid_argument("SubTree needs at least one child");
                }
                std::vector<Parameter> bounds(7);
                bool first = true;
                for (const auto& child : kids) {
                    for (size_t d = 0; d < 7; ++d) {
                        bounds[d] = first ? child->parameterSpace[d] : child->parameterSpace[d].span(&bounds[d]);
                    }
                    first = false;
                }
                return bounds;
            }
        };

    private:
        RTree(std::unique_ptr<Node>&& root) : m_root(std::move(root)) {}

    public:
        /**
         * Reference: Climate.RTree.create(values, childrenPerNode).
         */
        static RTree<T> create(const std::vector<std::pair<ParameterPoint, T>>& values,
                               int childrenPerNode = CHILDREN_PER_NODE) {
            if (values.empty()) {
                throw std::invalid_argument("Need at least one value to build the search tree.");
            }
            const size_t dimensions = values[0].first.parameterSpace().size();
            if (dimensions != 7) {
                throw std::runtime_error("Expecting parameter space to be 7, got " + std::to_string(dimensions));
            }
            std::vector<std::unique_ptr<Node>> leaves;
            leaves.reserve(values.size());
            for (const auto& p : values) {
                leaves.push_back(std::make_unique<Leaf>(p.first, p.second));
            }
            return RTree<T>(build(dimensions, std::move(leaves), childrenPerNode));
        }

        /**
         * Reference: Climate.RTree.search - Node.distance is the only metric.
         */
        T search(const TargetPoint& target) {
            const int64_t targetArray[7] = {
                target.temperature, target.humidity, target.continentalness,
                target.erosion, target.depth, target.weirdness, 0
            };
            Leaf* leaf = m_root->search(targetArray, getLastResult());
            setLastResult(leaf);
            return leaf->value;
        }

    private:
        // (min + max) / 2 of one dimension (Java long division).
        static int64_t center(const Node& node, size_t dimension) {
            const Parameter& parameter = node.parameterSpace[dimension];
            return (parameter.min() + parameter.max()) / 2;
        }

        // RTree.sort: a STABLE sort (List.sort is TimSort) by the centre of
        // `dimension`, then of each following dimension in turn.
        static void sortNodes(std::vector<std::unique_ptr<Node>>& children, size_t dimensions, size_t dimension,
                              bool absolute) {
            std::stable_sort(children.begin(), children.end(),
                [dimensions, dimension, absolute](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) {
                    for (size_t i = 0; i < dimensions; ++i) {
                        const size_t d = (dimension + i) % dimensions;
                        int64_t centerA = center(*a, d);
                        int64_t centerB = center(*b, d);
                        if (absolute) {
                            centerA = std::abs(centerA);
                            centerB = std::abs(centerB);
                        }
                        if (centerA != centerB) return centerA < centerB;
                    }
                    return false;
                });
        }

        // RTree.bucketize: runs of expectedChildrenCount consecutive nodes,
        // expectedChildrenCount = childrenPerNode ^ floor(log(n - 0.01) / log(childrenPerNode)).
        static std::vector<std::vector<size_t>> bucketize(size_t nodeCount, int childrenPerNode) {
            const int expectedChildrenCount = static_cast<int>(std::pow(
                static_cast<double>(childrenPerNode),
                std::floor(std::log(static_cast<double>(nodeCount) - 0.01) /
                           std::log(static_cast<double>(childrenPerNode)))));
            std::vector<std::vector<size_t>> buckets;
            std::vector<size_t> children;
            for (size_t i = 0; i < nodeCount; ++i) {
                children.push_back(i);
                if (static_cast<int>(children.size()) >= expectedChildrenCount) {
                    buckets.push_back(std::move(children));
                    children.clear();
                }
            }
            if (!children.empty()) buckets.push_back(std::move(children));
            return buckets;
        }

        // RTree.cost over the span of a bucket's nodes.
        static int64_t bucketCost(const std::vector<std::unique_ptr<Node>>& nodes, const std::vector<size_t>& bucket) {
            std::vector<Parameter> bounds(7);
            bool first = true;
            for (size_t index : bucket) {
                const Node& node = *nodes[index];
                for (size_t d = 0; d < 7; ++d) {
                    bounds[d] = first ? node.parameterSpace[d] : node.parameterSpace[d].span(&bounds[d]);
                }
                first = false;
            }
            int64_t result = 0;
            for (const Parameter& parameter : bounds) {
                result += std::abs(parameter.max() - parameter.min());
            }
            return result;
        }

        /**
         * Reference: Climate.RTree.build. For more than childrenPerNode
         * children: for each dimension in turn, re-sort the SAME list (each
         * stable sort starting from the previous one's order) and bucketize;
         * the first cheapest split's buckets are kept as they were at that
         * point, sorted as whole buckets by |centre| of that dimension, and
         * each is built recursively.
         */
        static std::unique_ptr<Node> build(size_t dimensions, std::vector<std::unique_ptr<Node>>&& children,
                                           int childrenPerNode) {
            if (children.empty()) {
                throw std::runtime_error("Need at least one child to build a node");
            }
            if (children.size() == 1) {
                return std::move(children[0]);
            }
            if (children.size() <= static_cast<size_t>(childrenPerNode)) {
                std::stable_sort(children.begin(), children.end(),
                    [dimensions](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) {
                        int64_t totalA = 0;
                        int64_t totalB = 0;
                        for (size_t d = 0; d < dimensions; ++d) {
                            totalA += std::abs(center(*a, d));
                            totalB += std::abs(center(*b, d));
                        }
                        return totalA < totalB;
                    });
                return std::make_unique<SubTree>(std::move(children));
            }

            // Node pointers stay valid while `children` is re-sorted, so the
            // winning buckets are recorded as node pointers.
            int64_t minCost = INT64_MAX;
            size_t minDimension = 0;
            std::vector<std::vector<Node*>> minBuckets;
            for (size_t d = 0; d < dimensions; ++d) {
                sortNodes(children, dimensions, d, false);
                const std::vector<std::vector<size_t>> buckets = bucketize(children.size(), childrenPerNode);
                int64_t totalCost = 0;
                for (const auto& bucket : buckets) totalCost += bucketCost(children, bucket);
                if (minCost > totalCost) {
                    minCost = totalCost;
                    minDimension = d;
                    minBuckets.clear();
                    for (const auto& bucket : buckets) {
                        std::vector<Node*> nodes;
                        nodes.reserve(bucket.size());
                        for (size_t index : bucket) nodes.push_back(children[index].get());
                        minBuckets.push_back(std::move(nodes));
                    }
                }
            }

            // Take ownership back from `children` in the winning buckets' order.
            std::unordered_map<Node*, std::unique_ptr<Node>> owned;
            owned.reserve(children.size());
            for (auto& child : children) {
                Node* raw = child.get();
                owned.emplace(raw, std::move(child));
            }
            std::vector<std::unique_ptr<Node>> bucketTrees;
            bucketTrees.reserve(minBuckets.size());
            for (const auto& bucket : minBuckets) {
                std::vector<std::unique_ptr<Node>> members;
                members.reserve(bucket.size());
                for (Node* raw : bucket) members.push_back(std::move(owned.at(raw)));
                bucketTrees.push_back(std::make_unique<SubTree>(std::move(members)));
            }
            // sort(minBuckets, dimensions, minDimension, true) - by the
            // buckets' own spans.
            sortNodes(bucketTrees, dimensions, minDimension, true);
            std::vector<std::unique_ptr<Node>> built;
            built.reserve(bucketTrees.size());
            for (auto& bucketTree : bucketTrees) {
                auto* subTree = static_cast<SubTree*>(bucketTree.get());
                built.push_back(build(dimensions, std::move(subTree->children), childrenPerNode));
            }
            return std::make_unique<SubTree>(std::move(built));
        }
    };

    // =========================================================================
    // ParameterList - List of biome parameter points with RTree index
    // Reference: Climate.java lines 273-320
    // =========================================================================

    template<typename T>
    class ParameterList {
    private:
        std::vector<std::pair<ParameterPoint, T>> m_values;
        RTree<T> m_index;

    public:
        ParameterList(const std::vector<std::pair<ParameterPoint, T>>& values)
            : m_values(values), m_index(RTree<T>::create(values)) {}

        const std::vector<std::pair<ParameterPoint, T>>& values() const {
            return m_values;
        }

        /**
         * Find the value with the best match to the target
         * Reference: Climate.java lines 290-292
         */
        T findValue(const TargetPoint& target) {
            return findValueIndex(target);
        }

        /**
         * Brute force search (for testing)
         * Reference: Climate.java lines 295-311
         */
        T findValueBruteForce(const TargetPoint& target) const {
            auto it = m_values.begin();
            const auto& first = *it;
            int64_t bestFitness = first.first.fitness(target);
            T best = first.second;

            ++it;
            while (it != m_values.end()) {
                int64_t fitness = it->first.fitness(target);
                if (fitness < bestFitness) {
                    bestFitness = fitness;
                    best = it->second;
                }
                ++it;
            }

            return best;
        }

        /**
         * Find value using RTree index
         * Reference: Climate.java lines 313-315
         */
        T findValueIndex(const TargetPoint& target) {
            return m_index.search(target);
        }
    };

    // =========================================================================
    // Sampler - Samples climate parameters from density functions
    // Reference: Climate.java lines 386-398
    // =========================================================================

    // Reference: 26.3 Climate.Sampler (record of six DensitySampler.Bound):
    // the router's climate functions bound to one SamplerContext. Point
    // queries go through sample(); the chunk biome fill samples whole
    // volumes (MultiNoiseBiomeSource.createResolverForChunk) through the
    // bound samplers directly. A default-constructed Sampler is unbound.
    class Sampler {
    public:
        Sampler() = default;
        Sampler(levelgen::density::BoundSampler temperature, levelgen::density::BoundSampler humidity,
                levelgen::density::BoundSampler continentalness, levelgen::density::BoundSampler erosion,
                levelgen::density::BoundSampler depth, levelgen::density::BoundSampler weirdness)
            : m_temperature(temperature), m_humidity(humidity), m_continentalness(continentalness),
              m_erosion(erosion), m_depth(depth), m_weirdness(weirdness) {}

        /**
         * Sample climate parameters at a quart position
         * Reference: 26.3 Climate.Sampler.sample
         */
        TargetPoint sample(int32_t quartX, int32_t quartY, int32_t quartZ) const;

        bool isBound() const { return m_temperature.sampler != nullptr; }

        const levelgen::density::BoundSampler& temperature() const { return m_temperature; }
        const levelgen::density::BoundSampler& humidity() const { return m_humidity; }
        const levelgen::density::BoundSampler& continentalness() const { return m_continentalness; }
        const levelgen::density::BoundSampler& erosion() const { return m_erosion; }
        const levelgen::density::BoundSampler& depth() const { return m_depth; }
        const levelgen::density::BoundSampler& weirdness() const { return m_weirdness; }

    private:
        levelgen::density::BoundSampler m_temperature;
        levelgen::density::BoundSampler m_humidity;
        levelgen::density::BoundSampler m_continentalness;
        levelgen::density::BoundSampler m_erosion;
        levelgen::density::BoundSampler m_depth;
        levelgen::density::BoundSampler m_weirdness;
    };

    // =========================================================================
    // Static factory methods
    // =========================================================================

    /**
     * Create a target point from float values
     * Reference: Climate.java lines 32-34
     */
    static TargetPoint target(float temperature, float humidity, float continentalness,
                             float erosion, float depth, float weirdness);

    /**
     * Create a parameter point from float values
     * Reference: Climate.java lines 36-38
     */
    static ParameterPoint parameters(float temperature, float humidity, float continentalness,
                                     float erosion, float depth, float weirdness, float offset);

    /**
     * Create a parameter point from Parameter ranges
     * Reference: Climate.java lines 40-42
     */
    static ParameterPoint parameters(const Parameter& temperature, const Parameter& humidity,
                                     const Parameter& continentalness, const Parameter& erosion,
                                     const Parameter& depth, const Parameter& weirdness, float offset);

    /**
     * Quantize a float coordinate to a long
     * Reference: Climate.java lines 44-46
     */
    static int64_t quantizeCoord(float coord);

    /**
     * Unquantize a long coordinate to a float
     * Reference: Climate.java lines 48-50
     */
    static float unquantizeCoord(int64_t coord);

};

} // namespace biome
} // namespace world
} // namespace minecraft
