// File: src/server/commands/ContextNumberProviders.hpp
//
// MC 26.3's context number providers (world/level/storage/loot/providers/
// number: ContextFloatProvider / ContextIntProvider and every type both
// registries hold) with the LootContext the `/data modify … compute` command
// builds for them (LootContextSources: default / block <pos> / entity
// <target>).
//
// The argument is MC's ResourceOrIdArgument: an id names a built-in entry
// (ContextFloatProviders / ContextIntProviders.bootstrap — "cooking/
// time_coal", "compostable/low" …); anything else is inline SNBT read with
// the providers' codecs ({type:"uniform",min:1,max:6}, inputs lists, holder
// fields that are themselves ids or inline providers).
//
// Conditions (conditional, number_dispatcher, and the int/float value
// checks) are MC LootItemConditions: match_block, int_value_check,
// float_value_check, table_bonus, survives_explosion, killed_by_player,
// entity_scores, inverted / all_of / any_of here, and every other kind
// through the enchantment system's condition subset (EnchantmentEffects.hpp).
//
// DELIBERATE DEVIATIONS (no engine system behind them): `environment_attribute`
// providers are refused at parse time (the engine has no environment
// attributes); the scoreboard is always empty, so `score` answers its
// fallback and `entity_scores` tests false; match_block's `nbt` / `components`
// sub-predicates and conditions outside the subsets above test false.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace Game {
    class Entity;
    class JavaRandom;
    struct EntityLevel;
    struct IBlockAccess;
}

namespace Server::NumberProviders {

    // MC LootContext, the parameters the compute sources set.
    struct Context {
        Game::JavaRandom*         random = nullptr;      // the level's
        Game::EntityLevel*        level = nullptr;
        const Game::IBlockAccess* blocks = nullptr;
        Game::Entity*             thisEntity = nullptr;  // THIS_ENTITY (the source's entity)
        Game::Entity*             targetEntity = nullptr;// TARGET_ENTITY (compute entity)
        glm::dvec3                origin{0.0};           // ORIGIN
        bool                      hasBlockState = false; // BLOCK_STATE (compute block)
        Game::BlockState          blockState{};
    };

    struct Node;
    using Provider = std::shared_ptr<const Node>;

    // ResourceOrIdArgument over the float / int registries. Null with MC's
    // message ("No element … of type …" / "Failed to parse …").
    Provider ParseFloatArgument(const std::string& text, std::string& error);
    Provider ParseIntArgument(const std::string& text, std::string& error);

    // ContextFloatProvider.getFloat (non-finite or failing -> 0) and
    // ContextIntProvider.getInt (failing -> 0).
    float GetFloat(const Provider& provider, Context& context);
    int   GetInt(const Provider& provider, Context& context);

} // namespace Server::NumberProviders
