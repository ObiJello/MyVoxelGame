#include "levelgen/material/MaterialRule.h"

#include "levelgen/material/MaterialRuleContext.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <stdexcept>
#include <string>
#include <utility>

// Reference: levelgen.material.rule.* (26.3), every rule's compile.

namespace minecraft {
namespace levelgen {
namespace material {

// ---- RuleReference (MaterialRule.HolderHolder) --------------------------------------

RuleReference::RuleReference(std::string key, MaterialRulePtr value) : m_key(std::move(key)), m_value(std::move(value)) {}

RuleEvaluatorPtr RuleReference::compile(MaterialRuleContext& context) const {
    return m_value->compile(context);
}

// ---- BandlandsRule ----------------------------------------------------------------------

const std::shared_ptr<const BandlandsRule>& BandlandsRule::instance() {
    static const auto s_instance = std::make_shared<const BandlandsRule>();
    return s_instance;
}

RuleEvaluatorPtr BandlandsRule::compile(MaterialRuleContext& context) const {
    class Evaluator final : public RuleEvaluator {
    public:
        explicit Evaluator(MaterialRuleContext& context) : m_context(context) {}
        BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
            return m_context.getBand(blockX, blockY, blockZ);
        }

    private:
        MaterialRuleContext& m_context;
    };
    return std::make_unique<Evaluator>(context);
}

// ---- BlockRule ------------------------------------------------------------------------------

BlockRule::BlockRule(BlockState* resultState) : m_resultState(resultState) {
    if (m_resultState == nullptr) throw std::invalid_argument("BlockRule: null result state");
}

RuleEvaluatorPtr BlockRule::compile(MaterialRuleContext& /*context*/) const {
    // Java's BlockRule is its own evaluator.
    class Evaluator final : public RuleEvaluator {
    public:
        explicit Evaluator(BlockState* resultState) : m_resultState(resultState) {}
        BlockState* tryApply(int32_t, int32_t, int32_t) override { return m_resultState; }

    private:
        BlockState* m_resultState;
    };
    return std::make_unique<Evaluator>(m_resultState);
}

// ---- ConditionRule ------------------------------------------------------------------------------

ConditionRule::ConditionRule(MaterialConditionPtr ifTrue, MaterialRulePtr thenRun)
    : m_ifTrue(std::move(ifTrue)), m_thenRun(std::move(thenRun)) {
    if (!m_ifTrue || !m_thenRun) throw std::invalid_argument("ConditionRule: null condition or rule");
}

RuleEvaluatorPtr ConditionRule::compile(MaterialRuleContext& context) const {
    class Evaluator final : public RuleEvaluator {
    public:
        Evaluator(ConditionEvaluatorPtr ifTrue, RuleEvaluatorPtr thenRun)
            : m_ifTrue(std::move(ifTrue)), m_thenRun(std::move(thenRun)) {}

        BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
            return !m_ifTrue->test() ? nullptr : m_thenRun->tryApply(blockX, blockY, blockZ);
        }

    private:
        ConditionEvaluatorPtr m_ifTrue;
        RuleEvaluatorPtr m_thenRun;
    };
    ConditionEvaluatorPtr ifTrue = m_ifTrue->compile(context);
    RuleEvaluatorPtr thenRun = m_thenRun->compile(context);
    return std::make_unique<Evaluator>(std::move(ifTrue), std::move(thenRun));
}

// ---- SequenceRule ---------------------------------------------------------------------------------

SequenceRule::SequenceRule(std::vector<MaterialRulePtr> sequence) : m_sequence(std::move(sequence)) {
    for (const MaterialRulePtr& rule : m_sequence) {
        if (!rule) throw std::invalid_argument("SequenceRule: null rule");
    }
}

RuleEvaluatorPtr SequenceRule::compile(MaterialRuleContext& context) const {
    if (m_sequence.size() == 1) return m_sequence.front()->compile(context);

    class Evaluator final : public RuleEvaluator {
    public:
        explicit Evaluator(std::vector<RuleEvaluatorPtr> sequence) : m_sequence(std::move(sequence)) {}

        BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
            for (const RuleEvaluatorPtr& rule : m_sequence) {
                BlockState* state = rule->tryApply(blockX, blockY, blockZ);
                if (state != nullptr) return state;
            }
            return nullptr;
        }

    private:
        std::vector<RuleEvaluatorPtr> m_sequence;
    };
    std::vector<RuleEvaluatorPtr> sequence;
    sequence.reserve(m_sequence.size());
    for (const MaterialRulePtr& rule : m_sequence) sequence.push_back(rule->compile(context));
    return std::make_unique<Evaluator>(std::move(sequence));
}

// ---- OreVeinRule --------------------------------------------------------------------------------------

namespace {

BlockState* requireState(const char* name) {
    BlockState* state = world::level::block::Blocks::getDefaultState(name);
    if (state == nullptr) throw std::runtime_error(std::string("OreVeinRule: block not registered: ") + name);
    return state;
}

} // namespace

std::shared_ptr<const OreVeinRule> OreVeinRule::create(VeinType type, density::DensityFunctionPtr density,
                                                        density::DensityFunctionPtr richness,
                                                        density::DensityFunctionPtr fillerGap) {
    if (type == VeinType::COPPER) {
        return std::make_shared<const OreVeinRule>(requireState("minecraft:copper_ore"),
                                                   requireState("minecraft:raw_copper_block"),
                                                   requireState("minecraft:granite"), CHANCE_OF_RAW_ORE_BLOCK,
                                                   std::move(density), std::move(richness), std::move(fillerGap));
    }
    return std::make_shared<const OreVeinRule>(requireState("minecraft:deepslate_iron_ore"),
                                               requireState("minecraft:raw_iron_block"),
                                               requireState("minecraft:tuff"), CHANCE_OF_RAW_ORE_BLOCK,
                                               std::move(density), std::move(richness), std::move(fillerGap));
}

OreVeinRule::OreVeinRule(BlockState* oreBlock, BlockState* rawOreBlock, BlockState* fillerBlock, float rawOreChance,
                         density::DensityFunctionPtr density, density::DensityFunctionPtr richness,
                         density::DensityFunctionPtr fillerGap)
    : m_oreBlock(oreBlock),
      m_rawOreBlock(rawOreBlock),
      m_fillerBlock(fillerBlock),
      m_rawOreChance(rawOreChance),
      m_density(std::move(density)),
      m_richness(std::move(richness)),
      m_fillerGap(std::move(fillerGap)) {
    if (!m_oreBlock || !m_rawOreBlock || !m_fillerBlock) throw std::invalid_argument("OreVeinRule: null block state");
    if (!m_density || !m_richness || !m_fillerGap) throw std::invalid_argument("OreVeinRule: null density function");
}

RuleEvaluatorPtr OreVeinRule::compile(MaterialRuleContext& context) const {
    // SharedConstants.DEBUG_DISABLE_ORE_VEINS / DEBUG_ORE_VEINS are off: the
    // default state is null and the filler is the rule's own.
    class Evaluator final : public RuleEvaluator {
    public:
        Evaluator(const OreVeinRule& rule, DensityGetter densitySampler, DensityGetter richnessSampler,
                  DensityGetter fillerGapSampler, random::AnyPositionalRandomFactory randomFactory)
            : m_rule(rule),
              m_densitySampler(std::move(densitySampler)),
              m_richnessSampler(std::move(richnessSampler)),
              m_fillerGapSampler(std::move(fillerGapSampler)),
              m_randomFactory(randomFactory) {}

        BlockState* tryApply(int32_t blockX, int32_t blockY, int32_t blockZ) override {
            const float density = m_densitySampler.get();
            if (density <= 0.0f) return nullptr;
            random::AnyRandomSource random = m_randomFactory.at(blockX, blockY, blockZ);
            if (random.nextFloat() > density) return nullptr;
            const float richness = m_richnessSampler.get();
            if (random.nextFloat() < richness && m_fillerGapSampler.get() < 0.0f) {
                return random.nextFloat() < m_rule.m_rawOreChance ? m_rule.m_rawOreBlock : m_rule.m_oreBlock;
            }
            return m_rule.m_fillerBlock;
        }

    private:
        const OreVeinRule& m_rule;
        DensityGetter m_densitySampler;
        DensityGetter m_richnessSampler;
        DensityGetter m_fillerGapSampler;
        random::AnyPositionalRandomFactory m_randomFactory;
    };
    DensityGetter densitySampler = context.getDensitiesInChunk(m_density, true);
    DensityGetter richnessSampler = context.getDensitiesInChunk(m_richness, true);
    DensityGetter fillerGapSampler = context.getDensitiesInChunk(m_fillerGap, false);
    random::AnyPositionalRandomFactory randomFactory = context.getOrCreateRandomFactory("minecraft:ore");
    return std::make_unique<Evaluator>(*this, std::move(densitySampler), std::move(richnessSampler),
                                       std::move(fillerGapSampler), randomFactory);
}

} // namespace material
} // namespace levelgen
} // namespace minecraft
