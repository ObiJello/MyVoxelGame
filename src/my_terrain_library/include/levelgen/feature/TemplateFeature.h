#pragma once

#include "levelgen/feature/Feature.h"
#include "levelgen/structure/TemplateEngine.h"
#include <string>
#include <vector>

// Reference: 26.3 net/minecraft/world/level/levelgen/feature/TemplateFeature.java

namespace minecraft {
namespace levelgen {

/**
 * TemplateFeatureConfiguration - a weighted list of structure templates,
 * each with the rotations it may take (TemplateEntry.of = all four).
 */
struct TemplateFeatureConfiguration {
    struct Entry {
        std::string templateId;
        int weight = 1;
        std::vector<int> rotations{0, 1, 2, 3};  // Rotation ordinals
    };
    std::vector<Entry> templates;
    // The processor list's RuleProcessor append_loot rules (desert well).
    std::vector<structure::TemplatePlaceSettings::AppendLootRule> appendLootRules;

    TemplateFeatureConfiguration() = default;
    explicit TemplateFeatureConfiguration(std::vector<Entry> entries) : templates(std::move(entries)) {}
};

/**
 * TemplateFeature - 26.3: places a structure template centred on the origin,
 * turned by a random rotation, with the feature's random driving the palette
 * and the container loot seeds (StructurePlaceSettings.setRandom).
 */
class TemplateFeature : public Feature<TemplateFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<TemplateFeatureConfiguration>& context) override;
};

} // namespace levelgen
} // namespace minecraft
