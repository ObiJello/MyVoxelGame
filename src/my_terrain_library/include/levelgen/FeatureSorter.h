#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>

// Reference: net/minecraft/world/level/biome/FeatureSorter.java

namespace minecraft {
namespace levelgen {

/**
 * StepFeatureData - Features for one generation step with index mapping
 * Reference: FeatureSorter.StepFeatureData record
 */
struct StepFeatureData {
    std::vector<placement::PlacedFeature*> features;
    std::map<placement::PlacedFeature*, int> indexMap;

    StepFeatureData() = default;

    explicit StepFeatureData(const std::vector<placement::PlacedFeature*>& f)
        : features(f)
    {
        // Build index mapping: feature -> index in features vector
        for (size_t i = 0; i < features.size(); ++i) {
            indexMap[features[i]] = static_cast<int>(i);
        }
    }

    /**
     * Get the index of a feature in this step
     * Reference: StepFeatureData.indexMapping()
     */
    int getIndex(placement::PlacedFeature* feature) const {
        auto it = indexMap.find(feature);
        return (it != indexMap.end()) ? it->second : -1;
    }
};

/**
 * FeatureData - Internal structure for sorting
 * Reference: FeatureSorter.FeatureData record
 */
struct FeatureData {
    int featureIndex;
    int step;
    placement::PlacedFeature* feature;

    bool operator<(const FeatureData& other) const {
        if (step != other.step) return step < other.step;
        return featureIndex < other.featureIndex;
    }

    bool operator==(const FeatureData& other) const {
        return step == other.step && featureIndex == other.featureIndex;
    }
};

/**
 * FeatureSorter - Sorts features across biomes into deterministic order
 * Reference: FeatureSorter.java
 *
 * This ensures all biomes place features in a consistent global order,
 * which is critical for deterministic world generation.
 */
class FeatureSorter {
public:
    /**
     * Build sorted features per step from multiple feature sources
     * Reference: FeatureSorter.buildFeaturesPerStep()
     *
     * @tparam T The type of feature source (e.g., Biome*)
     * @param featureSources List of sources that provide features
     * @param featureGetter Function to get features-per-step from a source
     * @param tryReducingError If true, try to resolve cycles
     * @return Vector of StepFeatureData, one per generation step
     */
    template<typename T>
    static std::vector<StepFeatureData> buildFeaturesPerStep(
        const std::vector<T>& featureSources,
        std::function<std::vector<std::vector<placement::PlacedFeature*>>(const T&)> featureGetter,
        bool tryReducingError = true
    ) {
        // Map feature -> global index
        std::map<placement::PlacedFeature*, int> featureIndex;
        int nextFeatureIndex = 0;

        // Build edges: feature -> set of features that come after it
        std::map<FeatureData, std::set<FeatureData>> edges;
        int maxStep = 0;

        // Process each feature source (biome)
        bool firstBiome = true;
        int biomeIdx = 0;
        for (const T& featureSource : featureSources) {
            std::vector<FeatureData> featureList;
            auto featuresForStep = featureGetter(featureSource);
            maxStep = std::max(maxStep, static_cast<int>(featuresForStep.size()));

            if (firstBiome) {
                firstBiome = false;
            }

            // Collect all features with their indices
            for (size_t stepIdx = 0; stepIdx < featuresForStep.size(); ++stepIdx) {
                for (placement::PlacedFeature* feature : featuresForStep[stepIdx]) {
                    // Assign index if new
                    if (featureIndex.find(feature) == featureIndex.end()) {
                        featureIndex[feature] = nextFeatureIndex++;
                    }
                    featureList.push_back({featureIndex[feature], static_cast<int>(stepIdx), feature});
                }
            }

            // Build dependency edges: feature[i] -> feature[i+1]
            for (size_t i = 0; i < featureList.size(); ++i) {
                if (edges.find(featureList[i]) == edges.end()) {
                    edges[featureList[i]] = std::set<FeatureData>();
                }
                if (i < featureList.size() - 1) {
                    edges[featureList[i]].insert(featureList[i + 1]);
                }
            }
            biomeIdx++;
        }

        // DFS topological sort
        std::set<FeatureData> discovered;
        std::set<FeatureData> currentlyVisiting;
        std::vector<FeatureData> sortedFeatures;

        for (const auto& [feature, _] : edges) {
            if (discovered.find(feature) == discovered.end()) {
                depthFirstSearch(edges, discovered, currentlyVisiting, sortedFeatures, feature);
            }
        }

        // Reverse to get correct order
        std::reverse(sortedFeatures.begin(), sortedFeatures.end());

        // Group by step
        std::vector<StepFeatureData> result;
        for (int step = 0; step < maxStep; ++step) {
            std::vector<placement::PlacedFeature*> featuresInStep;
            for (const auto& fd : sortedFeatures) {
                if (fd.step == step) {
                    featuresInStep.push_back(fd.feature);
                }
            }
            result.push_back(StepFeatureData(featuresInStep));
        }

        return result;
    }

private:
    /**
     * DFS traversal for topological sort
     * Reference: Graph.depthFirstSearch()
     *
     * @return true if cycle detected
     */
    static bool depthFirstSearch(
        const std::map<FeatureData, std::set<FeatureData>>& edges,
        std::set<FeatureData>& discovered,
        std::set<FeatureData>& currentlyVisiting,
        std::vector<FeatureData>& result,
        const FeatureData& node
    ) {
        if (currentlyVisiting.find(node) != currentlyVisiting.end()) {
            // Cycle detected
            return true;
        }
        if (discovered.find(node) != discovered.end()) {
            // Already processed
            return false;
        }

        currentlyVisiting.insert(node);

        auto it = edges.find(node);
        if (it != edges.end()) {
            for (const auto& neighbor : it->second) {
                if (depthFirstSearch(edges, discovered, currentlyVisiting, result, neighbor)) {
                    // Cycle detected in subtree - must clean up before returning
                    currentlyVisiting.erase(node);
                    return true;
                }
            }
        }

        currentlyVisiting.erase(node);
        discovered.insert(node);
        result.push_back(node);
        return false;
    }
};

} // namespace levelgen
} // namespace minecraft
