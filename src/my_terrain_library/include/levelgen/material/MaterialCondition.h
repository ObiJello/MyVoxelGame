#pragma once

#include "levelgen/material/VerticalAnchor.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

// Reference: levelgen.material.condition (26.3) - MaterialCondition,
// ConditionEvaluator and every condition type the MATERIAL_CONDITION_TYPE
// registry holds (MaterialRules.bootstrapConditions).
//
// A MaterialCondition is immutable data (decoded from worldgen/
// material_condition or built in code) and is shared between threads. compile()
// binds it to one MaterialRuleContext - one chunk's surface pass - and returns
// the stateful evaluator the rule tree tests per block. An evaluator borrows
// its context and must not outlive it.

namespace minecraft {
namespace world {
namespace biome {
class Biome;
}
} // namespace world

namespace levelgen {
namespace material {

class MaterialRuleContext;

// condition.ConditionEvaluator.
class ConditionEvaluator {
public:
    virtual ~ConditionEvaluator() = default;
    virtual bool test() = 0;
};
using ConditionEvaluatorPtr = std::unique_ptr<ConditionEvaluator>;

class MaterialCondition;
using MaterialConditionPtr = std::shared_ptr<const MaterialCondition>;

// condition.MaterialCondition.
class MaterialCondition {
public:
    virtual ~MaterialCondition() = default;

    virtual ConditionEvaluatorPtr compile(MaterialRuleContext& context) const = 0;

    // The MATERIAL_CONDITION_TYPE id ("minecraft:stone_depth"); a registry
    // reference (HolderHolder) has none and answers "".
    virtual const char* type() const = 0;
};

// placement.CaveSurface: "ceiling" / "floor".
enum class CaveSurface { CEILING, FLOOR };

// HolderSet<Biome> for biome_is: a direct list of biome ids or a biome tag,
// already expanded. Membership is by biome id, which is what Holder identity
// means for the engine's interned Biome objects.
class BiomeSet {
public:
    BiomeSet() = default;

    static BiomeSet direct(std::vector<std::string> ids);
    // A tag ("minecraft:is_ocean", no '#') and the ids it expands to.
    static BiomeSet named(std::string tag, std::vector<std::string> ids);

    bool contains(const std::string& id) const { return m_lookup.count(id) != 0; }
    bool contains(const world::biome::Biome* biome) const;

    // Iteration order: the list order, or the tag's (unordered) expansion.
    const std::vector<std::string>& ids() const { return m_ids; }
    bool isTag() const { return !m_tag.empty(); }
    const std::string& tag() const { return m_tag; }

private:
    std::string m_tag;
    std::vector<std::string> m_ids;
    std::unordered_set<std::string> m_lookup;
};

// MaterialCondition.HolderHolder: a registry reference. Compiling it compiles
// the referenced value afresh, as Java's HolderHolder does.
class ConditionReference final : public MaterialCondition {
public:
    ConditionReference(std::string key, MaterialConditionPtr value);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return ""; }

    const std::string& key() const { return m_key; }
    const MaterialConditionPtr& value() const { return m_value; }

private:
    std::string m_key;
    MaterialConditionPtr m_value;
};

// AbovePreliminarySurfaceCondition (enum singleton).
class AbovePreliminarySurfaceCondition final : public MaterialCondition {
public:
    static const std::shared_ptr<const AbovePreliminarySurfaceCondition>& instance();

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:above_preliminary_surface"; }
};

// BiomeCondition (record: biomes).
class BiomeCondition final : public MaterialCondition {
public:
    explicit BiomeCondition(BiomeSet biomes);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:biome"; }

    const BiomeSet& biomes() const { return m_biomes; }

private:
    bool canNeverMatch(const std::set<std::string>& possibleBiomes) const;
    bool willAlwaysMatch(const std::set<std::string>& possibleBiomes) const;

    BiomeSet m_biomes;
};

// HoleCondition (enum singleton).
class HoleCondition final : public MaterialCondition {
public:
    static const std::shared_ptr<const HoleCondition>& instance();

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:hole"; }
};

// NoiseThresholdCondition (record: noise, minThreshold, maxThreshold, is3d).
class NoiseThresholdCondition final : public MaterialCondition {
public:
    NoiseThresholdCondition(std::string noise, double minThreshold, double maxThreshold, bool is3d);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:noise_threshold"; }

    const std::string& noise() const { return m_noise; }
    double minThreshold() const { return m_minThreshold; }
    double maxThreshold() const { return m_maxThreshold; }
    bool is3d() const { return m_is3d; }

private:
    std::string m_noise;   // ResourceKey<NormalNoise>, full identifier
    double m_minThreshold;
    double m_maxThreshold;
    bool m_is3d;
};

// NotCondition (record: target).
class NotCondition final : public MaterialCondition {
public:
    explicit NotCondition(MaterialConditionPtr target);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:not"; }

    const MaterialConditionPtr& target() const { return m_target; }

private:
    MaterialConditionPtr m_target;
};

// SteepCondition (enum singleton).
class SteepCondition final : public MaterialCondition {
public:
    static const std::shared_ptr<const SteepCondition>& instance();

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:steep"; }
};

// StoneDepthCondition (record: offset, addSurfaceDepth, secondaryDepthRange,
// surfaceType).
class StoneDepthCondition final : public MaterialCondition {
public:
    StoneDepthCondition(int32_t offset, bool addSurfaceDepth, int32_t secondaryDepthRange, CaveSurface surfaceType);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:stone_depth"; }

    int32_t offset() const { return m_offset; }
    bool addSurfaceDepth() const { return m_addSurfaceDepth; }
    int32_t secondaryDepthRange() const { return m_secondaryDepthRange; }
    CaveSurface surfaceType() const { return m_surfaceType; }

private:
    int32_t m_offset;
    bool m_addSurfaceDepth;
    int32_t m_secondaryDepthRange;
    CaveSurface m_surfaceType;
};

// TemperatureCondition (enum singleton).
class TemperatureCondition final : public MaterialCondition {
public:
    static const std::shared_ptr<const TemperatureCondition>& instance();

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:temperature"; }
};

// VerticalGradientCondition (record: randomName, trueAtAndBelow,
// falseAtAndAbove).
class VerticalGradientCondition final : public MaterialCondition {
public:
    VerticalGradientCondition(std::string randomName, VerticalAnchor trueAtAndBelow, VerticalAnchor falseAtAndAbove);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:vertical_gradient"; }

    const std::string& randomName() const { return m_randomName; }
    const VerticalAnchor& trueAtAndBelow() const { return m_trueAtAndBelow; }
    const VerticalAnchor& falseAtAndAbove() const { return m_falseAtAndAbove; }

private:
    std::string m_randomName;   // Identifier, full
    VerticalAnchor m_trueAtAndBelow;
    VerticalAnchor m_falseAtAndAbove;
};

// WaterCondition (record: offset, surfaceDepthMultiplier, addStoneDepth).
class WaterCondition final : public MaterialCondition {
public:
    WaterCondition(int32_t offset, int32_t surfaceDepthMultiplier, bool addStoneDepth);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:water"; }

    int32_t offset() const { return m_offset; }
    int32_t surfaceDepthMultiplier() const { return m_surfaceDepthMultiplier; }
    bool addStoneDepth() const { return m_addStoneDepth; }

private:
    int32_t m_offset;
    int32_t m_surfaceDepthMultiplier;
    bool m_addStoneDepth;
};

// YCondition (record: anchor, surfaceDepthMultiplier, addStoneDepth).
class YCondition final : public MaterialCondition {
public:
    YCondition(VerticalAnchor anchor, int32_t surfaceDepthMultiplier, bool addStoneDepth);

    ConditionEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:y_above"; }

    const VerticalAnchor& anchor() const { return m_anchor; }
    int32_t surfaceDepthMultiplier() const { return m_surfaceDepthMultiplier; }
    bool addStoneDepth() const { return m_addStoneDepth; }

private:
    VerticalAnchor m_anchor;
    int32_t m_surfaceDepthMultiplier;
    bool m_addStoneDepth;
};

} // namespace material
} // namespace levelgen
} // namespace minecraft
