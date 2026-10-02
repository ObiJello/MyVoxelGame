// File: src/common/world/loot/ContextNumberProviderNames.hpp
//
// The names of MC 26.3's context number providers (world/level/storage/loot/
// providers/number/{floats,ints}) — the `/data modify … compute` argument:
//
//   * the built-in registry entries (ContextFloatProviders / ContextIntProviders
//     .bootstrap): "cooking/speed_default", "compostable/low", …;
//   * the provider TYPES (ContextFloatProviderTypes / ContextIntProviderTypes)
//     with the fields each one's codec reads.
//
// Shared by the server's parser (server/commands/ContextNumberProviders) and
// the chat's completion, so both know the same set.
#pragma once

#include <array>
#include <string_view>

namespace Game::ContextNumberProviderNames {

    // ContextFloatProviders.bootstrap.
    inline constexpr std::array<std::string_view, 4> kFloatIds = {
        "brewing/speed_default", "cooking/fast_speed_multiplier",
        "cooking/normal_speed_multiplier", "cooking/speed_default",
    };

    // ContextIntProviders.bootstrap.
    inline constexpr std::array<std::string_view, 25> kIntIds = {
        "brewing/uses_default",
        "compostable/always_add_one", "compostable/low", "compostable/low_medium",
        "compostable/medium", "compostable/medium_high",
        "cooking/fast_burn_time_reduction_factor", "cooking/normal_burn_time_reduction_factor",
        "cooking/time_bamboo", "cooking/time_blaze_rod", "cooking/time_boats", "cooking/time_coal",
        "cooking/time_coal_block", "cooking/time_dried_kelp_block", "cooking/time_dry_plants",
        "cooking/time_hanging_signs", "cooking/time_lava_bucket", "cooking/time_roots",
        "cooking/time_wood_blocks", "cooking/time_wood_items_extra_small",
        "cooking/time_wood_items_large", "cooking/time_wood_items_small", "cooking/time_wood_slabs",
        "cooking/time_wool", "cooking/time_wool_carpets",
    };

    struct TypeFields {
        std::string_view type;
        std::array<std::string_view, 3> fields;   // empty entries unused
    };

    // ContextFloatProviderTypes, with each codec's fields.
    inline constexpr std::array<TypeFields, 28> kFloatTypes = {{
        {"abs", {"input"}}, {"add", {"inputs"}}, {"avg", {"inputs"}}, {"ceil", {"input"}},
        {"conditional", {"condition", "on_true", "on_false"}}, {"constant", {"value"}}, {"cos", {"input"}},
        {"div", {"left", "right"}}, {"enchantment_level", {"amount"}}, {"environment_attribute", {"attribute"}},
        {"floor", {"input"}}, {"from_int", {"input"}}, {"length", {"inputs"}}, {"max", {"inputs"}},
        {"min", {"inputs"}}, {"mod", {"left", "right"}}, {"mul", {"inputs"}}, {"negate", {"input"}},
        {"number_dispatcher", {"cases", "default"}}, {"pow", {"base", "exponent"}}, {"round", {"input"}},
        {"sin", {"input"}}, {"sqrt", {"input"}}, {"storage", {"storage", "path", "fallback"}},
        {"sub", {"left", "right"}}, {"truncate", {"input"}}, {"uniform", {"min", "max"}},
        {"weighted_list", {"distribution"}},
    }};

    // ContextIntProviderTypes, with each codec's fields.
    inline constexpr std::array<TypeFields, 23> kIntTypes = {{
        {"abs", {"input"}}, {"add", {"inputs"}}, {"avg", {"inputs"}}, {"binomial", {"n", "p"}},
        {"conditional", {"condition", "on_true", "on_false"}}, {"constant", {"value"}},
        {"div", {"left", "right"}}, {"environment_attribute", {"attribute"}},
        {"floor_div", {"left", "right"}}, {"floor_mod", {"left", "right"}}, {"from_float", {"input"}},
        {"max", {"inputs"}}, {"min", {"inputs"}}, {"mod", {"left", "right"}}, {"mul", {"inputs"}},
        {"negate", {"input"}}, {"number_dispatcher", {"cases", "default"}}, {"pow", {"base", "exponent"}},
        {"score", {"target", "score", "fallback"}}, {"storage", {"storage", "path", "fallback"}},
        {"sub", {"left", "right"}}, {"uniform", {"min", "max"}}, {"weighted_list", {"distribution"}},
    }};

} // namespace Game::ContextNumberProviderNames
