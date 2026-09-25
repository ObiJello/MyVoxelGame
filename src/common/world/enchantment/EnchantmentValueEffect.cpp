// File: src/common/world/enchantment/EnchantmentValueEffect.cpp
#include "EnchantmentValueEffect.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/tags/DataTags.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace Game {

    namespace {

        std::string_view StripDefaultNamespace(std::string_view id) {
            constexpr std::string_view kPrefix = "minecraft:";
            return id.substr(0, kPrefix.size()) == kPrefix ? id.substr(kPrefix.size()) : id;
        }

        std::string WithNamespace(std::string id) {
            if (id.find(':') == std::string::npos) id = "minecraft:" + id;
            return id;
        }

        // A codec float field, `fallback` when absent.
        float ReadFloat(const nlohmann::json& j, const char* key, float fallback) {
            if (!j.contains(key) || !j[key].is_number()) return fallback;
            return j[key].get<float>();
        }

    } // namespace

    // ── LevelBasedValue ─────────────────────────────────────────────────────

    float LevelBasedValue::Calculate(int level) const {
        switch (type) {
            case Type::Constant:
                return a;
            case Type::Linear:
                // Linear.calculate: base + perLevelAboveFirst * (level - 1).
                return a + b * static_cast<float>(level - 1);
            case Type::Clamped: {
                // Mth.clamp(value, min, max): below min → min, else min(value, max).
                if (children.empty()) return 0.0f;
                const float v = children[0].Calculate(level);
                return v < a ? a : std::min(v, b);
            }
            case Type::Fraction: {
                if (children.size() < 2) return 0.0f;
                const float denominator = children[1].Calculate(level);
                return denominator == 0.0f ? 0.0f : children[0].Calculate(level) / denominator;
            }
            case Type::LevelsSquared:
                return static_cast<float>(level * level) + a;
            case Type::Exponent:
                if (children.size() < 2) return 0.0f;
                return static_cast<float>(std::pow(static_cast<double>(children[0].Calculate(level)),
                                                   static_cast<double>(children[1].Calculate(level))));
            case Type::Lookup:
                // Lookup.calculate: values[level - 1] while in range, else the fallback.
                if (level >= 1 && level <= static_cast<int>(values.size())) {
                    return values[static_cast<size_t>(level - 1)];
                }
                return children.empty() ? 0.0f : children[0].Calculate(level);
        }
        return 0.0f;
    }

    bool LevelBasedValue::Parse(const nlohmann::json& j, LevelBasedValue& out) {
        out = LevelBasedValue{};
        if (j.is_number()) {                       // Constant.CODEC: the bare float
            out.a = j.get<float>();
            return true;
        }
        if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return false;
        const std::string_view type = StripDefaultNamespace(j["type"].get_ref<const std::string&>());
        auto child = [&](const char* key) {
            LevelBasedValue v;
            const bool ok = j.contains(key) && Parse(j[key], v);
            out.children.push_back(std::move(v));
            return ok;
        };
        if (type == "linear") {
            out.type = Type::Linear;
            out.a = ReadFloat(j, "base", 0.0f);
            out.b = ReadFloat(j, "per_level_above_first", 0.0f);
            return true;
        }
        if (type == "clamped") {
            out.type = Type::Clamped;
            out.a = ReadFloat(j, "min", 0.0f);
            out.b = ReadFloat(j, "max", 0.0f);
            return child("value");
        }
        if (type == "fraction") {
            out.type = Type::Fraction;
            const bool n = child("numerator");
            const bool d = child("denominator");
            return n && d;
        }
        if (type == "levels_squared") {
            out.type = Type::LevelsSquared;
            out.a = ReadFloat(j, "added", 0.0f);
            return true;
        }
        if (type == "exponent") {
            out.type = Type::Exponent;
            const bool base = child("base");
            const bool power = child("power");
            return base && power;
        }
        if (type == "lookup") {
            out.type = Type::Lookup;
            if (j.contains("values") && j["values"].is_array()) {
                for (const auto& v : j["values"]) if (v.is_number()) out.values.push_back(v.get<float>());
            }
            return child("fallback");
        }
        return false;
    }

    // ── EnchantmentValueEffect ──────────────────────────────────────────────

    float EnchantmentValueEffect::Process(int level, JavaRandom& random, float value) const {
        switch (type) {
            case Type::Add:
                return value + first.Calculate(level);
            case Type::Multiply:
                return value * first.Calculate(level);
            case Type::Set:
                return first.Calculate(level);
            case Type::Exponential:
                // ScaleExponentially.process: value * base ^ exponent, in double.
                return static_cast<float>(static_cast<double>(value) *
                                          std::pow(static_cast<double>(first.Calculate(level)),
                                                   static_cast<double>(second.Calculate(level))));
            case Type::AllOf:
                for (const EnchantmentValueEffect& e : effects) value = e.Process(level, random, value);
                return value;
            case Type::RemoveBinomial: {
                // RemoveBinomial.process, statement for statement: a normal
                // approximation for large, well-spread counts, else one draw
                // per unit.
                const float n = value;
                const float p = first.Calculate(level);
                int drop = 0;
                if (!(n <= 128.0f) && !(n * p < 20.0f) && !(n * (1.0f - p) < 20.0f)) {
                    const double miu = std::floor(static_cast<double>(n * p));
                    const double sigma = std::sqrt(static_cast<double>(n * p * (1.0f - p)));
                    // Math.round(double): floor(x + 0.5).
                    drop = static_cast<int>(std::floor(miu + random.NextGaussian() * sigma + 0.5));
                    drop = std::clamp(drop, 0, static_cast<int>(n));
                } else {
                    for (int y = 0; static_cast<float>(y) < n; ++y) {
                        if (random.NextFloat() < p) ++drop;
                    }
                }
                return n - static_cast<float>(drop);
            }
        }
        return value;
    }

    bool EnchantmentValueEffect::Parse(const nlohmann::json& j, EnchantmentValueEffect& out) {
        out = EnchantmentValueEffect{};
        if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return false;
        const std::string_view type = StripDefaultNamespace(j["type"].get_ref<const std::string&>());
        auto value = [&](const char* key, LevelBasedValue& into) {
            return j.contains(key) && LevelBasedValue::Parse(j[key], into);
        };
        if (type == "add")             { out.type = Type::Add;            return value("value", out.first); }
        if (type == "set")             { out.type = Type::Set;            return value("value", out.first); }
        if (type == "multiply")        { out.type = Type::Multiply;       return value("factor", out.first); }
        if (type == "remove_binomial") { out.type = Type::RemoveBinomial; return value("chance", out.first); }
        if (type == "exponential") {
            out.type = Type::Exponential;
            const bool base = value("base", out.first);
            const bool exponent = value("exponent", out.second);
            return base && exponent;
        }
        if (type == "all_of") {
            out.type = Type::AllOf;
            if (!j.contains("effects") || !j["effects"].is_array()) return false;
            for (const auto& e : j["effects"]) {
                EnchantmentValueEffect inner;
                if (!Parse(e, inner)) return false;
                out.effects.push_back(std::move(inner));
            }
            return true;
        }
        return false;
    }

    bool ItemHolderSetContains(const std::vector<std::string>& entries, ItemID item) {
        if (entries.empty()) return false;
        const std::string slug = std::string(ItemRegistry::Slug(item));
        if (slug.empty()) return false;
        const std::string id = "minecraft:" + slug;
        const std::vector<std::string>* tags = nullptr;   // fetched lazily
        for (const std::string& e : entries) {
            if (e.empty()) continue;
            if (e[0] == '#') {
                if (!tags) tags = &DataTags::TagsFor(DataTags::Registry::Item, id);
                const std::string tag = "#" + WithNamespace(e.substr(1));
                if (std::find(tags->begin(), tags->end(), tag) != tags->end()) return true;
            } else if (WithNamespace(e) == id) {
                return true;
            }
        }
        return false;
    }

} // namespace Game
