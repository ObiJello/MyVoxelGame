// File: src/common/world/enchantment/EnchantmentValueEffect.hpp
//
// The data-driven number machinery of MC's enchantment effects:
//
//   LevelBasedValue          world/item/enchantment/LevelBasedValue.java —
//                            a number that depends on the enchantment level
//                            (constant, linear, clamped, fraction,
//                            levels_squared, exponent, lookup).
//   EnchantmentValueEffect   world/item/enchantment/effects/
//                            EnchantmentValueEffect.java — how an effect
//                            rewrites a value (add, multiply, set,
//                            remove_binomial, exponential, all_of).
//
// Parsed straight from the enchantment JSON (data/minecraft/enchantment/*),
// the same codecs' rules. The conditions that gate them (ConditionalEffect's
// `requirements`) and every other effect shape live in EnchantmentEffects.hpp;
// the value components (damage, damage_protection, item_damage,
// block_experience, ...) are lists of these gated by those.
#pragma once

#include "common/entity/Item.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace Game {

    class JavaRandom;

    struct LevelBasedValue {
        enum class Type : uint8_t {
            Constant, Linear, Clamped, Fraction, LevelsSquared, Exponent, Lookup
        };
        Type  type = Type::Constant;
        // Constant: a = value. Linear: a = base, b = per_level_above_first.
        // Clamped: a = min, b = max. LevelsSquared: a = added.
        float a = 0.0f;
        float b = 0.0f;
        // Clamped: {value}. Fraction: {numerator, denominator}.
        // Exponent: {base, power}. Lookup: {fallback}.
        std::vector<LevelBasedValue> children;
        std::vector<float> values;   // Lookup

        // MC LevelBasedValue.calculate.
        float Calculate(int level) const;

        // MC LevelBasedValue.CODEC: a bare number is a Constant, otherwise the
        // type-dispatched object. Returns false (and leaves `out` a zero
        // Constant) on anything it cannot read.
        static bool Parse(const nlohmann::json& j, LevelBasedValue& out);
    };

    struct EnchantmentValueEffect {
        enum class Type : uint8_t {
            Add, Multiply, Set, RemoveBinomial, Exponential, AllOf
        };
        Type type = Type::Add;
        // Add / Set: value. Multiply: factor. RemoveBinomial: chance.
        // Exponential: base (first) and exponent (second).
        LevelBasedValue first;
        LevelBasedValue second;
        std::vector<EnchantmentValueEffect> effects;   // AllOf

        // MC EnchantmentValueEffect.process(level, random, value).
        float Process(int level, JavaRandom& random, float value) const;

        static bool Parse(const nlohmann::json& j, EnchantmentValueEffect& out);
    };

    // MC HolderSet.contains(item) over raw entries ("#ns:tag" / "ns:item"),
    // through DataTags' item tags. Shared with Repairable and the enchantment
    // definitions' supported / primary item sets.
    bool ItemHolderSetContains(const std::vector<std::string>& entries, ItemID item);

} // namespace Game
