#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/material/MaterialCondition.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Reference: levelgen.material.rule (26.3) - MaterialRule, RuleEvaluator and
// every rule type the MATERIAL_RULE_TYPE registry holds
// (MaterialRules.bootstrapRules): block, bandlands, sequence, condition,
// ore_vein.
//
// Like conditions, rules are immutable shared data; compile() binds one to a
// MaterialRuleContext and returns the evaluator buildSurface asks per block.
// tryApply answers the state to place, or nullptr (Java's null) to fall
// through to the next rule of a sequence.

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
class BlockState;
}
} // namespace block
} // namespace level
} // namespace world

namespace levelgen {
namespace material {

using BlockState = ::minecraft::world::level::block::state::BlockState;

class MaterialRuleContext;

// rule.RuleEvaluator.
class RuleEvaluator {
public:
    virtual ~RuleEvaluator() = default;
    virtual BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) = 0;
};
using RuleEvaluatorPtr = std::unique_ptr<RuleEvaluator>;

class MaterialRule;
using MaterialRulePtr = std::shared_ptr<const MaterialRule>;

// rule.MaterialRule.
class MaterialRule {
public:
    virtual ~MaterialRule() = default;

    virtual RuleEvaluatorPtr compile(MaterialRuleContext& context) const = 0;

    // The MATERIAL_RULE_TYPE id ("minecraft:sequence"); a registry reference
    // (HolderHolder) has none and answers "".
    virtual const char* type() const = 0;
};

// MaterialRule.HolderHolder: a registry reference. Compiling it compiles the
// referenced value afresh, as Java's HolderHolder does.
class RuleReference final : public MaterialRule {
public:
    RuleReference(std::string key, MaterialRulePtr value);

    RuleEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return ""; }

    const std::string& key() const { return m_key; }
    const MaterialRulePtr& value() const { return m_value; }

private:
    std::string m_key;
    MaterialRulePtr m_value;
};

// BandlandsRule (enum singleton): the badlands clay band at the block.
class BandlandsRule final : public MaterialRule {
public:
    static const std::shared_ptr<const BandlandsRule>& instance();

    RuleEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:bandlands"; }
};

// BlockRule (record: resultState).
class BlockRule final : public MaterialRule {
public:
    explicit BlockRule(BlockState* resultState);

    RuleEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:block"; }

    BlockState* resultState() const { return m_resultState; }

private:
    BlockState* m_resultState;
};

// ConditionRule (record: ifTrue, thenRun).
class ConditionRule final : public MaterialRule {
public:
    ConditionRule(MaterialConditionPtr ifTrue, MaterialRulePtr thenRun);

    RuleEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:condition"; }

    const MaterialConditionPtr& ifTrue() const { return m_ifTrue; }
    const MaterialRulePtr& thenRun() const { return m_thenRun; }

private:
    MaterialConditionPtr m_ifTrue;
    MaterialRulePtr m_thenRun;
};

// SequenceRule (record: sequence).
class SequenceRule final : public MaterialRule {
public:
    explicit SequenceRule(std::vector<MaterialRulePtr> sequence);

    RuleEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:sequence"; }

    const std::vector<MaterialRulePtr>& sequence() const { return m_sequence; }

private:
    std::vector<MaterialRulePtr> m_sequence;
};

// OreVeinRule (record: oreBlock, rawOreBlock, fillerBlock, rawOreChance,
// density, richness, fillerGap).
class OreVeinRule final : public MaterialRule {
public:
    static constexpr float VEININESS_THRESHOLD = 0.4f;
    static constexpr int32_t EDGE_ROUNDOFF_BEGIN = 20;
    static constexpr float MAX_EDGE_ROUNDOFF = 0.2f;
    static constexpr float VEIN_SOLIDNESS = 0.7f;
    static constexpr float MIN_RICHNESS = 0.1f;
    static constexpr float MAX_RICHNESS = 0.3f;
    static constexpr float MAX_RICHNESS_THRESHOLD = 0.6f;
    static constexpr float CHANCE_OF_RAW_ORE_BLOCK = 0.02f;
    static constexpr float SKIP_ORE_IF_GAP_NOISE_IS_BELOW = -0.3f;

    // OreVeinRule.VeinType.
    enum class VeinType { COPPER, IRON };
    static int32_t minY(VeinType type) { return type == VeinType::COPPER ? 0 : -60; }
    static int32_t maxY(VeinType type) { return type == VeinType::COPPER ? 50 : -8; }
    // VeinType.create: the type's blocks, raw-ore chance 0.02.
    static std::shared_ptr<const OreVeinRule> create(VeinType type, density::DensityFunctionPtr density,
                                                     density::DensityFunctionPtr richness,
                                                     density::DensityFunctionPtr fillerGap);

    OreVeinRule(BlockState* oreBlock, BlockState* rawOreBlock, BlockState* fillerBlock, float rawOreChance,
                density::DensityFunctionPtr density, density::DensityFunctionPtr richness,
                density::DensityFunctionPtr fillerGap);

    RuleEvaluatorPtr compile(MaterialRuleContext& context) const override;
    const char* type() const override { return "minecraft:ore_vein"; }

    BlockState* oreBlock() const { return m_oreBlock; }
    BlockState* rawOreBlock() const { return m_rawOreBlock; }
    BlockState* fillerBlock() const { return m_fillerBlock; }
    float rawOreChance() const { return m_rawOreChance; }
    const density::DensityFunctionPtr& density() const { return m_density; }
    const density::DensityFunctionPtr& richness() const { return m_richness; }
    const density::DensityFunctionPtr& fillerGap() const { return m_fillerGap; }

private:
    BlockState* m_oreBlock;
    BlockState* m_rawOreBlock;
    BlockState* m_fillerBlock;
    float m_rawOreChance;
    density::DensityFunctionPtr m_density;
    density::DensityFunctionPtr m_richness;
    density::DensityFunctionPtr m_fillerGap;
};

} // namespace material
} // namespace levelgen
} // namespace minecraft
