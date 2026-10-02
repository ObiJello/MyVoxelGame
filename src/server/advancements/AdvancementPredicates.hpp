// File: src/server/advancements/AdvancementPredicates.hpp
//
// MC's advancement predicates (net.minecraft.advancements.predicates) and the
// loot conditions a criterion's ContextAwarePredicate is made of
// (net.minecraft.world.level.storage.loot.predicates), evaluated straight
// from the data pack's JSON:
//
//   LootItemCondition  entity_properties, location_check,
//                      block_state_property, match_tool, all_of, any_of,
//                      inverted, random_chance, weather_check, time_check
//                      — a JSON list is all_of, as LootItemCondition.CODEC
//                      reads one.
//   EntityPredicate    type, distance, location, stepping_on,
//                      movement_affected_by, movement, effects, flags,
//                      equipment, vehicle, passenger, targeted_entity,
//                      periodic_tick, components (the entity variant
//                      components), type_specific (player, lightning,
//                      fishing_hook, slime/cube_mob, sheep, raider)
//   LocationPredicate  position, dimension, biomes, structures, smokey,
//                      light, block, fluid, can_see_sky
//   ItemPredicate      through ComponentNbt::ItemPredicateMatches over the
//                      predicate's NBT form (so items, count, components and
//                      the partial predicates match exactly what the
//                      container-lock / can_place_on paths already do)
//   DamageSourcePredicate, DamagePredicate, DistancePredicate,
//   MobEffectsPredicate, BlockPredicate, StatePropertiesPredicate,
//   MinMaxBounds.
//
// The context (MC LootContext) carries what the criterion's trigger supplied:
// the level and ORIGIN, THIS_ENTITY, the BLOCK_STATE and the TOOL.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <optional>

namespace Game {
    class Entity;
    struct ItemStack;
    struct DamageSourceInfo;
}

namespace Server {
    class ServerLevel;
    class ServerPlayer;
}

namespace Server::Advancements {

    struct LootContext {
        ServerLevel*      level  = nullptr;       // the level ORIGIN is in
        std::optional<glm::dvec3> origin;          // ORIGIN
        Game::Entity*     thisEntity = nullptr;    // THIS_ENTITY
        std::optional<Game::BlockState> blockState;   // BLOCK_STATE
        const Game::ItemStack* tool = nullptr;     // TOOL
    };

    // MC EntityPredicate.createContext(player, entity): THIS_ENTITY = the
    // entity, ORIGIN = the player's position, in the player's level.
    LootContext EntityContext(ServerPlayer& player, Game::Entity* entity);

    // A ContextAwarePredicate (a condition object or a list = all_of). An
    // absent (null) predicate passes.
    bool TestConditions(const nlohmann::json* conditions, const LootContext& ctx);

    // EntityPredicate.matches(level, position, entity). `position` is MC's
    // ORIGIN (what `distance` measures from); null entity never matches.
    bool TestEntity(const nlohmann::json& predicate, ServerLevel* level,
                    const std::optional<glm::dvec3>& position, Game::Entity* entity);

    // LocationPredicate.matches(level, x, y, z).
    bool TestLocation(const nlohmann::json& predicate, ServerLevel* level, double x, double y, double z);

    // ItemPredicate.test. Empty stacks fail anything but an empty predicate.
    bool TestItem(const nlohmann::json& predicate, const Game::ItemStack& stack);

    // DamageSourcePredicate.matches(player, source).
    bool TestDamageSource(const nlohmann::json& predicate, ServerPlayer& player,
                          const Game::DamageSourceInfo& source);
    // DamagePredicate.matches(player, source, dealt, taken, blocked).
    bool TestDamage(const nlohmann::json& predicate, ServerPlayer& player, const Game::DamageSourceInfo& source,
                    float dealt, float taken, bool blocked);

    // DistancePredicate.matches(fx, fy, fz, tx, ty, tz).
    bool TestDistance(const nlohmann::json& predicate, const glm::dvec3& from, const glm::dvec3& to);

    // MinMaxBounds.Ints / Doubles: a bare number is an exact match, an
    // object {min?, max?} a range. Absent passes.
    bool TestBounds(const nlohmann::json* bounds, double value);

    // StatePropertiesPredicate.matches(state).
    bool TestStateProperties(const nlohmann::json& predicate, Game::BlockState state);

    // A HolderSet<Block> entry list ("minecraft:x", ["a","b"], "#tag").
    bool BlockMatchesHolderSet(const nlohmann::json& set, Game::BlockState state);

    // MC registry ids of what the predicates compare.
    std::string EntityTypeIdOf(const Game::Entity& entity);   // "minecraft:zombie" / "minecraft:player"
    std::string BlockIdOf(Game::BlockState state);             // "minecraft:stone"
    std::string DimensionIdOf(const ServerLevel* level);       // "minecraft:overworld"

    // The player behind a level's player entity (PlayerEntityView), else null.
    ServerPlayer* PlayerOf(const Game::Entity* entity);

} // namespace Server::Advancements
