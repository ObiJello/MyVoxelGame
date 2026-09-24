#include "levelgen/material/MaterialRules.h"

#include "levelgen/density/WorldgenRegistries.h"
#include "levelgen/density/terrain/TerrainSettings.h"
#include "levelgen/structure/StructureSet.h"   // structure::BiomeTags

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

// Reference: MaterialRules.bootstrapRules / bootstrapConditions and every
// rule's and condition's CODEC (26.3); MaterialRule.CODEC and
// MaterialCondition.CODEC (RegistryCodecs.holder over the DIRECT_CODEC);
// VerticalAnchor.CODEC; RegistryCodecs.holderSet(BIOME).

namespace minecraft {
namespace levelgen {
namespace material {

using nlohmann::json;

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("material codec: " + message);
}

std::string excerpt(const json& value) {
    std::string text = value.dump();
    if (text.size() > 200) text = text.substr(0, 200) + "...";
    return text;
}

const json& field(const json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end()) fail(std::string("No key ") + name + " in " + excerpt(object));
    return *it;
}

// Codec.INT over JsonOps: any number (Number.intValue), a boolean as 1/0.
int32_t intValue(const json& value, const char* name) {
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_number_integer()) return static_cast<int32_t>(value.get<int64_t>());
    if (value.is_number_unsigned()) return static_cast<int32_t>(value.get<uint64_t>());
    if (value.is_number_float()) {
        const double v = value.get<double>();
        if (!std::isfinite(v)) fail(std::string("Not a number: ") + name);
        return static_cast<int32_t>(static_cast<int64_t>(std::trunc(v)));
    }
    fail(std::string("Not a number: ") + name + " " + excerpt(value));
}

int32_t intField(const json& object, const char* name) {
    return intValue(field(object, name), name);
}

// Codec.intRange(min, max).
int32_t intRangeField(const json& object, const char* name, int32_t min, int32_t max) {
    const int32_t v = intField(object, name);
    if (v < min || v > max) {
        fail("Value " + std::to_string(v) + " outside of range [" + std::to_string(min) + ":" + std::to_string(max) +
             "] (" + name + ")");
    }
    return v;
}

// Codec.DOUBLE over JsonOps.
double doubleValue(const json& value, const char* name) {
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_number()) return value.get<double>();
    fail(std::string("Not a number: ") + name + " " + excerpt(value));
}

double doubleField(const json& object, const char* name) {
    return doubleValue(field(object, name), name);
}

// Codec.BOOL over JsonOps: a boolean, or a number (!= 0).
bool boolValue(const json& value, const char* name) {
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return intValue(value, name) != 0;
    fail(std::string("Not a boolean: ") + name + " " + excerpt(value));
}

bool boolField(const json& object, const char* name) {
    return boolValue(field(object, name), name);
}

// Codec.BOOL.optionalFieldOf(name, fallback): absent -> fallback; present
// but malformed is an error.
bool optionalBoolField(const json& object, const char* name, bool fallback) {
    auto it = object.find(name);
    if (it == object.end() || it->is_null()) return fallback;
    return boolValue(*it, name);
}

bool validNamespaceChar(char c) {
    return c == '_' || c == '-' || c == '.' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool validPathChar(char c) {
    return validNamespaceChar(c) || c == '/';
}

// Identifier.parse / Identifier.CODEC: "path" -> "minecraft:path".
std::string identifier(const std::string& text, const char* what) {
    const size_t colon = text.find(':');
    const std::string ns = colon == std::string::npos ? "minecraft" : text.substr(0, colon);
    const std::string path = colon == std::string::npos ? text : text.substr(colon + 1);
    if (ns.empty() || path.empty() ||
        !std::all_of(ns.begin(), ns.end(), validNamespaceChar) ||
        !std::all_of(path.begin(), path.end(), validPathChar)) {
        fail(std::string("Not a valid resource location: ") + text + " (" + what + ")");
    }
    return ns + ":" + path;
}

std::string identifierValue(const json& value, const char* what) {
    if (!value.is_string()) fail(std::string("Not a string: ") + what + " " + excerpt(value));
    return identifier(value.get<std::string>(), what);
}

std::string identifierField(const json& object, const char* name) {
    return identifierValue(field(object, name), name);
}

// placement.CaveSurface.CODEC (StringRepresentable.fromEnum).
CaveSurface caveSurfaceField(const json& object, const char* name) {
    const json& value = field(object, name);
    if (!value.is_string()) fail(std::string("Not a string: ") + name);
    const std::string s = value.get<std::string>();
    if (s == "ceiling") return CaveSurface::CEILING;
    if (s == "floor") return CaveSurface::FLOOR;
    fail("Unknown element name: " + s + " (" + name + ")");
}

// BlockState.CODEC.
BlockState* blockStateField(const json& object, const char* name) {
    const json& value = field(object, name);
    try {
        return density::TerrainSettings::parseBlockState(value);
    } catch (const std::exception& e) {
        fail(std::string(name) + ": " + e.what());
    }
}

} // namespace

// ---- Registry ------------------------------------------------------------------------------

MaterialRuleRegistry& MaterialRuleRegistry::get() {
    static MaterialRuleRegistry registry(density::WorldgenRegistries::get());
    return registry;
}

MaterialRuleRegistry::MaterialRuleRegistry(density::WorldgenRegistries& registries) : m_registries(registries) {}

MaterialRulePtr MaterialRuleRegistry::rule(const std::string& key) {
    const std::string id = identifier(key, "material_rule key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto cached = m_rules.find(id);
    if (cached != m_rules.end()) return cached->second;
    if (m_decodingRules.count(id) != 0) fail("Cyclic reference to material_rule " + id);

    m_decodingRules.insert(id);
    MaterialRulePtr value;
    try {
        auto factory = m_ruleFactories.find(id);
        if (factory != m_ruleFactories.end()) {
            value = factory->second();
            if (!value) fail("factory built no rule");
        } else {
            value = decodeRule(m_registries.readEntry(RULE_REGISTRY, id));
        }
    } catch (const std::exception& e) {
        m_decodingRules.erase(id);
        throw std::runtime_error(std::string("worldgen/material_rule ") + id + ": " + e.what());
    }
    m_decodingRules.erase(id);
    m_rules.emplace(id, value);
    return value;
}

MaterialConditionPtr MaterialRuleRegistry::condition(const std::string& key) {
    const std::string id = identifier(key, "material_condition key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto cached = m_conditions.find(id);
    if (cached != m_conditions.end()) return cached->second;
    if (m_decodingConditions.count(id) != 0) fail("Cyclic reference to material_condition " + id);

    m_decodingConditions.insert(id);
    MaterialConditionPtr value;
    try {
        auto factory = m_conditionFactories.find(id);
        if (factory != m_conditionFactories.end()) {
            value = factory->second();
            if (!value) fail("factory built no condition");
        } else {
            value = decodeCondition(m_registries.readEntry(CONDITION_REGISTRY, id));
        }
    } catch (const std::exception& e) {
        m_decodingConditions.erase(id);
        throw std::runtime_error(std::string("worldgen/material_condition ") + id + ": " + e.what());
    }
    m_decodingConditions.erase(id);
    m_conditions.emplace(id, value);
    return value;
}

bool MaterialRuleRegistry::hasRule(const std::string& key) {
    const std::string id = identifier(key, "material_rule key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_rules.count(id) != 0 || m_ruleFactories.count(id) != 0 || m_registries.hasEntry(RULE_REGISTRY, id);
}

bool MaterialRuleRegistry::hasCondition(const std::string& key) {
    const std::string id = identifier(key, "material_condition key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_conditions.count(id) != 0 || m_conditionFactories.count(id) != 0 ||
           m_registries.hasEntry(CONDITION_REGISTRY, id);
}

void MaterialRuleRegistry::registerRule(const std::string& key, MaterialRulePtr rule) {
    if (!rule) throw std::invalid_argument("registerRule: null rule for " + key);
    const std::string id = identifier(key, "material_rule key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_ruleFactories.erase(id);
    m_rules[id] = std::move(rule);
}

void MaterialRuleRegistry::registerCondition(const std::string& key, MaterialConditionPtr condition) {
    if (!condition) throw std::invalid_argument("registerCondition: null condition for " + key);
    const std::string id = identifier(key, "material_condition key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_conditionFactories.erase(id);
    m_conditions[id] = std::move(condition);
}

void MaterialRuleRegistry::registerRuleFactory(const std::string& key, RuleFactory factory) {
    if (!factory) throw std::invalid_argument("registerRuleFactory: null factory for " + key);
    const std::string id = identifier(key, "material_rule key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_rules.erase(id);
    m_ruleFactories[id] = std::move(factory);
}

void MaterialRuleRegistry::registerConditionFactory(const std::string& key, ConditionFactory factory) {
    if (!factory) throw std::invalid_argument("registerConditionFactory: null factory for " + key);
    const std::string id = identifier(key, "material_condition key");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_conditions.erase(id);
    m_conditionFactories[id] = std::move(factory);
}

// ---- MaterialRule.CODEC / MaterialCondition.CODEC -------------------------------------------

MaterialRulePtr MaterialRuleRegistry::parseRule(const json& value) {
    // RegistryFileCodec: a string names a registry entry (Holder.Reference ->
    // HolderHolder); anything else is a direct element (unwrapped).
    if (value.is_string()) {
        const std::string id = identifierValue(value, "material_rule reference");
        return std::make_shared<RuleReference>(id, rule(id));
    }
    return decodeRule(value);
}

MaterialConditionPtr MaterialRuleRegistry::parseCondition(const json& value) {
    if (value.is_string()) {
        const std::string id = identifierValue(value, "material_condition reference");
        return std::make_shared<ConditionReference>(id, condition(id));
    }
    return decodeCondition(value);
}

// MaterialRule.DIRECT_CODEC: MATERIAL_RULE_TYPE.byNameCodec().dispatch(...).
MaterialRulePtr MaterialRuleRegistry::decodeRule(const json& value) {
    if (!value.is_object()) fail("Not a material rule: " + excerpt(value));
    const std::string type = identifierField(value, "type");

    if (type == "minecraft:block") {
        return std::make_shared<BlockRule>(blockStateField(value, "result_state"));
    }
    if (type == "minecraft:bandlands") {
        return BandlandsRule::instance();
    }
    if (type == "minecraft:sequence") {
        const json& list = field(value, "sequence");
        if (!list.is_array()) fail("Not a list: sequence " + excerpt(list));
        std::vector<MaterialRulePtr> sequence;
        sequence.reserve(list.size());
        for (const json& element : list) sequence.push_back(parseRule(element));
        return std::make_shared<SequenceRule>(std::move(sequence));
    }
    if (type == "minecraft:condition") {
        MaterialConditionPtr ifTrue = parseCondition(field(value, "if_true"));
        MaterialRulePtr thenRun = parseRule(field(value, "then_run"));
        return std::make_shared<ConditionRule>(std::move(ifTrue), std::move(thenRun));
    }
    if (type == "minecraft:ore_vein") {
        BlockState* oreBlock = blockStateField(value, "ore_block");
        BlockState* rawOreBlock = blockStateField(value, "raw_ore_block");
        BlockState* fillerBlock = blockStateField(value, "filler_block");
        // Codec.floatRange(0.0F, 1.0F).
        const float rawOreChance = static_cast<float>(doubleField(value, "raw_ore_chance"));
        if (!(rawOreChance >= 0.0f && rawOreChance <= 1.0f)) {
            fail("Value " + std::to_string(rawOreChance) + " outside of range [0.0:1.0] (raw_ore_chance)");
        }
        density::DensityFunctionPtr density = m_registries.parseDensityFunction(field(value, "density"));
        density::DensityFunctionPtr richness = m_registries.parseDensityFunction(field(value, "richness"));
        density::DensityFunctionPtr fillerGap = m_registries.parseDensityFunction(field(value, "filler_gap"));
        return std::make_shared<OreVeinRule>(oreBlock, rawOreBlock, fillerBlock, rawOreChance, std::move(density),
                                             std::move(richness), std::move(fillerGap));
    }
    fail("Unknown material rule type: " + type);
}

// MaterialCondition.DIRECT_CODEC: MATERIAL_CONDITION_TYPE.byNameCodec().dispatch(...).
MaterialConditionPtr MaterialRuleRegistry::decodeCondition(const json& value) {
    if (!value.is_object()) fail("Not a material condition: " + excerpt(value));
    const std::string type = identifierField(value, "type");

    if (type == "minecraft:biome") {
        return std::make_shared<BiomeCondition>(parseBiomeSet(field(value, "biome_is")));
    }
    if (type == "minecraft:noise_threshold") {
        std::string noise = identifierField(value, "noise");
        const double minThreshold = doubleField(value, "min_threshold");
        const double maxThreshold = doubleField(value, "max_threshold");
        const bool is3d = optionalBoolField(value, "is_3d", false);
        return std::make_shared<NoiseThresholdCondition>(std::move(noise), minThreshold, maxThreshold, is3d);
    }
    if (type == "minecraft:vertical_gradient") {
        std::string randomName = identifierField(value, "random_name");
        const VerticalAnchor trueAtAndBelow = parseVerticalAnchor(field(value, "true_at_and_below"));
        const VerticalAnchor falseAtAndAbove = parseVerticalAnchor(field(value, "false_at_and_above"));
        return std::make_shared<VerticalGradientCondition>(std::move(randomName), trueAtAndBelow, falseAtAndAbove);
    }
    if (type == "minecraft:y_above") {
        const VerticalAnchor anchor = parseVerticalAnchor(field(value, "anchor"));
        const int32_t surfaceDepthMultiplier = intRangeField(value, "surface_depth_multiplier", -20, 20);
        const bool addStoneDepth = boolField(value, "add_stone_depth");
        return std::make_shared<YCondition>(anchor, surfaceDepthMultiplier, addStoneDepth);
    }
    if (type == "minecraft:water") {
        const int32_t offset = intField(value, "offset");
        const int32_t surfaceDepthMultiplier = intRangeField(value, "surface_depth_multiplier", -20, 20);
        const bool addStoneDepth = boolField(value, "add_stone_depth");
        return std::make_shared<WaterCondition>(offset, surfaceDepthMultiplier, addStoneDepth);
    }
    if (type == "minecraft:temperature") {
        return TemperatureCondition::instance();
    }
    if (type == "minecraft:steep") {
        return SteepCondition::instance();
    }
    if (type == "minecraft:not") {
        return std::make_shared<NotCondition>(parseCondition(field(value, "invert")));
    }
    if (type == "minecraft:hole") {
        return HoleCondition::instance();
    }
    if (type == "minecraft:above_preliminary_surface") {
        return AbovePreliminarySurfaceCondition::instance();
    }
    if (type == "minecraft:stone_depth") {
        const int32_t offset = intField(value, "offset");
        const bool addSurfaceDepth = boolField(value, "add_surface_depth");
        const int32_t secondaryDepthRange = intField(value, "secondary_depth_range");
        const CaveSurface surfaceType = caveSurfaceField(value, "surface_type");
        return std::make_shared<StoneDepthCondition>(offset, addSurfaceDepth, secondaryDepthRange, surfaceType);
    }
    fail("Unknown material condition type: " + type);
}

// ---- VerticalAnchor.CODEC ---------------------------------------------------------------------

VerticalAnchor MaterialRuleRegistry::parseVerticalAnchor(const json& value) {
    // Codec.xor over the four single-field codecs: exactly one must read.
    if (!value.is_object()) fail("Not a vertical anchor: " + excerpt(value));
    static const std::pair<const char*, VerticalAnchor::Kind> kForms[] = {
        {"absolute", VerticalAnchor::Kind::ABSOLUTE},
        {"above_bottom", VerticalAnchor::Kind::ABOVE_BOTTOM},
        {"below_top", VerticalAnchor::Kind::BELOW_TOP},
        {"relative_to_sea_level", VerticalAnchor::Kind::RELATIVE_TO_SEA_LEVEL},
    };
    const char* found = nullptr;
    VerticalAnchor anchor;
    for (const auto& [name, kind] : kForms) {
        if (value.find(name) == value.end()) continue;
        if (found != nullptr) {
            fail(std::string("Both alternatives read successfully, can not pick the correct one (") + found + ", " +
                 name + ") in " + excerpt(value));
        }
        found = name;
        const int32_t v = intRangeField(value, name, VerticalAnchor::MIN_VALUE, VerticalAnchor::MAX_VALUE);
        anchor = VerticalAnchor(kind, v);
    }
    if (found == nullptr) fail("Not a vertical anchor (no absolute/above_bottom/below_top/relative_to_sea_level): " +
                               excerpt(value));
    return anchor;
}

// ---- RegistryCodecs.holderSet(BIOME) ------------------------------------------------------------

BiomeSet MaterialRuleRegistry::parseBiomeSet(const json& value) {
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (!text.empty() && text[0] == '#') {
            const std::string tag = identifier(text.substr(1), "biome tag");
            const std::unordered_set<std::string>& members = structure::BiomeTags::resolve("#" + tag);
            std::vector<std::string> ids(members.begin(), members.end());
            std::sort(ids.begin(), ids.end());
            return BiomeSet::named(tag, std::move(ids));
        }
        // A single id is a one-element direct set (compact list).
        return BiomeSet::direct({identifier(text, "biome")});
    }
    if (value.is_array()) {
        std::vector<std::string> ids;
        ids.reserve(value.size());
        for (const json& element : value) ids.push_back(identifierValue(element, "biome"));
        return BiomeSet::direct(std::move(ids));
    }
    fail("Not a biome holder set: " + excerpt(value));
}

} // namespace material
} // namespace levelgen
} // namespace minecraft
