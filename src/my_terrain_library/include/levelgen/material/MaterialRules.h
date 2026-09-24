#pragma once

#include "levelgen/material/MaterialCondition.h"
#include "levelgen/material/MaterialRule.h"
#include "levelgen/material/VerticalAnchor.h"

#include "external/json.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Reference: levelgen.material.MaterialRules (26.3) - the builders the
// vanilla bootstrap uses - and the MATERIAL_RULE / MATERIAL_CONDITION
// registries with their codecs.
//
// Registries: data/<ns>/worldgen/material_rule/<path>.json and
// data/<ns>/worldgen/material_condition/<path>.json under the density
// registries' data root, read and decoded the first time something names them,
// then kept. Entries can also be registered from code (the engine's own
// dimensions build theirs in C++); a registered entry wins over a file of the
// same name. A factory registration builds its entry on first use, so a rule
// that names blocks only some worlds register fails only the dimension that
// uses it.
//
// Codecs (MaterialRule.CODEC / MaterialCondition.CODEC): a string is a
// registry reference (Holder.Reference -> HolderHolder, resolved when decoded),
// an object dispatches on "type" to the rule's or condition's MapCodec. Field
// names, required fields and defaults mirror the Java codecs. Decoding errors
// throw std::runtime_error naming the entry and the complaint.

namespace minecraft {
namespace levelgen {
namespace density {
class WorldgenRegistries;
}

namespace material {

class MaterialRuleRegistry {
public:
    static constexpr const char* RULE_REGISTRY = "material_rule";
    static constexpr const char* CONDITION_REGISTRY = "material_condition";

    using RuleFactory = std::function<MaterialRulePtr()>;
    using ConditionFactory = std::function<MaterialConditionPtr()>;

    // The process-wide registries over density::WorldgenRegistries::get().
    static MaterialRuleRegistry& get();

    explicit MaterialRuleRegistry(density::WorldgenRegistries& registries);

    MaterialRuleRegistry(const MaterialRuleRegistry&) = delete;
    MaterialRuleRegistry& operator=(const MaterialRuleRegistry&) = delete;

    // The registry value for a key ("minecraft:overworld"; the namespace
    // defaults to minecraft). Throws when there is no such entry.
    MaterialRulePtr rule(const std::string& key);
    MaterialConditionPtr condition(const std::string& key);
    bool hasRule(const std::string& key);
    bool hasCondition(const std::string& key);

    // Code-built entries; they replace any cached or file entry of that key.
    void registerRule(const std::string& key, MaterialRulePtr rule);
    void registerCondition(const std::string& key, MaterialConditionPtr condition);
    void registerRuleFactory(const std::string& key, RuleFactory factory);
    void registerConditionFactory(const std::string& key, ConditionFactory factory);

    // MaterialRule.CODEC / MaterialCondition.CODEC.
    MaterialRulePtr parseRule(const nlohmann::json& json);
    MaterialConditionPtr parseCondition(const nlohmann::json& json);

    // VerticalAnchor.CODEC: exactly one of absolute / above_bottom /
    // below_top / relative_to_sea_level, each in [-2032, 2031].
    static VerticalAnchor parseVerticalAnchor(const nlohmann::json& json);
    // RegistryCodecs.holderSet(BIOME): "#tag", one id, or a list of ids.
    static BiomeSet parseBiomeSet(const nlohmann::json& json);

    density::WorldgenRegistries& worldgenRegistries() { return m_registries; }

private:
    MaterialRulePtr decodeRule(const nlohmann::json& json);
    MaterialConditionPtr decodeCondition(const nlohmann::json& json);

    density::WorldgenRegistries& m_registries;

    // Recursive: decoding one entry decodes the entries it references.
    std::recursive_mutex m_mutex;
    std::unordered_map<std::string, MaterialRulePtr> m_rules;
    std::unordered_map<std::string, MaterialConditionPtr> m_conditions;
    std::unordered_map<std::string, RuleFactory> m_ruleFactories;
    std::unordered_map<std::string, ConditionFactory> m_conditionFactories;
    std::unordered_set<std::string> m_decodingRules;        // cycle guard
    std::unordered_set<std::string> m_decodingConditions;   // cycle guard
};

// Registers the engine's own dimensions' material rules (idempotent):
//   "obeycraft:hush"                  - The Hush
//   "aether:aether"                   - The Aether (skylands surface_rule)
//   "twilight_forest:twilight_forest" - The Twilight Forest (twilight_noise_gen)
// Each is built on first use (factory), so their blocks are resolved only when
// that dimension's generator asks for its rule.
void registerModMaterialRules();

// MaterialRules' builders (the Java static helpers), for code-built rule sets.
namespace MaterialRules {

MaterialRulePtr getRule(const std::string& key);             // registry reference
MaterialConditionPtr getCondition(const std::string& key);   // registry reference

MaterialConditionPtr stoneDepthCheck(int32_t offset, bool addSurfaceDepth, CaveSurface surfaceType);
MaterialConditionPtr stoneDepthCheck(int32_t offset, bool addSurfaceDepth, int32_t secondaryDepthRange,
                                     CaveSurface surfaceType);
MaterialConditionPtr not_(MaterialConditionPtr target);
MaterialConditionPtr yBlockCheck(const VerticalAnchor& anchor, int32_t surfaceDepthMultiplier);
MaterialConditionPtr yStartCheck(const VerticalAnchor& anchor, int32_t surfaceDepthMultiplier);
MaterialConditionPtr waterBlockCheck(int32_t offset, int32_t surfaceDepthMultiplier);
MaterialConditionPtr waterStartCheck(int32_t offset, int32_t surfaceDepthMultiplier);
MaterialConditionPtr isBiome(std::vector<std::string> biomes);
MaterialConditionPtr noiseCondition2d(const std::string& noise, double minRange);
MaterialConditionPtr noiseCondition2d(const std::string& noise, double minRange, double maxRange);
MaterialConditionPtr noiseCondition3d(const std::string& noise, double minRange);
MaterialConditionPtr noiseCondition3d(const std::string& noise, double minRange, double maxRange);
MaterialConditionPtr verticalGradient(const std::string& randomName, const VerticalAnchor& trueAtAndBelow,
                                      const VerticalAnchor& falseAtAndAbove);
MaterialConditionPtr steep();
MaterialConditionPtr hole();
MaterialConditionPtr abovePreliminarySurface();
MaterialConditionPtr temperature();

MaterialRulePtr ifTrue(MaterialConditionPtr condition, MaterialRulePtr next);
// Throws on an empty list ("Need at least 1 rule for a sequence").
MaterialRulePtr sequence(std::vector<MaterialRulePtr> rules);
MaterialRulePtr state(BlockState* state);
// The block's default state; throws when the block is not registered.
MaterialRulePtr state(const std::string& blockName);
MaterialRulePtr bandlands();

} // namespace MaterialRules

} // namespace material
} // namespace levelgen
} // namespace minecraft
