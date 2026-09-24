#include "levelgen/material/MaterialCondition.h"

#include "levelgen/density/JavaMath.h"
#include "levelgen/material/MaterialRuleContext.h"
#include "world/biome/Biome.h"

#include <algorithm>
#include <climits>
#include <utility>

// Reference: levelgen.material.condition.* (26.3), every condition's compile.

namespace minecraft {
namespace levelgen {
namespace material {

namespace {

// Mth.map(double...) = lerp(inverseLerp(value, fromMin, fromMax), toMin, toMax).
double mthMap(double value, double fromMin, double fromMax, double toMin, double toMax) {
    const double alpha = (value - fromMin) / (fromMax - fromMin);
    return toMin + alpha * (toMax - toMin);
}

class ConstantEvaluator final : public ConditionEvaluator {
public:
    explicit ConstantEvaluator(bool value) : m_value(value) {}
    bool test() override { return m_value; }

private:
    bool m_value;
};

} // namespace

// ---- VerticalAnchor -------------------------------------------------------------

std::string VerticalAnchor::toString() const {
    switch (m_kind) {
        case Kind::ABSOLUTE:
            return std::to_string(m_value) + " absolute";
        case Kind::ABOVE_BOTTOM:
            return std::to_string(m_value) + " above bottom";
        case Kind::BELOW_TOP:
            return std::to_string(m_value) + " below top";
        case Kind::RELATIVE_TO_SEA_LEVEL:
            return std::to_string(m_value) + " relative to sea level";
    }
    return std::to_string(m_value);
}

// ---- BiomeSet ---------------------------------------------------------------------

BiomeSet BiomeSet::direct(std::vector<std::string> ids) {
    BiomeSet set;
    set.m_ids = std::move(ids);
    set.m_lookup.insert(set.m_ids.begin(), set.m_ids.end());
    return set;
}

BiomeSet BiomeSet::named(std::string tag, std::vector<std::string> ids) {
    BiomeSet set = direct(std::move(ids));
    set.m_tag = std::move(tag);
    return set;
}

bool BiomeSet::contains(const world::biome::Biome* biome) const {
    return biome != nullptr && contains(biome->getName());
}

// ---- ConditionReference (MaterialCondition.HolderHolder) --------------------------

ConditionReference::ConditionReference(std::string key, MaterialConditionPtr value)
    : m_key(std::move(key)), m_value(std::move(value)) {}

ConditionEvaluatorPtr ConditionReference::compile(MaterialRuleContext& context) const {
    return m_value->compile(context);
}

// ---- AbovePreliminarySurfaceCondition -------------------------------------------

const std::shared_ptr<const AbovePreliminarySurfaceCondition>& AbovePreliminarySurfaceCondition::instance() {
    static const auto s_instance = std::make_shared<const AbovePreliminarySurfaceCondition>();
    return s_instance;
}

ConditionEvaluatorPtr AbovePreliminarySurfaceCondition::compile(MaterialRuleContext& context) const {
    class Evaluator final : public ConditionEvaluator {
    public:
        explicit Evaluator(MaterialRuleContext& context) : m_context(context) {}
        bool test() override { return m_context.blockY() >= m_context.getMinSurfaceLevel(); }

    private:
        MaterialRuleContext& m_context;
    };
    return std::make_unique<Evaluator>(context);
}

// ---- BiomeCondition -----------------------------------------------------------------

BiomeCondition::BiomeCondition(BiomeSet biomes) : m_biomes(std::move(biomes)) {}

ConditionEvaluatorPtr BiomeCondition::compile(MaterialRuleContext& ruleContext) const {
    const PossibleBiomes* possibleBiomes = ruleContext.possibleBiomes();
    if (possibleBiomes != nullptr) {
        if (canNeverMatch(*possibleBiomes)) return std::make_unique<ConstantEvaluator>(false);
        if (willAlwaysMatch(*possibleBiomes)) return std::make_unique<ConstantEvaluator>(true);
    }

    class Evaluator final : public MaterialRuleContext::LazyYCondition {
    public:
        Evaluator(MaterialRuleContext& context, const BiomeSet& biomes) : LazyYCondition(context), m_biomes(biomes) {}

    protected:
        bool compute() override {
            // biomes.contains(getBiome()), with the last answer kept per biome
            // (the engine's membership test is by id).
            const world::biome::Biome* biome = context.getBiome();
            if (!m_hasLast || biome != m_lastBiome) {
                m_lastBiome = biome;
                m_lastResult = m_biomes.contains(biome);
                m_hasLast = true;
            }
            return m_lastResult;
        }

    private:
        const BiomeSet& m_biomes;
        const world::biome::Biome* m_lastBiome = nullptr;
        bool m_lastResult = false;
        bool m_hasLast = false;
    };
    return std::make_unique<Evaluator>(ruleContext, m_biomes);
}

bool BiomeCondition::canNeverMatch(const std::set<std::string>& possibleBiomes) const {
    for (const std::string& id : m_biomes.ids()) {
        if (possibleBiomes.count(id) != 0) return false;
    }
    return true;
}

bool BiomeCondition::willAlwaysMatch(const std::set<std::string>& possibleBiomes) const {
    for (const std::string& possible : possibleBiomes) {
        if (!m_biomes.contains(possible)) return false;
    }
    return true;
}

// ---- HoleCondition --------------------------------------------------------------------

const std::shared_ptr<const HoleCondition>& HoleCondition::instance() {
    static const auto s_instance = std::make_shared<const HoleCondition>();
    return s_instance;
}

ConditionEvaluatorPtr HoleCondition::compile(MaterialRuleContext& context) const {
    class Evaluator final : public MaterialRuleContext::LazyXZCondition {
    public:
        explicit Evaluator(MaterialRuleContext& context) : LazyXZCondition(context) {}

    protected:
        bool compute() override { return context.surfaceDepth() <= 0; }
    };
    return std::make_unique<Evaluator>(context);
}

// ---- NoiseThresholdCondition ------------------------------------------------------------

NoiseThresholdCondition::NoiseThresholdCondition(std::string noise, double minThreshold, double maxThreshold,
                                                 bool is3d)
    : m_noise(std::move(noise)), m_minThreshold(minThreshold), m_maxThreshold(maxThreshold), m_is3d(is3d) {}

ConditionEvaluatorPtr NoiseThresholdCondition::compile(MaterialRuleContext& ruleContext) const {
    class Evaluator final : public ConditionEvaluator {
    public:
        Evaluator(DoubleSupplier& noise, double minThreshold, double maxThreshold)
            : m_noise(noise), m_minThreshold(minThreshold), m_maxThreshold(maxThreshold) {}

        bool test() override {
            const double value = m_noise.getAsDouble();
            return value >= m_minThreshold && value <= m_maxThreshold;
        }

    private:
        DoubleSupplier& m_noise;
        double m_minThreshold;
        double m_maxThreshold;
    };
    return std::make_unique<Evaluator>(ruleContext.getNoiseSampler(m_noise, m_is3d), m_minThreshold, m_maxThreshold);
}

// ---- NotCondition ----------------------------------------------------------------------------

NotCondition::NotCondition(MaterialConditionPtr target) : m_target(std::move(target)) {}

ConditionEvaluatorPtr NotCondition::compile(MaterialRuleContext& context) const {
    class Evaluator final : public ConditionEvaluator {
    public:
        explicit Evaluator(ConditionEvaluatorPtr target) : m_target(std::move(target)) {}
        bool test() override { return !m_target->test(); }

    private:
        ConditionEvaluatorPtr m_target;
    };
    return std::make_unique<Evaluator>(m_target->compile(context));
}

// ---- SteepCondition ----------------------------------------------------------------------------

const std::shared_ptr<const SteepCondition>& SteepCondition::instance() {
    static const auto s_instance = std::make_shared<const SteepCondition>();
    return s_instance;
}

ConditionEvaluatorPtr SteepCondition::compile(MaterialRuleContext& context) const {
    class Evaluator final : public MaterialRuleContext::LazyXZCondition {
    public:
        explicit Evaluator(MaterialRuleContext& context) : LazyXZCondition(context) {}

    protected:
        bool compute() override { return context.surfaceGradientX() <= -4 || context.surfaceGradientZ() >= 4; }
    };
    return std::make_unique<Evaluator>(context);
}

// ---- StoneDepthCondition ------------------------------------------------------------------------

StoneDepthCondition::StoneDepthCondition(int32_t offset, bool addSurfaceDepth, int32_t secondaryDepthRange,
                                         CaveSurface surfaceType)
    : m_offset(offset),
      m_addSurfaceDepth(addSurfaceDepth),
      m_secondaryDepthRange(secondaryDepthRange),
      m_surfaceType(surfaceType) {}

ConditionEvaluatorPtr StoneDepthCondition::compile(MaterialRuleContext& ruleContext) const {
    class Evaluator final : public MaterialRuleContext::LazyYCondition {
    public:
        Evaluator(MaterialRuleContext& context, const StoneDepthCondition& condition, bool ceiling)
            : LazyYCondition(context), m_condition(condition), m_ceiling(ceiling) {}

    protected:
        bool compute() override {
            const int32_t stoneDepth = m_ceiling ? context.stoneDepthBelow() : context.stoneDepthAbove();
            const int32_t surfaceDepth = m_condition.m_addSurfaceDepth ? context.surfaceDepth() : 0;
            const int32_t secondarySurfaceDepth =
                m_condition.m_secondaryDepthRange == 0
                    ? 0
                    : density::jmath::d2i(mthMap(context.getSurfaceSecondary(), -1.0, 1.0, 0.0,
                                                 static_cast<double>(m_condition.m_secondaryDepthRange)));
            return stoneDepth <= 1 + m_condition.m_offset + surfaceDepth + secondarySurfaceDepth;
        }

    private:
        const StoneDepthCondition& m_condition;
        bool m_ceiling;
    };
    const bool ceiling = m_surfaceType == CaveSurface::CEILING;
    return std::make_unique<Evaluator>(ruleContext, *this, ceiling);
}

// ---- TemperatureCondition -------------------------------------------------------------------------

const std::shared_ptr<const TemperatureCondition>& TemperatureCondition::instance() {
    static const auto s_instance = std::make_shared<const TemperatureCondition>();
    return s_instance;
}

ConditionEvaluatorPtr TemperatureCondition::compile(MaterialRuleContext& context) const {
    class Evaluator final : public ConditionEvaluator {
    public:
        explicit Evaluator(MaterialRuleContext& context) : m_context(context) {}

        bool test() override {
            const world::biome::Biome* biome = m_context.getBiome();
            return biome != nullptr && biome->coldEnoughToSnow(m_context.blockPos(), m_context.getSeaLevel());
        }

    private:
        MaterialRuleContext& m_context;
    };
    return std::make_unique<Evaluator>(context);
}

// ---- VerticalGradientCondition ----------------------------------------------------------------------

VerticalGradientCondition::VerticalGradientCondition(std::string randomName, VerticalAnchor trueAtAndBelow,
                                                     VerticalAnchor falseAtAndAbove)
    : m_randomName(std::move(randomName)), m_trueAtAndBelow(trueAtAndBelow), m_falseAtAndAbove(falseAtAndAbove) {}

ConditionEvaluatorPtr VerticalGradientCondition::compile(MaterialRuleContext& ruleContext) const {
    class Evaluator final : public MaterialRuleContext::LazyYCondition {
    public:
        Evaluator(MaterialRuleContext& context, int32_t trueAtAndBelow, int32_t falseAtAndAbove,
                  random::AnyPositionalRandomFactory randomFactory)
            : LazyYCondition(context),
              m_trueAtAndBelow(trueAtAndBelow),
              m_falseAtAndAbove(falseAtAndAbove),
              m_randomFactory(randomFactory) {}

    protected:
        bool compute() override {
            const int32_t blockY = context.blockY();
            if (blockY <= m_trueAtAndBelow) return true;
            if (blockY >= m_falseAtAndAbove) return false;
            const double probability = mthMap(static_cast<double>(blockY), static_cast<double>(m_trueAtAndBelow),
                                              static_cast<double>(m_falseAtAndAbove), 1.0, 0.0);
            random::AnyRandomSource random = m_randomFactory.at(context.blockX(), blockY, context.blockZ());
            return static_cast<double>(random.nextFloat()) < probability;
        }

    private:
        int32_t m_trueAtAndBelow;
        int32_t m_falseAtAndAbove;
        random::AnyPositionalRandomFactory m_randomFactory;
    };
    const int32_t trueAtAndBelow = ruleContext.resolveAnchorY(m_trueAtAndBelow);
    const int32_t falseAtAndAbove = ruleContext.resolveAnchorY(m_falseAtAndAbove);
    random::AnyPositionalRandomFactory randomFactory = ruleContext.getOrCreateRandomFactory(m_randomName);
    return std::make_unique<Evaluator>(ruleContext, trueAtAndBelow, falseAtAndAbove, randomFactory);
}

// ---- WaterCondition -----------------------------------------------------------------------------------

WaterCondition::WaterCondition(int32_t offset, int32_t surfaceDepthMultiplier, bool addStoneDepth)
    : m_offset(offset), m_surfaceDepthMultiplier(surfaceDepthMultiplier), m_addStoneDepth(addStoneDepth) {}

ConditionEvaluatorPtr WaterCondition::compile(MaterialRuleContext& ruleContext) const {
    class Evaluator final : public MaterialRuleContext::LazyYCondition {
    public:
        Evaluator(MaterialRuleContext& context, const WaterCondition& condition)
            : LazyYCondition(context), m_condition(condition) {}

    protected:
        bool compute() override {
            return context.waterHeight() == INT32_MIN ||
                   context.blockY() + (m_condition.m_addStoneDepth ? context.stoneDepthAbove() : 0) >=
                       context.waterHeight() + m_condition.m_offset +
                           context.surfaceDepth() * m_condition.m_surfaceDepthMultiplier;
        }

    private:
        const WaterCondition& m_condition;
    };
    return std::make_unique<Evaluator>(ruleContext, *this);
}

// ---- YCondition ------------------------------------------------------------------------------------------

YCondition::YCondition(VerticalAnchor anchor, int32_t surfaceDepthMultiplier, bool addStoneDepth)
    : m_anchor(anchor), m_surfaceDepthMultiplier(surfaceDepthMultiplier), m_addStoneDepth(addStoneDepth) {}

ConditionEvaluatorPtr YCondition::compile(MaterialRuleContext& ruleContext) const {
    class Evaluator final : public MaterialRuleContext::LazyYCondition {
    public:
        Evaluator(MaterialRuleContext& context, const YCondition& condition)
            : LazyYCondition(context), m_condition(condition) {}

    protected:
        bool compute() override {
            return context.blockY() + (m_condition.m_addStoneDepth ? context.stoneDepthAbove() : 0) >=
                   context.resolveAnchorY(m_condition.m_anchor) +
                       context.surfaceDepth() * m_condition.m_surfaceDepthMultiplier;
        }

    private:
        const YCondition& m_condition;
    };
    return std::make_unique<Evaluator>(ruleContext, *this);
}

} // namespace material
} // namespace levelgen
} // namespace minecraft
