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

            // DEBUG: For first biome, print feature counts per step
            if (firstBiome) {
                int totalFeats = 0;
                for (const auto& sv : featuresForStep) totalFeats += sv.size();
                std::cerr << "DEBUG FeatureSorter: First biome has " << featuresForStep.size() << " steps, " << totalFeats << " total features" << std::endl;
                // Print ALL steps, not just non-empty
                for (size_t s = 0; s < featuresForStep.size(); ++s) {
                    std::cerr << "  step " << s << ": size=" << featuresForStep[s].size() << std::endl;
                }
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
                    // Track potential cycle: if i33 -> i34 or i34 -> i33
                    int fromIdx = featureList[i].featureIndex;
                    int toIdx = featureList[i + 1].featureIndex;
                    // Track edges involving indices 33, 34, 44 (cycle participants)
                    if ((fromIdx == 33 || fromIdx == 34 || fromIdx == 44) &&
                        (toIdx == 33 || toIdx == 34 || toIdx == 44)) {
                        std::cerr << "EDGE: i" << fromIdx << "->i" << toIdx << " in biome " << biomeIdx
                                  << " at listPos " << i << " step=" << featureList[i].step << std::endl;
                    }
                }
            }
            biomeIdx++;
        }

        // DEBUG: Print edges map step distribution
        std::map<int, int> edgeSteps;
        for (const auto& [key, _] : edges) {
            edgeSteps[key.step]++;
        }
        std::cerr << "DEBUG FeatureSorter: Edges map by step: ";
        for (const auto& [step, count] : edgeSteps) {
            std::cerr << "step" << step << "=" << count << " ";
        }
        std::cerr << "(total keys: " << edges.size() << ")" << std::endl;

        // DEBUG: Print first few edges keys
        std::cerr << "DEBUG: First 10 edges keys: ";
        int edgeCnt = 0;
        for (const auto& [key, _] : edges) {
            if (edgeCnt++ < 10) std::cerr << "(s" << key.step << ",i" << key.featureIndex << ") ";
        }
        std::cerr << std::endl;

        // DFS topological sort
        std::set<FeatureData> discovered;
        std::set<FeatureData> currentlyVisiting;
        std::vector<FeatureData> sortedFeatures;

        int dfsCallCount = 0;
        int skippedAsDiscovered = 0;
        for (const auto& [feature, _] : edges) {
            if (discovered.find(feature) == discovered.end()) {
                dfsCallCount++;
                depthFirstSearch(edges, discovered, currentlyVisiting, sortedFeatures, feature);
            } else {
                skippedAsDiscovered++;
                if (skippedAsDiscovered <= 3) {
                    std::cerr << "DEBUG: Skipped (step=" << feature.step << ", idx=" << feature.featureIndex
                              << ") as already discovered" << std::endl;
                }
            }
        }
        std::cerr << "DEBUG FeatureSorter: DFS started " << dfsCallCount << " times, skipped=" << skippedAsDiscovered
                  << ", discovered=" << discovered.size() << ", sortedFeatures=" << sortedFeatures.size() << std::endl;

        // Debug: print sample of discovered entries
        std::cerr << "DEBUG: First 5 discovered entries: ";
        int cnt = 0;
        for (const auto& d : discovered) {
            if (cnt++ < 5) std::cerr << "(s" << d.step << ",i" << d.featureIndex << ") ";
        }
        std::cerr << std::endl;

        // Reverse to get correct order
        std::reverse(sortedFeatures.begin(), sortedFeatures.end());

        // DEBUG: Print step counts in sorted features
        std::map<int, int> stepCounts;
        for (const auto& fd : sortedFeatures) {
            stepCounts[fd.step]++;
        }
        std::cerr << "DEBUG FeatureSorter: Sorted features by step: ";
        for (const auto& [step, count] : stepCounts) {
            std::cerr << "step" << step << "=" << count << " ";
        }
        std::cerr << std::endl;

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

        // Output sorted features in Java-compatible format for comparison
        // Format: STEP=<step> IDX=<index> <feature_name>
        std::cerr << "\n# C++ Feature Order Output\n";
        std::cerr << "# Format: STEP=<step> IDX=<index> <feature_name>\n\n";
        for (int step = 0; step < maxStep; ++step) {
            const auto& stepData = result[step];
            std::cerr << "# Step " << step << ": " << stepData.features.size() << " features\n";
            for (size_t idx = 0; idx < stepData.features.size(); ++idx) {
                placement::PlacedFeature* feat = stepData.features[idx];
                const std::string& name = feat->getName();
                std::cerr << "STEP=" << step << " IDX=" << idx << " "
                          << (name.empty() ? "(unnamed)" : name);
                // Debug: print pointer for step 4 features
                if (step == 4) {
                    std::cerr << " ptr=" << (void*)feat;
                }
                std::cerr << "\n";
            }
            std::cerr << "\n";
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
        static int dfsDebugCount = 0;
        static int cycleCount = 0;
        bool doDebug = (dfsDebugCount < 50);
        if (doDebug) {
            std::cerr << "  DFS enter: (s" << node.step << ",i" << node.featureIndex << ") discovered.size=" << discovered.size() << std::endl;
        }

        if (currentlyVisiting.find(node) != currentlyVisiting.end()) {
            // Cycle detected
            cycleCount++;
            if (cycleCount == 1) {
                std::cerr << "  DFS CYCLE at (s" << node.step << ",i" << node.featureIndex
                          << ") feature ptr=" << (void*)node.feature << std::endl;
                std::cerr << "  Currently visiting: ";
                for (const auto& cv : currentlyVisiting) {
                    std::cerr << "(s" << cv.step << ",i" << cv.featureIndex << ") ";
                }
                std::cerr << std::endl;
            }
            return true;
        }
        if (discovered.find(node) != discovered.end()) {
            // Already processed
            if (doDebug) std::cerr << "  DFS skip (s" << node.step << ",i" << node.featureIndex << ") already discovered" << std::endl;
            return false;
        }

        currentlyVisiting.insert(node);
        dfsDebugCount++;

        auto it = edges.find(node);
        if (it != edges.end()) {
            if (doDebug) std::cerr << "  DFS (s" << node.step << ",i" << node.featureIndex << ") has " << it->second.size() << " neighbors" << std::endl;
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
        if (doDebug) std::cerr << "  DFS added (s" << node.step << ",i" << node.featureIndex << ") to result" << std::endl;
        return false;
    }
};

} // namespace levelgen
} // namespace minecraft
