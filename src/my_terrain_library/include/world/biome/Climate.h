#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <memory>
#include <functional>
#include <unordered_map>

// Forward declarations
namespace minecraft {
    namespace density {
        class DensityFunction;
    }
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
        static constexpr int CHILDREN_PER_NODE = 6;

        // Forward declaration of Node types
        class Node;
        class Leaf;
        class SubTree;

        // Distance metric function type
        using DistanceMetric = std::function<int64_t(const Node*, const std::vector<int64_t>&)>;

    private:
        std::unique_ptr<Node> m_root;

        // Thread-local cache to match Java's ThreadLocal<Leaf> lastResult behavior
        // Each thread maintains its own lastResult cache per RTree instance
        // This avoids race conditions and provides spatial locality benefits
        static std::unordered_map<const RTree*, Leaf*>& getThreadLocalCache() {
            static thread_local std::unordered_map<const RTree*, Leaf*> cache;
            return cache;
        }

        Leaf* getLastResult() const {
            auto& cache = getThreadLocalCache();
            auto it = cache.find(this);
            return (it != cache.end()) ? it->second : nullptr;
        }

        void setLastResult(Leaf* leaf) const {
            getThreadLocalCache()[this] = leaf;
        }

    public:
        /**
         * Node - Base class for RTree nodes
         * Reference: Climate.java lines 203-225
         */
        class Node {
        public:
            std::vector<Parameter> parameterSpace;

            Node(const std::vector<Parameter>& params) : parameterSpace(params) {}
            virtual ~Node() = default;

            /**
             * Search for the closest leaf node
             */
            virtual Leaf* search(const std::vector<int64_t>& target, Leaf* candidate,
                                const DistanceMetric& distanceMetric) = 0;

            /**
             * Calculate distance from this node to target
             * Reference: Climate.java lines 212-220
             */
            int64_t distance(const std::vector<int64_t>& target) const {
                int64_t dist = 0;
                for (size_t i = 0; i < 7 && i < parameterSpace.size(); ++i) {
                    int64_t d = parameterSpace[i].distance(target[i]);
                    dist += d * d;  // Mth.square
                }
                return dist;
            }
        };

        /**
         * Leaf - Leaf node containing a biome value
         * Reference: Climate.java lines 227-238
         */
        class Leaf : public Node {
        public:
            T value;

            Leaf(const ParameterPoint& point, const T& val)
                : Node(point.parameterSpace()), value(val) {}

            Leaf* search(const std::vector<int64_t>& target, Leaf* candidate,
                        const DistanceMetric& distanceMetric) override {
                return this;
            }
        };

        /**
         * SubTree - Internal node containing child nodes
         * Reference: Climate.java lines 240-270
         */
        class SubTree : public Node {
        public:
            std::vector<std::unique_ptr<Node>> children;

            SubTree(const std::vector<Parameter>& params, std::vector<std::unique_ptr<Node>>&& kids)
                : Node(params), children(std::move(kids)) {}

            SubTree(std::vector<std::unique_ptr<Node>>&& kids)
                : Node(buildParameterSpace(kids)), children(std::move(kids)) {}

            /**
             * Search through children for closest match
             * Reference: Climate.java lines 252-269
             */
            Leaf* search(const std::vector<int64_t>& target, Leaf* candidate,
                        const DistanceMetric& distanceMetric) override {
                int64_t minDistance = candidate == nullptr ? INT64_MAX : distanceMetric(candidate, target);
                Leaf* closestLeaf = candidate;

                for (auto& child : children) {
                    int64_t childDistance = distanceMetric(child.get(), target);
                    if (minDistance > childDistance) {
                        Leaf* leaf = child->search(target, closestLeaf, distanceMetric);
                        int64_t leafDistance = (child.get() == leaf) ? childDistance : distanceMetric(leaf, target);
                        if (minDistance > leafDistance) {
                            minDistance = leafDistance;
                            closestLeaf = leaf;
                        }
                    }
                }

                return closestLeaf;
            }

        private:
            static std::vector<Parameter> buildParameterSpace(const std::vector<std::unique_ptr<Node>>& kids) {
                if (kids.empty()) {
                    throw std::invalid_argument("SubTree needs at least one child");
                }

                std::vector<Parameter> bounds(7);
                bool first = true;

                for (const auto& child : kids) {
                    for (size_t d = 0; d < 7 && d < child->parameterSpace.size(); ++d) {
                        if (first) {
                            bounds[d] = child->parameterSpace[d];
                        } else {
                            bounds[d] = bounds[d].span(&child->parameterSpace[d]);
                        }
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
         * Create an RTree from a list of parameter point / value pairs
         * Reference: Climate.java lines 70-82
         */
        static RTree<T> create(const std::vector<std::pair<ParameterPoint, T>>& values) {
            if (values.empty()) {
                throw std::invalid_argument("Need at least one value to build the search tree.");
            }

            size_t dimensions = values[0].first.parameterSpace().size();
            if (dimensions != 7) {
                throw std::runtime_error("Expecting parameter space to be 7, got " + std::to_string(dimensions));
            }

            // Create leaf nodes
            std::vector<std::unique_ptr<Node>> leaves;
            leaves.reserve(values.size());
            for (const auto& p : values) {
                leaves.push_back(std::make_unique<Leaf>(p.first, p.second));
            }

            return RTree<T>(build(dimensions, std::move(leaves)));
        }

        /**
         * Search for the best matching value
         * Reference: Climate.java lines 196-201
         */
        T search(const TargetPoint& target, const DistanceMetric& distanceMetric) {
            std::vector<int64_t> targetArray = target.toParameterArray();
            Leaf* lastResult = getLastResult();
            Leaf* leaf = m_root->search(targetArray, lastResult, distanceMetric);
            setLastResult(leaf);
            return leaf->value;
        }

        /**
         * Search using default distance metric
         */
        T search(const TargetPoint& target) {
            return search(target, [](const Node* node, const std::vector<int64_t>& t) {
                return node->distance(t);
            });
        }

    private:
        /**
         * Recursively build the tree
         * Reference: Climate.java lines 84-125
         */
        static std::unique_ptr<Node> build(size_t dimensions, std::vector<std::unique_ptr<Node>>&& children) {
            if (children.empty()) {
                throw std::runtime_error("Need at least one child to build a node");
            }

            if (children.size() == 1) {
                return std::move(children[0]);
            }

            if (children.size() <= CHILDREN_PER_NODE) {
                // Sort by total center magnitude
                // Reference: Climate.java lines 90-99
                std::sort(children.begin(), children.end(),
                    [dimensions](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) {
                        int64_t totalA = 0, totalB = 0;
                        for (size_t d = 0; d < dimensions; ++d) {
                            const Parameter& pa = a->parameterSpace[d];
                            const Parameter& pb = b->parameterSpace[d];
                            totalA += std::abs((pa.min() + pa.max()) / 2);
                            totalB += std::abs((pb.min() + pb.max()) / 2);
                        }
                        return totalA < totalB;
                    });

                return std::make_unique<SubTree>(std::move(children));
            }

            // Find optimal dimension for splitting
            // Reference: Climate.java lines 102-120
            // Try each dimension, calculate bucket costs, keep minimum
            int64_t minCost = INT64_MAX;
            size_t minDimension = 0;

            // Calculate expected children per bucket
            double logBase = std::log(static_cast<double>(CHILDREN_PER_NODE));
            double logValue = std::log(static_cast<double>(children.size()) - 0.01);
            size_t expectedChildrenCount = static_cast<size_t>(
                std::pow(static_cast<double>(CHILDREN_PER_NODE),
                        std::floor(logValue / logBase)));

            for (size_t d = 0; d < dimensions; ++d) {
                // Sort by this dimension
                sortNodes(children, dimensions, d, false);

                // Calculate bucket costs without moving nodes
                // Reference: Climate.java lines 108-119
                int64_t totalCost = 0;
                size_t bucketStart = 0;
                while (bucketStart < children.size()) {
                    size_t bucketEnd = std::min(bucketStart + expectedChildrenCount, children.size());
                    totalCost += costRange(children, bucketStart, bucketEnd);
                    bucketStart = bucketEnd;
                }

                if (totalCost < minCost) {
                    minCost = totalCost;
                    minDimension = d;
                }
            }

            // Re-sort by the best dimension and create actual buckets
            sortNodes(children, dimensions, minDimension, false);
            auto buckets = bucketize(children);

            // Convert buckets to SubTrees for sorting
            std::vector<std::unique_ptr<SubTree>> subTrees;
            for (auto& bucket : buckets) {
                subTrees.push_back(std::make_unique<SubTree>(std::move(bucket)));
            }

            // Sort SubTrees by the best dimension (absolute=true)
            // Reference: Climate.java line 122
            sortSubTrees(subTrees, dimensions, minDimension, true);

            // Recursively build subtrees from each bucket's children
            // Reference: Climate.java line 123
            std::vector<std::unique_ptr<Node>> builtChildren;
            for (auto& subTree : subTrees) {
                builtChildren.push_back(build(dimensions, std::move(subTree->children)));
            }

            return std::make_unique<SubTree>(std::move(builtChildren));
        }

        static void sortNodes(std::vector<std::unique_ptr<Node>>& children,
                             size_t dimensions, size_t primaryDim, bool absolute) {
            std::sort(children.begin(), children.end(),
                [dimensions, primaryDim, absolute](const std::unique_ptr<Node>& a,
                                                   const std::unique_ptr<Node>& b) {
                    for (size_t i = 0; i < dimensions; ++i) {
                        size_t d = (primaryDim + i) % dimensions;
                        const Parameter& pa = a->parameterSpace[d];
                        const Parameter& pb = b->parameterSpace[d];
                        int64_t centerA = (pa.min() + pa.max()) / 2;
                        int64_t centerB = (pb.min() + pb.max()) / 2;
                        if (absolute) {
                            centerA = std::abs(centerA);
                            centerB = std::abs(centerB);
                        }
                        if (centerA != centerB) {
                            return centerA < centerB;
                        }
                    }
                    return false;
                });
        }

        /**
         * Sort SubTrees by their parameterSpace
         * Reference: Climate.java lines 127-135
         */
        static void sortSubTrees(std::vector<std::unique_ptr<SubTree>>& subTrees,
                                 size_t dimensions, size_t primaryDim, bool absolute) {
            std::sort(subTrees.begin(), subTrees.end(),
                [dimensions, primaryDim, absolute](const std::unique_ptr<SubTree>& a,
                                                   const std::unique_ptr<SubTree>& b) {
                    // Multi-dimensional comparison like sortNodes
                    for (size_t i = 0; i < dimensions; ++i) {
                        size_t d = (primaryDim + i) % dimensions;
                        const Parameter& pa = a->parameterSpace[d];
                        const Parameter& pb = b->parameterSpace[d];
                        int64_t centerA = (pa.min() + pa.max()) / 2;
                        int64_t centerB = (pb.min() + pb.max()) / 2;
                        if (absolute) {
                            centerA = std::abs(centerA);
                            centerB = std::abs(centerB);
                        }
                        if (centerA != centerB) {
                            return centerA < centerB;
                        }
                    }
                    return false;
                });
        }

        /**
         * Calculate cost of a range of children (sum of parameter ranges of merged space)
         * Reference: Climate.java lines 165-173
         */
        static int64_t costRange(const std::vector<std::unique_ptr<Node>>& children,
                                 size_t start, size_t end) {
            if (start >= end || start >= children.size()) return 0;

            // Build combined parameter space for this range
            std::vector<Parameter> combined(7);
            bool first = true;

            for (size_t i = start; i < end && i < children.size(); ++i) {
                const auto& node = children[i];
                for (size_t d = 0; d < 7 && d < node->parameterSpace.size(); ++d) {
                    if (first) {
                        combined[d] = node->parameterSpace[d];
                    } else {
                        combined[d] = combined[d].span(&node->parameterSpace[d]);
                    }
                }
                first = false;
            }

            // Calculate cost: sum of |max - min| for all dimensions
            int64_t result = 0;
            for (const auto& param : combined) {
                result += std::abs(param.max() - param.min());
            }
            return result;
        }

        /**
         * Calculate cost of a parameter space array
         * Reference: Climate.java lines 165-173
         */
        static int64_t cost(const std::vector<Parameter>& parameterSpace) {
            int64_t result = 0;
            for (const auto& param : parameterSpace) {
                result += std::abs(param.max() - param.min());
            }
            return result;
        }

        /**
         * Bucketize nodes into groups
         * Reference: Climate.java lines 145-163
         */
        static std::vector<std::vector<std::unique_ptr<Node>>> bucketize(
            std::vector<std::unique_ptr<Node>>& nodes) {

            std::vector<std::vector<std::unique_ptr<Node>>> buckets;
            std::vector<std::unique_ptr<Node>> currentBucket;

            // Calculate expected children per bucket
            double logBase = std::log(static_cast<double>(CHILDREN_PER_NODE));
            double logValue = std::log(static_cast<double>(nodes.size()) - 0.01);
            int expectedChildrenCount = static_cast<int>(
                std::pow(static_cast<double>(CHILDREN_PER_NODE),
                        std::floor(logValue / logBase)));

            for (auto& node : nodes) {
                currentBucket.push_back(std::move(node));
                if (static_cast<int>(currentBucket.size()) >= expectedChildrenCount) {
                    buckets.push_back(std::move(currentBucket));
                    currentBucket.clear();
                }
            }

            if (!currentBucket.empty()) {
                buckets.push_back(std::move(currentBucket));
            }

            return buckets;
        }

        /**
         * Calculate cost of a bucket (sum of parameter ranges)
         * Reference: Climate.java lines 165-173
         */
        static int64_t cost(const std::vector<std::unique_ptr<Node>>& bucket) {
            if (bucket.empty()) return 0;

            // Build combined parameter space
            std::vector<Parameter> combined(7);
            bool first = true;

            for (const auto& node : bucket) {
                for (size_t d = 0; d < 7 && d < node->parameterSpace.size(); ++d) {
                    if (first) {
                        combined[d] = node->parameterSpace[d];
                    } else {
                        combined[d] = combined[d].span(&node->parameterSpace[d]);
                    }
                }
                first = false;
            }

            int64_t result = 0;
            for (const auto& param : combined) {
                result += std::abs(param.max() - param.min());
            }
            return result;
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

    class Sampler {
    private:
        minecraft::density::DensityFunction* m_temperature;
        minecraft::density::DensityFunction* m_humidity;
        minecraft::density::DensityFunction* m_continentalness;
        minecraft::density::DensityFunction* m_erosion;
        minecraft::density::DensityFunction* m_depth;
        minecraft::density::DensityFunction* m_weirdness;
        std::vector<ParameterPoint> m_spawnTarget;

    public:
        Sampler(minecraft::density::DensityFunction* temperature,
                minecraft::density::DensityFunction* humidity,
                minecraft::density::DensityFunction* continentalness,
                minecraft::density::DensityFunction* erosion,
                minecraft::density::DensityFunction* depth,
                minecraft::density::DensityFunction* weirdness,
                const std::vector<ParameterPoint>& spawnTarget = {})
            : m_temperature(temperature)
            , m_humidity(humidity)
            , m_continentalness(continentalness)
            , m_erosion(erosion)
            , m_depth(depth)
            , m_weirdness(weirdness)
            , m_spawnTarget(spawnTarget)
        {
        }

        /**
         * Sample climate parameters at a quart position
         * Reference: Climate.java lines 387-393
         */
        TargetPoint sample(int32_t quartX, int32_t quartY, int32_t quartZ) const;

        /**
         * Find spawn position
         * Reference: Climate.java lines 395-397
         */
        core::BlockPos findSpawnPosition() const;

        // Accessors
        minecraft::density::DensityFunction* temperature() const { return m_temperature; }
        minecraft::density::DensityFunction* humidity() const { return m_humidity; }
        minecraft::density::DensityFunction* continentalness() const { return m_continentalness; }
        minecraft::density::DensityFunction* erosion() const { return m_erosion; }
        minecraft::density::DensityFunction* depth() const { return m_depth; }
        minecraft::density::DensityFunction* weirdness() const { return m_weirdness; }
        const std::vector<ParameterPoint>& spawnTarget() const { return m_spawnTarget; }
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

    /**
     * Create an empty sampler
     * Reference: Climate.java lines 52-55
     */
    static Sampler empty();

    /**
     * Find spawn position from target climates
     * Reference: Climate.java lines 57-59
     */
    static core::BlockPos findSpawnPosition(const std::vector<ParameterPoint>& targetClimates,
                                           const Sampler& sampler);
};

} // namespace biome
} // namespace world
} // namespace minecraft
